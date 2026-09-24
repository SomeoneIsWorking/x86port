#!/usr/bin/env python3
"""Run the same runtime build and tests locally and on each CI host."""

from __future__ import annotations

import argparse
from pathlib import Path

from x86port_checks.build import verify


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=root / "build" / "verify")
    parser.add_argument("--jit-common", type=Path, default=root.parent / "jit-common")
    parser.add_argument("--cc", required=True)
    parser.add_argument("--cxx", required=True)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument(
        "--binary128-model",
        action="store_true",
        help="build with Android x86-64's binary128 long double (Linux x86-64, Clang)",
    )
    args = parser.parse_args()
    verify(
        root,
        args.build_dir,
        args.jit_common,
        args.cc,
        args.cxx,
        args.jobs,
        args.binary128_model,
    )


if __name__ == "__main__":
    main()
