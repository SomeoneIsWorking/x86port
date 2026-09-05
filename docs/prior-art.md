# Prior art

## Bochs software x87 math (2026-09-05)

`cmake/softfloat.cmake` consumes Bochs revision
`4ceffd60c1556f574fd0e35d2343ea579d8988ca` from
<https://github.com/bochs-emu/Bochs>. It compiles the SoftFloat 3e sources and
8086-SSE specialization plus the FPU polynomial, trigonometric, remainder and
logarithmic math leaves. No Bochs CPU dispatcher or guest execution loop is
linked. Upstream files retain their license notices in the fetched dependency.

`src/x86port/x87_softfloat_atan.cpp` is derived from that revision's
`bochs/cpu/fpu/fpatan.cc`. Its original SoftFloat derivative notice and Stanislav
Shwartsman attribution remain in the file. The local change extends the atan
alternating series from eleven to twenty terms (degree 39), with nearest-even
binary128 rational coefficients. The reduction's inclusive 1/4 boundary exposed
about 1.28e-14 error in the upstream eleven-term approximation; the additional
terms reduce the truncation bound below 2e-24 on that reduced interval. Tests
retain the original tolerance rather than widening it to fit the algorithm.

`x87_softfloat.cpp` and `cmake/softfloat/config.h` are local integration owners.
The binary80 math intermediate does not make ARM64's binary64 X86pCpu storage
exact. Control-word rounding is passed to the math routines, but full precision,
exception and status conformance remains partial; see the state ledger.
