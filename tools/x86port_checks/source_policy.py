"""Shipping-source policy for x86port diagnostics and configuration."""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

DIAGNOSTIC_OWNER = Path("src/x86port/diagnostic.c")
CONFIG_OWNER = Path("src/x86port/config.c")
SOURCE_LINE_LIMIT = 1200
# Existing x64 lowering debt is frozen, not permission for new monoliths.
# Ratchet this exact ceiling downward as responsibilities are extracted.
LEGACY_SOURCE_LIMITS = {Path("src/x86port/jit_x64.c"): 1343}

_TARGET_PATTERN = re.compile(
    r"add_library\(\s*(?P<target>[A-Za-z0-9_]+)\s+STATIC(?P<body>.*?)\)",
    re.DOTALL,
)
_SOURCE_TOKEN = re.compile(
    r"(?:^|\s)(src/x86port/[A-Za-z0-9_./-]+\.(?:c|cpp|h))(?=\s|$)"
)
_COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.DOTALL)
_OUTPUT_PATTERNS = (
    re.compile(r"\b(?:v?printf|v?fprintf|puts|fputs|perror)\s*\("),
    re.compile(r"\b(?:stderr|stdout|STDERR_FILENO|STDOUT_FILENO)\b"),
    re.compile(r"\b(?:OutputDebugString(?:A|W)?|__android_log_print)\s*\("),
    re.compile(r"\bstd::(?:cerr|cout|clog)\b"),
)
_ENVIRONMENT_PATTERN = re.compile(r"\b(?:getenv|secure_getenv|_dupenv_s)\s*\(")
# EM_JS bodies are JavaScript that clang-format lays out as C. Measured: a
# template literal written as `${failure.name}: ${failure.message}` came back
# from the formatter as `${failure.name} : $ { failure.message }`, which is
# still valid JavaScript and interpolates nothing, so the browser's own words
# for a failed run were replaced by the source text of the expression. Build
# such strings with concatenation instead; nothing here rewrites a `+`.
_TEMPLATE_LITERAL_PATTERN = re.compile(r"`[^`\n]*\$\{")


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
        body = match.group("body")
        # Audit every declared architecture branch, not only the local host.
        # CMake owns these source lists; a backend moved behind a variable
        # must remain visible to the same product policy.
        for variable in re.findall(r"\$\{([A-Za-z0-9_]+)\}", body):
            values = re.findall(
                rf"set\(\s*{re.escape(variable)}\s+([^)]*)\)", cmake_text
            )
            if not values or any("${" in value for value in values):
                raise RuntimeError(f"cannot resolve product source variable {variable}")
            body = body.replace("${" + variable + "}", "\n".join(values))
        sources = tuple(
            dict.fromkeys(Path(token) for token in _SOURCE_TOKEN.findall(body))
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
    line_count = len(text.splitlines())
    line_limit = LEGACY_SOURCE_LIMITS.get(relative, SOURCE_LINE_LIMIT)
    if line_count > line_limit:
        violations.append(
            SourceViolation(
                relative,
                line_limit + 1,
                "source size",
                f"{line_count} lines exceeds the {line_limit}-line ceiling",
            )
        )
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
        if _TEMPLATE_LITERAL_PATTERN.search(line):
            violations.append(
                SourceViolation(
                    relative, line_number, "formatter-hostile template literal", line.strip()
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
