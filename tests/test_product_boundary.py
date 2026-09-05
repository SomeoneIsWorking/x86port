"""Focused tests for portable archive-symbol spelling."""

from __future__ import annotations

import unittest

from x86port_checks.product_boundary import watched_symbols


class WatchedSymbolsTest(unittest.TestCase):
    def test_accepts_elf_and_mach_o_c_symbol_spelling(self) -> None:
        watched = frozenset({"x86p_step", "x86p_execute_decoded"})

        self.assertEqual(watched_symbols({"x86p_step"}, watched), {"x86p_step"})
        self.assertEqual(
            watched_symbols({"_x86p_execute_decoded"}, watched),
            {"x86p_execute_decoded"},
        )

    def test_does_not_strip_arbitrary_underscores(self) -> None:
        watched = frozenset({"x86p_step"})

        self.assertEqual(watched_symbols({"__x86p_step", "x86p_step_extra"}, watched), set())


if __name__ == "__main__":
    unittest.main()
