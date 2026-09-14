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

Run under node, the tool now says what is actually true:

    kernel: 64 guest instruction(s), block translated 64 of them into 5426 host byte(s)
    REFUSED: the jit column stopped at unsupported instruction (exit 1) running the kernel once,
             at guest offset +0 in the block (block covers 64 instructions); it cannot be timed

**Offset +0 with EIP unchanged is not an instruction the backend could not lower.**
It is a block with no runnable body at all: module instantiation needs a worker,
and a plain `node` build has none (this document says so above, which is exactly
the kind of assumption the version of this tool that printed `0.03 ns/insn` was
happy to make). Translation covering all 64 instructions is therefore not
evidence that anything ran, and the bench has to be built the way the product is
built -- pthreads, proxied main thread -- before it can measure the wasm backend
at all.

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
