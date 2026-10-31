#!/usr/bin/env python3
"""Regression test for audit IDLE-AFTER-02 (P3): guest PC band display.

`tools/run_idle_scenario.py` used to print the band range with a 21-bit
upper mask (`band << 17 + 0x1FFFFF`), so `dq8_part_010` claimed
`0x140000-0x33ffff` — three bands wide.  Bands are 17 bits
(tools/split_dispatch.py: band = page >> 5, 32 guest pages x 4 KiB = 0x20000),
so both bounds of a printed range must fall into the SAME band and the next
address must start the next band.  No runtime code is involved.

Run:  python3 tools/test_idle_scenario_band.py
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from run_idle_scenario import Symbolizer, guest_pc_band_range  # noqa: E402


class GuestBandRangeTest(unittest.TestCase):
    def assert_band_coherent(self, band):
        lo, hi = guest_pc_band_range(band)
        self.assertEqual(lo >> 17, band, "lower bound outside band %d" % band)
        self.assertEqual(hi >> 17, band, "upper bound outside band %d" % band)
        self.assertEqual((hi + 1) >> 17, band + 1,
                         "first address after range not in next band")
        self.assertEqual(hi - lo + 1, 0x20000, "band width must be 17 bits")

    def test_part0(self):
        self.assertEqual(guest_pc_band_range(0), (0x000000, 0x01FFFF))
        self.assert_band_coherent(0)

    def test_part10_audit_example(self):
        # audit example: dq8_part_010 must be 0x140000..0x15ffff
        self.assertEqual(guest_pc_band_range(10), (0x140000, 0x15FFFF))
        self.assert_band_coherent(10)

    def test_part11_boundary(self):
        self.assert_band_coherent(11)
        self.assert_band_coherent(127)   # last declared band in dq8_rt.h
        self.assert_band_coherent(4095)  # 12-bit page space / 32 pages

    def test_describe_formats_fixed_range(self):
        sym = Symbolizer.__new__(Symbolizer)
        sym.lookup = lambda rip: "dq8_part_010"
        self.assertEqual(sym.describe(0xdeadbeef),
                         "dq8_part_010 [guest pc 0x140000-0x15ffff]")


if __name__ == "__main__":
    unittest.main()
