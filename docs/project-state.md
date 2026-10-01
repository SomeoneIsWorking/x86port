# Project state

Epic intent lives in `project-goals.md`, ownership in `codemap.md`, and ordered
implementation gates in `migration.md`.

Comparison baseline: the offline generated-source pipeline that emitted large
guest C corpora and compiled them into each title. This product instead consumes
the user's original binary and dynamically translates non-native guest code at
runtime; explicit interpreter-only execution is a separately linked diagnostic.

| ID | Capability | State | Evidence or exact gap |
| --- | --- | --- | --- |
| S001 | Runtime x86-32 decoding consumes guest bytes without a maintainer generator | verified | `src/x86port/decode.c` uses pinned Zydis v4.1.1 at runtime |
| S002 | CPU state, memory, flags, integer, SIMD and x87 semantics provide one reusable model | partial | Hardware-oracle and interpreter differentials pass; title-required instruction, exception, interrupt and timing coverage is not demonstrated |
| S003 | A test-only interpreter oracle exercises the shared decoder and semantic model | verified | `x86port_test_oracle` is built only when x86port is the top-level CMake project |
| S004 | The x64 backend translates and executes runtime basic blocks | partial | Whole-machine differential against the oracle passes; title-required emitter, exception and interrupt coverage incomplete, and no consumer gameplay conformance yet |
| S005 | Dynarec is the product default; bounded fallback is controlled and explicit interpreter mode is diagnostic-only | partial | Product archive inspection proves no interpreter selector or oracle dispatch edge; the bounded fallback owner, typed reasons, counters and return-to-JIT tests are not implemented |
| S006 | Static Substrate and offline guest-code generation are absent | verified | No generator, generated corpus or offline generation workflow exists in the tree |
| S007 | An ARM64 product JIT backend is available | partial | Encoder and backend build for arm64/aarch64; runtime behavior qualified only under qemu-aarch64, not on a device |
| S008 | Runtime dispatch supports image-aware native overrides and scoped original calls through the JIT | partial | Interception, inline dispatch, translation callbacks and leaves are covered against the oracle; image identity, override install/remove invalidation and scoped original calls unverified as one contract |
| S009 | Executable-code publication, caching and invalidation preserve runtime correctness | partial | `jit-common` publication and range invalidation used on every backend; full host matrix of publication, cache identity and multi-instance behavior not demonstrated |
| S010 | Runtime configuration is typed, explicit and instance-owned | missing | No configuration owner exists yet; no `src/x86port/config.c` |
| S011 | Library diagnostics use one configurable sink and explicit failure status | verified | `src/x86port/diagnostic.{h,c}` owns the sink; guest faults and unsupported instructions return typed statuses |
| S012 | Source ownership and the normal verifier enforce the quality limits | partial | `tools/verify_source_policy.py` enforces the 1,200-line ceiling, diagnostics and configuration ownership; x64 lowering debt is ratcheted at 1,343 lines and `exec.c` is near the limit |
| S013 | The decoder is checked against a byte-level corpus from a Ghidra export | partial | `tools/corpus_extract.py` + `tools/decode_diff.c`; the title corpus comparison has not been rerun on the merged tree |
| S014 | X-Men 2 consumes the canonical JIT-only boundary and passes gameplay conformance | partial | Its JIT path exists; the gameplay binary's link/selector audit, dispatch and invalidation evidence, and representative gameplay are open |
| S015 | Little Fighter 2 consumes the canonical JIT-only boundary and passes gameplay conformance | missing | Its Win32/DirectDraw seams and generated guest C are not yet migrated |
| S016 | A WebAssembly product JIT backend is available | partial | Encoder, lowering, arena lifetime, storage and threaded host tests pass; raw x87 state forms, far/segment transfers and 16-bit forms still refuse, and browser consumer gameplay is unqualified |

Host CI runs the asset-free graph on Linux x86-64 (Clang, plus a binary128
`long double` model matching Android x86-64), macOS x86-64 and arm64 (Apple
Clang) and Windows x86-64 (clang-cl). `test_integer_tail`, `test_x87_fn`,
`test_simd` and `test_string_ops` compare against instructions executed on
physical x86-64 hardware and exit 77 elsewhere. Windows x87 value-bearing forms
and Apple ARM64 binary64 x87 state remain partial; Android executable-memory,
ABI, packaging and gameplay are unverified.

Current focus: S014 — link X-Men 2 onto the JIT-only product target and verify
its actual gameplay link and selector.
