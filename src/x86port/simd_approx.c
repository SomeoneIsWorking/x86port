#include "simd_approx.h"

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <xmmintrin.h>

int x86p_simd_approx_available(void) {
  return 1;
}

void x86p_simd_rcpps(void *dst, const void *src) {
  _mm_storeu_ps((float *)dst, _mm_rcp_ps(_mm_loadu_ps((const float *)src)));
}

void x86p_simd_rsqrtps(void *dst, const void *src) {
  _mm_storeu_ps((float *)dst, _mm_rsqrt_ps(_mm_loadu_ps((const float *)src)));
}

float x86p_simd_rcpss(float x) {
  return _mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(x)));
}

float x86p_simd_rsqrtss(float x) {
  return _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(x)));
}

#else

int x86p_simd_approx_available(void) {
  return 0;
}

void x86p_simd_rcpps(void *dst, const void *src) {
  (void)dst;
  (void)src;
}

void x86p_simd_rsqrtps(void *dst, const void *src) {
  (void)dst;
  (void)src;
}

float x86p_simd_rcpss(float x) {
  return x;
}

float x86p_simd_rsqrtss(float x) {
  return x;
}

#endif
