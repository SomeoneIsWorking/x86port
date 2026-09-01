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
| `tests/test_three_dnow.c` | 427 checks, including the refusals and their denominators |

3DNow! is first because it is measured to be 90% of what `pc/xmen2`'s static
translator could not handle, and because it is the largest piece of coverage
that does not depend on the still-open decoder decision.

## Build

```sh
cmake -S . -B build -DCMAKE_C_COMPILER=clang && cmake --build build -j
ctest --test-dir build --output-on-failure
```
