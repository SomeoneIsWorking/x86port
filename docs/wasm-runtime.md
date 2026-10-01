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

When a contiguous mapping carries the page permission table described in
[memory.md](memory.md), the emitted access is a bounds compare, one or two
permission loads and a direct wasm load or store -- no call and no search. The
second load is for an access straddling a page boundary, which a one-page check
would let through. `test_wasm_perms` proves this against the emitted guard
rather than against the helpers: agreement between two reimplementations of the
rule would say nothing about the code the translator writes.

Provision the selected SDK under `build/deps/emsdk` or set `EMSDK` to an
existing SDK checkout. Activate 4.0.16 with the SDK's own installer. No game
files are needed for these synthetic checks.

The maintainer commands run from this repository with the SDK activated:

```sh
emcmake uv run --frozen cmake -S . -B build/wasm -G Ninja -DCMAKE_BUILD_TYPE=Release
uv run --frozen cmake --build build/wasm --target test_wasm_runtime test_wasm_sparse test_wasm_perms test_wasm_integer test_wasm_integer_tail test_wasm_x87 test_wasm_cond test_x87_memory test_x87_ext80_widen test_wasm_simd test_memory_sparse test_memory_perms test_x87_software
uv run --frozen ctest --test-dir build/wasm --timeout 30 -R '^(test_wasm_runtime|test_wasm_sparse|test_wasm_perms|test_wasm_integer|test_wasm_integer_tail|test_wasm_x87|test_wasm_cond|test_x87_memory|test_x87_ext80_widen|test_wasm_simd|test_memory_sparse|test_memory_perms|test_x87_software)$' --output-on-failure
```

Qualify the shared-memory worker route separately:

```sh
emcmake uv run --frozen cmake -S . -B build/wasm-threaded -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-pthread -DCMAKE_CXX_FLAGS=-pthread '-DCMAKE_EXE_LINKER_FLAGS=-pthread -sPROXY_TO_PTHREAD=1'
uv run --frozen cmake --build build/wasm-threaded --target test_wasm_runtime test_wasm_sparse test_wasm_perms test_wasm_integer test_wasm_integer_tail test_wasm_x87 test_wasm_cond test_x87_memory test_x87_ext80_widen test_wasm_simd test_memory_sparse test_memory_perms test_x87_software
uv run --frozen ctest --test-dir build/wasm-threaded --timeout 30 -R '^(test_wasm_runtime|test_wasm_sparse|test_wasm_perms|test_wasm_integer|test_wasm_integer_tail|test_wasm_x87|test_wasm_cond|test_x87_memory|test_x87_ext80_widen|test_wasm_simd|test_memory_sparse|test_memory_perms|test_x87_software)$' --output-on-failure
```

Configure and build sequentially when build trees share a FetchContent source
checkout: reconfiguration may update that checkout while another compiler is
reading it. Test-only CMake composition sets `EXIT_RUNTIME=1` so worker-backed
command-line tests finish after `main`; consuming products do not inherit this
option. The lifetime test enables memory growth to cover imported-memory
stability, and Emscripten explicitly warns about its cost with pthreads.

These tests prove runtime translation, calls to real imported helpers, precise
faults, independent engine ownership, invalidation/remapping, capacity-driven
reclamation, sparse memory and software floating point. They do not qualify a
browser game's rendering, storage, scheduling or performance. Remaining
instruction families and consumer evidence belong to S016 in
[project-state.md](project-state.md).
