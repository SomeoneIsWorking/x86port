# x86port — agent guidance

`x86port` is the title-neutral x86-32 runtime used by native/dynarec game
ports. Read the local authorities before changing it:

- `docs/project-goals.md` — durable outcomes and non-goals;
- `docs/project-state.md` — what is verified, partial, or missing now;
- `docs/codemap.md` — responsibility and placement;
- `docs/migration.md` — ordered implementation and landing gates;
- `shared/jit-common/docs/migration.md` — the portfolio contract this project
  implements.

The local documents replace the previous static-recompiler migration story.
Do not recreate that story in source comments, tests, plans, or compatibility
options.

## Product execution contract

- A gameplay product has one guest execution path: native overrides where the
  title owns behavior, and runtime JIT/dynarec translation for every remaining
  guest instruction.
- The zero-argument product starts in dynarec mode and offers every cold block
  to the JIT. A bounded interpreter fallback may run only after typed failed or
  unsupported compilation, or when executing emitted code would be unsafe; it
  records the reason, guest PC, block count, and instruction count before
  returning to JIT dispatch.
- A gameplay build has no explicit interpreter selector, environment switch,
  profiling-first interpretation, missing-backend fallback, or silent
  instruction-step escape. Interpreter-only execution lives in a separately
  built diagnostic target. Pure semantic helpers remain shared owners rather
  than duplicated implementations.
- There is no `Substrate` engine and no offline guest-code generation. Build,
  install, provisioning, and release paths must not emit guest C/C++, object
  files, precompiled guest bodies, seed tables, or generated dispatch maps.
- A runtime-populated code cache is disposable user data. It is never a
  fresh-install prerequisite or a checked-in product input.
- The x64 backend is the current implementation. ARM64 product translation is
  required and currently absent; lack of a host backend is a refusal, never a
  reason to interpret the whole product.

The product/test link boundary now satisfies this contract at framework level:
`x86port_runtime` has no interpreter selector or interpreter dispatch edge, and
unsupported instructions return a named refusal. Treat `docs/project-state.md`
as authoritative for the remaining emitter, architecture, and real-consumer
conformance gaps.

## Ownership boundaries

- This repository owns x86-32 CPU state, decode, semantics, runtime translation,
  executable-image-aware block identity, invalidation, bounded exits, and the
  title-neutral native-call boundary.
- Consumers own game identity, title addresses, native implementations, Win32
  service policy, graphics presentation, audio, input, saves, and packaging.
  Do not add title-specific addresses or behavior here.
- Zydis owns instruction decoding only. Guest semantics remain in `x86port`.
- `jit-common` owns only contracts already demonstrated by at least two platform
  frameworks. Its current code-memory and block-cache primitives may be
  consumed; do not move an x86-only interface there speculatively.
- A missing reusable abstraction is implemented and proven locally first. It
  moves to `jit-common` only after a second real framework needs the same
  semantics.

## Self-contained engineering guardrails

- The repository's structure, configuration, and diagnostics are defined here;
  no other game project is an architectural template or runtime dependency.
- Split by responsibility before extending a large source file. First-party
  source files must stay below 1,200 lines; a file already beyond that limit is
  extraction territory and must not grow. Do not replace a monolith with
  numbered fragments or catch-all utility modules.
- Runtime configuration is explicit, typed, instance-owned, and passed through
  APIs. Product behavior must not depend on process-global state or environment
  parsing. Standalone tests and tools may parse their own command line or
  environment at their boundary.
- The library has one configurable diagnostic sink boundary. Library code must
  not scatter `printf`/`fprintf`, silently log-and-continue, or select behavior
  through logging. Tests and tools may write their own reports; consuming games
  adapt the library sink to their project logger.
- Failures preserve valid state and are returned with a named status and useful
  context. Unknown instructions and unsafe emitted execution may enter only the
  bounded, counted fallback; unavailable host backends, invalid mappings, and
  executable-memory publication failures remain refusals.
- C11 is the library baseline. Agent verification uses Clang, all touched C/C++
  is formatted with the tracked `.clang-format`, and the normal verifier must
  include format, clang-tidy, structure, portability, and tests without
  weakening diagnostics.

## Verification discipline

- The product-link gate must inspect the actual consumer-facing library or
  executable and prove explicit interpreter selection and the test-oracle
  dispatcher are absent; any bounded product fallback has separate entry-edge
  and telemetry tests.
- JIT evidence reports nonzero translated block execution, cache/invalidation
  behavior, and every refusal with a denominator. A successful final CPU state
  alone does not prove that translated code ran.
- Differential tests use the separately linked interpreter-only oracle and stop
  at the first divergence. Gameplay and performance claims exclude fallback
  intervals even when their results match that oracle.
- Exercise executable-memory publication, instruction-cache coherence,
  invalidation, native dispatch, scoped original calls, exceptions, interrupts,
  and bounded exits on every supported host architecture.
- Boot and menus are checkpoints. Product conformance requires representative
  interactive gameplay in each consumer, including timing and relevant device
  state against an independent oracle.
- This repository is a library, so it has no `run.sh`. Consumers own launchers.
  Project tests and tools are built only when `x86port` is the top-level CMake
  project.

Do not call the current tree verified until the outstanding source edits have
been integrated, a clean Clang build has been identified as Clang, the focused
tests pass, and the combined repository gate is run once on the frozen diff.
