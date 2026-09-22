/*
 * x87_stack.c -- the eight registers, their tags, and TOP.
 *
 * What a register HOLDS and whether it holds anything: reading ST(i), writing
 * it, pushing, popping, and the tag every write derives. The arithmetic, the
 * status word, the control word, the memory formats and the transcendentals
 * are elsewhere -- this file is the file, and nothing in it computes a value.
 *
 * Both widths live here together on purpose. The `long double` entry points
 * are thin wrappers over the storage-typed ones, so there is ONE stack
 * discipline and ONE tag classifier however a host stores a register.
 */
#include "x87_stack.h"

#include "x87_binary128.h"

#include <math.h>
#include <string.h>

/*
 * THE TAG, FROM THE ARCHITECTURAL FIELDS. One rule, whatever the storage is.
 *
 * An exponent of all ones is an infinity or a NaN; a zero exponent with a zero
 * significand is a zero; everything else -- including a subnormal, an unnormal
 * and a pseudo-denormal -- is what the tag word calls valid. That is what the
 * hardware tags, and asking it of the fields also classifies the unsupported
 * encodings the way the hardware does, which `isnan`/`isinf` do not promise.
 */
static uint8_t classify_fields(uint64_t significand, uint16_t sign_exponent) {
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
 * BY ADDRESS, not by value. The caller already has the value in memory -- the
 * System V ABI passes a `long double` argument there, and the register file is
 * memory -- so taking its address lets the fields be read with two integer
 * loads from where it already is. Taking it by value made the compiler spill
 * it a SECOND time with `fstpt` purely to have somewhere to read bytes from,
 * and a ten-byte store cannot forward to the loads that follow it: that spill
 * and its two loads were 46% of x86p_x87_push, measured.
 */
static uint8_t classify(const X86pX87Reg *value) {
#if X86P_X87_BINARY128
  /* The storage already holds those fields. This runs on every write to a
     register -- on the binary128 form the arithmetic phrasing below was 9.5%
     of a profiled Android frame, because each comparison was a compiler-rt
     call. Here it is three integer tests. */
  return classify_fields(value->signif, value->sign_exp);
#elif X86P_EXACT_LONG_DOUBLE
  /*
   * The same three tests, on a host whose `long double` IS the ten-byte x87
   * object: significand in bytes 0-7, sign and exponent in bytes 8-9, read
   * where the caller already has them.
   *
   * The arithmetic phrasing below costs more than it looks. `v == 0.0L`,
   * `isnan` and `isinf` are three x87 compares, so the value has to be in the
   * FPU and the answer branched on. Measured: x86p_x87_push and
   * x86p_x87_set were 9.8% of a profiled Dead Zone gameplay frame.
   */
  uint64_t significand;
  uint16_t sign_exponent;
  memcpy(&significand, (const unsigned char *)value, 8);
  memcpy(&sign_exponent, (const unsigned char *)value + 8, 2);
  return classify_fields(significand, sign_exponent);
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
int x86p_x87_depth(const X86pX87 *f) {
  int i, n = 0;
  if (!f) {
    return 0;
  }
  for (i = 0; i < X86P_X87_REGS; i++) {
    if (f->tag[i] != (uint8_t)kX86pX87TagEmpty) {
      n++;
    }
  }
  return n;
}
int x86p_x87_get_raw(const X86pX87 *f, int i, X86pX87Reg *out) {
  int p;
  if (!f || !out || i < 0 || i >= X86P_X87_REGS) {
    return 0;
  }
  p = x86p_x87_phys(f, i);
  if (f->tag[p] == (uint8_t)kX86pX87TagEmpty) {
    return 0; /* reading an empty register is a fact, not a zero */
  }
  *out = f->reg[p];
  return 1;
}

int x86p_x87_set_raw(X86pX87 *f, int i, X86pX87Reg v) {
  int p;
  if (!f || i < 0 || i >= X86P_X87_REGS) {
    return 0;
  }
  p = x86p_x87_phys(f, i);
  f->reg[p] = v;
  f->tag[p] = classify(&v);
  return 1;
}

int x86p_x87_get(const X86pX87 *f, int i, long double *out) {
  X86pX87Reg raw;
  if (!out || !x86p_x87_get_raw(f, i, &raw)) {
    return 0;
  }
  *out = x86p_x87_reg_to_long_double(raw);
  return 1;
}

int x86p_x87_set(X86pX87 *f, int i, long double v) {
  return x86p_x87_set_raw(f, i, x86p_x87_reg_from_long_double(v));
}

int x86p_x87_push_raw(X86pX87 *f, X86pX87Reg v) {
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
  f->reg[p] = v;
  f->tag[p] = classify(&v);
  return 1;
}

int x86p_x87_push(X86pX87 *f, long double v) {
  return x86p_x87_push_raw(f, x86p_x87_reg_from_long_double(v));
}
int x86p_x87_pop_raw(X86pX87 *f, X86pX87Reg *out) {
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

int x86p_x87_pop(X86pX87 *f, long double *out) {
  X86pX87Reg raw;
  if (!x86p_x87_pop_raw(f, &raw)) {
    return 0;
  }
  if (out) {
    *out = x86p_x87_reg_to_long_double(raw);
  }
  return 1;
}
