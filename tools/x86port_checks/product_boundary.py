"""Inspect built archives for the x86port product/oracle link boundary."""

from __future__ import annotations

import subprocess
from pathlib import Path


PRODUCT_FORBIDDEN_SYMBOLS = frozenset(
    {
        "x86p_engine_from_env",
        "x86p_engine_parse",
        "x86p_engine_resolve",
        "x86p_execute_decoded",
        "x86p_step",
        "x86p_step_cached",
    }
)
ORACLE_REQUIRED_SYMBOLS = frozenset({"x86p_execute_decoded", "x86p_step"})


def global_symbols(nm: Path, archive: Path, *, defined_only: bool) -> set[str]:
    """Return global symbols from one archive, refusing tool errors."""
    command = [str(nm), "-g"]
    if defined_only:
        command.append("--defined-only")
    command.append(str(archive))
    result = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "no diagnostic"
        raise RuntimeError(f"{nm} could not inspect {archive}: {detail}")
    return {
        line.split()[-1]
        for line in result.stdout.splitlines()
        if line.split() and not line.rstrip().endswith(":")
    }


def verify_product_boundary(nm: Path, product: Path, oracle: Path) -> tuple[int, int]:
    """Prove forbidden oracle symbols are absent from product and present in oracle."""
    product_symbols = global_symbols(nm, product, defined_only=False)
    oracle_symbols = global_symbols(nm, oracle, defined_only=True)

    leaked = sorted(PRODUCT_FORBIDDEN_SYMBOLS & product_symbols)
    if leaked:
        raise RuntimeError(f"product archive references test-only symbols: {', '.join(leaked)}")

    missing = sorted(ORACLE_REQUIRED_SYMBOLS - oracle_symbols)
    if missing:
        raise RuntimeError(f"oracle archive is not a positive control; missing: {', '.join(missing)}")

    return len(product_symbols), len(oracle_symbols)
