#!/usr/bin/env python3
"""Bench checks for an open-hsdu board, run from a PC.

The board has ONE USB serial port. Opened at 115200 baud it is the text console; at any other baud it is the HSDU
data port (4800 here). The checks switch the baud rate of the one port as they go. Each check is a small function over
an object with the pyserial interface (`baudrate`, `rts`, `dtr`, `write`, `read`, ...), so the tests run them
against a fake port. Nothing here knows any game or pack protocol.

  python3 tools/openhsdu_bench.py find                  # list serial ports named open-hsdu
  python3 tools/openhsdu_bench.py console PORT selftest # one console command
  python3 tools/openhsdu_bench.py rts PORT              # does RTS reach the firmware (the Windows question)?
  python3 tools/openhsdu_bench.py echo PORT             # do transmitted bytes come back, with and without RTS?

`rts` and `echo` TRANSMIT a short burst at the firmware's power step (the lowest, by default). Fit the attenuator
and antenna first (docs/HARDWARE.md, docs/REGULATORY.md).
"""
import argparse
import re
import sys
import time

CONSOLE_BAUD = 115200
DATA_BAUD = 4800
# Opaque test bytes: nine 0xAA preamble bytes, then an arbitrary payload. The firmware never looks inside.
ECHO_PROBE = bytes([0xAA] * 9 + [0x31, 0x32, 0x33, 0x34, 0x35, 0x36])


class Report:
    """Collects PASS/FAIL lines so the same checks can print or be asserted on."""

    def __init__(self):
        self.lines = []
        self.failed = False

    def ok(self, text):
        self.lines.append("PASS  " + text)

    def fail(self, text):
        self.failed = True
        self.lines.append("FAIL  " + text)

    def info(self, text):
        self.lines.append("      " + text)

    def __str__(self):
        return "\n".join(self.lines)


def open_port(device, serial_module=None):
    """Opens the port with RTS low and DTR high *before* it opens, so a console session cannot key the radio."""
    if serial_module is None:
        import serial as serial_module
    ser = serial_module.Serial()
    ser.port = device
    ser.baudrate = CONSOLE_BAUD
    ser.timeout = 0.05
    ser.rts = False
    ser.dtr = True
    ser.open()
    return ser


def console_command(port, command, wait=0.4, clock=time.monotonic, sleep=time.sleep):
    """Switches to the console baud rate, sends one line, returns everything printed within `wait` seconds."""
    if port.baudrate != CONSOLE_BAUD:
        port.baudrate = CONSOLE_BAUD
        sleep(0.3)  # give the firmware time to see the new line coding
    port.reset_input_buffer()
    port.write((command + "\n").encode("ascii"))
    port.flush()
    deadline = clock() + wait
    out = b""
    while clock() < deadline:
        chunk = port.read(256)
        if chunk:
            out += chunk
            deadline = clock() + 0.15  # stay a little longer while text is still arriving
        else:
            sleep(0.02)
    return out.decode("ascii", "replace")


def status_fields(text):
    """Parses `status` output into a dict: state, dtr, rts, tx, rx, keyups, watchdog, faults, ..."""
    fields = {}
    for key, value in re.findall(r"([a-z\-]+)=([^\s]+)", text):
        fields[key] = value
    return fields


def check_selftest(port, report, clock=time.monotonic, sleep=time.sleep):
    text = console_command(port, "selftest", wait=0.8, clock=clock, sleep=sleep)
    report.info(text.strip().replace("\n", " | "))
    if "PASS" in text:
        report.ok("chip detected, registers read back, receiving")
        return True
    report.fail("selftest did not pass")
    return False


def _send_and_read(port, probe, rts, sleep, clock, lead=0.08, settle=0.03):
    """Sends `probe` the way a host program drives the real HSDU and returns what came back."""
    port.baudrate = DATA_BAUD
    sleep(0.3)
    port.dtr = True
    port.rts = False
    sleep(0.05)
    port.reset_input_buffer()
    if rts:
        port.rts = True
        sleep(lead)
    port.write(probe)
    port.flush()
    sleep(len(probe) * 10.0 / 4800.0 + settle + len(probe) * 0.001)
    if rts:
        port.rts = False
    # With no RTS the firmware adds its own 85 ms lead and 80 ms tail: wait for those too.
    deadline = clock() + (0.4 if rts else 0.7)
    got = b""
    while clock() < deadline:
        chunk = port.read(256)
        if chunk:
            got += chunk
        else:
            sleep(0.02)
    return got


def check_rts(port, report, clock=time.monotonic, sleep=time.sleep):
    """Does a lone RTS change over USB reach the firmware? Checked by behaviour, in key mode `rts`: bytes are sent
    (and echoed) only while RTS is up, so an echo with RTS and none without proves the line reached the firmware."""
    ask = lambda line: console_command(port, line, clock=clock, sleep=sleep)
    match = re.search(r"keymode (\w+)", ask("keymode"))
    previous = match.group(1) if match else "rts"  # the firmware default
    ask("keymode rts")
    before = status_fields(ask("status"))
    without = _send_and_read(port, ECHO_PROBE, rts=False, sleep=sleep, clock=clock)
    with_rts = _send_and_read(port, ECHO_PROBE, rts=True, sleep=sleep, clock=clock)
    ask("keymode " + previous)  # put back whatever it was (the choice is stored in flash)
    after = status_fields(ask("status"))
    if ECHO_PROBE in with_rts:
        report.ok("RTS reached the firmware and keyed the transmitter (the bytes came back)")
    else:
        report.fail("no echo with RTS up: RTS did not reach the firmware. Set key mode `either` or `data` on the console "
                    "(it is stored in flash): the firmware then keys on the bytes and supplies the lead-in and tail itself")
    if ECHO_PROBE in without:
        report.fail("bytes were echoed with RTS low in key mode `rts`: the firmware is not gating on RTS")
    else:
        report.ok("with RTS low in key mode `rts` nothing was transmitted")
    report.info("keyups %s -> %s" % (before.get("keyups"), after.get("keyups")))
    return not report.failed


def check_echo(port, report, probe=ECHO_PROBE, clock=time.monotonic, sleep=time.sleep):
    """Sends the probe with RTS (like the real HSDU is driven) and expects the same bytes back; then, with the key mode
    temporarily set to `either`, sends it with no RTS at all (the firmware keys on the data) and expects them back again.
    The key mode is put back afterwards."""
    ask = lambda line: console_command(port, line, clock=clock, sleep=sleep)
    match = re.search(r"keymode (\w+)", ask("keymode"))
    previous = match.group(1) if match else "rts"
    ok = True
    try:
        if previous == "data":
            report.info("key mode is `data`: RTS is ignored, so the with-RTS case is skipped")
        else:
            got = _send_and_read(port, probe, rts=True, sleep=sleep, clock=clock)
            if probe in got:
                report.ok("the transmitted bytes came back with RTS, %d bytes" % len(probe))
            else:
                report.fail("echo missing or wrong with RTS: sent %d bytes, got %d (%s)" % (len(probe), len(got), got[:24].hex()))
                ok = False
        ask("keymode either")
        got = _send_and_read(port, probe, rts=False, sleep=sleep, clock=clock)
        if probe in got:
            report.ok("the transmitted bytes came back without RTS (keyed by the data), %d bytes" % len(probe))
        else:
            report.fail("echo missing or wrong without RTS: sent %d bytes, got %d (%s)" % (len(probe), len(got), got[:24].hex()))
            ok = False
    finally:
        ask("keymode " + previous)
    return ok


def find_ports(list_ports=None):
    if list_ports is None:
        from serial.tools import list_ports as lp
        list_ports = lp.comports
    out = []
    for p in list_ports():
        text = " ".join(str(x) for x in (p.device, p.description, getattr(p, "product", ""), getattr(p, "manufacturer", "")))
        if "open-hsdu" in text.lower():
            out.append((p.device, p.description))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("find")
    c = sub.add_parser("console")
    c.add_argument("port")
    c.add_argument("line", nargs="+")
    for name in ("rts", "echo"):
        sub.add_parser(name).add_argument("port")
    args = ap.parse_args(argv)

    if args.cmd == "find":
        ports = find_ports()
        for dev, desc in ports:
            print(dev, desc)
        return 0 if ports else 1
    port = open_port(args.port)
    try:
        if args.cmd == "console":
            print(console_command(port, " ".join(args.line), wait=1.0), end="")
            return 0
        report = Report()
        (check_rts if args.cmd == "rts" else check_echo)(port, report)
        print(report)
        return 1 if report.failed else 0
    finally:
        port.close()


if __name__ == "__main__":
    sys.exit(main())
