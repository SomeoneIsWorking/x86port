#!/usr/bin/env python3
"""CLI for the x86port product/oracle archive boundary check."""

from __future__ import annotations

import argparse
from pathlib import Path

from x86port_checks.product_boundary import verify_product_boundary


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--nm", required=True, type=Path)
    parser.add_argument("--product", required=True, type=Path)
    parser.add_argument("--oracle", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        product_count, oracle_count = verify_product_boundary(args.nm, args.product, args.oracle)
    except RuntimeError as error:
        print(f"REFUSED: {error}")
        return 1
    print(
        "product/oracle boundary verified: "
        f"{product_count} product symbol(s), {oracle_count} oracle symbol(s), "
        "0 forbidden product references"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
