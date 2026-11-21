"""leave_loader: the register sequence, in order, and that a failing last write (the chip already reset) is not an error."""
import leave_loader as ll


class FakeEsp:
    def __init__(self, fail_from=None):
        self.writes, self.fail_from = [], fail_from

    def write_reg(self, addr, value):
        if self.fail_from is not None and len(self.writes) >= self.fail_from:
            raise IOError("chip gone")
        self.writes.append((addr, value))


def test_sequence_unlocks_sets_timeout_enables_then_locks():
    esp = FakeEsp()
    ll.leave_loader(esp)
    assert esp.writes == [
        (0x600080B0, 0x50D83AA1),
        (0x6000809C, 2000),
        (0x60008098, 0xD0000102),
        (0x600080B0, 0),
    ]


def test_chip_resetting_before_the_lock_write_is_fine():
    esp = FakeEsp(fail_from=3)
    ll.leave_loader(esp)
    assert len(esp.writes) == 3


def test_wrong_arguments_print_usage(capsys):
    assert ll.main([]) == 2
    assert "usage" in capsys.readouterr().out
