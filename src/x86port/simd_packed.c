#include "simd_packed.h"
#include <string.h>
void x86p_simd_addps(void *dst, const void *src) {
  float a[4], b[4];
  memcpy(a, dst, sizeof a);
  memcpy(b, src, sizeof b);
  for (unsigned i = 0; i < 4; i++) {
    a[i] = a[i] + b[i];
  }
  memcpy(dst, a, sizeof a);
}
void x86p_simd_subps(void *dst, const void *src) {
  float a[4], b[4];
  memcpy(a, dst, sizeof a);
  memcpy(b, src, sizeof b);
  for (unsigned i = 0; i < 4; i++) {
    a[i] = a[i] - b[i];
  }
  memcpy(dst, a, sizeof a);
}
void x86p_simd_mulps(void *dst, const void *src) {
  float a[4], b[4];
  memcpy(a, dst, sizeof a);
  memcpy(b, src, sizeof b);
  for (unsigned i = 0; i < 4; i++) {
    a[i] = a[i] * b[i];
  }
  memcpy(dst, a, sizeof a);
}
void x86p_simd_divps(void *dst, const void *src) {
  float a[4], b[4];
  memcpy(a, dst, sizeof a);
  memcpy(b, src, sizeof b);
  for (unsigned i = 0; i < 4; i++) {
    a[i] = a[i] / b[i];
  }
  memcpy(dst, a, sizeof a);
}
