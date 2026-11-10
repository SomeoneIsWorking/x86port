# x86port — agent guidance

`x86port` is the title-neutral x86-32 runtime used by native/dynarec game
ports. Local authorities:

- `docs/project-goals.md` — durable outcomes and non-goals;
- `docs/project-state.md` — what is verified, partial, or missing now;
- `docs/codemap.md` — responsibility and placement;
- `docs/migration.md` — ordered implementation and landing gates;
- `shared/jit-common/docs/migration.md` — the portfolio contract this project
  implements.

## Execution

Execution policy is in the `dynarec-port` and `dynarec-runtime` skills. Here:

- `x86port_runtime` is the consumer-facing library. It starts in dynarec mode,
  offers every cold block to the JIT, and has no interpreter selector,
  environment switch or interpreter dispatch edge; unsupported instructions
  return a named refusal.
- The bounded fallback runs only after a typed failed or unsupported
  compilation, or when executing emitted code would be unsafe, and returns to
  JIT dispatch. Interpreter-only execution is a separately built diagnostic
  target; pure semantic helpers are shared by both.
- x64 and ARM64 lowering backends exist; instruction and host conformance are
  partial (`docs/project-state.md`). Shared x87 value admission accepts native
  ext80 and binary128 hosts using software ext80 arithmetic/conversion, and
  retains the user-approved Apple ARM64 binary64 path with an explicit precision
  limitation. Raw MMX alias operations still require native ext80 object
  layout; other binary64 hosts refuse value-bearing forms.

## Ownership boundaries

- This repository owns x86-32 CPU state, decode, semantics, runtime translation,
  executable-image-aware block identity, invalidation, bounded exits, and the
  title-neutral native-call boundary.
- Consumers own game identity, title addresses, native implementations, Win32
  service policy, graphics presentation, audio, input, saves, and packaging.
  No title-specific addresses or behavior here.
- Zydis owns instruction decoding only. Guest semantics remain in `x86port`.
- `jit-common` takes only contracts at least two platform frameworks share (its
  `AGENTS.md`). Its code-memory and block-cache primitives may be consumed; an
  x86-only interface stays here.

## Library constraints

- C11 is the library baseline.
- `tools/x86port_checks/source_policy.py` caps first-party source files at
  1,200 lines; a file already beyond that must not grow.
- The library reads no environment and holds no process-global state:
  configuration is typed, instance-owned and passed through its APIs.
  Standalone tests and tools parse their own command line or environment.
- Diagnostics go through the library's one configurable sink; consumers adapt
  it to their project logger. Library code has no `printf`/`fprintf`.
- Unknown instructions and unsafe emitted execution may enter only the bounded,
  counted fallback; unavailable host backends, invalid mappings, and
  executable-memory publication failures are refusals with a named status.
- This repository is a library, so it has no `run.sh`; consumers own launchers.
  Project tests and tools are built only when `x86port` is the top-level CMake
  project.

## Tests

- The product-link gate inspects the consumer-facing library and proves the
  interpreter-only oracle and an explicit engine selector are absent from it.
- Differential tests link the separately built interpreter-only oracle and stop
  at the first divergence.
- Tests cover executable-memory publication, instruction-cache coherence,
  invalidation, native dispatch, scoped original calls, exceptions, interrupts,
  and bounded exits on every supported host architecture.

Focused tests build in `build/verify`. The full verifier configures the pinned
`jit-common` checkout, builds it, runs CTest, including the format, source-policy
and product-boundary checks:

```sh
uv run --frozen python tools/verify.py --build-dir build/verify --cc clang --cxx clang++
```
