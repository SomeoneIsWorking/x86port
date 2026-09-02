/*
 * simd_internal.h -- the shared vocabulary of the SIMD modules.
 *
 * Not a public header. It exists so simd.c, simd_int.c and simd_float.c agree
 * on one representation of a vector instead of three, which is the whole
 * reason the lane loops can be written once and used at both 64 and 128 bits.
 */
#ifndef X86PORT_SIMD_INTERNAL_H
#define X86PORT_SIMD_INTERNAL_H

#include "simd.h"

#include <stdint.h>
#include <string.h>

/*
 * A SIMD value, and how many of its bytes are live.
 *
 * `bytes` is 8 for an MMX operand and 16 for an XMM one, and every lane loop
 * derives its element count from it. Carrying the width WITH the value is what
 * lets PADDW be one function rather than PADDW-64 and PADDW-128: the operation
 * is identical and only the number of lanes differs, and a version that
 * assumed a width would silently process eight words where the guest asked for
 * four.
 */
typedef struct X86pVec {
  uint8_t b[16];
  unsigned bytes;
} X86pVec;

static inline uint8_t vec_u8(const X86pVec *v, unsigned i) {
  return v->b[i];
}

static inline uint16_t vec_u16(const X86pVec *v, unsigned i) {
  uint16_t x;
  memcpy(&x, v->b + i * 2u, sizeof x);
  return x;
}

static inline uint32_t vec_u32(const X86pVec *v, unsigned i) {
  uint32_t x;
  memcpy(&x, v->b + i * 4u, sizeof x);
  return x;
}

static inline uint64_t vec_u64(const X86pVec *v, unsigned i) {
  uint64_t x;
  memcpy(&x, v->b + i * 8u, sizeof x);
  return x;
}

static inline float vec_f32(const X86pVec *v, unsigned i) {
  float x;
  memcpy(&x, v->b + i * 4u, sizeof x);
  return x;
}

static inline void vec_set_u8(X86pVec *v, unsigned i, uint8_t x) {
  v->b[i] = x;
}

static inline void vec_set_u16(X86pVec *v, unsigned i, uint16_t x) {
  memcpy(v->b + i * 2u, &x, sizeof x);
}

static inline void vec_set_u32(X86pVec *v, unsigned i, uint32_t x) {
  memcpy(v->b + i * 4u, &x, sizeof x);
}

static inline void vec_set_u64(X86pVec *v, unsigned i, uint64_t x) {
  memcpy(v->b + i * 8u, &x, sizeof x);
}

static inline void vec_set_f32(X86pVec *v, unsigned i, float x) {
  memcpy(v->b + i * 4u, &x, sizeof x);
}

/*
 * The integer lane operations. `a` is the destination's current value and `b`
 * the source; the result is written to `out`, which may alias either.
 *
 * Returns 0 for an operation this file does not implement, so the dispatcher
 * refuses by name rather than leaving `out` at whatever it held.
 */
int x86p_simd_int(X86pSimdOp op, const X86pVec *a, const X86pVec *b, uint8_t imm, X86pVec *out);

/*
 * The single-precision lane operations.
 *
 * `flags` is written only by the two that write flags -- COMISS and UCOMISS,
 * which set ZF, PF and CF and clear OF, SF and AF -- and is left alone by
 * every other. Passing it in rather than returning a comparison result keeps
 * the one instruction that has an EFLAGS side effect from being special-cased
 * at the call site.
 */
int x86p_simd_float(X86pSimdOp op, const X86pVec *a, const X86pVec *b, uint8_t imm, X86pVec *out, X86pFlags *flags);

#endif /* X86PORT_SIMD_INTERNAL_H */
