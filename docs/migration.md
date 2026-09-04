# x86port native/dynarec migration plan

This is the implementation order and acceptance authority for `x86port`. It
implements the portfolio contract in `shared/jit-common/docs/migration.md`.
It replaces the former static-recompiler plan; there is no compatibility
product or alternate static execution mode to preserve.

## Target product boundary

The consumer-facing library contains CPU state, runtime decoding, shared
semantic helpers, a host JIT backend, executable code/cache ownership, bounded
dispatch, invalidation, and the native-call boundary. A game links that library
and always executes non-native guest code through the JIT.

The interpreter is built only into a separate framework-test target. That
target may compare interpreter and JIT state, but the gameplay library and
consumer executable must contain no interpreter dispatcher, interpreter engine
enum, selector spelling, fallback branch, or JIT-emitted call into interpreter
dispatch. A JIT may call a narrowly owned semantic helper; the helper implements
one CPU rule and does not decode or dispatch the next guest instruction.

Runtime translation is the only guest-code generation. No configure, build,
install, provision, or release action may emit guest C/C++, object files,
generated function maps, seed lists, or a precompiled title image.

## Gate 1 — remove the obsolete execution vocabulary

The obsolete selector implementation, tests, and `Substrate` vocabulary are
deleted rather than kept as compatibility paths. Source and symbol checks in
the combined framework gate must continue to enforce their absence.

This gate removes a stale path only; the product link boundary is Gate 2.

## Gate 2 — separate product and oracle link graphs

`x86port_runtime` is the concrete JIT-only product library and `x86port` aliases
it. Interpreter dispatch and oracle-only support live in
`x86port_test_oracle`, which is created only when this repository builds as the
top-level project.

Acceptance evidence:

- a consumer fixture links the product library and executes nonzero translated
  blocks;
- symbol/link inspection proves interpreter step/run functions and runtime
  engine selection are absent from the fixture and product archive;
- the product API exposes one execution contract and refuses an unavailable
  host backend rather than selecting an engine;
- the separately built differential target still exercises the same CPU state,
  decoder, memory, and semantic helpers;
- a negative fixture intentionally links the oracle target and proves the link
  audit can detect interpreter presence.

## Gate 3 — keep interpreter execution out of the x64 JIT

Every translatable instruction takes either host emission or a call to a
canonical narrow semantic helper. An instruction without either route stops at
its own EIP with a named unsupported status. It never calls the interpreter
step/dispatch loop.

Acceptance evidence:

- the title corpus reports every decoded instruction as emitted or handled by a
  named semantic helper, with no interpreter-dispatch category;
- unsupported semantics stop with a named refusal and cannot silently make
  progress through the oracle;
- differential tests still stop at the first whole-machine mismatch;
- nonzero translated blocks, cache hits, semantic-helper calls, and refusals are reported
  with denominators;
- exception, interrupt, memory-fault, and bounded-exit outcomes preserve the
  guest PC and state required by consumers.

## Gate 4 — complete the runtime ownership contract

Finish the title-neutral boundary needed by real native/dynarec products:

- key translated blocks and overrides by complete executable-image identity
  wherever modules can reload or reuse addresses;
- invalidate captured decisions when executable bytes or override state change;
- support native override dispatch and a scoped original call that suppresses
  only the current override and re-enters the JIT without recursion;
- keep CPU, cache, override, configuration, and diagnostic state per instance;
- use explicit bounded exits for host work, faults, interrupts, thread exit, and
  frame suspension.

Positive and controlled-negative tests must prove invalidation and override
identity. Tests that only show the expected route cannot prove that stale code
or a wrong image would have been rejected.

## Gate 5 — qualify x64 with real consumers

First synchronize the canonical framework with X-Men 2, then migrate Little
Fighter 2 without coupling CPU execution to graphics extraction.

For each consumer, require:

- fresh provisioning from the user's authenticated game files with no offline
  translator or generated guest corpus;
- the product-link and selector audit from Gate 2;
- nonzero JIT execution, native override execution, scoped original calls, and
  relevant invalidation;
- representative interactive gameplay with CPU/register, memory,
  interrupt/timing, and relevant device comparison against an independent
  oracle;
- a declared frame-time budget measured on the released x64 host class.

Boot, level load, menus, FMV, and headless no-present runs are checkpoints, not
completion evidence.

## Gate 6 — implement and qualify ARM64

Add a cohesive ARM64 backend behind the same product dispatcher and semantic
owners. Do not grow the x64 emitter into a multi-host monolith and do not use the
test interpreter as an ARM64 product path.

Acceptance evidence mirrors x64 and additionally covers ARM64 ABI transitions,
W^X publication, instruction-cache maintenance, cache invalidation, and each
consumer's representative gameplay and performance budget on its declared
Apple or Android host class.

## Gate 7 — land the self-contained quality boundary

Before calling the framework complete:

- extract `src/x86port/jit_x64.c` below the 1,200-line first-party source limit
  by real responsibility, and prevent `src/x86port/exec.c` or its replacement
  from becoming another monolith;
- add one explicit typed configuration owner; retain the configurable
  `X86pDiagnosticSink` boundary and its source-policy gate, with environment
  parsing confined to the configuration owner and standard-I/O presentation
  confined to the default diagnostic sink or standalone test/tool boundaries;
- make the normal verifier enforce clang-format, clang-tidy against real compile
  commands, structure limits, portability, unit/differential tests, and the
  product link audit;
- keep x86-only policy local and extract to `jit-common` only after two concrete
  platform frameworks prove identical semantics.

After both consumers pass their dynamic conformance gates, remove their final
dependency on `shared/recomp-x86`; the obsolete repository is then deleted by
the portfolio migration owner rather than retained as a legacy path.
