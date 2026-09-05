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
this proves the emitted calling convention on the local host. The native Windows
synthetic gate also passes in the hosted run below. Pointer-valued x87 helper
adapters remove host by-value argument-layout assumptions; they do not solve
MSVC's narrower register representation.

Evidence: [hosted run 33960632375](https://github.com/SomeoneIsWorking/x86port/actions/runs/33960632375)
at main commit `8d3840dcaf3b2ce7c717a93c086475b655868efd` passed the Linux x86-64,
Windows x86-64, and Intel macOS asset-free synthetic gates. These results do not
qualify complete x87 semantics, ARM64 hosts, or representative consumer gameplay.

| Host | State | Evidence or exact gap |
| --- | --- | --- |
| Linux x86-64 | hosted synthetic gate passed | The completed run above verifies S002's corrected SHLD/SHRD count-source/mask contract in the hosted synthetic gate. Complete instruction coverage and representative gameplay remain unqualified. |
| Windows x86-64 | partial; hosted synthetic gate passed | The completed native Windows run above passes the x87 control fixture, which checks 15 exact-host successes or 15 named precision refusals through product dispatch with unchanged CPU/memory and zero executed blocks; status-only x87 operations still execute. Local Clang normal and `-mlong-double-64` builds separately exercise both branches. Host-independent f80 storage and arithmetic/conversion/rounding remain required for full Windows x87; this host is not release-qualified. |
| macOS x86-64 | hosted synthetic gate passed | Run [33959170423](https://github.com/SomeoneIsWorking/x86port/actions/runs/33959170423) at `52f93d3` passed the Intel Apple Clang product and portable-oracle graph. The Linux compatibility-mode-only integer-tail hardware oracle reports a CTest skip on macOS; the archive boundary recognizes Mach-O's leading C-symbol underscore. This does not qualify Apple Silicon. |
| macOS arm64 | partial; not release-qualified | The ARM64 emitter/translator was integrated from `b15cc24`. Its recorded local synthetic differential used host binary64 x87 values, so that agreement cannot establish extended-precision fidelity. The approved integration retains those value-bearing forms through the JIT without claiming exact extended precision. ARM64 runtime CI and renewed conformance evidence are still required. |
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
boundary. The corrected multi-host oracle policy passed the hosted run recorded
under Host CI support; this does not establish complete guest semantics.

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
inexact x87 representation. Both host backends use `jit_x87_predicates.c` for
value admission: exact extended state, or the explicitly approved Apple ARM64
binary64 path described below. Other narrow-state hosts still refuse value
forms; status/control operations remain available. A portable f80 owner is
required for exact Windows and ARM64 floating-point conformance, not for this
approved integration.

The differential uses the shipping `jit-common` region publication API for
W^X transitions and instruction-cache coherence. Gap: ARM64 caller-saved helper
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
lowering ratcheted down to 1,885 lines. Gap: that x64 debt still needs extraction,
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

These are the incoming Mac branch's recorded results, not a verification of
this combined tree. The integration retains the independent Fedora/Windows
migration and CI contracts. The user reports playable Mac gameplay with decent
framerate; independent stock-behavior and quantified performance evidence remain
S014 gaps.

USER 2026-09-05: "Preserve the playable Mac behavior"

The combined admission policy preserves that Apple ARM64 execution path while
`x86p_x87_precision_is_exact()` remains false. The proper fidelity fix is
host-independent f80 storage and arithmetic; it is not a prerequisite for this
integration. Windows narrow-state value-bearing forms still refuse, and no
interpreter or gameplay execution selector is added.

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

## Combined Mac/Fedora integration (2026-09-05)

Exact incoming Mac commit `76e76ce6f66bc0ba341ad018cd0124180820ea3e` is
integrated with the Fedora/Windows main contracts and `jit-common`
`03ac795cbc39843e795cb8091fb96bff2b1c9017`. The frozen combined Linux Clang
22.1.8 gate (`tools/verify.py --build-dir build/mac-integration --cc clang
--cxx clang++ --jobs 2`) passed all 30 CTests. The startup discriminator
executed 4,195 translated cases with zero refusals/failures; software math
passed 3,023 checks, including 3,015 independent x87 comparisons; narrowing
passed 341 checks. The explicit Win64 ABI probe also executed both fifth-pointer
positive and negative cases on the Linux host. These are synthetic results,
not gameplay or native Windows/Mac qualification.

The focused `-mlong-double-64` contract build passed startup and control tests:
3,844 translated startup cases plus 351 named value-form refusals, and 16/16
control-suite precision refusals. On the exact Linux build those same 16
control cases execute. The Mac-only admission assertion requires executable
value forms while precision metadata remains false. New native Apple Silicon
CI runs alongside Intel macOS, Linux and Windows; its hosted result is pending.

All touched first-party files pass formatting; compiled touched translation
units pass clang-tidy. Four narrowly documented `performance-enum-size`
exemptions preserve public C enum ABI at the new C++ boundary, with C and C++
size assertions. The seven ARM64 lowering units pass local Clang syntax checks,
not local ARM64 execution. The existing normal-verifier quality gap in S012 is
unchanged. A subsequent unchanged build reported `ninja: no work to do`.

Independent integration review found that x64 admitted FWAIT but dispatched it
as FLD. The existing empty-stack fixture hid that fallthrough. A populated-stack
discriminator reproduced eight failures across the eight nonempty depths before
the explicit no-op dispatch fix; all nine depth cases now preserve CPU state.
The identical SHLD/SHRD publication helpers in both backends now share
`x86p_cpu_double_shift32`; direct tests preserve lazy flags and destination bytes
at masked-zero counts, and check unaligned writes and count masking. Existing
JIT register, memory, alias, and fault cases exercise the same owner. The combined
30-test gate was rerun after these shipping corrections and passed; focused
binary64-contract startup/control tests also passed.
