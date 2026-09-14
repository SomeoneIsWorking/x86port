# WebAssembly runtime verification

Emscripten 4.0.16 builds the shipping runtime as wasm32. The execution engine
uses module storage instead of native executable pages. It imports the same
C semantic helpers and linear memory that the host program uses, publishes
block exports into the indirect function table, and releases those references
only after the corresponding cache entries are gone. Browser instantiation
requires a worker. Sparse guest mapping uses the API in [memory.md](memory.md). Read/write
permission checks precede generated memory access; multi-access helpers preserve
the existing architectural fault order. A cross-host module builder supplies an
engine-side `X86pWasmPlan.memory_context` for semantic memory imports; the
in-process backend otherwise uses its live `X86pMem` address.

Provision the selected SDK under `build/deps/emsdk` or set `EMSDK` to an
existing SDK checkout. Activate 4.0.16 with the SDK's own installer. No game
files are needed for these synthetic checks.

The maintainer commands run from this repository with the SDK activated:

```sh
emcmake uv run --frozen cmake -S . -B build/wasm -G Ninja -DCMAKE_BUILD_TYPE=Release
uv run --frozen cmake --build build/wasm --target test_wasm_runtime test_wasm_sparse test_wasm_integer test_wasm_integer_tail test_wasm_x87 test_wasm_simd test_memory_sparse test_x87_software
uv run --frozen ctest --test-dir build/wasm --timeout 30 -R '^(test_wasm_runtime|test_wasm_sparse|test_wasm_integer|test_wasm_integer_tail|test_wasm_x87|test_wasm_simd|test_memory_sparse|test_x87_software)$' --output-on-failure
```

Qualify the shared-memory worker route separately:

```sh
emcmake uv run --frozen cmake -S . -B build/wasm-threaded -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-pthread -DCMAKE_CXX_FLAGS=-pthread '-DCMAKE_EXE_LINKER_FLAGS=-pthread -sPROXY_TO_PTHREAD=1'
uv run --frozen cmake --build build/wasm-threaded --target test_wasm_runtime test_wasm_sparse test_wasm_integer test_wasm_integer_tail test_wasm_x87 test_wasm_simd test_memory_sparse test_x87_software
uv run --frozen ctest --test-dir build/wasm-threaded --timeout 30 -R '^(test_wasm_runtime|test_wasm_sparse|test_wasm_integer|test_wasm_integer_tail|test_wasm_x87|test_wasm_simd|test_memory_sparse|test_x87_software)$' --output-on-failure
```

Configure and build sequentially when build trees share a FetchContent source
checkout: reconfiguration may update that checkout while another compiler is
reading it. Test-only CMake composition sets `EXIT_RUNTIME=1` so worker-backed
command-line tests finish after `main`; consuming products do not inherit this
option. The lifetime test enables memory growth to cover imported-memory
stability, and Emscripten explicitly warns about its cost with pthreads.

## The benchmark refuses rather than lying, and that is the honest answer

`tools/jit_bench.c` had three defects that made a wasm run report nonsense
(`jit 0.000 s`, `0.03 ns/insn`, "10127x faster than the interpreter"): the JIT
loop discarded `x86p_jit_enter`'s exit status, so a block that stopped at its
first instruction was timed as the fastest possible execution; the native-C
column's only sink was an unreachable `0xDEADBEEF` branch a wasm build drops
along with the loop; and no engine's result was compared with any other's,
although the header calls the interpreter the correctness authority.

All three are fixed. The timed loop now refuses on any exit other than
`kX86pJitExitBlockEnd`, the two native columns run on the cache-aligned global
the other columns use and fold their result into a printed sink, and one kernel
is run through the interpreter and the JIT from the same seed and compared with
`x86p_cpu_diff` -- the project's own predicate -- before anything is timed.

Run under node, the tool now says what is actually true, and then measures:

    kernel: 64 guest instruction(s), block translated 64 of them into 5673 host byte(s)
    agreement: interpreter and jit leave identical state after one kernel (eip 000100b8)

    column            native x86-64    wasm32 (node)
    jit               0.39 ns/insn     1.05 ns/insn
    native+flags      0.79             1.28
    interpreter       476.07           285.43

**A translated block costs ~2.7x more to run under wasm, and guest memory access
does not change that.** The kernel now performs two memory operations per eight
instructions -- 16 of its 64, on absolute addresses inside the guest arena --
because the register-only version of it could not test the obvious explanation
for the title's 20-27x: the product's blocks load and store constantly, and this
one did not. Measured both ways, the ratio is the same (register-only was 0.39
vs 1.11; with memory, 0.39 vs 1.05), so neither the emitted body nor the guest
memory path inside it accounts for that gap.

What is left is NOT a fixed per-entry cost, and an earlier version of this note
said it was: re-reading a real browser run shows the block rate is
phase-dependent -- `34,920,857 -> 344,534,636` block entries in 60 s, 5.16M/s,
while only 40 files were opened. Steady state is therefore a 3-4x gap against the
recorded native 15.5-21.3M/s, which is what these columns measure; the 20-27x
figure came from the boot/asset phase, and belongs to the work that phase does
per guest block (file opens through the multi-path resolver, archive reads, parse
loops). The engine's per-entry path is still unmeasured here -- this benchmark
calls `x86p_jit_enter` once per iteration (one `call_indirect`, 64 instructions,
~67 ns of wasm) while the product also runs a block lookup, boundary and
override policy, statistics and slice accounting per block, at 5.1 instructions
per block -- but it is a suspect for the steady-state factor, not an explanation
of the asset-phase one.

**The wasm backend's own numbers are still missing a denominator.** The only
measured relationship from a real title remains the negative one: a browser run
of `xmen2` executes 0.74M guest block entries/s against 15.5-21.3M/s natively for
the same counters, with zero refusals (issue #149 there).

So the wasm backend still has no measured throughput ratio. The only trustworthy
relationship remains the negative one: a browser run of the `xmen2` title
executes 0.74M guest block entries/s against 15.5-21.3M/s natively for the same
counters, with zero refusals (issue #149 there). Diagnostics: the product's
engine treats a non-Intercept/non-Budget run status as a refusal and links no
interpreter, so a slow run cannot be interpretation in disguise.

These tests prove runtime translation, calls to real imported helpers, precise
faults, independent engine ownership, invalidation/remapping, capacity-driven
reclamation, sparse memory and software floating point. They do not qualify a
browser game's rendering, storage, scheduling or performance. Remaining
instruction families and consumer evidence belong to S016 in
[project-state.md](project-state.md).
