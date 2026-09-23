#include "simd_packed.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

float x86p_sse_nan_result(float a, float b, float computed) {
  uint32_t bits;
  if (isnan(a)) {
    memcpy(&bits, &a, sizeof bits);
  } else if (isnan(b)) {
    memcpy(&bits, &b, sizeof bits);
  } else if (isnan(computed)) {
    bits = UINT32_C(0xFFC00000);
  } else {
    return computed;
  }
  bits |= UINT32_C(0x00400000);
  float quiet;
  memcpy(&quiet, &bits, sizeof quiet);
  return quiet;
}
void x86p_simd_addps(void *dst, const void *src) {
  float a[4], b[4];
  memcpy(a, dst, sizeof a);
  memcpy(b, src, sizeof b);
  for (unsigned i = 0; i < 4; i++) {
    a[i] = x86p_sse_nan_result(a[i], b[i], a[i] + b[i]);
  }
  memcpy(dst, a, sizeof a);
}
void x86p_simd_subps(void *dst, const void *src) {
  float a[4], b[4];
  memcpy(a, dst, sizeof a);
  memcpy(b, src, sizeof b);
  for (unsigned i = 0; i < 4; i++) {
    a[i] = x86p_sse_nan_result(a[i], b[i], a[i] - b[i]);
  }
  memcpy(dst, a, sizeof a);
}
void x86p_simd_mulps(void *dst, const void *src) {
  float a[4], b[4];
  memcpy(a, dst, sizeof a);
  memcpy(b, src, sizeof b);
  for (unsigned i = 0; i < 4; i++) {
    a[i] = x86p_sse_nan_result(a[i], b[i], a[i] * b[i]);
  }
  memcpy(dst, a, sizeof a);
}
void x86p_simd_divps(void *dst, const void *src) {
  float a[4], b[4];
  memcpy(a, dst, sizeof a);
  memcpy(b, src, sizeof b);
  for (unsigned i = 0; i < 4; i++) {
    a[i] = x86p_sse_nan_result(a[i], b[i], a[i] / b[i]);
  }
  memcpy(dst, a, sizeof a);
}
