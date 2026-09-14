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
the other columns use and fold their result into a volatile sink, and one kernel
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

## Translation, not execution, is what the slow phases cost

Two independent measurements agree, and they replace two earlier readings of this
note (a fixed per-entry cost, then the asset phase's own work per block):

* the `kernel:` line above is a *timed* translation of the shipping path, on both
  hosts, for the identical block: **2.040 ms under wasm against 0.209 ms native**.
  Ten times, roughly **32 us per guest instruction**;
* a consuming title's heartbeat deltas, one interval: `+34,819` blocks translated
  and `+1,164,611` blocks executed in 5.1 s, at 5.15 instructions per block. At
  the measured cost a 5.15-instruction block costs ~165 us to translate, so those
  translations alone account for the whole interval; natively the same work is
  ~0.57 s, about 11%.

So the phases that look pathologically slow are **compile-bound**: the browser is
building WebAssembly modules, and the code it builds runs at the 2.7x measured
above. The block rates a title reports are therefore a property of how much new
code it is touching, which is why they range from 0.73M/s in a translation-heavy
phase to 5.16M/s in a hot loop against 15.5-21.3M/s native.

**The designed-for fix is batching, and the shipping publish does not use it.**
`jit_wasm_module.h` defines `X86P_WASM_MAX_BODIES 64` with per-body names
`b0..b63`, has a module that takes several functions, and rejects a caller passing
more than 64 -- so multiple translated bodies in one module is a supported shape.
`x86p_jit_storage_translate` publishes one body per module, paying module
construction and instantiation for a ~5-instruction block. Amortising that across
bodies, and/or translating larger blocks, attacks the measured 32 us per
instruction directly. The gates are the wasm test suite and the agreement check
(this tool runs it on both hosts); a change here is an execution-engine change and
must keep those green, not a title workaround.

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
