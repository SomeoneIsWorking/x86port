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
| CPU and memory state | Registers, lazy flags container, mapped guest memory, state comparison, and double-shift value/flags publication after backend address validation | `src/x86port/cpu.{h,c}`, `src/x86port/cpu_compare.{h,c}`, `src/x86port/flags.{h,c}` | `X86pCpu`, `X86pMem`, `x86p_cpu_double_shift32` | — |
| Decode | Runtime byte decoding and operand normalization; no guest semantics | `src/x86port/decode.{h,c}`, `src/x86port/decode_cache.{h,c}` | `x86p_decode` | — |
| Integer semantics | ALU, conditions, bit operations, strings, BCD, privilege, and CPUID rules | `src/x86port/alu.{h,c}`, `src/x86port/cond.{h,c}`, `src/x86port/bit_ops.{h,c}`, `src/x86port/string_ops.{h,c}`, `src/x86port/bcd.{h,c}`, `src/x86port/privilege.{h,c}`, `src/x86port/cpuid.{h,c}` | Narrow module APIs | — |
| Floating and vector semantics | SIMD, 3DNow!, x87 state, arithmetic, execution, and transcendental rules | `src/x86port/simd.{h,c}`, `src/x86port/simd_int.c`, `src/x86port/simd_float.c`, `src/x86port/three_dnow.{h,c}`, `src/x86port/x87.{h,c}`, `src/x86port/x87_state.{h,c}`, `src/x86port/x87_exec.{h,c}`, `src/x86port/x87_transcendental.{h,c}` | Narrow module APIs | — |
| Software x87 functions | Host-independent math intermediates with explicit host-value adapters, integer conversions, stack functions, and register operations | `src/x86port/x87_softfloat.cpp`, `src/x86port/x87_softfloat_atan.cpp`, `src/x86port/x87_{integer,fn_stack,register}.c`, `cmake/softfloat.cmake` | `x86p_x87_fn_software_control` | `prior-art.md` |
| Test interpreter | Sequential decode/execute oracle, linked only into the separate framework-test target | `src/x86port/exec.{h,c}` | `x86p_step` | `migration.md` |
| Product JIT dispatcher | Block lookup, translation, bounded run exits, interception, profiling, invalidation, and the only legal fallback entry/telemetry boundary | `src/x86port/jit_engine.{h,c}`, `src/x86port/jit_profile.{h,c}`; future cohesive fallback module | `x86p_jit_engine_run` | `migration.md` |
| x64 backend | x86-32 basic-block lowering, host-call ABI ownership, and x86-64 machine-code emission | `src/x86port/jit_x64.{h,c}`, `src/x86port/jit_x64_internal.h`, `src/x86port/jit_x64_abi.h`, `src/x86port/jit_x64_{alu,branch,simd,x87_register}.c`, `src/x86port/jit_x64_x87.{h,c}`, `src/x86port/emit_x64.{h,c}` | `x86p_jit_translate` | `migration.md` |
| ARM64 backend | Block orchestration, arithmetic/lazy-flag lowering, x87 lowering, and host encoding with shared register/layout primitives | `src/x86port/jit_arm64.c`, `src/x86port/jit_arm64_internal.h`, `src/x86port/jit_arm64_integer.{h,c}`, `src/x86port/jit_arm64_{alu,branch,simd,x87_register}.c`, `src/x86port/jit_arm64_x87.{h,c}`, `src/x86port/emit_arm64.{h,c}` | `x86p_jit_translate` | `migration.md` |
| x87 translation admission | Decode-shape and value-admission policy shared by both host backends | `src/x86port/jit_x87_predicates.{h,c}` | `x87_*_is_emittable` | — |
| Runtime configuration | Explicit typed instance options; no product environment selector | Cohesive configuration module under `src/x86port/` | Product creation API | `migration.md` |
| Library diagnostics | One configurable sink for fatal library-contract reports; recoverable guest/runtime outcomes remain typed statuses | `src/x86port/diagnostic.{h,c}` | `x86p_diagnostic_set_sink`, `x86p_diagnostic_report`, `x86p_diagnostic_fatalf` | `migration.md` |
| Framework verification | Locked host build/test orchestration, unit, hardware-oracle, differential, product-only runtime, link-boundary, and architecture tests; silicon code publication uses the shared memory owner through `tests/oracle_code.h` | `tests/`, `tools/verify.py`, `tools/verify_product_boundary.py`, `tools/x86port_checks/` | `tools/verify.py`, CTest targets | `project-state.md` |
| Binary, corpus, and performance instruments | PE32 import/export/IAT inspection, decode comparison, coverage ranking, and benchmark reporting | `tools/` | `tools/pe.py`, standalone tool mains | — |
| Third-party decoding | Pinned Zydis instruction decoding and tables only | `vendor/zydis/` | Zydis API | — |
| Guest-neutral code memory/cache | W^X publication and address-to-block primitives shared by proven frameworks | Sibling `../jit-common/` repository | `jitcommon` CMake target | `../jit-common/docs/codemap.md` |
| Title policy | Game identity, native implementations, Win32/D3D/DirectDraw policy, input, audio, saves, and packaging | Consuming game repository | Title composition root | Consumer codemap |

## Source tree

```text
src/  —  13,868 lines, 54 files
└─ x86port/  —  13,868 lines, 54 files [.c .h]
```

Generated with the canonical codemap survey tool on 2026-09-04. Vendored,
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
oracle. `x87_integer.c`, `x87_register.c`, and `x87_fn_stack.c` own conversion,
register and stack rules. `x87_softfloat.cpp` owns the binary80 software math
bridge, and `x87_softfloat_atan.cpp` is the attributed approximation extension.
`cmake/softfloat.cmake` pins and builds that math dependency without Bochs's CPU
or instruction dispatcher; provenance and limitations live in
`docs/prior-art.md` and `docs/project-state.md`.
