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

THE FILE LIST INCLUDES WORK NOT YET COMMITTED, and that is the whole point of
the second selftest. This check used to enumerate with `git ls-files`, which
lists the INDEX -- so a file created and not yet added was invisible to it. The
check therefore passed on exactly the sources most likely to have drifted, the
ones being written, and only began refusing once they were committed. That is
not hypothetical: commit 70e6536 landed two unformatted new files behind a
green run of this very check, and the refusal arrived on the next run, after
the damage. A gate whose blind spot is new code is worse than no gate, because
its pass is read as a statement about the change in hand.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
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


def _git(repo: Path, *arguments: str) -> None:
    subprocess.run(
        ["git", "-C", str(repo), *arguments],
        check=True,
        capture_output=True,
        text=True,
        env={
            **os.environ,
            "GIT_AUTHOR_NAME": "format selftest",
            "GIT_AUTHOR_EMAIL": "selftest@invalid",
            "GIT_COMMITTER_NAME": "format selftest",
            "GIT_COMMITTER_EMAIL": "selftest@invalid",
        },
    )


def enumeration_selftest() -> str:
    """Prove the file list contains a source that has never been committed.

    The failure this exists for is silent: an enumeration that misses new work
    reports a clean pass, so nothing about the run looks wrong. It is checked
    in a throwaway repository rather than against the real tree because the
    interesting state -- a file that is new RIGHT NOW -- cannot be arranged in
    a checkout the caller is also trying to verify.
    """
    with tempfile.TemporaryDirectory(prefix="x86port-format-") as scratch:
        repo = Path(scratch)
        _git(repo, "init", "--quiet")
        (repo / ".gitignore").write_text("build/\n", encoding="utf-8")
        (repo / "committed.c").write_text("int committed(void);\n", encoding="utf-8")
        _git(repo, "add", ".gitignore", "committed.c")
        _git(repo, "commit", "--quiet", "-m", "seed")
        (repo / "brand_new.c").write_text("int brand_new(void);\n", encoding="utf-8")
        (repo / "vendor").mkdir()
        (repo / "vendor" / "theirs.c").write_text("int theirs(void);\n", encoding="utf-8")
        (repo / "build").mkdir()
        (repo / "build" / "generated.c").write_text("int gen(void);\n", encoding="utf-8")
        found = first_party_sources(repo)
        expected = ["brand_new.c", "committed.c"]
        if found != expected:
            raise RuntimeError(
                "the enumeration is wrong, so this check's passes do not mean "
                f"what they say: expected {expected}, got {found}. An "
                "uncommitted source missing from that list is the defect this "
                "control exists for -- `git ls-files` alone lists the index."
            )
        return f"{len(found)} source(s), including one never committed"


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
