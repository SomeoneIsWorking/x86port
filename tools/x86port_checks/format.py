"""Check that first-party sources match the tracked .clang-format.

Two rules this obeys, both repository guardrails rather than taste:

* A tool that cannot see something says so. With no clang-format on the
  machine, this SKIPs and names every place it looked. It never reports
  "formatted" from a scan it did not perform.
* The file list includes work not yet committed. `git ls-files --cached` alone
  lists the index, so a file written and not yet added would be invisible to
  this check; `--others --exclude-standard` adds everything git would let you
  add. `.gitignore` still decides what is generated and out of scope.

clang-format versions can disagree about the same style file, so the report
always names the binary it used and a version disagreement stays diagnosable.
"""

from __future__ import annotations

import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

# Where a formatter is looked for, in order, and every one of them is named in
# the skip so "no formatter" cannot be confused with "looked in the wrong
# place".
CANDIDATES = (
    "clang-format",
    "/opt/homebrew/opt/llvm/bin/clang-format",
    "/usr/local/opt/llvm/bin/clang-format",
    "/Library/Developer/CommandLineTools/usr/bin/clang-format",
)

# Everything first-party. vendor/ is other people's code and is not restyled.
SOURCE_GLOBS = ("*.c", "*.h", "*.cpp", "*.hpp")
EXCLUDED_PREFIXES = ("vendor/", "build/")

@dataclass(frozen=True)
class FormatResult:
    formatter: str
    checked: int
    drifting: tuple[str, ...]
    absent: tuple[str, ...]


def find_formatter() -> str | None:
    for candidate in CANDIDATES:
        found = shutil.which(candidate) if "/" not in candidate else candidate
        if found and Path(found).is_file():
            return found
    return None


def first_party_sources(root: Path) -> list[str]:
    """Every first-party source in the working tree, committed or not.

    `--cached` is the index and `--others --exclude-standard` is everything
    else git would let you add, so a file written five minutes ago is in this
    list. `.gitignore` still decides what is generated and out of scope, which
    is what keeps build trees out without a second rule.
    """
    listed = subprocess.run(
        [
            "git",
            "-C",
            str(root),
            "ls-files",
            "--cached",
            "--others",
            "--exclude-standard",
            "-z",
            "--",
            *SOURCE_GLOBS,
        ],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.split("\0")
    seen: dict[str, None] = {}
    for name in listed:
        if name and not name.startswith(EXCLUDED_PREFIXES):
            seen[name] = None
    return sorted(seen)


def _formatted(formatter: str, path: Path, root: Path) -> str:
    # --assume-filename is what makes --style=file resolve the tracked
    # .clang-format for content read on stdin as well as for a real path.
    return subprocess.run(
        [formatter, "--style=file", f"--assume-filename={path}"],
        cwd=root,
        check=True,
        capture_output=True,
        text=True,
        input=path.read_text(encoding="utf-8"),
    ).stdout


def verify_format(root: Path) -> FormatResult:
    formatter = find_formatter()
    if formatter is None:
        raise FileNotFoundError(
            "no clang-format. Looked for: " + ", ".join(CANDIDATES)
        )
    style = root / ".clang-format"
    if not style.is_file():
        raise RuntimeError(f"{style} is missing, so there is no style to check against")
    names = first_party_sources(root)
    if not names:
        raise RuntimeError(
            "git ls-files listed no first-party sources, so this would report a "
            "pass having compared nothing"
        )
    # A path in the index whose file is gone is a staged deletion, which has no
    # formatting to check. It is counted and named rather than dropped, so the
    # denominator stays the whole list.
    absent = tuple(name for name in names if not (root / name).is_file())
    present = [name for name in names if (root / name).is_file()]
    drifting = tuple(
        name
        for name in present
        if _formatted(formatter, root / name, root)
        != (root / name).read_text(encoding="utf-8")
    )
    return FormatResult(formatter, len(present), drifting, absent)
