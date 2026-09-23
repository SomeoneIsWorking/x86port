# Codemap

This map records responsibility and placement only. Capability status belongs
in `project-state.md`, epic intent in `project-goals.md`, and sequencing and
acceptance gates in `migration.md`.

## Architecture

```text
consuming title
  -> explicit per-instance x86port configuration
  -> native-call boundary or product JIT dispatcher
  -> host-specific emitter and executable block cache
  -> shared x86 CPU state, decode, memory, and semantic helpers
  -> bounded fallback executor only after typed compile/unsafe-execution refusal

separate framework test target
  -> interpreter oracle over the same decoder/state/semantic helpers
```

The explicit interpreter-only diagnostic and product edge are separate link
boundaries. Consumers never choose between engines at runtime; the product JIT
dispatcher alone may enter a bounded fallback for a refused or unsafe block.

## Ownership

| Subsystem | Responsibility | Current / target location | Entry point | Deep doc |
| --- | --- | --- | --- | --- |
| Build composition | Product-library boundary, separate tests/tools, dependency resolution | `CMakeLists.txt` | `x86port_runtime` target (`x86port` alias), `x86port_test_oracle` | `migration.md` |
| CPU state | Registers, lazy flags container, state comparison, and double-shift value/flags publication after backend address validation | `src/x86port/cpu.{h,c}`, `src/x86port/cpu_compare.{h,c}`, `src/x86port/flags.{h,c}` | `X86pCpu`, `x86p_cpu_double_shift32` | — |
| Guest memory | Borrowed contiguous desktop spans, a contiguous page permission table for hosts with no VM, or exact sparse guest mappings; scalar and byte access, read/write permission admission, partial mapping changes, native span resolution and mapping lifetime | `src/x86port/cpu.h`, `src/x86port/memory.c`, `src/x86port/memory_sparse.{h,c}` | `X86pMem`, `x86p_mem_resolve`, `x86p_sparse_map`, `x86p_mem_read_bytes` | `memory.md` |
| Decode | Runtime byte decoding and operand normalization; no guest semantics | `src/x86port/decode.{h,c}`, `src/x86port/decode_cache.{h,c}` | `x86p_decode` | — |
| Integer semantics | ALU, conditions, bit operations, strings, BCD, privilege, and CPUID rules | `src/x86port/alu.{h,c}`, `src/x86port/cond.{h,c}`, `src/x86port/bit_ops.{h,c}`, `src/x86port/string_ops.{h,c}`, `src/x86port/bcd.{h,c}`, `src/x86port/privilege.{h,c}`, `src/x86port/cpuid.{h,c}` | Narrow module APIs | — |
| Floating and vector semantics | SIMD, 3DNow!, x87 state, arithmetic, execution, and transcendental rules | `src/x86port/simd.{h,c}`, `src/x86port/simd_int.c`, `src/x86port/simd_float.c`, `src/x86port/three_dnow.{h,c}`, `src/x86port/x87.{h,c}`, `src/x86port/x87_state.{h,c}`, `src/x86port/x87_exec.{h,c}`, `src/x86port/x87_transcendental.{h,c}` | Narrow module APIs | — |
| x87 guest memory formats | Validated floating and signed-integer guest memory conversion shared by the test oracle and generated-code adapters, from an address, from operand bits a backend loaded itself, or into operand bytes a backend stores itself | `src/x86port/x87_memory.{h,c}` | `x86p_x87_read_value`, `x86p_x87_write_value`, `x86p_x87_reg_from_operand_bits`, `x86p_x87_operand_bytes_from_reg` | — |
| Exact operand widening | Reassembly of a binary32 or binary64 operand into the ext80 encoding, which rounds nothing and raises nothing, kept out of the softfloat that exists to make the rounding decision this direction does not have | `src/x86port/x87_ext80_widen.{h,c}` | `x86p_ext80_from_f32_bits`, `x86p_ext80_from_f64_bits` | — |
| Multi-access stack semantics | PUSHAD, POPAD and ENTER ordered register/stack transitions and precise access failure | `src/x86port/stack_ops.{h,c}` | `x86p_stack_pushad`, `x86p_stack_popad`, `x86p_stack_enter` | — |
| Software x87 functions | Host-independent ext80 arithmetic, rounding, narrowing and math intermediates with explicit binary128/ext80 adapters, integer conversions, stack functions, and register operations | `src/x86port/x87_softfloat.{h,cpp}`, `src/x86port/x87_softfloat_atan.cpp`, `src/x86port/x87_{integer,fn_stack,register}.c`, `cmake/softfloat.cmake` | `x86p_x87_fn_software_control` | `prior-art.md` |
| Test interpreter | Sequential decode/execute oracle, linked only into the separate framework-test target | `src/x86port/exec.{h,c}` | `x86p_step` | `migration.md` |
| Block chaining | Per-exit inline caches (guest address, translation) and the run header a chained exit steps, so translated blocks transfer to their successors without the dispatcher | `src/x86port/jit_chain.{h,c}` | `x86p_jit_chain_*` | — |
| Leaf sites | Per-CALL inline caches (runtime target, consumer leaf) for CALLs through a register or memory, refilled from the engine's leaf resolver up to four times, pooled per engine and returned on a flush | `src/x86port/jit_leaf_sites.{h,c}` | `x86p_jit_leaf_sites_*`, `x86p_jit_leaf_site_fill` | — |
| Product JIT dispatcher | Block lookup, translation, bounded run exits, interception, profiling, invalidation, and the only legal fallback entry/telemetry boundary | `src/x86port/jit_engine.{h,c}`, `src/x86port/jit_profile.{h,c}`; future cohesive fallback module | `x86p_jit_engine_run` | `migration.md` |
| x64 backend | x86-32 basic-block lowering, control transfers with chained exits and in-place leaf calls, direct or through a leaf site (`jit_x64_branch.c`), host-call ABI ownership, guest memory operands, the write-through guest register cache, inline x87 with its host-stack mirror, and x86-64 machine-code emission | `src/x86port/jit_x64.{h,c}`, `src/x86port/jit_x64_internal.h`, `src/x86port/jit_x64_abi.h`, `src/x86port/jit_x64_{alu,branch,cond,mem,simd,x87_register}.c`, `src/x86port/jit_x64_gpr.{h,c}`, `src/x86port/jit_x64_x87.{h,c}`, `src/x86port/jit_x64_x87_inline.{h,c}`, `src/x86port/emit_x64.{h,c}` | `x86p_jit_translate` | `migration.md` |
| ARM64 backend | Block orchestration, arithmetic/lazy-flag lowering, x87 lowering, and host encoding with shared register/layout primitives | `src/x86port/jit_arm64.c`, `src/x86port/jit_arm64_internal.h`, `src/x86port/jit_arm64_integer.{h,c}`, `src/x86port/jit_arm64_{alu,branch,simd,x87_register}.c`, `src/x86port/jit_arm64_x87.{h,c}`, `src/x86port/emit_arm64.{h,c}` | `x86p_jit_translate` | `migration.md` |
| Published JIT storage | Native W^X regions or bounded instantiated WASM modules, reclamation after cache invalidation and flush | `src/x86port/jit_storage.h`, `jit_storage_native.c`, `jit_storage_wasm.c` | `x86p_jit_storage_translate`, `x86p_jit_storage_reset` | `migration.md` |
| WebAssembly host | Worker-local Emscripten module instantiation, real semantic imports, owned indirect-table entries and engine diagnostic propagation | `src/x86port/jit_wasm_host.{h,c}` | `x86p_wasm_host_create`, `X86pWasmHost` | `migration.md` |
| WebAssembly encoder | The WebAssembly binary format only: module sections, padded size patching, LEB128, and the instruction encodings a lowering backend needs. No guest state and no lowering | `src/x86port/emit_wasm.{h,c}` | `x86p_wasm_module_begin`, `x86p_wasm_section_begin`, `x86p_wasm_ok`, `x86p_wasm_intact` | `migration.md` |
| WebAssembly module builder | The module a lowered block lives in: the fixed import layout, the block signature, the export names, and appending several block bodies to one module | `src/x86port/jit_wasm_module.{h,c}`, `jit_wasm_imports.{h,c}` | `x86p_wasm_module_init`, `x86p_wasm_module_body_begin`, `x86p_wasm_module_finish`, `x86p_wasm_import_address` | `migration.md` |
| WebAssembly guest-state access | The only place that knows X86pCpu's layout and the mapping a lowered block was translated for: registers at a width, the lazy flag fields, a memory operand's linear address, the bounds check, and the way out of a block | `src/x86port/jit_wasm_state.{h,c}`, `jit_wasm_memory.{h,c}` | `x86p_wasm_state_load_reg`, `x86p_wasm_state_guard`, `x86p_wasm_state_check`, `x86p_wasm_state_store_flags`, `x86p_wasm_state_exit_imm` | `project-state.md` |
| WebAssembly lowering | Guest basic block to a WebAssembly function body, with one dispatch-table row per instruction family. Host-independent: it takes explicit mapping offsets, so it builds and is tested on every host | `src/x86port/jit_wasm_lower.{h,c}`, `jit_wasm_internal.h`, `jit_wasm_alu.c`, `jit_wasm_move.c`, `jit_wasm_stack.c`, `jit_wasm_branch.c`, `jit_wasm_integer.{h,c}`, `jit_wasm_bitops.{h,c}`, `jit_wasm_control.{h,c}`, `jit_wasm_x87.{h,c}`, `jit_wasm_simd.{h,c}` | `x86p_wasm_lower_block`, `x86p_wasm_can_lower` | `project-state.md` |
| WebAssembly conditions | The 0-or-1 value of one of the sixteen condition codes, derived inline from the lazy flag state when the lowering knows which operation wrote it and asked of the shared authority when it does not. Jcc, SETcc, CMOVcc and FCMOVcc all go through it | `src/x86port/jit_wasm_cond.{h,c}` | `x86p_wasm_cond_value`, `x86p_wasm_cond_is_inline` | — |
| WebAssembly module lifetime | The engine has no unload, so instantiated modules are bounded here: a cap, a named refusal, and release driven by the caller. The engine itself is a vtable, which is what makes the accounting testable everywhere | `src/x86port/jit_wasm_arena.{h,c}` | `x86p_wasm_arena_publish`, `x86p_wasm_arena_entry`, `x86p_wasm_arena_release` | `project-state.md` |
| WebAssembly backend adapter | The `x86p_jit_*` contract on a wasm host, and the one part of the backend that assumes a host pointer is a linear-memory offset. Built only for Emscripten | `src/x86port/jit_wasm.{h,c}` | `x86p_jit_translate`, `x86p_jit_enter`, `x86p_jit_wasm_publish` | `project-state.md` |
| Backend outcome names | The contract's exit and status spellings, shared by every backend so two hosts cannot name the same outcome differently | `src/x86port/jit_status.c` | `x86p_jit_exit_name`, `x86p_jit_status_name` | — |
| x87 translation admission | Decode-shape and value-admission policy shared by both host backends | `src/x86port/jit_x87_predicates.{h,c}` | `x87_*_is_emittable` | — |
| Runtime configuration | Explicit typed instance options; no product environment selector | Cohesive configuration module under `src/x86port/` | Product creation API | `migration.md` |
| Library diagnostics | One configurable sink for fatal library-contract reports; recoverable guest/runtime outcomes remain typed statuses | `src/x86port/diagnostic.{h,c}` | `x86p_diagnostic_set_sink`, `x86p_diagnostic_report`, `x86p_diagnostic_fatalf` | `migration.md` |
| Framework verification | Locked host build/test orchestration, unit, hardware-oracle, differential, product-only runtime, link-boundary, and architecture tests; silicon code publication uses the shared memory owner through `tests/oracle_code.h` | `tests/`, `tools/verify.py`, `tools/verify_product_boundary.py`, `tools/verify_source_policy.py`, `tools/verify_format.py`, `tools/x86port_checks/` | `tools/verify.py`, CTest targets | `project-state.md` |
| Binary, corpus, and performance instruments | PE32 import/export/IAT inspection, decode comparison, coverage ranking, and benchmark reporting | `tools/` | `tools/pe.py`, standalone tool mains | — |
| Third-party decoding | Pinned Zydis instruction decoding and tables only | `vendor/zydis/` | Zydis API | — |
| Guest-neutral code memory/cache | W^X publication and address-to-block primitives shared by proven frameworks | Sibling `../jit-common/` repository | `jitcommon` native target and `jitcommon_cache` portable target | `../jit-common/docs/codemap.md` |
| Title policy | Game identity, native implementations, Win32/D3D/DirectDraw policy, input, audio, saves, and packaging | Consuming game repository | Title composition root | Consumer codemap |

## Source tree

```text
src/  —  23,060 lines, 111 files
└─ x86port/  —  23,060 lines, 111 files [.c .h .cpp]
```

Generated with the canonical codemap survey tool on 2026-09-08. Vendored,
test, tool, build, and scratch trees are intentionally outside this first-party
source ownership count.

## Where does new work go?

- Guest instruction meaning goes in the smallest semantic module under
  `src/x86port/`; both product JIT and test oracle call that owner.
- x64 or ARM64 lowering goes in its host backend, never in a title or the test
  interpreter.
- Block scheduling, invalidation, and bounded exits go in the product JIT
  dispatcher.
- Interpreter-only dispatch and oracle tracing go in the separately linked test
  owner. Bounded product fallback execution and per-reason counters go behind
  the product JIT dispatcher, never in a consumer or selector.
- Configuration validation goes in `src/x86port/config.{h,c}`. Diagnostic
  routing goes through `src/x86port/diagnostic.{h,c}`, not through standard I/O
  in an emitter, semantic owner, or consumer.
- A title address, service policy, renderer, input mapping, or save path goes in
  the consuming game.
- An abstraction moves to `jit-common` only after a second platform framework
  proves the same contract; until then it remains in its concrete owner.

The backend `jit_*_alu.c`, `jit_*_simd.c`, `jit_*_branch.c` and
`jit_*_x87_register.c` files own their cohesive lowering families. They emit
host operations or calls to narrow CPU semantic functions, never the test
interpreter dispatcher. `cpu.c` owns status transfers and counted-loop state;
`simd_packed.c` owns default-environment packed arithmetic shared with the
oracle. `x87_stack.h` owns the register file itself -- reading ST(i), writing it,
pushing, popping, and the tag every write derives -- as `static inline`,
because those are on every x87 operation the engine performs;
`x87_stack.c` gives them external definitions for callers outside the
library.
`x87_integer.c`, `x87_register.c`, and `x87_fn_stack.c` own conversion,
instruction-level register operations and the transcendental stack rules. `x87_softfloat.cpp` owns the binary80 software math
bridge, and `x87_softfloat_atan.cpp` is the attributed approximation extension.
`cmake/softfloat.cmake` pins and builds that math dependency without Bochs's CPU
or instruction dispatcher; provenance and limitations live in
`docs/prior-art.md` and `docs/project-state.md`.
