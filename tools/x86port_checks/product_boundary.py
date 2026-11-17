"""Inspect built archives for the x86port product/oracle link boundary."""

from __future__ import annotations

import subprocess
from pathlib import Path

PRODUCT_FORBIDDEN_SYMBOLS = frozenset(
    {
        "x86p_engine_from_env",
        "x86p_engine_parse",
        "x86p_engine_resolve",
        "x86p_step",
        "x86p_step_cached",
    }
)
ORACLE_REQUIRED_SYMBOLS = frozenset({"x86p_step"})

# The one semantic executor ships in the product, but only these objects may
# call it; a backend reaching it would be an unbounded, uncounted interpreter edge.
SEMANTIC_EXECUTOR = "x86p_execute_decoded"
SEMANTIC_EXECUTOR_CALLERS = frozenset({"jit_fallback.c"})

ArchiveSymbols = dict[str, tuple[set[str], set[str]]]


def archive_symbols(nm: Path, archive: Path) -> ArchiveSymbols:
    """Return each archive member's (defined, undefined) global symbols, refusing tool errors."""
    result = subprocess.run(
        [str(nm), "-g", str(archive)],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "no diagnostic"
        raise RuntimeError(f"{nm} could not inspect {archive}: {detail}")
    members: ArchiveSymbols = {}
    current: tuple[set[str], set[str]] | None = None
    for line in result.stdout.splitlines():
        fields = line.split()
        if not fields:
            continue
        if line.rstrip().endswith(":") and len(fields) == 1:
            current = members.setdefault(member_name(fields[0][:-1]), (set(), set()))
            continue
        if current is None:
            current = members.setdefault("", (set(), set()))
        undefined = len(fields) >= 2 and fields[-2] == "U"
        (current[1] if undefined else current[0]).add(fields[-1])
    return members


def member_name(raw: str) -> str:
    """Return a member's source file name across ELF, Mach-O and archive spellings."""
    name = raw.rsplit("(", 1)[-1].rstrip(")") if raw.endswith(")") else raw
    name = Path(name).name
    for suffix in (".o", ".obj"):
        if name.endswith(suffix):
            return name[: -len(suffix)]
    return name


def watched_symbols(symbols: set[str], watched: frozenset[str]) -> set[str]:
    """Return watched C symbols across ELF and Mach-O nm spelling.

    Mach-O prefixes external C symbols with one underscore. Normalizing only
    the names this boundary actually watches avoids changing unrelated archive
    symbol counts or conflating a legitimate ELF symbol that starts with an
    underscore.
    """
    return {name for name in watched if name in symbols or f"_{name}" in symbols}


def verify_product_boundary(nm: Path, product: Path, oracle: Path) -> tuple[int, int]:
    """Prove the oracle step is absent from the product and the executor has only its named callers."""
    product_members = archive_symbols(nm, product)
    oracle_members = archive_symbols(nm, oracle)
    product_symbols = set().union(*(defined | undefined for defined, undefined in product_members.values()))
    oracle_defined = set().union(*(defined for defined, _ in oracle_members.values()))

    leaked = sorted(watched_symbols(product_symbols, PRODUCT_FORBIDDEN_SYMBOLS))
    if leaked:
        raise RuntimeError(f"product archive references test-only symbols: {', '.join(leaked)}")

    executor = frozenset({SEMANTIC_EXECUTOR})
    callers = sorted(
        name
        for name, (_, undefined) in product_members.items()
        if watched_symbols(undefined, executor) and name not in SEMANTIC_EXECUTOR_CALLERS
    )
    if callers:
        raise RuntimeError(f"{SEMANTIC_EXECUTOR} is called from outside the bounded fallback: {', '.join(callers)}")
    if not any(watched_symbols(defined, executor) for defined, _ in product_members.values()):
        raise RuntimeError(f"product archive does not define {SEMANTIC_EXECUTOR}; the check would prove nothing")

    missing = sorted(ORACLE_REQUIRED_SYMBOLS - watched_symbols(oracle_defined, ORACLE_REQUIRED_SYMBOLS))
    if missing:
        raise RuntimeError(f"oracle archive is not a positive control; missing: {', '.join(missing)}")

    return len(product_symbols), len(oracle_defined)
