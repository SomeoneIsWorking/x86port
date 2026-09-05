/* Narrow packed arithmetic shared by the oracle and emitted calls.
 * Uses the default host binary32 environment; see simd_float.c for MXCSR gaps.
 */
#ifndef X86PORT_SIMD_PACKED_H
#define X86PORT_SIMD_PACKED_H
void x86p_simd_addps(void *dst, const void *src);
void x86p_simd_subps(void *dst, const void *src);
void x86p_simd_mulps(void *dst, const void *src);
void x86p_simd_divps(void *dst, const void *src);
#endif
