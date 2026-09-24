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
| S016 | A WebAssembly product JIT backend is available | partial | S002 | G001, G002 |

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
| macOS arm64 | partial; not release-qualified | [Native job 101311854341](https://github.com/SomeoneIsWorking/x86port/actions/runs/33968164307/job/101311854341) at `96f7665` compiled successfully and passed 26/30 tests, including product/JIT, ARM64 emission, startup/control, and software-x87 checks. Its four failures were unavailable x86-64 hardware oracles returning an error, not ARM64 JIT failures. Those suites now report explicit skips; the complete hosted gate after that classification fix remains pending. The approved JIT value path retains binary64 x87 state without claiming extended precision or gameplay/release conformance. |
| Android x86-64 | partial; binary128 model gated | Bionic's x86-64 `long double` is binary128, which no other x86-64 host has. The x64 backend's x87 memory-operand slow paths passed their host-widened ext80 scratch to adapters that read it as `long double`, and wedged X-Men 2 on the API 35 emulator (xmen2 #172); they now decode it with `x86p_x87_from_f80`. `tools/verify.py --binary128-model` (Clang `-mlong-double-128` on Linux, glibc's `_Float128` libm bound by `tests/binary128_libm_shim.c`) builds the whole graph with that layout and is a Linux CI step; reverting the fix fails seven suites there. Emulator gameplay and packaging remain the consumer's evidence. |
| Android arm64-v8a | partial backend; unverified host | ARM64 emission exists, but Android executable-memory, ABI, packaging, and gameplay verification have not been performed. |

`test_integer_tail`, `test_x87_fn`, `test_simd`, and `test_string_ops`
compare against instructions executed on physical x86-64 hardware. That oracle
is inapplicable on ARM64: exit 77 reports zero host instructions and unchecked
semantics, never a semantic pass. Intel macOS separately skips the integer-tail
suite because its 32-bit compatibility-mode oracle is Linux-specific. On
supported hosts, oracle allocation/publication failures, unexpected zero
executions, and actual mismatches remain errors. All portable semantic and
native-host JIT/runtime tests remain required. Four compiled unavailable-branch
fixtures exercise the actual suite entry points and require exit 77; CMake
also asserts the real suites' skip registration. These portable fixtures do
not execute ARM64 code or replace native hosted qualification.

The classification change passed the normal Linux Clang 22.1.8 gate (35/35
CTests): the four hardware suites executed 7,296 / 80 / 658 / 960 host
instructions respectively with zero failures, and all four unavailable-branch
fixtures passed. Touched first-party formatting and clang-tidy checks passed.

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
binary64 path described below.

All three backends chain. ARM64 (`jit_arm64_branch.c`) gives a block two
slots, each exit comparing its slot's guest address against the 64-bit EIP in
X0 and branching (`br`) to the linked host past the prologue after the run's
stop and budget checks; a computed exit that misses probes the block cache's
front array, and every failed transfer in a block shares one return that names
the pending slot. Under `qemu-aarch64` `test_jit_engine` chains 4,089 of 4,096
block entries; removing the budget check hangs its bounded cycle and dropping
the pending store fails 8 tests. The same file enforces x64's instruction and
tail byte bounds (`X86P_JIT_WORST_CASE_INSN_BYTES`, `X86P_JIT_EPILOGUE_BYTES`),
which a leaf-site CALL passed at 488 bytes until the shared return and the
site's out-of-line refill brought it within 384. A WebAssembly transfer is a
tail call through a one-function trampoline module rather than a jump, so a
chain runs in constant stack (the main module cannot hold the tail call, which
Binaryen's asyncify refuses); a block chains its first
`X86P_WASM_CHAIN_SLOTS` (4) exits;
a block relowered into a shared module reuses the slots it was published with,
and its exit to another block of that module tail-calls it directly when the
slot names that block's entry, skipping the trampoline (`chain_exits_direct`;
`test_wasm_runtime` checks a ring that makes such calls and a slot relinked to
a retranslated sibling, which must not call the old body); an exit to a computed EIP that misses its slot probes the block cache's
front array (`jit_wasm_chain.h`). An exit to its own block's entry links and
probes like any other, so a guest loop goes round without the dispatcher
(`test_jit_engine`'s spin and alternating indirect jump, `test_wasm_runtime`'s
spin). `test_jit_engine` checks that a non-chaining backend
chains nothing rather than asserting x64's chained counts there. Leaves follow chaining: the WebAssembly backend calls
one in place as x64 does, through one import that makes the main module's
indirect call, reading a CALL-through-register's leaf site as linear memory
and reusing that site when the block is relowered (`jit_wasm_leaf.h`;
`test_wasm_leaves` checks direct, declined, refilled and exhausted calls
against the interpreter, and a 100-caller ring that stays chained, 19,999 of
20,000 entries, across six compactions with one fill per site), over a flat
mapping, a page-permission table, and engine data above 128 MiB where every
address constant takes five bytes. An instruction's size bound excludes its
chained exits, which the chain reserve pays for, each held to
`X86P_WASM_CHAIN_EXIT_BYTES`: a leaf CALL has two. ARM64 calls
leaves in place too, direct CALLs and CALL-through-register sites alike, with
a site's refill in the block tail since it runs only on a miss;
`test_jit_engine`'s leaf and site tests pass there, and redirecting the
refill's branch back to its leaf call fails four of them. Gap: ARM64 chaining
and leaves are unmeasured on an ARM64 device; the qemu differential is their
only evidence.

`x86p_jit_engine_run` now takes the caller's per-run state and hands it to the
intercept and dispatch callbacks. The run loop consults the intercept once per
block boundary, and a consumer's interception predicate needs the guest call
frame it is currently inside -- which is per-thread. Before this the consumer
had to reach that frame through a thread-local; on an Android shared object
below API 29 a thread-local is emulated, so each of those reads is a call
through a pthread key, and `x86_guest_call_top` measured 13.4% of the X-Men 2
port library's samples. The new pointer lives on the run's own stack, so it is
per-thread by construction and cannot be raced by a second guest thread the way
the registered user pointer can. `test_jit_engine` checks that the value
arrives at the callback, so a run that dropped it would fail rather than pass
with the callback ignoring it.

A forward REP STOS fills its span in one call, the way a forward REP MOVS
already copied one. `x86p_mem_fill` carries the same admission rule as
`x86p_mem_copy_disjoint` -- nonempty, wholly mapped, no write observer, no
sparse mapping -- and every case it refuses keeps element-wise execution, so a
partial fault still leaves ECX and EDI exactly where the guest would see them.
`test_string_copy`'s differential now runs STOS beside MOVS against its
independent one-element reference across widths, both directions, nine counts,
thirteen operand positions, with and without an observer, and at the top of the
address space: 22,579 state checks. Breaking the fill by one bit in the filled
byte fails 177 of them, so the bulk path is known to be reached rather than
assumed to be.

Binary128 hosts (including Android ARM64 and
Emscripten) now convert numerical state through ext80 software arithmetic rather
than refusing ordinary value forms. MMX aliases the register's ten x87 bytes,
which the binary128 register's signif/sign_exp pair holds at the same offsets,
so MMX runs there too; every register store zeroes the pair's six padding
bytes. This numerical bridge does not establish complete raw-state
Windows/ARM64 floating-point conformance.

On binary128 hosts, x87 value admission now classifies zero, NaN, infinity, and
finite values from their fields and widens f32/f64 inputs directly to binary128
bits. This removes compiler-rt comparison and widening calls from the hot
classifier while preserving the existing software arithmetic for operations;
`test_x87_binary128` covers subnormals, signed zero, infinities, NaNs, and
ordering against the host reference. The focused and combined Clang gates pass
with 43/43 tests.

Android API 21 NDK ARM64 static bionic executables now run under QEMU
AArch64 10.2.2: software x87 passes 3,068 checks, narrow state 341 checks,
startup 4,357 translated cases with zero precision refusals, and control
10,178 checks including 16 value-bearing x87 cases with zero refusals.
These are user-mode Android binaries, not an Android OS/APK or device
performance result.

A Jcc or SETcc no longer always calls `x86p_cond` on ARM64. `jit_arm64_cond.c`
lowers the condition onto AArch64's own NZCV for the Add, Sub, Logic, Inc and
Dec lazy-flag kinds at width 4, with one `cmp`/`cmn` and a `cset`; everything
else -- PF, narrower widths, the Inc/Dec conditions that read CF or OF -- still
calls the shared evaluator, which remains the one authority. The motivation was
a thread-attributed profile of the arm64 Android build of X-Men 2: `x86p_cond`
was 59 samples of the port's own thread against 424 in all emitted guest code.
`jit_x64_cond.c` is the same split for x64, which lowers nothing inline yet and
counts its calls so the field is not a silent zero.

The lowering is no longer width-4 only: a narrower operation's operands are
left-aligned into the top of the word (`lsl` by 32 - 8w) before the compare, so
the 32-bit NZCV is exactly the narrow operation's, and the shift also discards
the bits above the width that flags.c masks on read rather than on store. That
took the synthetic corpus from 32 to 55 of its 249 conditions, still with zero
divergences, and the running title from 76.5% of its conditions lowered inline
to 98.2% (15,777 of 16,059). A Jcc also reads the host condition directly with
its `csel` now instead of materialising 0/1 with a `cset` and testing it again,
and a condition that is constant for the kind picks its successor at
translation time.

Evidence is the differential against the interpreter oracle on AArch64 under
`qemu-aarch64`: 25,155 checks with zero failures over 1,423 generated programs
and 20,020 translated guest instructions, 823 of the blocks ending in a
translated branch. 32 of the 249 conditions in that run took the inline path;
that rate is a property of randomly generated programs, which rarely place a
width-4 ALU immediately before a branch, so `jit_coverage` and the engine now
report the same ratio over real title code. `test_jit_x64` checks that the
inline and helper counts add up to the conditions actually emitted and refuses
a run that emitted none, so a lowering that silently emitted nothing cannot
pass as one that was never reached.

Carry-in derivation is no longer emitted for binary ALU operations on either
backend. `x86p_flags_carry_in_is_live` in `flags.h` is the one rule: only the
Inc and Dec lazy-flag kinds ever read `carry_in` back, because every other kind
derives CF from a, b and r. A binary ALU records Add, Sub or Logic, so both the
derivation -- whose unknown-predecessor arm is a call to `x86p_flag_cf` per
block, paid on every entry to that block -- and the `FLAG_CARRY_IN` store were
dead work there. `cpu_compare.c`, which already skipped the field when it was
not live, now consumes the same rule instead of open-coding it, so a state
comparison and a translator cannot disagree about what is architectural. The
unary INC/DEC/NEG path still pays for both. Motivation: `x86p_flag_cf` was 5.07%
of the arm64 Android build's port-library samples, twice `x86p_cond`. On the
same AArch64 differential corpus the carry-in helper calls fell from 789 to 268
over 1,423 blocks, with 25,155 checks and zero failures against the oracle.

Binary128 hosts no longer pay a general softfloat conversion for ordinary
values. `x87_f128_ext80.{h,cpp}` reassembles the bits directly when a value is a
finite normal that both formats hold exactly -- which is every value that has
been through ext80 arithmetic or arrived as f32/f64 -- and refuses zero,
subnormal, infinity, NaN and ext80 unnormals to the existing
`f128_to_extF80`/`extF80_to_f128` path. `test_x87_f128_ext80` checks both
directions against softfloat itself and the ext80 round trip over 28,372 cases,
passing natively and on the ARM64 Cuttlefish device alongside the unchanged
3,140 software-math and 341 narrowing checks. An NDK arm64 micro-benchmark under
`qemu-aarch64` measured a multiply/add pair at 214 -> 145 ns per operation
(softfloat arithmetic alone is 99 ns), so the conversions were roughly half of
the cost of every guest x87 operation on that host. That is a leaf measurement,
not a frame-rate claim.

The differential uses the shipping `jit-common` region publication API for
W^X transitions and instruction-cache coherence. Gap: ARM64 caller-saved helper
register lifetimes, macOS execution, Android runtime/package behavior, and
representative consumer gameplay remain unverified in the merged tree. The
new backend still duplicates some decode-level policy and semantic helper
adapters; S012 remains partial. No ARM64 release support
is claimed from encoder tests or historical host-double differential output.

### S016 — WebAssembly backend

Evidence: `emit_wasm.{h,c}` encodes the WebAssembly binary format -- module
sections with five-byte padded size slots patched in place, canonical unsigned
and sign-terminated signed LEB128, imports of the host memory/table/helpers, and
the instruction encodings a lowering backend needs. `test_emit_wasm` builds
twelve modules with it and hands each to a real WebAssembly engine, which
validates it against the specification and runs it: 12 of 12 validated and ran,
covering arithmetic, five-byte constants, sign- and zero-extending memory access
with an offset immediate, structured `if`/`else`, a `block`/`loop` with both
branch directions, a direct call to an imported helper, an indirect call through
the imported table into a second module built by the same encoder, 64-bit
widening for a multiply's high word, `select`/`drop`, and a trap. The engine's
independence is what the test rests on: the C side never reads its own bytes
back and agrees with itself.

The encoder's own refusals are checked without an engine, because those must be
caught here rather than by an engine far from the cause: sticky overflow, an
unbalanced control region, and an unclosed size slot each make
`x86p_wasm_ok()` false. Both halves were confirmed to FIRE by mutation --
dropping the signed-LEB128 sign guard makes 64, 8192 and 1048576 read back
negative, and making a padded size slot's last byte carry a continuation bit
makes every module fail validation with the engine's own "length overflow while
decoding section length".

Lowering evidence: `jit_wasm_lower.c` and its per-family units turn a guest
basic block into a WebAssembly function body, with one dispatch-table row per
instruction family so "can this be lowered" and "how" cannot drift apart.
`test_jit_wasm` lowers 38 blocks, lays an X86pCpu and a 4 KiB guest arena into
the engine's linear memory, runs each block in node, and compares the WHOLE
machine against the separately linked interpreter oracle -- every register, the
guest EIP, all six fields of the lazy flag state, the direction flag, the rest
of X86pCpu byte for byte, and every byte of guest memory. 38 of 38 lowered
blocks ran in the engine and matched, over 1,097 individual checks.

The lowering is host-independent, and that is what makes the above possible: it
takes the guest mapping as three plain integers rather than a host pointer, so
it builds and runs on a developer machine with no Emscripten toolchain. Only
`jit_wasm.c`, the adapter that presents the `x86p_jit_*` contract, assumes a
host pointer is a linear-memory offset, and it is the only conditionally built
part of the backend.

The imported helpers a lowered block calls -- `x86p_alu`, `x86p_alu_unary`,
`x86p_cond`, `x86p_flag_cf` -- are C functions node cannot reach, so the test
calls the REAL function on the state the block starts from and hands the oracle
its return value and the bytes it wrote. Nothing about guest semantics is
reimplemented in JavaScript. Two rules keep that honest and are enforced by the
driver rather than assumed of the case table: a case contains at most one
helper-using instruction and it is the first, and the arguments the block
actually passed are compared against what the C side expected.

Confirmed to FIRE by mutation. Thirteen deliberate defects were introduced one
at a time; twelve were caught: a bounds check using `>=` instead of `>` (caught
by the last address a byte fits at), checking against `size` instead of
`size - w` (an access that straddles the end), a signed instead of an unsigned
compare (an address below the mapping), swapping the flag tuple's `a` and `b`,
widening a narrow register store to 32 bits, storing `carry_in` before the
bounds check rather than after (caught only by a faulting ALU whose carry byte
disagrees with the derived CF), passing a helper's arguments in the wrong order,
POP writing its destination before advancing ESP, MOVSX lowered as MOVZX, RET
ignoring its release count, Jcc taking the wrong arm, and a memory access
dropping the FS base.

The thirteenth did NOT fire, and is recorded rather than papered over: making
LEA add the segment base changes nothing observable, because this decoder leaves
the operand's segment at DS for an LEA carrying an FS prefix, so no case can
distinguish the two. That split states the architectural rule; it is not
verified by a test, and `jit_wasm_move.c` says so at the site.

Four of the twelve were caught only after cases were ADDED for them -- the first
mutation round found the corpus had no access at either edge of the mapping and
no faulting ALU at all, which is the honest reason those cases exist.

Module lifetime: `jit_wasm_arena.{h,c}` bounds live instantiations, because a
module per block retains engine objects and indirect-table entries until the host
drops them. The arena refuses publication at its 1,024-module cap; the JIT engine
first invalidates a selected cache entry and releases its module before reusing
the slot. Other cached translations survive module pressure. The arena counts
cap refusals apart from engine rejections.
`test_jit_wasm_arena` drives it through a stub engine with deterministic
instantiation failures: 32 checks over publish/release accounting,
the cap, release-all, a rejected module, an unreachable export, an arena with no
engine, and a host that can create but not destroy.

The Emscripten host now runs generated modules through the shipping dispatcher.
`jit_storage_native.c` owns protected code pages; `jit_storage_wasm.c` owns
module publication, cache-ordered reclamation and indirect-table entries.
The WASM path consumes `jitcommon_cache` without linking native executable-memory
primitives. Imports call the compiled C semantic owners, rather than recorded
helper answers. The host binds its 45 stable imports once per worker instead of
rebuilding their JS object for every block, and passes emitted module bytes as
a synchronous view without a second copy. Compiling on a browser main thread
is explicitly refused.

Emscripten 4.0.16 and Node 24.19.0 executed the product library both in
ordinary wasm32 builds and with `-pthread -sPROXY_TO_PTHREAD=1`. A two-worker
test creates, runs, and destroys independent JIT instances on different
pthreads, matching the browser worker-local module registry. The threaded
WASM build and all 45 registered tests pass; six host-only oracle tests report
their unsupported host explicitly as skipped. The WebAssembly product-link audit counts
570 product symbols and ten oracle symbols with zero forbidden product
references. The combined native Clang 22.1.8 gate passes all 41 tests; the
`jit-common` cache/code-memory split passes its three-test gate including
clang-format and clang-tidy. Emscripten warns that combining pthreads with
memory growth can slow JavaScript accesses; product performance remains
unqualified. Recorded synthetic results:

- `test_wasm_runtime`: 6,448 checks, zero failures. A 100-block ring, past a
  compaction batch, chains 9,762 of 10,000 entries cold and 9,960 warm (the rest
  are the depth cap's dispatches), counts every block in EAX, never transfers
  into the run's stop address, and drops a link whose target was invalidated.
  One RET shared by a hundred call sites chains 29,882 of 30,000 entries
  through the probe, against 19,999 without it. The helper chain, warm cache,
  interior-byte invalidation and unsupported-instruction discriminator report
  three translated blocks, 17 entries and one refusal. Two 1,040-block runs
  cross module and cache capacity respectively, release discarded modules,
  preserve a recent block through module pressure, preserve another engine's
  entries, bind imports once per host, and leave zero live modules after destroy.
  The minimum advertised storage capacity and memory growth are also exercised.
- `test_wasm_integer`: 236 translated cases, 37 expected faults, 3,072 checks,
  zero divergences against the separately linked interpreter. MUL/IMUL,
  DIV/IDIV, 32-bit-address strings, LOOP variants and PUSHFD/POPFD use the same
  production arithmetic, fault and repeat owners as native hosts.
- `test_wasm_sparse`: 28 checks, five translations, eight entries, zero failures.
  Guest addresses map to separately owned host allocations, including scalar
  accesses crossing allocation boundaries, precise holes and remapping after
  explicit invalidation. `test_memory_sparse` passes 933 checks on both hosts.
- `test_wasm_perms`: nine checks through `x86p_jit_engine_run`, so the emitted
  guard answers rather than the helpers. A store straddling into a read-only
  page, a load straddling into a write-only page and an access straddling into
  an unmapped page each fault with the CPU byte-for-byte unchanged and neither
  page written; restoring the byte lets the SAME translated block through with
  no invalidation; an address past the window still faults on bounds.
  `test_memory_perms` passes 18 checks on the helpers, and clearing the table
  restores host-enforced permissions so the checks are measuring the table.
- `test_wasm_integer_tail`: 307 translated entries, 47 fault/refusal cases,
  3,961 checks. Double shifts, bit/BCD operations, conditional moves, flag
  transfers, PUSHAD/POPAD/ENTER and trap/protection exits retain CPU/memory
  state and exact refusal denominators. Stack semantics are shared with the oracle.
- `test_wasm_x87`: 533 translated entries, 108 memory faults, six explicit raw
  form refusals and 4,518 checks. Tests include stack depths, PC/RC, conditional
  moves and permissions. FCOMI's explicit ST0/STi decoding exposed an oracle
  self-comparison defect; independent less/greater/equal/NaN controls prove the
  corrected source selection in both paths.
- `test_wasm_simd`: 846 translated entries, 14 fault cases and 12,133 checks.
  XMM movement, packed integer and single-float arithmetic, conversions, masks,
  MXCSR and EMMS use the existing lane owners. Nonfinite/out-of-range integer
  conversions explicitly return integer-indefinite rather than undefined C casts.
- `test_x87_software`: 3,140 checks, zero failures on wasm32. Arithmetic,
  ext80/binary128 conversion and guest rounding use software math; this host
  performs zero independent x87 hardware comparisons. The native control
  executed 9,120 checks and 8,995 hardware comparisons with zero failures.
  FPATAN and logarithm regressions exercise every precision/rounding control
  against hardware: transcendental intermediates retain ext80 precision while
  FSQRT respects guest precision. The pre-fix control produced 225 failures.

Still partial: far/segment transfers, XLAT, IRETD, BOUND, ARPL, SLDT, LFP,
16-bit memory addressing and stack forms remain absent. Raw MMX/ext80 aliasing
cannot be represented by binary128 numerical values; raw x87 state forms and
FXTRACT remain refused. Existing SIMD MXCSR rounding/DAZ/FTZ and approximation
limitations persist. Generated writes to executable pages still need compiled-page
classification, cache invalidation and an instruction-complete side exit before
stale later instructions in the active block can execute. Whole-space invalidation
preserves callbacks/configuration and is intended between block entries.
Module batching and representative browser consumer gameplay/performance remain
unqualified. These synthetic results establish runtime execution and its lifetime
boundary, not a playable X-Men 2 or Little Fighter 2 release.

### S008 — native and original dispatch

The JIT engine has consumer interception, inline dispatch, translation
boundary callbacks, and leaves: host code a chaining x64 or WebAssembly translation calls in
place for a direct CALL, which completes the call or declines having changed
nothing, and a declined call reaches the callee through the dispatcher
(`test_jit_engine`, `test_a_leaf_completes_a_direct_call_in_place`, both routes
checked against the interpreter). A CALL through a register or memory calls
the leaf for its runtime target through a per-site cache that stops refilling
after four targets (`jit_leaf_sites.c`; `test_a_leaf_completes_an_indirect_call_through_its_site`
checks one-target, declining and alternating sites against the interpreter,
and `test_the_widest_indirect_call_fits_with_its_site` the widest form within
the 384-byte instruction bound). Gap: complete image/module-generation identity, override
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
value forms while precision metadata remains false. Native Apple Silicon CI
runs alongside Intel macOS, Linux and Windows; its observed result and the
hardware-oracle applicability boundary are recorded under Host CI support.

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
That incoming branch's full 30-test graph recorded four unavailable
hardware-oracle failures on ARM64 and three integer/flag oracle failures under
Rosetta. The ARM64 classification is addressed under Host CI support; Rosetta's
reported differences remain unresolved and are not Fedora CI evidence.

The exact `f84d2a832abc11fe9990274e15e423477caa3e46` follow-up was integrated
with `96f7665` without losing its FWAIT and shared double-shift corrections.
The combined Linux Clang 22.1.8 gate passed all 31 CTests. The local copy test
passed 11,271 checks, including six additional malformed width/prefix/operation
refusals with unchanged CPU and memory; the emitted startup test passed 4,213
cases with zero refusals or failures. Touched compiled units pass clang-tidy,
all touched first-party source passes formatting, and the unchanged second build
performs no work. These are local synthetic results, not hosted or gameplay FPS
qualification of the follow-up.

Copy admission preserves the existing memory and invalidation contracts rather
than extending them: the caller provides valid host backing, and span checks
validate guest bounds and wrap, not host page permissions. With an observer
installed the fast path refuses and retains per-element write notification.
Without one, the existing caller-owned explicit JIT invalidation (or diagnostic
no-cache mode) remains necessary for code writes; neither the previous scalar
path nor this bulk path supplies automatic cache invalidation.
