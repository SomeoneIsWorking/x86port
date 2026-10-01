#!/usr/bin/env python3
"""Verify that shipping x86port sources use owned config and diagnostics."""

from __future__ import annotations

import argparse
from pathlib import Path

from x86port_checks.source_policy import verify_source_policy


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        count, violations = verify_source_policy(args.root.resolve())
    except (OSError, RuntimeError) as error:
        print(f"REFUSED: {error}")
        return 1
    if violations:
        print(
            f"REFUSED: scanned {count} product source(s), found {len(violations)} violation(s)"
        )
        for violation in violations:
            print(
                f"  {violation.path}:{violation.line}: {violation.policy}: {violation.excerpt}"
            )
        return 1
    print(
        f"source policy passed: scanned {count} product source(s), found 0 violations"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
