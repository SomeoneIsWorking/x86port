/* x87.c -- see x87.h for why ST(i) is a position and not a register. */
#include "x87.h"

#include <fenv.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#if LDBL_MANT_DIG == 64 && LDBL_MAX_EXP == 16384
#define X86P_EXACT_LONG_DOUBLE 1
#else
#define X86P_EXACT_LONG_DOUBLE 0
#endif

static const char *kOpNames[] = {"add", "sub", "mul", "div"};
_Static_assert((int)(sizeof kOpNames / sizeof kOpNames[0]) == (int)kX86pX87OpCount, "every X86pX87Op needs a name");

static const char *kInsnNames[] = {"fld",    "fild",   "fst",   "fist",   "farith", "fcom",  "fxch",   "fchs",
                                   "fabs",   "fldz",   "fld1",  "fldpi",  "fnstsw", "fldcw", "fnstcw", "ffree",
                                   "fninit", "fn",     "fwait", "fnclex", "ftst",   "fcomi", "fldl2e", "fldl2t",
                                   "fldln2", "fldlg2", "fcmov", "fnsave", "frstor"};
_Static_assert((int)(sizeof kInsnNames / sizeof kInsnNames[0]) == (int)kX86pX87InsnCount,
               "every X86pX87Insn needs a name -- a refusal that cannot name itself is not a report");

const char *x86p_x87_insn_name(X86pX87Insn insn) {
  if ((unsigned)insn >= (unsigned)kX86pX87InsnCount) {
    return "unknown";
  }
  return kInsnNames[(int)insn];
}

const char *x86p_x87_op_name(X86pX87Op op) {
  if ((unsigned)op >= (unsigned)kX86pX87OpCount) {
    return "unknown";
  }
  return kOpNames[(int)op];
}

int x86p_x87_precision_is_exact(void) {
  /* 64 mantissa bits and a 15-bit exponent is x87's extended format exactly.
     Anything else -- quad, or a 53-bit double -- computes different low bits
     for values that are not exactly representable. */
  return X86P_EXACT_LONG_DOUBLE;
}

/*
 * The x87 extended format in memory, on a little-endian host: eight bytes of
 * mantissa then two of sign-and-exponent. memcpy rather than a cast through a
 * union of long double and bytes, because the object is ten significant bytes
 * inside a sixteen-byte allocation and only the ten mean anything.
 */
#define X87_MANTISSA_BYTES 8u
#define X87_SIGN_EXP_OFFSET 8u

int x86p_x87_mmx_read(const X86pX87 *f, int n, uint64_t *out) {
#if X86P_EXACT_LONG_DOUBLE
  uint64_t v = 0u;
  if (!f || !out || n < 0 || n >= X86P_X87_REGS) {
    return 0;
  }
  memcpy(&v, &f->reg[n], X87_MANTISSA_BYTES);
  *out = v;
  return 1;
#else
  (void)f;
  (void)n;
  (void)out;
  return 0;
#endif
}

int x86p_x87_mmx_write(X86pX87 *f, int n, uint64_t v) {
#if X86P_EXACT_LONG_DOUBLE
  uint16_t sign_exp = 0xFFFFu;
  int i;
  if (!f || n < 0 || n >= X86P_X87_REGS) {
    return 0;
  }
  memcpy(&f->reg[n], &v, X87_MANTISSA_BYTES);
  memcpy((uint8_t *)&f->reg[n] + X87_SIGN_EXP_OFFSET, &sign_exp, sizeof sign_exp);
  /* Any MMX write marks the WHOLE file valid, not just the register written.
     That is what the hardware does, and it is why a guest must EMMS before
     going back to x87 rather than merely avoiding the register it used. */
  for (i = 0; i < X86P_X87_REGS; i++) {
    f->tag[i] = (uint8_t)kX86pX87TagValid;
  }
  return 1;
#else
  (void)f;
  (void)n;
  (void)v;
  return 0;
#endif
}

void x86p_x87_emms(X86pX87 *f) {
  int i;
  if (!f) {
    return;
  }
  for (i = 0; i < X86P_X87_REGS; i++) {
    f->tag[i] = (uint8_t)kX86pX87TagEmpty;
  }
}

void x86p_x87_reset(X86pX87 *f) {
  int i;
  if (!f) {
    return;
  }
  memset(f, 0, sizeof *f);
  for (i = 0; i < X86P_X87_REGS; i++) {
    f->tag[i] = (uint8_t)kX86pX87TagEmpty;
  }
  f->control = X86P_X87_CW_INIT;
}

/* ST(i) -> physical register. The whole point of the module in one line. */
static int phys(const X86pX87 *f, int i) {
  return (f->top + i) & (X86P_X87_REGS - 1);
}

static uint8_t classify(long double v) {
  if (v == 0.0L) {
    return (uint8_t)kX86pX87TagZero;
  }
  if (isnan(v) || isinf(v)) {
    return (uint8_t)kX86pX87TagSpecial;
  }
  return (uint8_t)kX86pX87TagValid;
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

uint16_t x86p_x87_status(const X86pX87 *f) {
  if (!f) {
    return 0;
  }
  /* TOP is part of the word a guest reads. Omitting it is invisible until
     something switches on the whole status word rather than on C0-C3. */
  return (uint16_t)((f->status & ~(7u << X86P_X87_TOP_SHIFT)) | ((uint16_t)(f->top & 7u) << X86P_X87_TOP_SHIFT));
}

void x86p_x87_clear_exceptions(X86pX87 *f) {
  if (!f) {
    return;
  }
  f->status &= (uint16_t)~X86P_X87_FNCLEX_MASK;
}

int x86p_x87_get(const X86pX87 *f, int i, long double *out) {
  int p;
  if (!f || !out || i < 0 || i >= X86P_X87_REGS) {
    return 0;
  }
  p = phys(f, i);
  if (f->tag[p] == (uint8_t)kX86pX87TagEmpty) {
    return 0; /* reading an empty register is a fact, not a zero */
  }
  *out = f->reg[p];
  return 1;
}

int x86p_x87_set(X86pX87 *f, int i, long double v) {
  int p;
  if (!f || i < 0 || i >= X86P_X87_REGS) {
    return 0;
  }
  p = phys(f, i);
  f->reg[p] = v;
  f->tag[p] = classify(v);
  return 1;
}

int x86p_x87_push(X86pX87 *f, long double v) {
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
  f->tag[p] = classify(v);
  return 1;
}

int x86p_x87_push_constant(X86pX87 *f, X86pX87Insn instruction) {
  long double value;

  switch (instruction) {
  case kX86pX87InsnConstZero:
    value = 0.0L;
    break;
  case kX86pX87InsnConstOne:
    value = 1.0L;
    break;
  case kX86pX87InsnConstPi:
    value = 3.14159265358979323846264338327950288L;
    break;
  case kX86pX87InsnConstLog2E:
    value = 1.44269504088896340735992468100189214L;
    break;
  case kX86pX87InsnConstLog2T:
    value = 3.32192809488736234787031942948939018L;
    break;
  case kX86pX87InsnConstLn2:
    value = 0.693147180559945309417232121458176568L;
    break;
  case kX86pX87InsnConstLog102:
    value = 0.301029995663981195213738894724493027L;
    break;
  default:
    return 0;
  }
  return x86p_x87_push(f, value);
}

int x86p_x87_pop(X86pX87 *f, long double *out) {
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

/*
 * Round an integral-valued long double under the current rounding control.
 * Written out rather than delegated to nearbyintl, because that follows the
 * HOST's rounding mode -- which is not the guest's, and which this framework
 * must not be changing behind the host program's back.
 */
static long double round_to_integer(uint16_t control, long double x) {
  long double r = truncl(x);
  long double d = x - r;
  int negative = (x < 0.0L);
  if (d == 0.0L) {
    return r;
  }
  switch (control & X86P_X87_RC_MASK) {
  case X86P_X87_RC_TRUNCATE:
    return r;
  case X86P_X87_RC_DOWN:
    return negative ? r - 1.0L : r;
  case X86P_X87_RC_UP:
    return negative ? r : r + 1.0L;
  default: /* nearest, TIES TO EVEN -- not "away from zero", which is the
              rounding people expect and the hardware does not do */
    if (d > 0.5L || d < -0.5L) {
      return negative ? r - 1.0L : r + 1.0L;
    }
    if (d == 0.5L || d == -0.5L) {
      if (fmodl(r, 2.0L) != 0.0L) {
        return negative ? r - 1.0L : r + 1.0L;
      }
    }
    return r;
  }
}

long double x86p_x87_round(const X86pX87 *f, long double v) {
  int bits;
  int e;
  long double m;
  if (!f) {
    return v;
  }
  /*
   * Precision control rounds every RESULT, not just stores. A guest that sets
   * single precision and receives extended-precision results diverges slowly
   * and everywhere, which is the hardest kind of divergence to trace back.
   *
   * IT NARROWS THE MANTISSA AND NOTHING ELSE. Rounding by casting through
   * `float` or `double` -- which is what this did until the hardware sweep at
   * PC=single was run -- also clamps the EXPONENT to that type's range, so a
   * result near 1e300 came back as infinity and one near 1e-300 came back
   * denormalised or zero. The register stays 80 bits wide whatever PC says;
   * only the number of significand bits changes.
   */
  switch (f->control & X86P_X87_PC_MASK) {
  case X86P_X87_PC_SINGLE:
    bits = 24;
    break;
  case X86P_X87_PC_DOUBLE:
    bits = 53;
    break;
  default:
    return v; /* extended: the register's own precision, nothing to do */
  }
  if (v == 0.0L || !isfinite(v)) {
    return v;
  }
  /* Scale so that `bits` significand bits sit in the integer part, round
     there under the guest's rounding control, and scale back. */
  m = frexpl(v, &e);
  m = scalbnl(m, bits);
  m = round_to_integer(f->control, m);
  return scalbnl(m, e - bits);
}

/*
 * THE OPERATION IS PERFORMED AT THE GUEST'S PRECISION, NOT ROUNDED AFTERWARDS.
 *
 * x87 rounds ONCE: the infinitely precise result of the operation is rounded
 * to the precision the control word selects. Computing in extended and then
 * calling x86p_x87_round() rounds TWICE, and the two disagree -- measured over
 * 2,359,296 operations against this host's FPU, 27,930 of them (1.18%) came
 * out different, every single one at a narrowed precision and none at
 * PC=extended, where there is no second rounding to do. FADD(1.0, 2^-16000)
 * with RC=up is the clearest: one rounding gives the next value above 1.0,
 * two give 1.0 exactly.
 *
 * So on an x86 host the guest's control word is loaded into the real FPU and
 * the real instruction is executed. That is not a shortcut around writing the
 * semantics -- it IS the semantics, on the only unit that implements them
 * exactly, and it also gets the denormal and underflow behaviour at narrowed
 * precision right, which the two-step form does not.
 *
 * The host's own control word is restored immediately, because this framework
 * is a library inside someone else's process and must not leave the FPU
 * configured for the guest.
 */
#if (defined(__x86_64__) || defined(__i386__)) && X86P_EXACT_LONG_DOUBLE
#define X86P_X87_HOST_FPU 1

#define HOST_OP(name, insn)                                                                                            \
  static long double name(long double x, long double y, uint16_t cw) {                                                 \
    long double r;                                                                                                     \
    uint16_t saved;                                                                                                    \
    __asm__ volatile("fnstcw %0\n\tfldcw %4\n\tfldt %3\n\tfldt %2\n\t" insn " %%st(1), %%st\n\t"                       \
                     "fstpt %1\n\tfstp %%st(0)\n\tfldcw %0"                                                            \
                     : "=m"(saved), "=m"(r)                                                                            \
                     : "m"(x), "m"(y), "m"(cw)                                                                         \
                     : "st", "st(1)", "memory");                                                                       \
    return r;                                                                                                          \
  }

HOST_OP(host_add, "fadd")
HOST_OP(host_sub, "fsub")
HOST_OP(host_mul, "fmul")
HOST_OP(host_div, "fdiv")

static long double host_arith(X86pX87Op op, long double x, long double y, uint16_t cw) {
  switch (op) {
  case kX86pX87Add:
    return host_add(x, y, cw);
  case kX86pX87Sub:
    return host_sub(x, y, cw);
  case kX86pX87Mul:
    return host_mul(x, y, cw);
  case kX86pX87Div:
  default:
    return host_div(x, y, cw);
  }
}
#endif

long double x86p_x87_arith_portable(uint16_t control, X86pX87Op op, long double x, long double y) {
  X86pX87 scratch;
  long double r;
  switch (op) {
  case kX86pX87Add:
    r = x + y;
    break;
  case kX86pX87Sub:
    r = x - y;
    break;
  case kX86pX87Mul:
    r = x * y;
    break;
  case kX86pX87Div:
    r = x / y;
    break;
  case kX86pX87OpCount:
  default:
    return 0.0L;
  }
  /* Only the control word matters to the rounding, so a bare one is enough --
     and this keeps a single implementation of the narrowing. */
  memset(&scratch, 0, sizeof scratch);
  scratch.control = control;
  return x86p_x87_round(&scratch, r);
}

int x86p_x87_arith(X86pX87 *f, X86pX87Op op, int dst, long double src, int reverse) {
  long double a, r;
  if (!f || !x86p_x87_get(f, dst, &a)) {
    if (f) {
      f->status |= X86P_X87_IE | X86P_X87_SF;
    }
    return 0;
  }
  {
    /* `reverse` swaps the operands and nothing else -- FSUBR and FDIVR differ
       from FSUB and FDIV in operand order alone, and writing them out
       separately is how one of the four acquires a bug the other three do
       not have. */
    long double x = reverse ? src : a;
    long double y = reverse ? a : src;
    if ((unsigned)op >= (unsigned)kX86pX87OpCount) {
      return 0;
    }
    if (op == kX86pX87Div && y == 0.0L && x != 0.0L && !isnan(x)) {
      /* Divide by zero is a named condition with a defined result -- a signed
         infinity -- not an error to refuse. The guest's own handler is masked
         by default and expects the infinity. */
      f->status |= X86P_X87_ZE;
    }
#if defined(X86P_X87_HOST_FPU)
    /* One rounding, at the guest's precision, on the unit that defines it. */
    r = host_arith(op, x, y, f->control);
#else
    /* No x87 unit here: two roundings, a measured 1.18% divergence at narrowed
       precision, named by x86p_x87_precision_is_exact() rather than passed off
       as parity. */
    r = x86p_x87_arith_portable(f->control, op, x, y);
#endif
  }
  return x86p_x87_set(f, dst, r);
}

int x86p_x87_compare(X86pX87 *f, long double other) {
  long double a;
  if (!f) {
    return 0;
  }
  if (!x86p_x87_get(f, 0, &a)) {
    f->status |= X86P_X87_IE | X86P_X87_SF;
    return 0;
  }
  f->status &= (uint16_t)~(X86P_X87_C0 | X86P_X87_C2 | X86P_X87_C3);
  if (isnan(a) || isnan(other)) {
    /* UNORDERED sets all three, which is a distinct outcome from both equal
       and less-than. Guests branch on it, and collapsing it into one of the
       others produces a comparison that is right until a NaN appears. */
    f->status |= X86P_X87_C0 | X86P_X87_C2 | X86P_X87_C3;
    f->status |= X86P_X87_IE;
    return 1;
  }
  if (a > other) {
    /* all three clear */
  } else if (a < other) {
    f->status |= X86P_X87_C0;
  } else {
    f->status |= X86P_X87_C3;
  }
  return 1;
}

/* ---- formats ------------------------------------------------------------ */

long double x86p_x87_from_f32(uint32_t bits) {
  float v;
  memcpy(&v, &bits, sizeof v);
  return (long double)v;
}

long double x86p_x87_from_f64(uint64_t bits) {
  double v;
  memcpy(&v, &bits, sizeof v);
  return (long double)v;
}

/*
 * Narrow an 80-bit value to 32- or 64-bit under the GUEST's rounding control.
 *
 * `FST m32` rounds the significand to single the same way FADD rounds a result:
 * per the RC field of the control word. A plain `(float)v` cast always rounds
 * to nearest-even -- the store-side twin of the truncation bug
 * x86p_x87_to_int documents, right only for values that were already
 * representable. On an x86 host the real FPU does it exactly with the guest
 * control word loaded and the host's restored straight after, because this
 * framework runs inside someone else's process.
 */
#if defined(X86P_X87_HOST_FPU)
static uint64_t host_narrow(long double v, uint16_t cw, int is64) {
  uint16_t saved;
  if (is64) {
    double out;
    uint64_t bits;
    __asm__ volatile("fnstcw %0\n\tfldcw %3\n\tfldt %2\n\tfstpl %1\n\tfldcw %0"
                     : "=m"(saved), "=m"(out)
                     : "m"(v), "m"(cw)
                     : "st", "memory");
    memcpy(&bits, &out, sizeof bits);
    return bits;
  } else {
    float out;
    uint32_t bits;
    __asm__ volatile("fnstcw %0\n\tfldcw %3\n\tfldt %2\n\tfstps %1\n\tfldcw %0"
                     : "=m"(saved), "=m"(out)
                     : "m"(v), "m"(cw)
                     : "st", "memory");
    memcpy(&bits, &out, sizeof bits);
    return bits;
  }
}
#else
static uint64_t host_narrow(long double v, uint16_t cw, int is64) {
  /* No x87 unit: steer the standard rounding direction for the narrowing
     conversion, which C99 Annex F ties to the current mode. Restored after,
     like the host-FPU path. */
  const int save = fegetround();
  uint64_t bits = 0;
  switch (cw & X86P_X87_RC_MASK) {
  case X86P_X87_RC_DOWN:
    fesetround(FE_DOWNWARD);
    break;
  case X86P_X87_RC_UP:
    fesetround(FE_UPWARD);
    break;
  case X86P_X87_RC_TRUNCATE:
    fesetround(FE_TOWARDZERO);
    break;
  default:
    fesetround(FE_TONEAREST);
    break;
  }
  if (is64) {
    double out = (double)v;
    memcpy(&bits, &out, sizeof out);
  } else {
    float out = (float)v;
    uint32_t narrow;
    memcpy(&narrow, &out, sizeof out);
    bits = narrow;
  }
  fesetround(save);
  return bits;
}
#endif

uint32_t x86p_x87_to_f32(const X86pX87 *f, long double v) {
  return (uint32_t)host_narrow(v, f ? f->control : X86P_X87_CW_INIT, 0);
}

uint64_t x86p_x87_to_f64(const X86pX87 *f, long double v) {
  return host_narrow(v, f ? f->control : X86P_X87_CW_INIT, 1);
}

/*
 * The 80-bit format, which is the only one that stores the mantissa's leading
 * bit explicitly: ten bytes, little-endian, 64 mantissa bits then 15 exponent
 * bits then the sign. On a host whose long double IS that format the bytes are
 * the object's own first ten -- but that is asserted rather than assumed, and
 * on any other host it is assembled by hand, because a memcpy of a 16-byte
 * quad would silently store the wrong thing.
 */
long double x86p_x87_from_f80(const uint8_t bytes[10]) {
  if (!bytes) {
    return 0.0L;
  }
#if X86P_EXACT_LONG_DOUBLE
  {
    long double v = 0.0L;
    memcpy(&v, bytes, 10);
    return v;
  }
#else
  {
    /* Decoded arithmetically: sign, exponent, and an explicit mantissa whose
       leading bit is part of the stored value rather than implied. */
    uint64_t mant = 0;
    int i;
    int exp;
    int sign;
    long double v;
    for (i = 7; i >= 0; i--) {
      mant = (mant << 8) | bytes[i];
    }
    exp = ((int)bytes[9] & 0x7F) << 8 | (int)bytes[8];
    sign = (bytes[9] & 0x80) ? -1 : 1;
    if (exp == 0 && mant == 0) {
      return sign < 0 ? -0.0L : 0.0L;
    }
    v = ldexpl((long double)mant, exp - 16383 - 63);
    return sign < 0 ? -v : v;
  }
#endif
}

void x86p_x87_to_f80(long double v, uint8_t bytes[10]) {
  if (!bytes) {
    return;
  }
#if X86P_EXACT_LONG_DOUBLE
  {
    memcpy(bytes, &v, 10);
    return;
  }
#else
  {
    int exp2 = 0;
    long double m = frexpl(v < 0 ? -v : v, &exp2);
    uint64_t mant;
    int e;
    memset(bytes, 0, 10);
    if (v == 0.0L) {
      if (signbit(v)) {
        bytes[9] = 0x80;
      }
      return;
    }
    /* frexp gives [0.5,1); x87 stores [1,2) with the leading bit explicit. */
    mant = (uint64_t)ldexpl(m, 64);
    e = exp2 + 16382;
    memcpy(bytes, &mant, 8);
    bytes[8] = (uint8_t)(e & 0xFF);
    bytes[9] = (uint8_t)(((e >> 8) & 0x7F) | (v < 0 ? 0x80 : 0));
  }
#endif
}

int x86p_x87_to_int(const X86pX87 *f, long double v, int width_bytes, int64_t *out) {
  long double r;
  int64_t lo, hi;
  if (!out || !f) {
    return 0;
  }
  if (isnan(v) || isinf(v)) {
    return 0;
  }
  /*
   * FIST ROUNDS, it does not truncate -- unless the control word says
   * truncate, which is what FISTTP and a compiler's (int) cast arrange by
   * flipping RC. Assuming truncation is the classic x87 porting bug: it is
   * right for every value a test happens to pick that is already an integer.
   */
  switch (f->control & X86P_X87_RC_MASK) {
  case X86P_X87_RC_DOWN:
    r = floorl(v);
    break;
  case X86P_X87_RC_UP:
    r = ceill(v);
    break;
  case X86P_X87_RC_TRUNCATE:
    r = truncl(v);
    break;
  default:
    /* Round to NEAREST, ties to EVEN -- not away from zero. rintl follows the
       host mode, so nearbyintl with the default mode is the honest call. */
    r = nearbyintl(v);
    break;
  }
  switch (width_bytes) {
  case 2:
    lo = -32768;
    hi = 32767;
    break;
  case 4:
    lo = -2147483647LL - 1;
    hi = 2147483647LL;
    break;
  case 8:
    lo = INT64_MIN;
    hi = INT64_MAX;
    break;
  default:
    return 0;
  }
  if (r < (long double)lo || r > (long double)hi) {
    return 0; /* the guest-visible invalid operation, not a wrapped value */
  }
  *out = (int64_t)r;
  return 1;
}
