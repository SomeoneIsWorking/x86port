/*
 * THE REGISTER FILE, INLINE.
 *
 * x87's eight registers are a rotating file: ST(0) is whatever TOP points at,
 * and every access is that one addition. Reading, writing, pushing and popping
 * are a handful of instructions each, and they are on EVERY x87 operation the
 * engine performs -- so the implementations live here and x87_stack.c defines
 * the public `x86p_x87_*` symbols as one-line calls to them.
 *
 * That is not two implementations. It is one, in a header, with external
 * definitions for the callers outside this library. Measured, it matters:
 * moving these out of x87.c into their own translation unit turned the
 * accessors x86p_x87_arith_raw uses on every operation into cross-unit calls,
 * and get_raw, set_raw and the long-double conversions appeared in a gameplay
 * profile as 14.9% between them where they had previously been inlined away.
 */
#ifndef X86PORT_X87_STACK_H
#define X86PORT_X87_STACK_H

#include "x87.h"

#include <math.h>
#include <string.h>

/* ST(i) -> physical register. The whole point of the module in one line. */
static inline int x86p_x87_phys(const X86pX87 *f, int i) {
  return (f->top + i) & (X86P_X87_REGS - 1);
}

/*
 * THE TAG, FROM THE ARCHITECTURAL FIELDS. One rule, whatever the storage is.
 *
 * An exponent of all ones is an infinity or a NaN; a zero exponent with a zero
 * significand is a zero; everything else -- including a subnormal, an unnormal
 * and a pseudo-denormal -- is what the tag word calls valid. That is what the
 * hardware tags, and asking it of the fields also classifies the unsupported
 * encodings the way the hardware does, which `isnan`/`isinf` do not promise.
 */
static inline uint8_t x86p_x87_tag_of_fields(uint64_t significand, uint16_t sign_exponent) {
  const uint16_t exponent = (uint16_t)(sign_exponent & 0x7FFFu);
  if (exponent == 0x7FFFu) {
    return (uint8_t)kX86pX87TagSpecial;
  }
  if (exponent == 0u && significand == 0u) {
    return (uint8_t)kX86pX87TagZero;
  }
  return (uint8_t)kX86pX87TagValid;
}

/*
 * The class of an occupied register, for the tag word x87_state.c writes. The
 * register file itself records only whether a register is occupied: the class
 * is derived here, from the contents, when a guest asks for the word.
 */
static inline uint8_t x86p_x87_tag_of(const X86pX87Reg *value) {
#if X86P_X87_BINARY128
  /* The storage already holds those fields. */
  return x86p_x87_tag_of_fields(value->signif, value->sign_exp);
#elif X86P_EXACT_LONG_DOUBLE
  /* A host whose `long double` IS the ten-byte x87 object: significand in
     bytes 0-7, sign and exponent in bytes 8-9. */
  uint64_t significand;
  uint16_t sign_exponent;
  memcpy(&significand, (const unsigned char *)value, 8);
  memcpy(&sign_exponent, (const unsigned char *)value + 8, 2);
  return x86p_x87_tag_of_fields(significand, sign_exponent);
#else
  /* No exact layout to read: ask the arithmetic. A host whose `long double` is
     a double or a binary128 has no ten-byte object here to take fields from. */
  const X86pX87Reg v = *value;
  if (v == 0.0L) {
    return (uint8_t)kX86pX87TagZero;
  }
  if (isnan(v) || isinf(v)) {
    return (uint8_t)kX86pX87TagSpecial;
  }
  return (uint8_t)kX86pX87TagValid;
#endif
}

/* Reading an empty register is a fact, not a zero: 0, and *out untouched. */
static inline int x86p_x87_read(const X86pX87 *f, int i, X86pX87Reg *out) {
  int p;
  if (!f || !out || i < 0 || i >= X86P_X87_REGS) {
    return 0;
  }
  p = x86p_x87_phys(f, i);
  if (f->tag[p] == (uint8_t)kX86pX87TagEmpty) {
    return 0;
  }
  *out = f->reg[p];
  return 1;
}

/*
 * THE ONE STORE INTO THE REGISTER FILE. On a binary128 host the register is a
 * signif/sign_exp pair in sixteen bytes, and the six between are padding the
 * file keeps zero: it is compared as memory (the WASM differential memcmps the
 * whole X86pX87), and a value's own padding cannot be relied on -- a pair
 * returned by value comes back in RAX:RDX with RDX's upper bits unspecified.
 * So the store writes the fields over a zeroed slot, whatever `v` carried.
 */
static inline void x86p_x87_store_slot(X86pX87Reg *slot, X86pX87Reg v) {
#if X86P_X87_BINARY128
  memset(slot, 0, sizeof *slot);
  slot->signif = v.signif;
  slot->sign_exp = v.sign_exp;
#else
  *slot = v;
#endif
}

static inline int x86p_x87_write(X86pX87 *f, int i, X86pX87Reg v) {
  int p;
  if (!f || i < 0 || i >= X86P_X87_REGS) {
    return 0;
  }
  p = x86p_x87_phys(f, i);
  x86p_x87_store_slot(&f->reg[p], v);
  f->tag[p] = (uint8_t)kX86pX87TagValid;
  return 1;
}

static inline int x86p_x87_push_value(X86pX87 *f, X86pX87Reg v) {
  int p;
  if (!f) {
    return 0;
  }
  p = (f->top - 1) & (X86P_X87_REGS - 1);
  if (f->tag[p] != (uint8_t)kX86pX87TagEmpty) {
    /* STACK OVERFLOW. Reported, because the guest is entitled to find out and
       because silently wrapping TOP produces an engine that drifts from
       hardware with no symptom for thousands of instructions. */
    f->status |= X86P_X87_IE | X86P_X87_SF | X86P_X87_C1;
    return 0;
  }
  f->top = (uint8_t)p;
  x86p_x87_store_slot(&f->reg[p], v);
  f->tag[p] = (uint8_t)kX86pX87TagValid;
  return 1;
}

static inline int x86p_x87_pop_value(X86pX87 *f, X86pX87Reg *out) {
  int p;
  if (!f) {
    return 0;
  }
  p = f->top & (X86P_X87_REGS - 1);
  if (f->tag[p] == (uint8_t)kX86pX87TagEmpty) {
    f->status |= X86P_X87_IE | X86P_X87_SF;
    f->status &= (uint16_t)~X86P_X87_C1; /* C1 clear distinguishes underflow */
    return 0;
  }
  if (out) {
    *out = f->reg[p];
  }
  f->tag[p] = (uint8_t)kX86pX87TagEmpty;
  f->top = (uint8_t)((p + 1) & (X86P_X87_REGS - 1));
  return 1;
}

#if !X86P_X87_BINARY128
/*
 * On a native-ext80 host these are the identity, and they are on every
 * long-double entry point. Out of line they showed up in a gameplay profile as
 * 2.38% spent entering and leaving a function that returns its argument.
 * The binary128 host's versions are real conversions and live in
 * x87_softfloat.cpp.
 */
static inline X86pX87Reg x86p_x87_reg_of(long double v) {
  return v;
}

static inline long double x86p_x87_long_double_of(X86pX87Reg v) {
  return v;
}
#else
#define x86p_x87_reg_of x86p_x87_reg_from_long_double
#define x86p_x87_long_double_of x86p_x87_reg_to_long_double
#endif

#endif /* X86PORT_X87_STACK_H */
