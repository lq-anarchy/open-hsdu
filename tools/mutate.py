#!/usr/bin/env python3
"""A small mutation-testing runner for the core library.

It copies the repository to a scratch directory, applies ONE small change at a time to a line of core/src (swap a
comparison, flip && and ||, flip a constant, delete a statement), rebuilds, runs the test suite, and records whether
any test failed ("killed") or all passed ("survived"). A survivor is either a missing test or an equivalent mutant (a
change that cannot alter behaviour); each one needs a human look.

  python3 tools/mutate.py --count 150 --seed 1 core/src/modem.cpp core/src/device.cpp
  python3 tools/mutate.py --list core/src/modem.cpp          # show the mutants without running them

Not run in CI (it takes many minutes); run it before a release. Results go to stdout as a table and to --out as JSON.
"""
import argparse
import json
import random
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# (regex, replacement, label). Applied to code only (comments and strings are skipped), one occurrence at a time.
OPERATORS = [
    (r"(?<![<>=!&|+\-*/%^])==(?!=)", "!=", "== -> !="),
    (r"!=(?!=)", "==", "!= -> =="),
    (r"<=", ">", "<= -> >"),
    (r">=", "<", ">= -> <"),
    (r"(?<![<\-])<(?![<=])", ">=", "< -> >="),
    (r"(?<![>\-])>(?![>=])", "<=", "> -> <="),
    (r"&&", "||", "&& -> ||"),
    (r"\|\|", "&&", "|| -> &&"),
    (r"(?<![+])\+(?![+=])", "-", "+ -> -"),
    (r"\btrue\b", "false", "true -> false"),
    (r"\bfalse\b", "true", "false -> true"),
]
NUMBER = re.compile(r"(?<![\w.])(\d+)(?![\w.])")
STATEMENT = re.compile(r"^\s+(?:\+\+|--)?[A-Za-z_][\w:\.\->\[\]]*\s*(?:\(.*\)|\s*[-+|&]?=\s*[^=].*|\+\+|--);\s*(//.*)?$")
SKIP_LINE = re.compile(r"^\s*(//|#|\*|/\*|$)|^\s*(case|default|return|break|continue|else|for|while|if|switch|namespace|static_assert)\b")


def code_part(line):
    """The part of a line before any // comment, and with string literals blanked, so we never mutate text."""
    out, i, in_str = [], 0, False
    while i < len(line):
        c = line[i]
        if in_str:
            out.append(" ")
            if c == "\\":
                i += 1
                out.append(" ")
            elif c == '"':
                in_str = False
        else:
            if c == '"':
                in_str = True
                out.append(" ")
            elif line.startswith("//", i):
                break
            else:
                out.append(c)
        i += 1
    return "".join(out)


def mutants_for(path):
    lines = Path(path).read_text().splitlines(keepends=True)
    found = []
    for n, raw in enumerate(lines):
        if SKIP_LINE.match(raw):
            continue
        code = code_part(raw)
        for pat, rep, label in OPERATORS:
            for m in re.finditer(pat, code):
                found.append((path, n, m.start(), m.end(), rep, label))
        for m in NUMBER.finditer(code):
            v = int(m.group(1))
            if v not in (0, 1) and "0x" not in code[max(0, m.start() - 2):m.start()]:
                found.append((path, n, m.start(), m.end(), str(v + 1), "%d -> %d" % (v, v + 1)))
        if STATEMENT.match(raw) and "=" not in code.split("(")[0]:
            found.append((path, n, None, None, None, "delete statement"))
    return found


def apply(mutant, text_lines):
    _, n, s, e, rep, _ = mutant
    lines = list(text_lines)
    if s is None:
        indent = re.match(r"\s*", lines[n]).group(0)
        lines[n] = indent + ";" + "\n"
    else:
        lines[n] = lines[n][:s] + rep + lines[n][e:]
    return lines


def run(cmd, cwd, timeout):
    try:
        r = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout, text=True)
        return r.returncode, r.stdout
    except subprocess.TimeoutExpired:
        return 124, "timeout"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--count", type=int, default=100, help="how many mutants to sample (0 = all)")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--only", action="append", default=[], metavar="FILE:LINE",
                    help="run only the mutants on this line (repeatable); use it to re-check survivors")
    ap.add_argument("--out", default="")
    ap.add_argument("--timeout", type=int, default=180)
    args = ap.parse_args()

    root = Path(__file__).resolve().parent.parent
    all_mutants = []
    for f in args.files:
        all_mutants += mutants_for(root / f)
    if args.only:
        wanted = {(str((root / o.rsplit(":", 1)[0]).resolve()), int(o.rsplit(":", 1)[1]) - 1) for o in args.only}
        all_mutants = [m for m in all_mutants if (str(Path(m[0]).resolve()), m[1]) in wanted]
    rng = random.Random(args.seed)
    rng.shuffle(all_mutants)
    chosen = all_mutants if args.count == 0 else all_mutants[: args.count]
    if args.list:
        for m in chosen:
            print("%s:%d  %s" % (Path(m[0]).relative_to(root), m[1] + 1, m[5]))
        print(len(all_mutants), "possible mutants,", len(chosen), "chosen")
        return 0

    work = Path(tempfile.mkdtemp(prefix="openhsdu-mutate-"))
    try:
        subprocess.run(["rsync", "-a", "--exclude", ".git", "--exclude", "build*", "--exclude", "firmware/.pio",
                        "--exclude", ".claude", str(root) + "/", str(work) + "/"], check=True)
        code, out = run(["cmake", "-S", ".", "-B", "build", "-DCMAKE_BUILD_TYPE=Debug"], work, 300)
        code, out = run(["cmake", "--build", "build", "-j", "8"], work, 900)
        if code != 0:
            print("baseline build failed:\n" + out[-2000:])
            return 2
        code, out = run(["./build/openhsdu_tests"], work, args.timeout)
        if code != 0:
            print("baseline tests fail; fix them first\n" + out[-1500:])
            return 2

        def evaluate(m):
            rel = str(Path(m[0]).relative_to(root))
            target = work / rel
            original = (root / rel).read_text().splitlines(keepends=True)
            target.write_text("".join(apply(m, original)))
            code, out = run(["cmake", "--build", "build", "-j", "8"], work, 600)
            if code != 0:
                status = "invalid"
            else:
                code, out = run(["./build/openhsdu_tests", "--abort-after=1"], work, args.timeout)
                status = "survived" if code == 0 else "killed"
            target.write_text("".join(original))
            return rel, status

        results = []
        for i, m in enumerate(chosen, 1):
            rel, status = evaluate(m)
            note = ""
            if status == "survived":
                # A survivor is only believed if it survives twice: the first pass has shown the odd false survivor.
                rel, again = evaluate(m)
                if again != "survived":
                    status, note = again, " (survived once, killed on re-check)"
            results.append({"file": rel, "line": m[1] + 1, "mutation": m[5], "status": status})
            print("[%3d/%d] %-9s %s:%d  %s%s" % (i, len(chosen), status, rel, m[1] + 1, m[5], note), flush=True)
        run(["cmake", "--build", "build", "-j", "8"], work, 600)

        valid = [r for r in results if r["status"] != "invalid"]
        killed = [r for r in valid if r["status"] == "killed"]
        print("\n%d mutants tried, %d invalid (did not compile), %d valid: %d killed, %d survived (%.1f%% killed)" % (
            len(results), len(results) - len(valid), len(valid), len(killed), len(valid) - len(killed),
            100.0 * len(killed) / max(1, len(valid))))
        for r in valid:
            if r["status"] == "survived":
                print("  SURVIVED %s:%d  %s" % (r["file"], r["line"], r["mutation"]))
        if args.out:
            Path(args.out).write_text(json.dumps(results, indent=1))
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
