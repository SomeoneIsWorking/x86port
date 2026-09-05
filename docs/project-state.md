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
| S007 | An ARM64 product JIT backend is available | partial | S002 | G001, G002 |
| S008 | Runtime dispatch supports image-aware native overrides and scoped original calls through the JIT | partial | S004, S005 | G001, G002, G003 |
| S009 | Executable-code publication, caching, and invalidation preserve runtime correctness | partial | S004 | G001, G003, G004 |
| S010 | Runtime configuration is typed, explicit, and instance-owned | missing | S005 | G004 |
| S011 | Library diagnostics use one configurable sink and explicit failure status | verified | — | G004 |
| S012 | Source ownership and normal verification enforce the repository quality limits | partial | — | G004 |
| S013 | Differential and corpus instruments prove both positive and negative outcomes with denominators | partial | S001, S002, S003, S004 | G001, G003 |
| S014 | X-Men 2 consumes the canonical JIT-only product boundary and passes representative gameplay conformance | partial | S005, S008, S009 | G001, G002, G004 |
| S015 | Little Fighter 2 consumes the canonical JIT-only product boundary and passes representative gameplay conformance | missing | S005, S008, S009 | G001, G002, G004 |

## Host CI support

The CI workflow runs the full asset-free CMake test graph against the pinned
`jit-common` checkout, including product/oracle link inspection and synthetic
JIT execution:

The 2026-09-05 combined Linux gate used `uv run --frozen python tools/verify.py`
with Clang 22.1.8 for C and C++, exact `jit-common`
`4512a2054b0ecf737b6dd0b03f23713d23550b3c`, and passed all 27 CTests after
integrating the ARM64 encoder and backend source. This Linux result executes
the x64 backend and does not qualify ARM64 runtime behavior. The
Win64 ABI executable positive/negative probe ran locally through `ms_abi`;
this proves the emitted calling convention on the local host, while actual
Windows OS evidence still requires its hosted job. Pointer-valued x87 helper
adapters remove host by-value argument-layout assumptions; they do not solve
MSVC's narrower register representation.

| Host | State | Evidence or exact gap |
| --- | --- | --- |
| Linux x86-64 | hosted re-verification pending | Run [33959623707](https://github.com/SomeoneIsWorking/x86port/actions/runs/33959623707) exposed an immediate-count oracle defect in SHLD/SHRD after the prior run passed on another host. S002 records the corrected count-source/mask contract and local hardware evidence; a new hosted run is required. |
| Windows x86-64 | partial; hosted re-verification pending | Run [33959623707](https://github.com/SomeoneIsWorking/x86port/actions/runs/33959623707) compiled successfully and reached every synthetic test; only the x87 control fixture failed because it expected value-bearing translation on binary64 `long double`. That fixture now checks 15 exact-host successes or 15 named precision refusals through product dispatch with unchanged CPU/memory and zero executed blocks; status-only x87 operations still execute. Local Clang normal and `-mlong-double-64` builds exercise both branches, not Windows OS conformance. Host-independent f80 storage and arithmetic/conversion/rounding remain required for full Windows x87; hosted re-verification is pending and this host is not release-qualified. |
| macOS x86-64 | hosted synthetic gate passed | Run [33959170423](https://github.com/SomeoneIsWorking/x86port/actions/runs/33959170423) at `52f93d3` passed the Intel Apple Clang product and portable-oracle graph. The Linux compatibility-mode-only integer-tail hardware oracle reports a CTest skip on macOS; the archive boundary recognizes Mach-O's leading C-symbol underscore. This does not qualify Apple Silicon. |
| macOS arm64 | partial; not release-qualified | The ARM64 emitter/translator was integrated from `b15cc24`. Its recorded local synthetic differential used host binary64 x87 values, so that agreement cannot establish extended-precision fidelity. Shared precision admission now refuses those value-bearing forms. ARM64 runtime CI and renewed conformance evidence are still required. |
| Android arm64-v8a | partial backend; unverified host | ARM64 emission exists, but Android executable-memory, ABI, packaging, and gameplay verification have not been performed. |

## Evidence and exact gaps

### S001 — runtime decoder

Evidence: `src/x86port/decode.c` uses pinned Zydis v4.1.1 at runtime. The
recorded 2026-09-01 corpus comparison examined 2,168,629 instructions from 20
X-Men 2 modules, decoded every instruction, and reported 37 explicit length
differences attributable to the architectural treatment of `FWAIT`; it did not
silence those differences.

### S002 — shared CPU and semantic model

The repository contains explicit CPU/memory state and separate flag, integer,
SIMD, string, privilege, BCD, and x87 semantic owners. Local hardware-oracle
evidence includes 2,258,432 flag comparisons, 2,342,080 integer-ALU
comparisons, and 7,296 integer-tail instructions. Results and architecturally
defined flags are correctness gates; each oracle separately reports a
denominator for ISA-undefined flag variation rather than assuming one host
CPU's values are portable. The SAR oracle keeps CF in its correctness mask
at and beyond the operand width; only SHL/SHR omit that undefined CF case.
The byte-width flag sweep covers every nonzero masked count through 31.
Double-shift masks derive the effective count from the decoded immediate or CL,
not from an unrelated register-sweep value. Controlled mask-corruption checks
retain defined-bit failures while accepting undefined-bit variation. The
SHLD/SHRD boundary at a 16-bit count of 16 executes against hardware and compares
the result and CF/SF/ZF/PF; only greater counts are refused, as specified in the
[Intel SDM SHLD/SHRD reference](https://cdrdv2-public.intel.com/835757/325383-sdm-vol-2abcd.pdf).
OF is undefined above count one and AF is undefined for every nonzero count;
neither invalidates the other outputs at the width boundary.
Gap: complete title-required instruction, exception,
interrupt, and timing coverage has not been demonstrated under the new product
boundary, and the corrected multi-host oracle policy still needs a hosted
confirmation run.

### S003 — test-only interpreter oracle

Evidence: `x86port_test_oracle` is created only when this repository is the top-level
CMake project. It owns `exec.c`, `x87_exec.c`, decode-cache support, and CPU
comparison, while its differential tests use the shipping decoder, state,
memory, and semantic helpers from `x86port_runtime`. The product-boundary test
uses the oracle archive as a positive and controlled-negative symbol fixture
and normalizes only its watched C symbols across ELF spelling and Mach-O's
leading underscore.

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
FCOM/FCOMP status, NaN, pop, empty-stack, and fault behavior. FNSTSW AX is
differentially covered across all TOP values and preserves upper EAX, integer
flags, and x87 state. FNCLEX `DB E2` is decoded and executed through the
shipping JIT; its focused differential proves that only B/ES/SF and the six
exception flags are cleared while TOP, condition codes, control, tags, register
data, general registers, and integer flags remain unchanged, and neighboring
FNINIT remains a named refusal. The product-only runtime fixture independently
executes FNCLEX before a named stopper without linking the interpreter oracle.
Gap: title-required emitter, exception, and interrupt coverage is incomplete,
and the backend has not yet passed representative consumer gameplay
conformance.

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

Evidence: `emit_arm64.{h,c}`, `jit_arm64.c`, `jit_arm64_integer.c`, and `jit_arm64_x87.c`
implement an ARM64 encoder and runtime backend selected by CMake on
`arm64`/`aarch64`. Commit `b15cc24` records an Apple Silicon synthetic run
of 21,130 checks across 1,258 programs. This is historical mechanism evidence,
not verification of the merged tree or complete guest semantics: macOS uses
binary64 `long double`, and the prior interpreter/JIT comparison shared that
inexact x87 representation. Both host backends now use `jit_x87_predicates.c`
to refuse value-bearing x87 translation unless the semantic owner has exact
extended state; status-only operations remain available. A portable f80 owner
is required for Windows and ARM64 floating-point conformance.

The differential uses the shipping `jit-common` region publication API for
W^X transitions and instruction-cache coherence. ARM64 caller-saved helper
register lifetimes, macOS execution, Android runtime/package behavior, and
representative consumer gameplay remain unverified in the merged tree. The
new backend still duplicates some decode-level policy and semantic helper
adapters; S012 remains partial. No ARM64 release support
is claimed from encoder tests or historical host-double differential output.

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
has begun moving out of the x64 backend. ARM64 integer arithmetic lowering
owns a separate module, keeping its block orchestrator below 1,200 lines.
The source-policy gate checks every declared product backend, including inactive
architecture branches, and enforces a 1,200-line ceiling with the existing x64
lowering frozen at 1,925 lines. Gap: that x64 debt still needs extraction,
`src/x86port/exec.c` is near the limit, and the normal verifier does not yet
enforce clang-format, clang-tidy, all first-party structure, and portability
as one gate.

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
