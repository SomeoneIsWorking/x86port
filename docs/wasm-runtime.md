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

## Translation is what the slow phases cost

Two independent measurements agree, and they replace three earlier readings of
this note (a fixed per-entry cost, then the asset phase's own work per block,
then "publication is 87% of translation"):

* a consuming title's heartbeat deltas in a translation-heavy phase: a few
  thousand to ~22,000 misses per second, each costing ~0.1-0.2 ms. That alone
  accounts for the interval; natively the same translations cost about a tenth as
  much. The phase is **compile-bound**, and the code it produces runs at the 2.7x
  measured above;
* the `translate cost:` lines this tool prints, which separate the two halves:
  emission **0.081 ms** per block under wasm (against 0.130 ms natively -- the
  emitter is fine), and publication **~0.094 ms warm** on top.

The 0.52 ms publication figure quoted in an earlier version of this note came
from the FIRST module in a fresh process and is cold-compile noise; warm
publication is the ~0.09 ms above. That matters, because it means the cost is
**per block, not per module**, and there is very little for module-level
amortization to win.

**Measured, not argued: batching a run of blocks into one module does not pay.**
It was implemented (`x86p_jit_translate_chain`, one module per run, entries from
`b0..bN`, invalidation widened to the unit), gated green, and measured: a chain of
32 blocks cost **0.128 ms/block** against **0.175 ms/block** one at a time -- 1.37x
in this tool, and no improvement at all in the title's own same-phase block rate
(0.32-0.43M/s batched against 0.47-0.72M/s unbatched), even though misses fell
3.5-13x exactly as designed. Chaining through a taken branch also translated dead
fall-through that the run then paid for. It was reverted (x86port `eb9028e`).

### The cost has a per-BLOCK floor, and that is what is left to attack

Measuring the same tool's two kernel sizes separates the two halves of the
per-block cost on the wasm host:

| block | measured | per instruction |
|---|---|---|
| 4 instructions | 0.035 ms (35 us) | 8.8 us |
| 64 instructions | 0.165 ms (165 us) | 2.6 us |

Two points fix `cost = F + n*v`: **F ~ 26 us per block, v ~ 2.2 us per
instruction**. Real code averages 5.15 instructions per block, so **about 70% of
every translation is the per-block floor** rather than the instructions in it. On
the native host the same pair is **1.8 us/instruction at both sizes** -- there is
no floor to amortize.

That also explains why batching lost, and it is the same fact: the floor is per
BODY (its function, its export, its indirect-table entry), not per module. A
module holding 32 bodies measured **128 us/block, worse than a fresh one-body
module at 35 us**, because packing bodies together keeps the body count -- and the
whole module is compiled at instantiation.

So the lever is **fewer blocks in total**, i.e. longer blocks, rather than more
compact packaging of the same small ones: at 16 instructions per block the
per-instruction cost falls to ~3.8 us and at 64 to ~2.6 us, a 2.3-3.4x cut, and
dispatch falls with the block count too. Natively the same change is roughly
neutral (no floor, only fewer dispatches), so it is a wasm-host win.

**Implemented, and measured: a block continues past a conditional.** A
conditional already writes the TAKEN path's own exit, so the fall-through can
carry on in the same body with nothing added -- `x86p_wasm_continue_lower()`
returns that variant for `jcc`/`jecxz` and NULL for every unconditional exit,
and `x86p_wasm_jcc_lower()` is now literally the continue form plus the
fall-through epilogue, so a block that ends at a branch emits the same bytes it
always did. `block formation:` in the bench reports it directly: a
5-instruction kernel containing a branch is **5 instructions in one block** where
it used to be 2, and `formation cost:` measures the same five instructions as
**1 block in 0.050 ms against 2 blocks in 0.124 ms** -- 2.5x, in the tool, on the
wasm host.

The native backend is deliberately left alone: it has no per-block floor to
amortize, so its formation still ends at a branch (the bench reports 2
instructions for the same kernel there, which is correct rather than stale).

These tests prove runtime translation, calls to real imported helpers, precise
faults, independent engine ownership, invalidation/remapping, capacity-driven
reclamation, sparse memory and software floating point. They do not qualify a
browser game's rendering, storage, scheduling or performance. Remaining
instruction families and consumer evidence belong to S016 in
[project-state.md](project-state.md).
