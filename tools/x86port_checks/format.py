"""Check that first-party sources match the tracked .clang-format.

CLAUDE.md requires the normal verifier to include a format check, and it did
not have one. The cost of that was measured rather than imagined: a ten-file
WebAssembly backend landed with seven files drifting from the tracked style,
while the other 126 first-party files matched it exactly -- so the repository
does hold to the style, and nothing was checking.

Two rules this obeys, both of them repository guardrails rather than taste:

* A tool that cannot see something says so. With no clang-format on the
  machine, this SKIPs and names every place it looked. It never reports
  "formatted" from a scan it did not perform.
* A negative result carries its denominator. The report says how many files
  were compared, not just how many failed.

One known hazard, stated rather than hidden: clang-format versions can disagree
about the same style file, so a CI host with a different version could report
drift a developer's machine does not. Checked on the tree this landed with,
clang-format 21.0.0 and 23.1.0 agree on all 133 files. The report always names
the binary it used, so a version disagreement is diagnosable instead of
mysterious.
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

# A deliberately misformatted translation unit for the negative control. If
# clang-format leaves this alone, the check is not checking anything.
MISFORMATTED = "int  f( int a ,int b ){if(a){return b ;}\n  return   a;}\n"


@dataclass(frozen=True)
class FormatResult:
    formatter: str
    checked: int
    drifting: tuple[str, ...]


def find_formatter() -> str | None:
    for candidate in CANDIDATES:
        found = shutil.which(candidate) if "/" not in candidate else candidate
        if found and Path(found).is_file():
            return found
    return None


def tracked_sources(root: Path) -> list[Path]:
    listed = subprocess.run(
        ["git", "-C", str(root), "ls-files", *SOURCE_GLOBS],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.splitlines()
    return [
        root / name
        for name in listed
        if not name.startswith(EXCLUDED_PREFIXES)
    ]


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
    sources = tracked_sources(root)
    if not sources:
        raise RuntimeError(
            "git ls-files listed no first-party sources, so this would report a "
            "pass having compared nothing"
        )
    drifting = tuple(
        str(path.relative_to(root))
        for path in sources
        if _formatted(formatter, path, root) != path.read_text(encoding="utf-8")
    )
    return FormatResult(formatter, len(sources), drifting)


def selftest(root: Path) -> str:
    """Prove the comparison fires. Returns the formatter it proved it with."""
    formatter = find_formatter()
    if formatter is None:
        raise FileNotFoundError(
            "no clang-format. Looked for: " + ", ".join(CANDIDATES)
        )
    rewritten = subprocess.run(
        [formatter, "--style=file", f"--assume-filename={root / 'src' / 'probe.c'}"],
        cwd=root,
        check=True,
        capture_output=True,
        text=True,
        input=MISFORMATTED,
    ).stdout
    if rewritten == MISFORMATTED:
        raise RuntimeError(
            "the negative control was left unchanged, so this check cannot "
            "detect drift and its passes mean nothing"
        )
    return formatter
