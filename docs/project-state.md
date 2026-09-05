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

| Host | State | Evidence or exact gap |
| --- | --- | --- |
| Linux x86-64 | supported | `.github/workflows/ci.yml` uses Clang and runs the complete CTest graph. |
| Windows x86-64 | unsupported | The current top-level CMake graph unconditionally links the POSIX `m` library into `x86port_runtime`; Windows has no such library, so the native x64 product cannot link and a Windows job would fail before exercising it. Portability must be fixed at that owner before adding a Windows job. |
| macOS x86-64 | supported | `.github/workflows/ci.yml` uses Intel Apple Clang and runs the complete CTest graph against the x64 emitter. |
| macOS arm64 | supported | `src/x86port` now emits ARM64 host code (`jit_arm64.c`, `jit_arm64_x87.c`, `emit_arm64.c`), selected by `CMakeLists.txt` when `CMAKE_SYSTEM_PROCESSOR` matches `arm64`/`aarch64`. The full CTest graph passes 24/28 on Apple Silicon; the remaining 4 are the honest "this host is not x86-64" oracle refusals (`test_integer_tail`, `test_x87_fn`, `test_simd`, `test_string_ops`), which need real x86-64 hardware and are not part of S007. No CI job runs this yet — see S007 gap. |
| Android arm64-v8a | unsupported | ARM64 code emission now exists (see macOS arm64 above), but nothing has exercised it under Android's toolchain, page size, or W^X model; that verification has not been attempted. |

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

Evidence: `src/x86port/emit_arm64.c` is a from-scratch AArch64 encoder (AAPCS64
calling convention, register roles kept outside X0-X7 so no per-call liveness
juggling is needed), self-tested independently of any x86 input. `jit_arm64.c`
and `jit_arm64_x87.c` port the full x64 translator's instruction coverage and
x87 stack semantics onto it, selected at configure time by `CMakeLists.txt`
(`CMAKE_SYSTEM_PROCESSOR` matches `arm64`/`aarch64`) so exactly one backend
trio links into `x86port_runtime`. `test_jit_x64` runs its full differential
fuzzer against the interpreter oracle through this backend: 21,130/21,130
checks pass across 1,258 programs (branches, memory faults, x87 stack effects,
carry-in propagation). `test_jit_engine` proves the dispatch loop's cache
fill/flush/invalidate/rewind paths run for real on this backend, sized against
the host's actual page size rather than an assumed one. Executable-memory
publication, the per-thread MAP_JIT write/execute toggle, and instruction-cache
invalidation (`__builtin___clear_cache`) are exercised by the test harness on
Apple Silicon, matching `jit-common`'s product-path pattern.

Gap: no CI job runs this backend yet (macOS arm64 CI still uses Intel Apple
Clang), so regressions here are only caught locally. Product conformance —
X-Men 2 actually running gameplay through this backend via `x2native`, not
just passing synthetic differential tests — has not been attempted; that is
S014's remaining scope. Android arm64-v8a has not been exercised at all.

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

## Local Apple Silicon execution repair (2026-09-05)

S004/S007 remain partial. Both backends now lower memory shifts/rotates and
ADC/SBB forms without clobbering their host ABI's saved registers; status and
CPU transfers, one-operand MUL/IMUL widths, 32-bit SHLD/SHRD, and LOOP/LOOPE/
LOOPNE with the effective counter width are covered by `test_jit_startup`.
`test_jit_startup` reports 4,186 translated cases with zero failures on both
local ARM64 and Rosetta x64 builds. SSE coverage includes packed bit operations, full and partial moves, SHUFPS
(all 256 immediates, alias and fault forms), and packed ADD/SUB/MUL/DIV through
narrow shared semantic leaves. x87 coverage adds control-word loads/stores,
integer memory conversions/arithmetic, register comparison/sign/exchange,
and the supported transcendental stack operations. Unsupported instructions
still stop by name; the product archive never links the oracle dispatcher.

S009: publication now flushes only newly emitted bytes through
`jc_code_publish_range`; it closes the write window with a zero-byte range
when translation fails. A consumer macOS sample previously spent 404 of 414
main-thread samples invalidating the accumulated code prefix. No persistent
cache is introduced or required.

`test_x87_software` reports 3,023 checks with zero failures on the local x64
Rosetta run, including 3,015 independent x87 comparisons; the ARM64 run uses
independent host math checks and cannot supply a hardware x87 oracle. Numerical
tolerance remains 8 * DBL_EPSILON * max(1, magnitude), not bit equality.
Bochs provenance and the adapted atan polynomial are in `prior-art.md`.

Exact gaps: ARM64 x87 state is still binary64, unmasked x87 exception behavior
and complete precision control are not established, and SIMD MXCSR rounding,
DAZ/FTZ and exception semantics retain the existing default-environment
limitation. FXTRACT, approximate SSE reciprocals and other unimplemented forms
still refuse. Four hardware-oracle tests cannot run on ARM64. Rosetta is useful
for emitted x64 ABI/result regressions, but is not Fedora or real x86-64 silicon;
three full-suite oracle failures there remain outside this change. These are
not gameplay conformance passes or CI results.

The independent Fedora migration/CI commits are not incorporated here. Local
consumer observations reach the main menu and tutorial with nonzero JIT
execution and zero refusals; representative interactive play and independent
stock behavior remain S014 gaps.

## Floating-point store overhead (2026-09-05)

The portable x87 store conversion leaves the host rounding environment alone
when it already matches the guest control word. On hosts whose `long double`
is binary64, storing binary64 copies the value without a redundant rounding
mode transition. The conversion scope explicitly enables FENV_ACCESS; a host
service or another guest context may change the rounding mode, so no cached
mode is assumed.

`test_x87_narrow` runs 341 checks over all sixteen host/guest rounding-mode
pairs, including positive/negative halfway, subnormal and overflow values,
negative zero, and preservation of the host mode and pre-existing exception
flags. ARM64 and Rosetta x64 pass. An optional ten-million-store CPU-time
benchmark on this Mac measured f32 stores at 14.63 -> 5.56 ns and f64 at
12.41 -> 3.89 ns; this is leaf overhead, not a gameplay FPS claim. Existing x87
and JIT startup regressions also pass. This optimization does not change the
previously recorded ARM64 precision limitations.

## Repeated memory-copy overhead (2026-09-05)

The shared string semantic owner now bulk-copies forward REP MOVS only when
both complete spans fit the memory mapping, do not wrap guest addresses, are
disjoint, and no memory-write observer is installed. Memory-span admission and
host pointer translation remain in `cpu.c`. Backward, overlapping, watched,
and partially invalid copies retain element-wise progress and fault reporting.

`test_string_copy` passes 11,247 checks on ARM64 and Rosetta x64, comparing CPU
state, bytes, fault addresses, and write-observer counts/order against
single-element execution across byte/word/dword widths, both directions,
overlap, zero/huge counts, mapping ends, and 32-bit address wrap. Direct
admission controls prove both accepted and refused bulk copies. Eighteen
emitted-code REP MOVS cases bring `test_jit_startup` to 4,204 passing cases on
both hosts. The optional 100,000-copy benchmark measured 4 KB forward MOVSD at
9,482.1 -> 104.0 ns/copy on this Mac; this measures the leaf operation, not FPS.
The full 30-test graph retains four unavailable hardware-oracle failures on
ARM64 and the three previously recorded integer/flag oracle failures under
Rosetta. This does not resolve those baseline gaps or establish Fedora CI.
