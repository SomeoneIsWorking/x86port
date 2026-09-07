# x86port native/dynarec migration plan

This is the implementation order and acceptance authority for `x86port`. It
implements the portfolio contract in `shared/jit-common/docs/migration.md`.
It replaces the former static-recompiler plan; there is no compatibility
product or alternate static execution mode to preserve.

## Target product boundary

The consumer-facing library contains CPU state, runtime decoding, shared
semantic helpers, a host JIT backend, executable code/cache ownership, bounded
dispatch, invalidation, and the native-call boundary. A game links that library
and offers every non-native guest block to the JIT first.

Interpreter-only execution is built into a separate framework-test target. That
target may compare interpreter and JIT state. The gameplay library has no
interpreter engine enum or selector spelling; its only legal interpretation
edge is a bounded fallback owned by the JIT dispatcher after typed failed or
unsupported compilation, or unsafe emitted execution. The edge records reason,
guest PC, blocks, and instructions before returning to JIT dispatch. A JIT may
also call a narrowly owned semantic helper; that helper implements one CPU rule
and does not decode or dispatch the next guest instruction.

Runtime translation is the only guest-code generation. No configure, build,
install, provision, or release action may emit guest C/C++, object files,
generated function maps, seed lists, or a precompiled title image.

## Gate 1 — remove the obsolete execution vocabulary

The obsolete selector implementation, tests, and `Substrate` vocabulary are
deleted rather than kept as compatibility paths. Source and symbol checks in
the combined framework gate must continue to enforce their absence.

This gate removes a stale path only; the product link boundary is Gate 2.

## Gate 2 — separate product and oracle link graphs

`x86port_runtime` is the concrete dynarec-default product library and `x86port` aliases
it. Interpreter dispatch and oracle-only support live in
`x86port_test_oracle`, which is created only when this repository builds as the
top-level project.

Acceptance evidence:

- a consumer fixture links the product library and executes nonzero translated
  blocks;
- symbol/link inspection proves explicit interpreter-only selection and the
  test-oracle dispatcher are absent from the fixture and product archive;
- the product API exposes one execution contract and refuses an unavailable
  host backend rather than selecting an engine;
- the separately built differential target still exercises the same CPU state,
  decoder, memory, and semantic helpers;
- a negative fixture intentionally links the oracle target and proves the link
  audit can detect interpreter presence.

## Gate 3 — bound every non-JIT instruction

Every translatable instruction takes either host emission or a call to a
canonical narrow semantic helper. An instruction without either route stops at
its own EIP with a named unsupported status or enters the bounded product
fallback with that exact reason. Fallback executes only that refused/unsafe
block, records blocks and instructions by reason, and returns to JIT dispatch;
it is never a profiling first pass, asynchronous-compilation bridge,
missing-backend substitute, or compatibility mode.

Acceptance evidence:

- the title corpus reports every decoded instruction as emitted, handled by a
  named semantic helper, or entered through a named/countable fallback edge;
- unsupported semantics stop with a named refusal or bounded fallback and
  cannot silently make progress through the diagnostic oracle;
- differential tests still stop at the first whole-machine mismatch;
- nonzero translated blocks, cache hits, semantic-helper calls, refusals, and
  fallback blocks/instructions by reason are reported with denominators;
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
owners. Do not grow the x64 emitter into a multi-host monolith and do not use
bounded fallback as a substitute for an ARM64 product backend.

Acceptance evidence mirrors x64 and additionally covers ARM64 ABI transitions,
W^X publication, instruction-cache maintenance, cache invalidation, and each
consumer's representative gameplay and performance budget on its declared
Apple or Android host class.

## Gate 8 — implement and qualify WebAssembly

A browser host cannot execute either existing backend, and the compiler says so
rather than the code merely being wrong: `jit-common`'s code region fails to
build for wasm32 with "llvm.clear_cache is not supported on wasm". There is no
operation in WebAssembly that transfers control to a buffer of bytes. So a wasm
host is not a third target of the same shape -- translated code there is a
MODULE handed to the engine, compiled by it, and called through a function
reference.

Bounded fallback is not a substitute here either, and neither is the
interpreter: the product execution contract forbids an interpreter-backed
gameplay build, and an x86 interpreter inside a browser would not be playable
regardless.

Ordered work:

1. **`emit_wasm.{h,c}` — the binary format only.** Done. Its oracle is a real
   WebAssembly engine, which validates each emitted module against the
   specification and runs it; the encoder cannot be right merely by agreeing
   with the test's reading of the format. The test SKIPs without an engine
   rather than passing, and refuses a pass in which zero modules reached one.
2. **Lowering — a first instruction set, engine-verified.** Partly done. The
   backend selection now names an unknown host instead of treating every
   non-ARM64 host as x86-64, and Emscripten selects `jit_wasm.c`. Lowering
   lives apart from that adapter, in `jit_wasm_lower.c` and its per-family
   units, and takes the guest mapping as three plain integers rather than a
   host pointer -- so it builds on every host and is tested on every host.
   `test_jit_wasm` lowers a block, runs it in a real engine with an X86pCpu and
   a guest arena in linear memory, and compares the whole machine against the
   separately linked interpreter oracle.

   All ten backend files also COMPILE for the target: Emscripten 4.0.16 built
   them under `-Wall -Wextra -Werror` with no warnings and no x86-64 emitter
   object in the build, which is what makes the adapter's
   `_Static_assert(sizeof(void *) == 4)` checked rather than assumed. No wasm32
   binary has been executed, so that is a compile, not a run.

   What is lowered: MOV at 8/16/32 bits, MOVZX/MOVSX, the inline ALU shapes
   (ADD, OR, AND, SUB, XOR, CMP, TEST) with register, immediate and memory
   operands, NOT inline, ADC/SBB and the shifts and rotates through `x86p_alu`,
   INC/DEC/NEG through `x86p_alu_unary`, LEA, XCHG, SETcc, PUSH/POP, LEAVE,
   CDQ/CWDE, CLD/STD, JMP and CALL both relative and indirect, Jcc, JECXZ, and
   RET with and without a release count.

   What is not: x87, SIMD and 3DNow!, the string operations, MUL/DIV/IMUL/IDIV,
   SHLD/SHRD, the BCD and bit-scan families, PUSHFD/POPFD, PUSHAD/POPAD, ENTER,
   LOOP, the interrupt and privileged instructions, 16-bit addressing, and the
   16-bit stack forms. Each is refused by name at the instruction, so the
   remaining set is a ranked work list rather than a count.
3. **Module lifetime is a correctness requirement, not tuning.** Partly done.
   `jit_wasm_arena.{h,c}` bounds the number of live instantiations, refuses by
   name at the cap rather than growing, and counts refusals apart from
   engine rejections; the module builder takes several block bodies so a caller
   can batch. What is NOT done is the wiring: nothing releases a module when the
   block cache discards a block, and nothing batches yet, because the dispatch
   loop has no wasm publication edge. Eviction inside the arena is deliberately
   absent -- the block cache holds entry addresses the arena handed out and does
   not consult it before entering one.
4. **x87 has no host floating point to delegate to.** WebAssembly has no
   floating-point environment: no rounding-mode control and no exception flags,
   which is why `x87.c`'s `#pragma FENV_ACCESS` is refused outright for wasm32.
   The software float path must be the only one on this host, and the cost of
   that on top of a wasm JIT is unmeasured.
5. **Compilation happens off the main thread.** Synchronous module compilation
   is unrestricted only off the browser's main thread, which is where a
   blocking guest has to run anyway.

Acceptance evidence mirrors x64 and ARM64: nonzero translated block execution
with denominators, cache and invalidation behaviour, every refusal counted, and
a consumer's representative gameplay rather than a boot checkpoint.

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
  product link audit. **clang-format is now enforced**: `verify_format` compares
  every first-party source against the tracked `.clang-format` and reports the
  denominator, `verify_format_negative` proves the comparison fires, and both
  SKIP rather than pass when no formatter is present. That gap was not
  theoretical -- a ten-file backend landed with seven files off the tracked
  style while the other 126 matched it exactly. clang-tidy against real compile
  commands is still missing;
- keep x86-only policy local and extract to `jit-common` only after two concrete
  platform frameworks prove identical semantics.

Both consumers must resolve only the maintained `x86port` and `jit-common`
runtime contracts; no parallel execution-framework dependency may remain.
