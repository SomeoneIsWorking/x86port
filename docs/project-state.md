# Project state

This is the factual capability inventory for `x86port`. Epic intent lives in
`project-goals.md`, ownership in `codemap.md`, and ordered implementation gates
in `migration.md`.

## Comparison baseline

The replacement baseline is an offline generated-source pipeline that emitted
large guest C corpora and compiled them into each title. The intended `x86port`
product instead consumes the user's original binary and dynamically translates
non-native guest code at runtime. Explicit interpreter-only execution is a
diagnostic; a product fallback, once implemented, is bounded to refused or
unsafe JIT blocks and must report its reason and counts.

## Current focus

S014 is the current focus: migrate X-Men 2 onto the now-separated JIT-only
product target and verify its actual gameplay link and selector.

## Capability inventory

| ID | Capability or outcome | State | Factual dependency | Goals |
| --- | --- | --- | --- | --- |
| S001 | Runtime x86-32 decoding consumes guest bytes without a maintainer generator | verified | — | G001, G003 |
| S002 | CPU state, memory, flags, integer, SIMD, and x87 semantics provide one reusable model | partial | S001 | G001, G003 |
| S003 | A test-only interpreter oracle exercises the shared decoder and semantic model | verified | S001, S002 | G003 |
| S004 | The x64 backend translates and executes runtime basic blocks | partial | S001, S002 | G001, G002, G003 |
| S005 | Dynarec is the product default; bounded fallback is controlled and explicit interpreter mode is diagnostic-only | partial | S002, S004 | G001, G002 |
| S006 | Static Substrate and offline guest-code generation are absent from x86port interfaces and workflows | verified | — | G001, G004 |
| S007 | An ARM64 product JIT backend is available | missing | S002 | G001, G002 |
| S008 | Runtime dispatch supports image-aware native overrides and scoped original calls through the JIT | partial | S004, S005 | G001, G002, G003 |
| S009 | Executable-code publication, caching, and invalidation preserve runtime correctness | partial | S004 | G001, G003, G004 |
| S010 | Runtime configuration is typed, explicit, and instance-owned | missing | S005 | G004 |
| S011 | Library diagnostics use one configurable sink and explicit failure status | verified | — | G004 |
| S012 | Source ownership and normal verification enforce the repository quality limits | partial | — | G004 |
| S013 | Differential and corpus instruments prove both positive and negative outcomes with denominators | partial | S001, S002, S003, S004 | G001, G003 |
| S014 | X-Men 2 consumes the canonical JIT-only product boundary and passes representative gameplay conformance | partial | S005, S008, S009 | G001, G002, G004 |
| S015 | Little Fighter 2 consumes the canonical JIT-only product boundary and passes representative gameplay conformance | missing | S005, S008, S009 | G001, G002, G004 |

## Evidence and exact gaps

### S001 — runtime decoder

Evidence: `src/x86port/decode.c` uses pinned Zydis v4.1.1 at runtime. The
recorded 2026-09-01 corpus comparison examined 2,168,629 instructions from 20
X-Men 2 modules, decoded every instruction, and reported 37 explicit length
differences attributable to the architectural treatment of `FWAIT`; it did not
silence those differences.

### S002 — shared CPU and semantic model

The repository contains explicit CPU/memory state and separate flag, integer,
SIMD, string, privilege, BCD, and x87 semantic owners. Committed hardware-oracle
evidence includes 2,187,776 flag comparisons and 2,342,080 integer-ALU
comparisons. Gap: complete title-required instruction, exception, interrupt,
and timing coverage has not been demonstrated under the new product boundary.

### S003 — test-only interpreter oracle

Evidence: `x86port_test_oracle` is created only when this repository is the top-level
CMake project. It owns `exec.c`, `x87_exec.c`, decode-cache support, and CPU
comparison, while its differential tests use the shipping decoder, state,
memory, and semantic helpers from `x86port_runtime`. The product-boundary test
uses the oracle archive as a positive and controlled-negative symbol fixture.

### S004 — x64 JIT

The x64 emitter, block translator, dispatcher, profiling, interception, and
differential tests exist. Native integer and x87 emission are exercised by the
whole-machine differential, and unsupported translations return a named
product refusal without entering the test-oracle dispatcher. Focused
differentials cover all SETcc conditions and byte destinations, LEAVE ordering,
CDQ sign edges, MUL r/m32 widening and flag behavior, unsigned and signed DIV
r/m32 faults, two- and three-operand
IMUL result and flag behavior, REP string progress and termination, XCHG r/m32
ordering, all seven x87 constant loads including stack overflow, and memory-form
FCOM/FCOMP status, NaN, pop, empty-stack, and fault behavior. Gap: title-required
emitter, exception, and interrupt coverage is incomplete, and the backend has
not yet passed representative consumer gameplay conformance.

### S005 — product execution selection and fallback

`x86port_runtime` is the concrete dynarec product target and `x86port` is its
compatibility-free CMake alias. A product-only fixture executes translated
blocks and receives a named unsupported refusal; archive inspection proves the
current product neither defines nor references the interpreter-only oracle or
an explicit engine selector. Gap: the permitted bounded fallback owner,
typed-entry reasons, counters, and return-to-JIT tests are not implemented, and
X-Men 2 and Little Fighter 2 have not both passed the selector/fallback audit
against their actual gameplay executables.

### S006 — no static execution path

Evidence: the repository contains no guest-source generator, generated guest corpus,
static engine selector, or offline generation workflow. The former engine
selector implementation and its tests were deleted rather than retained as a
legacy option. Consumer removal is tracked by their own state and S014/S015.

### S007 — ARM64 backend

Missing capability: implement and qualify runtime ARM64 code emission,
executable-memory publication, instruction-cache coherence, ABI transitions,
invalidation, and product conformance. A bounded instruction fallback cannot
substitute for a missing ARM64 backend.

### S008 — native and original dispatch

The JIT engine has consumer interception, inline dispatch, and translation
boundary callbacks. Gap: complete image/module-generation identity, override
installation/removal invalidation, disabled override behavior, and a scoped
original call that re-enters the JIT without recursion are not yet verified as
one product contract.

### S009 — code cache and invalidation

The dispatcher consumes `jit-common` code memory and block cache primitives and
exposes range invalidation. Gap: controlled positive and negative tests for all
consumer-relevant executable-memory changes, cache identity, publication, and
multi-instance behavior have not been demonstrated on every declared host.

### S010 — configuration boundary

Missing capability: replace product-facing environment selection and implicit
availability masks with explicit typed, per-instance configuration. Tests and
standalone tools may retain private command-line or environment parsing at
their own boundary.

### S011 — diagnostic boundary

Evidence: `src/x86port/diagnostic.{h,c}` and the source-policy and diagnostic
tests exercise the shipping diagnostic boundary and its negative cases.

`src/x86port/diagnostic.{h,c}` owns the configurable C sink and the default
standard-error presentation. Semantic owners route violated programming
contracts through that boundary before aborting; guest faults, unsupported
instructions, and bounded exits continue to return typed statuses. The
diagnostic test injects a sink and proves an actual invalid-width path reaches
it before aborting. The source-policy gate derives the shipping source set from
the CMake product target, finds zero direct-output or process-environment
violations outside the named owners, and proves its negative path against both
classes plus a platform-debug-output call.

### S012 — structure and quality gate

The tree has cohesive modules and a tracked `.clang-format`; x87 JIT emission
has begun moving out of the x64 backend. Gap: `src/x86port/jit_x64.c` remains
above the 1,200-line source limit, `src/x86port/exec.c` is near it, and the
normal verifier does not yet enforce clang-format, clang-tidy, structure, and
portability as one gate.

### S013 — verification instruments

The repository contains PE32 import/export/IAT inspection, decoder-corpus
comparison, JIT coverage and benchmark tools, hardware-oracle tests, and
whole-machine interpreter/JIT differentials.
The product-only fixture independently proves nonzero translated execution and
unsupported refusal, while archive inspection proves interpreter symbols are
absent and rejects the oracle as a deliberately contaminated product. Gap: the
title corpus and real-consumer instruments still need qualification on every
declared host architecture.

### S014 — X-Men 2 consumer

X-Men 2 has a no-generated x64 JIT path and is the proven source of current
framework work. Gap: synchronize and verify the canonical framework, prove the
actual gameplay binary neither links nor selects the test interpreter, exercise
native and original dispatch plus invalidation, and pass representative
interactive gameplay on every declared host architecture.

### S015 — Little Fighter 2 consumer

Missing capability: migrate Little Fighter 2's existing Win32/DirectDraw and
native seams to the canonical JIT-only product target, remove its generator and
generated guest C, then pass representative interactive gameplay conformance
on every declared host architecture.
