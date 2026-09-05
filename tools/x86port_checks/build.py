"""Portable orchestration of the authoritative CMake product/test graph."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

JIT_COMMON_REVISION = "4512a2054b0ecf737b6dd0b03f23713d23550b3c"


def verify(
    root: Path, build_dir: Path, dependency: Path, cc: str, cxx: str, jobs: int
) -> None:
    root = root.resolve()
    build_dir = build_dir.resolve()
    dependency = dependency.resolve()
    if not build_dir.is_relative_to(root / "build") or build_dir == root / "build":
        raise ValueError(
            "verification build must be a named child of this repository's build/"
        )
    if jobs < 1:
        raise ValueError("jobs must be positive")
    revision = subprocess.run(
        ["git", "-C", str(dependency), "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    if revision != JIT_COMMON_REVISION:
        raise ValueError(
            f"jit-common {dependency} is {revision}; expected {JIT_COMMON_REVISION}"
        )
    commands = (
        [
            "cmake",
            "-S",
            str(root),
            "-B",
            str(build_dir),
            "-G",
            "Ninja",
            "-DCMAKE_BUILD_TYPE=Debug",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            f"-DCMAKE_C_COMPILER={cc}",
            f"-DCMAKE_CXX_COMPILER={cxx}",
            f"-DPython3_EXECUTABLE={sys.executable}",
            f"-DX86PORT_JITCOMMON_DIR={dependency}",
        ],
        ["cmake", "--build", str(build_dir), "--parallel", str(jobs)],
        ["ctest", "--test-dir", str(build_dir), "--output-on-failure"],
    )
    for command in commands:
        subprocess.run(command, cwd=root, check=True)
