/* RCPPS, RCPSS, RSQRTPS and RSQRTSS: the hardware's table approximations, which only an x86 host
 * reproduces, by running the same instruction. Other hosts refuse them by name. */
#ifndef X86PORT_SIMD_APPROX_H
#define X86PORT_SIMD_APPROX_H

#ifdef __cplusplus
extern "C" {
#endif

/* Nonzero when this host computes the approximations. */
int x86p_simd_approx_available(void);
/* Four lanes of 1/x or 1/sqrt(x) from `src` into `dst`; no-ops where unavailable. */
void x86p_simd_rcpps(void *dst, const void *src);
void x86p_simd_rsqrtps(void *dst, const void *src);
float x86p_simd_rcpss(float x);
float x86p_simd_rsqrtss(float x);

#ifdef __cplusplus
}
#endif

#endif
