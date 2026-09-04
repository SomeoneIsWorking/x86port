"""Shipping-source policy for x86port diagnostics and configuration."""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

DIAGNOSTIC_OWNER = Path("src/x86port/diagnostic.c")
CONFIG_OWNER = Path("src/x86port/config.c")

_TARGET_PATTERN = re.compile(
    r"add_library\(\s*(?P<target>[A-Za-z0-9_]+)\s+STATIC(?P<body>.*?)\)",
    re.DOTALL,
)
_SOURCE_TOKEN = re.compile(r"(?:^|\s)(src/x86port/[A-Za-z0-9_./-]+\.[ch])(?=\s|$)")
_COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.DOTALL)
_OUTPUT_PATTERNS = (
    re.compile(r"\b(?:v?printf|v?fprintf|puts|fputs|perror)\s*\("),
    re.compile(r"\b(?:stderr|stdout|STDERR_FILENO|STDOUT_FILENO)\b"),
    re.compile(r"\b(?:OutputDebugString(?:A|W)?|__android_log_print)\s*\("),
    re.compile(r"\bstd::(?:cerr|cout|clog)\b"),
)
_ENVIRONMENT_PATTERN = re.compile(r"\b(?:getenv|secure_getenv|_dupenv_s)\s*\(")


@dataclass(frozen=True)
class SourceViolation:
    path: Path
    line: int
    policy: str
    excerpt: str


def _target_sources(cmake_text: str, target: str) -> tuple[Path, ...]:
    for match in _TARGET_PATTERN.finditer(cmake_text):
        if match.group("target") != target:
            continue
        sources = tuple(
            Path(token) for token in _SOURCE_TOKEN.findall(match.group("body"))
        )
        if not sources:
            raise RuntimeError(f"{target} declares no directly listed source files")
        return sources
    raise RuntimeError(f"CMake target {target} was not found")


def product_sources(root: Path) -> tuple[Path, ...]:
    """Read the product source list from the build authority, not a duplicate list."""
    cmake_path = root / "CMakeLists.txt"
    try:
        cmake_text = cmake_path.read_text(encoding="utf-8")
    except OSError as error:
        raise RuntimeError(f"cannot read {cmake_path}: {error}") from error
    sources = _target_sources(cmake_text, "x86port_runtime")
    missing = [path for path in sources if not (root / path).is_file()]
    if missing:
        raise RuntimeError(
            "product source list names missing files: " + ", ".join(map(str, missing))
        )
    return sources


def _without_comments(text: str) -> str:
    return _COMMENT.sub(lambda match: "\n" * match.group(0).count("\n"), text)


def scan_source(path: Path, text: str) -> list[SourceViolation]:
    """Return every direct-output or process-environment policy violation."""
    relative = Path(path)
    code = _without_comments(text)
    violations: list[SourceViolation] = []
    for line_number, line in enumerate(code.splitlines(), start=1):
        if relative != DIAGNOSTIC_OWNER and any(
            pattern.search(line) for pattern in _OUTPUT_PATTERNS
        ):
            violations.append(
                SourceViolation(relative, line_number, "direct output", line.strip())
            )
        if relative != CONFIG_OWNER and _ENVIRONMENT_PATTERN.search(line):
            violations.append(
                SourceViolation(
                    relative, line_number, "process environment", line.strip()
                )
            )
    return violations


def verify_source_policy(root: Path) -> tuple[int, list[SourceViolation]]:
    """Scan every product-linked C source from x86port_runtime's CMake declaration."""
    sources = product_sources(root)
    violations: list[SourceViolation] = []
    for relative in sources:
        source = root / relative
        violations.extend(scan_source(relative, source.read_text(encoding="utf-8")))
    return len(sources), violations


def selftest() -> int:
    """Prove both forbidden classes fire and each named owner is allowed."""
    bad_cases = {
        Path("src/x86port/cpu.c"): 'fprintf(stderr, "bad\\n");',
        Path("src/x86port/alu.c"): 'const char *value = getenv("BAD");',
        Path("src/x86port/jit_engine.c"): 'OutputDebugStringA("bad");',
    }
    violations = [
        violation
        for path, text in bad_cases.items()
        for violation in scan_source(path, text)
    ]
    if len(violations) != len(bad_cases):
        raise RuntimeError(
            f"negative control detected {len(violations)} of {len(bad_cases)} violations"
        )
    if scan_source(DIAGNOSTIC_OWNER, 'fprintf(stderr, "central\\n");'):
        raise RuntimeError("diagnostic owner was incorrectly rejected")
    if scan_source(CONFIG_OWNER, 'const char *value = getenv("X86PORT_OPTION");'):
        raise RuntimeError("configuration owner was incorrectly rejected")
    return len(violations)
