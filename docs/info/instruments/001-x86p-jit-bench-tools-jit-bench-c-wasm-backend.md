---
id: I001
kind: instrument
status: DISTRUSTED
created: 2026-09-14
distrusted_on: 2026-09-14
---

## Instrument

x86p_jit_bench (tools/jit_bench.c, wasm backend)

## Validated by

NOT validated for the wasm backend. It has only ever been run against a backend whose block executes, so it has never shown the other answer. Run under node (build/wasm-bench) it reports jit 0.000 s / 0.03 ns per instruction and '10127.50x faster than the interpreter', which is impossible, and its native-C column reports 0.08 ns per instruction.

## Known failure modes

(none recorded yet)

## DISTRUSTED 2026-09-14

Two of its four columns measure nothing: the JIT loop discards x86p_jit_enter's exit status ((void) cast) and never compares an engine's result, so a block that refuses immediately is timed as the fastest possible execution; and the native-C column's only sink is a dead 0xDEADBEEF branch, so a wasm build can drop the loop. Fix before quoting any wasm-vs-native number: fail on any exit other than kX86pJitExitBlockEnd, sink every engine's result into something observable, and require the engines' final states to agree.

> Every result this instrument produced is suspect until it is re-validated.
