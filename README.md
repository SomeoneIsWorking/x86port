# x86port

The x86-32 platform framework: guest execution, the Win32 HLE, and the guest
graphics frontends. Consumed by `pc/xmen2` and `pc/lf2`; replaces the
build-time translator in `shared/recomp-x86`.

The architecture and the migration plan this belongs to are in
`shared/jit-common/docs/migration.md`; progress is tracked there as S040–S047.

## What is here today

| path | what |
|---|---|
| `src/x86port/three_dnow.{h,c}` | 3DNow! semantics — 19 opcodes implemented, 5 approximation instructions refused by name |
| `src/x86port/decode.{h,c}` | one instruction, guest bytes in — Zydis v4.1.1 (`vendor/zydis`) for decode, semantics ours |
| `tools/decode_diff.c` | the differential instrument: decode a title's whole corpus and report agreement with a denominator |
| `tools/corpus_extract.py` | Ghidra export → decoder-diff input |
| `tests/` | 478 checks across two suites, including every refusal and its denominator |

3DNow! is first because it is measured to be 90% of what `pc/xmen2`'s static
translator could not handle, and because it is the largest piece of coverage
that does not depend on the still-open decoder decision.

## Verifying the decoder against a real corpus

```sh
python3 tools/corpus_extract.py ~/repo/pc/xmen2/scratch/recomp | ./build/x86p_decode_diff
```

2,168,629 instructions, 0 undecodable. See `AGENTS.md` for what the 37 length
disagreements are and why they are not defects.

## Build

```sh
cmake -S . -B build -DCMAKE_C_COMPILER=clang && cmake --build build -j
ctest --test-dir build --output-on-failure
```
