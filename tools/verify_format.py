#!/usr/bin/env python3
"""Verify first-party sources match the tracked .clang-format."""

from __future__ import annotations

import argparse
from pathlib import Path

from x86port_checks.format import selftest, verify_format

SKIP = 77


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--root", type=Path)
    mode.add_argument("--selftest", type=Path)
    parser.add_argument(
        "--require-formatter",
        action="store_true",
        help=(
            "treat a missing clang-format as a failure rather than a skip. CI "
            "hosts that install one deliberately pass this, so a silent skip "
            "cannot masquerade as enforcement."
        ),
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = (args.root or args.selftest).resolve()
    try:
        if args.selftest:
            formatter = selftest(root)
            print(f"format negative control passed: {formatter} rewrote misformatted input")
            return 0
        result = verify_format(root)
    except FileNotFoundError as error:
        if args.require_formatter:
            print(f"REFUSED: {error}. --require-formatter says its absence is the failure.")
            return 1
        print(f"{error}. This check measures nothing without one, so it SKIPs.")
        return SKIP
    except (OSError, RuntimeError) as error:
        print(f"REFUSED: {error}")
        return 1
    if result.drifting:
        print(
            f"REFUSED: compared {result.checked} first-party source(s) with "
            f"{result.formatter}; {len(result.drifting)} do not match "
            f"{root / '.clang-format'}:"
        )
        for name in result.drifting:
            print(f"  {name}")
        print(f"  fix: {result.formatter} -i --style=file " + " ".join(result.drifting))
        return 1
    print(
        f"format OK: {result.checked} of {result.checked} first-party source(s) "
        f"match .clang-format ({result.formatter})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
