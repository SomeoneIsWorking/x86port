# x86port

`x86port` is the title-neutral x86-32 CPU runtime for native/dynarec game
ports. Gameplay products use native overrides for deliberately owned behavior
and translate every other guest instruction at runtime from the user's original
binary.

The product contract is deliberately narrow:

- x64 and ARM64 are the required product JIT backends;
- an interpreter may exist only in a separately built test target;
- the product library and gameplay executables contain no interpreter,
  interpreter selector, or interpreter fallback;
- there is no static `Substrate`, offline guest-code generator, generated guest
  corpus, or precompiled guest image.

## Current state

The concrete `x86port_runtime` target is the JIT-only product library;
`x86port` aliases it for consumers. It contains the x64 decoder, CPU state and
semantics, emitter, executable code memory/cache integration, bounded dispatch,
interception, profiling, and invalidation APIs. Unsupported translations return
a named refusal and never enter the separately built `x86port_test_oracle`.
Product-only execution and archive-symbol gates enforce that boundary. x64
instruction and consumer conformance coverage is still incomplete, and ARM64
code emission is absent. Shipping diagnostics use the configurable
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
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build -j
ctest --test-dir build --output-on-failure
```

This is a library, not a runnable game, so it has no `run.sh`. A consuming title
owns provisioning, packaging, and its default launcher.
