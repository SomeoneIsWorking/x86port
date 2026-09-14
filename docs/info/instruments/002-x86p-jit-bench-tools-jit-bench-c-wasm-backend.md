---
id: I002
kind: instrument
status: trusted
created: 2026-09-14
---

## Instrument

x86p_jit_bench (tools/jit_bench.c, wasm backend)

## Validated by

Run under node it now REFUSES instead of lying: 'REFUSED: the jit column stopped at unsupported instruction (exit 1) running the kernel once; it cannot be timed'. That is the other answer -- the tool can now report 'this backend does not run this kernel' as distinct from 'this backend is fast'. Natively it prints 'agreement: interpreter and jit leave identical state after one kernel' and plausible columns (native+flags 0.95 ns/insn, jit 0.39, interpreter 466.74). Supersedes the distrusted record I001.

## Known failure modes

(none recorded yet)
