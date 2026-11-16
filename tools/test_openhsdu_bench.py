"""Tests for openhsdu_bench.py against a fake firmware on a fake serial port. Run: python3 -m pytest tools"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))

import openhsdu_bench as bench  # noqa: E402


class FakeTime:
    def __init__(self):
        self.now = 0.0

    def clock(self):
        return self.now

    def sleep(self, dt):
        self.now += dt


class FakeBoard:
    """One serial port, two personalities by baud rate, like the firmware.

    rts_reaches: does a lone RTS change get through (False models the Windows problem).
    gate_on_rts: does key mode `rts` really gate on RTS.
    """

    def __init__(self, rts_reaches=True, gate_on_rts=True, selftest_pass=True, echo=True):
        self.baudrate = bench.DATA_BAUD
        self._rts = False
        self.dtr = False
        self.rts_reaches = rts_reaches
        self.gate_on_rts = gate_on_rts
        self.selftest_pass = selftest_pass
        self.echo = echo
        self.keymode = "rts"  # the firmware default
        self.keyups = 0
        self._out = b""
        self.sent = []

    @property
    def rts(self):
        return self._rts

    @rts.setter
    def rts(self, value):
        if value and not self._rts and self.baudrate != bench.CONSOLE_BAUD and self.rts_reaches:
            self.keyups += 1
        self._rts = value

    def reset_input_buffer(self):
        self._out = b""

    def flush(self):
        pass

    def read(self, n):
        out, self._out = self._out[:n], self._out[n:]
        return out

    def write(self, b):
        self.sent.append((self.baudrate, bytes(b)))
        if self.baudrate == bench.CONSOLE_BAUD:
            self._out += self._console(b.decode().strip()).encode()
            return
        if not self.echo:
            return
        rts_up = self._rts and self.rts_reaches
        if self.keymode == "rts" and self.gate_on_rts and not rts_up:
            return  # ignored: not keyed
        self._out += bytes(b)

    def _console(self, line):
        if line == "selftest":
            return ("part=0x00 version=0x14 detected\nregisters: all match profile proven\npatable: ok\n"
                    "marcstate=0x0D (receiving)\nPASS\n") if self.selftest_pass else "part=0x00 version=0x14 detected\nFAIL\n"
        if line == "keymode":
            return "keymode %s\n" % self.keymode
        if line.startswith("keymode "):
            self.keymode = line.split()[1]
            return "ok keymode %s\n" % self.keymode
        if line == "status":
            return "state=rx dtr=0 rts=0 lockout=0 keymode=%s\ntx=0 rx=0 keyups=%d watchdog=0 faults=0\n" % (self.keymode, self.keyups)
        return "error: unknown command\n"


def test_report_tracks_failure():
    r = bench.Report()
    r.ok("a")
    assert not r.failed
    r.fail("b")
    assert r.failed
    assert "PASS  a" in str(r) and "FAIL  b" in str(r)


def test_console_command_switches_to_the_console_baud_and_collects_output():
    t = FakeTime()
    board = FakeBoard()
    out = bench.console_command(board, "keymode data", clock=t.clock, sleep=t.sleep)
    assert board.baudrate == bench.CONSOLE_BAUD
    assert out == "ok keymode data\n"
    assert t.now < 3.0


def test_status_fields():
    f = bench.status_fields("state=rx dtr=1 rts=0 lockout=0 keymode=either\ntx=3 rx=0 keyups=2 watchdog=0 faults=0\n")
    assert f["state"] == "rx" and f["keyups"] == "2" and f["tx"] == "3"


def test_selftest_pass_and_fail():
    t = FakeTime()
    r = bench.Report()
    assert bench.check_selftest(FakeBoard(), r, clock=t.clock, sleep=t.sleep)
    r2 = bench.Report()
    assert not bench.check_selftest(FakeBoard(selftest_pass=False), r2, clock=t.clock, sleep=t.sleep)
    assert r2.failed


def test_rts_reaches_firmware_and_gates_transmission():
    t = FakeTime()
    board = FakeBoard()
    r = bench.Report()
    assert bench.check_rts(board, r, clock=t.clock, sleep=t.sleep)
    assert board.keymode == "rts"  # it puts the mode back to what it found
    assert "RTS reached the firmware" in str(r)
    assert "nothing was transmitted" in str(r)


def test_rts_never_arrives_is_reported_with_the_remedy():
    t = FakeTime()
    board = FakeBoard(rts_reaches=False)
    r = bench.Report()
    assert not bench.check_rts(board, r, clock=t.clock, sleep=t.sleep)
    assert "RTS did not reach the firmware" in str(r)
    assert "`either` or `data`" in str(r)
    assert "stored in flash" in str(r)


def test_a_firmware_that_ignores_rts_gating_is_caught():
    t = FakeTime()
    board = FakeBoard(gate_on_rts=False)
    r = bench.Report()
    assert not bench.check_rts(board, r, clock=t.clock, sleep=t.sleep)
    assert "not gating on RTS" in str(r)


def test_the_rts_check_restores_a_non_default_key_mode():
    t = FakeTime()
    board = FakeBoard()
    board.keymode = "either"
    bench.check_rts(board, bench.Report(), clock=t.clock, sleep=t.sleep)
    assert board.keymode == "either"


def test_the_rts_check_never_leaves_the_port_with_rts_up():
    t = FakeTime()
    board = FakeBoard()
    bench.check_rts(board, bench.Report(), clock=t.clock, sleep=t.sleep)
    assert board.rts is False


def test_echo_passes_with_and_without_rts_and_restores_the_key_mode():
    t = FakeTime()
    board = FakeBoard()  # firmware default: key mode rts
    r = bench.Report()
    assert bench.check_echo(board, r, clock=t.clock, sleep=t.sleep)
    assert "with RTS" in str(r) and "without RTS" in str(r)
    probes = [b for baud, b in board.sent if baud == bench.DATA_BAUD]
    assert probes == [bench.ECHO_PROBE, bench.ECHO_PROBE]
    assert board.keymode == "rts"


def test_echo_in_data_mode_skips_the_with_rts_case():
    t = FakeTime()
    board = FakeBoard()
    board.keymode = "data"
    r = bench.Report()
    assert bench.check_echo(board, r, clock=t.clock, sleep=t.sleep)
    assert "skipped" in str(r)
    assert board.keymode == "data"


def test_echo_missing_is_reported_for_each_case():
    t = FakeTime()
    r = bench.Report()
    assert not bench.check_echo(FakeBoard(echo=False), r, clock=t.clock, sleep=t.sleep)
    assert str(r).count("echo missing") == 2


def test_probe_is_opaque_nine_0xaa_then_arbitrary_bytes():
    p = bench.ECHO_PROBE
    assert p[:9] == bytes([0xAA] * 9)  # nine preamble bytes
    assert len(p) > 9


def test_open_port_sets_rts_low_before_opening():
    events = []

    class Ser:
        def __init__(self):
            self._rts = None

        def __setattr__(self, k, v):
            if k in ("rts", "dtr", "port", "baudrate", "timeout"):
                events.append((k, v))
            object.__setattr__(self, k, v)

        def open(self):
            events.append(("open", None))

    class Mod:
        Serial = Ser

    bench.open_port("COM9", serial_module=Mod)
    names = [e[0] for e in events]
    assert names.index("rts") < names.index("open")
    assert ("rts", False) in events and ("dtr", True) in events and ("baudrate", bench.CONSOLE_BAUD) in events


def test_find_ports_matches_by_name_only():
    class P:
        def __init__(self, device, description, product=""):
            self.device, self.description, self.product, self.manufacturer = device, description, product, ""

    ports = bench.find_ports(lambda: [P("COM3", "USB Serial"), P("COM7", "open-hsdu"), P("COM8", "x", "Open-HSDU")])
    assert [d for d, _ in ports] == ["COM7", "COM8"]
    assert bench.find_ports(lambda: []) == []
