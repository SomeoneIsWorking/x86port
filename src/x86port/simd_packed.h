/* Narrow packed arithmetic shared by the oracle and emitted calls.
 * Uses the default host binary32 environment; see simd_float.c for MXCSR gaps.
 * NaN results follow SSE, not the host: see x86p_sse_nan_result.
 */
#ifndef X86PORT_SIMD_PACKED_H
#define X86PORT_SIMD_PACKED_H
void x86p_simd_addps(void *dst, const void *src);
void x86p_simd_subps(void *dst, const void *src);
void x86p_simd_mulps(void *dst, const void *src);
void x86p_simd_divps(void *dst, const void *src);
/*
 * The result of an SSE arithmetic instruction whose host-computed value is
 * `computed`, for first source `a` and second source `b`. C leaves the NaN of
 * `a op b` unspecified -- a compiler may commute a multiply, and an AArch64 or
 * WebAssembly host returns a different default -- so the SSE rule is applied
 * here: the first source's NaN, else the second's, both quieted, else the
 * 0xFFC00000 indefinite for an invalid operation.
 */
float x86p_sse_nan_result(float a, float b, float computed);
#endif
