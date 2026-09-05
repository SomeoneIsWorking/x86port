# x86port

`x86port` is the title-neutral x86-32 CPU runtime for native/dynarec game
ports. Gameplay products use native overrides for deliberately owned behavior
and translate every other guest instruction at runtime from the user's original
binary.

The product contract is deliberately narrow:

- x64 and ARM64 are the required product JIT backends;
- dynarec is always the product default and every cold block reaches the JIT;
- a bounded, counted interpreter fallback may run only after a typed compile
  refusal or unsafe emitted execution; explicit interpreter mode remains a
  separately built diagnostic target and missing backends never fall back;
- there is no static `Substrate`, offline guest-code generator, generated guest
  corpus, or precompiled guest image.

## Current state

The concrete `x86port_runtime` target is the JIT-only product library;
`x86port` aliases it for consumers. It contains the x64 decoder, CPU state and
semantics, emitter, executable code memory/cache integration, bounded dispatch,
interception, profiling, and invalidation APIs. Unsupported translations return
a named refusal. The permitted bounded product fallback and its telemetry are
not implemented yet and remain distinct from `x86port_test_oracle`.
Product-only execution and archive-symbol gates enforce the current boundary. x64
instruction and consumer conformance coverage is still incomplete. ARM64
code emission exists but host qualification and exact x87 state are incomplete.
Shipping diagnostics use the configurable
`X86pDiagnosticSink` boundary; only its default implementation writes to
standard error. Guest faults and unsupported instructions remain typed runtime
statuses rather than fatal log-and-continue paths.

Those facts are tracked in [docs/project-state.md](docs/project-state.md). The
target architecture and ordered replacement gates are in
[docs/migration.md](docs/migration.md); ownership is in
[docs/codemap.md](docs/codemap.md).

## Repository layout

| Path | Responsibility |
| --- | --- |
| `src/x86port/` | CPU state, decode, semantics, diagnostics, JIT dispatch, code emission, profiling, and invalidation |
| `tests/` | Hermetic unit tests and the separately linked interpreter/JIT differential oracle |
| `tools/` | PE32 inspection, corpus decode comparison, coverage reporting, and benchmarking |
| `vendor/zydis/` | Pinned Zydis decoder source and generated decode tables; no guest semantics |
| `docs/` | Goals, factual state, ownership, and migration gates |

`jit-common` is consumed from a sibling checkout or an explicit
`X86PORT_JITCOMMON_DIR`. New contracts stay local until two platform frameworks
prove that they share the same semantics.

## Development build

The top-level build includes framework tests and the private interpreter oracle.
A consuming `add_subdirectory` build receives only `x86port_runtime` and its
`x86port` alias.

```sh
uv run --frozen python tools/verify.py --cc clang --cxx clang++
```

The verifier requires the exact `jit-common` revision named in
`tools/x86port_checks/build.py`; use `--jit-common PATH` to select an isolated
checkout. Its CMake/Ninja dependencies and Python interpreter are shared by
the local and hosted build/test paths. macOS verification passes
`--cc /usr/bin/clang --cxx /usr/bin/clang++` to exercise AppleClang. Windows
uses `--cc clang-cl --cxx clang-cl` from an x64 Visual Studio developer
environment with LLVM's `llvm-nm` installed. Windows support remains partial:
the MSVC x87 value representation still needs the f80 work recorded in the
project-state inventory; CI retains the corresponding runtime tests.

This is a library, not a runnable game, so it has no `run.sh`. A consuming title
owns provisioning, packaging, and its default launcher.
