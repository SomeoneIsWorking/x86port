/* x87.c -- see x87.h for why ST(i) is a position and not a register. */
#include "x87.h"

#include "x87_stack.h"

#include "x87_binary128.h"
#include "x87_ext80_arith.h"
#include "x87_ext80_narrow.h"
#include "x87_ext80_widen.h"
#include "x87_op_census.h"
#include "x87_softfloat.h"

#include <fenv.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

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
  /* This describes the object layout used by raw-register access, not whether
     software operations can preserve ext80 values in a wider host type. */
  return X86P_EXACT_LONG_DOUBLE;
}

int x86p_x87_values_are_supported(void) {
  /* IEEE binary128 contains every finite ext80 value. Arithmetic/conversions
   * on that host pass through the software ext80 owner, which rounds once at
   * the guest precision; native binary128 arithmetic is never the guest FPU. */
#if LDBL_MANT_DIG == 113 && LDBL_MAX_EXP == 16384
  return 1;
#else
  return X86P_EXACT_LONG_DOUBLE;
#endif
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

/*
 * The edge between the storage type and `long double`. On a native ext80 host
 * these are identities the compiler removes; on a binary128 host they are the
 * exact reassembly in x87_f128_ext80.c, with the general softfloat conversion
 * behind it for the encodings that reassembly refuses.
 */
#if X86P_X87_BINARY128
/* x86p_x87_reg_from_long_double and x86p_x87_reg_to_long_double live in
   x87_softfloat.cpp on this host, because the conversion they need is the one
   widen()/narrow() already own: the exact field reassembly first, the general
   softfloat conversion only for the encodings it refuses. Reimplementing them
   here through x86p_x87_to_f80 cost 7% of the wrapper path, measured, because
   that route always takes the general conversion. */

X86pX87Reg x86p_x87_reg_from_f80(const uint8_t bytes[10]) {
  X86pX87Reg r;
  memcpy(&r.signif, bytes, 8);
  r.sign_exp = (uint16_t)((uint16_t)bytes[8] | ((uint16_t)bytes[9] << 8));
  return r;
}

void x86p_x87_reg_to_f80(X86pX87Reg v, uint8_t bytes[10]) {
  memcpy(bytes, &v.signif, 8);
  bytes[8] = (uint8_t)(v.sign_exp & 0xFFu);
  bytes[9] = (uint8_t)(v.sign_exp >> 8);
}
#else
X86pX87Reg x86p_x87_reg_from_long_double(long double v) {
  return x86p_x87_reg_of(v);
}

long double x86p_x87_reg_to_long_double(X86pX87Reg v) {
  return x86p_x87_long_double_of(v);
}

X86pX87Reg x86p_x87_reg_from_f80(const uint8_t bytes[10]) {
  return x86p_x87_from_f80(bytes);
}

void x86p_x87_reg_to_f80(X86pX87Reg v, uint8_t bytes[10]) {
  x86p_x87_to_f80(v, bytes);
}
#endif

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
#if LDBL_MANT_DIG == 113 && LDBL_MAX_EXP == 16384
  value = x86p_x87_software_constant(f ? f->control : X86P_X87_CW_INIT, value);
#endif
  return x86p_x87_push(f, value);
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

/*
 * THE SAME OPERATION, WITHOUT MOVING EITHER OPERAND THROUGH MEMORY.
 *
 * The sequence above names its operands with "m" constraints, so both go out
 * to the stack as ten-byte `fstpt` stores and come straight back as `fldt`
 * loads -- and a ten-byte access cannot be forwarded from the store buffer, so
 * each pair is a stall of about fifteen cycles. Measured on the game's Dead
 * Zone route, `x86p_x87_arith_raw` was 15.2% of the run and its two hottest
 * instructions were the loads after those stores.
 *
 * WHEN THE HOST CONTROL WORD ALREADY IS THE GUEST'S, loading it and restoring
 * it are both no-ops -- so the operation is exactly what the C operator
 * compiles to, which is the same instruction with its operands left in x87
 * registers. This is not an approximation of the sequence above and has no
 * precondition on the VALUES: the rounding mode, the precision and the six
 * exception masks are the same bits, so the result and the flags it raises are
 * the same. Measured over 20,000,000 multiplies: 12.2 ns through the sequence
 * above, 4.8 ns through this one.
 *
 * It is worth testing for because a guest asks for its own mode and then keeps
 * it. Measured over 98,792,279 operations on that route, X-Men Legends II runs
 * every one at 64-bit precision and 98.3% of them at round-to-nearest -- which
 * is this host's own default state, so the test succeeds far more often than
 * it fails. When it fails, the sequence above runs unchanged.
 */
static long double host_arith(X86pX87Op op, long double x, long double y, uint16_t cw) {
  uint16_t host;
  __asm__ volatile("fnstcw %0" : "=m"(host));
  if (host == cw) {
    switch (op) {
    case kX86pX87Add:
      return x + y;
    case kX86pX87Sub:
      return x - y;
    case kX86pX87Mul:
      return x * y;
    case kX86pX87Div:
    default:
      return x / y;
    }
  }
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

#if X86P_X87_BINARY128
/* The divide-by-zero test, asked of the architectural fields rather than of a
   binary128 value. Each comparison it replaces was a compiler-rt call. */
static int reg_is_zero(X86pX87Reg v) {
  return (v.sign_exp & 0x7FFFu) == 0u && v.signif == 0u;
}

static int reg_is_nan(X86pX87Reg v) {
  /* Exponent all ones: infinity has the explicit integer bit and nothing
     below it, so anything else with that exponent is a NaN. */
  return (v.sign_exp & 0x7FFFu) == 0x7FFFu && (v.signif << 1) != 0u;
}
#endif

#if X86P_X87_BINARY128
/*
 * The storage holds ten bytes of architectural state in a sixteen-byte object,
 * and the six between them are padding this file never reads.
 *
 * Something else does. The register file is compared as memory -- the WASM
 * differential does one memcmp of the whole X86pX87, which is what lets it
 * catch a tag, a TOP or a control word that the value comparison would miss --
 * so a register whose padding was never written makes two runs that agree
 * about every number differ anyway. Leaving it uninitialised failed FLD32 and
 * FLD64 there, five cases each, while the conversion itself was right to the
 * bit. Zeroing first is what the softfloat wrapper this replaces did, and the
 * two stores that follow leave one 16-byte zero behind.
 */
static X86pX87Reg reg_of_ext80(X86pExt80 wide) {
  X86pX87Reg out;
  memset(&out, 0, sizeof out);
  out.signif = wide.signif;
  out.sign_exp = wide.sign_exp;
  return out;
}
#endif

void x86p_x87_set_op_census(X86pX87 *f, X86pX87OpCensus *census) {
  if (f) {
    f->op_census = census;
  }
}

/*
 * The register file's fields in the format the rules take, and back again.
 * See x87_ext80_arith.h for why these exist rather than another layer over
 * x86p_x87_get_raw and x86p_x87_set_raw.
 */
#if X86P_X87_BINARY128

int x86p_x87_ext80_of_st(const X86pX87 *f, int i, X86pExt80 *out) {
  int p;
  if (!f || !out || i < 0 || i >= X86P_X87_REGS) {
    return 0;
  }
  p = x86p_x87_phys(f, i);
  if (f->tag[p] == (uint8_t)kX86pX87TagEmpty) {
    return 0;
  }
  out->signif = f->reg[p].signif;
  out->sign_exp = f->reg[p].sign_exp;
  return 1;
}

int x86p_x87_arith_ext80_fast(X86pX87 *f, X86pX87Op op, int dst, X86pExt80 src, int reverse) {
  X86pExt80 a;
  X86pExt80 x;
  X86pExt80 y;
  X86pExt80 r;
  uint16_t raised = 0u;
  int answered;
  int p;
  if (!x86p_x87_ext80_of_st(f, dst, &a)) {
    return 0;
  }
  /* `reverse` swaps the operands and nothing else, exactly as the long form
     does; these are two 16-byte values rather than two registers. */
  x = reverse ? src : a;
  y = reverse ? a : src;
  /* The census lives on both paths or on neither: an operation this one
     answers never reaches x86p_x87_arith_raw, so counting only there would
     make the instrument quietly under-report the moment this landed. */
  if (f->op_census) {
    x86p_x87_census_note_ext80(f, op, x, y);
  }
  if (op == kX86pX87Mul) {
    answered = x86p_ext80_mul_ordinary(f->control, x, y, &r, &raised);
  } else if (op == kX86pX87Add || op == kX86pX87Sub) {
    answered = x86p_ext80_add_ordinary(f->control, x, y, op == kX86pX87Sub, &r, &raised);
  } else {
    return 0;
  }
  if (!answered) {
    return 0;
  }
  /*
   * The whole 16-byte object is written, not just the two fields. The register
   * file is compared as memory -- the WASM differential does one memcmp of the
   * X86pX87 -- so a write that left the six padding bytes alone would differ
   * from the long path on bytes no value depends on. This is the same store
   * reg_of_ext80 makes, and jit_wasm_x87_load.c's emitted one.
   */
  p = x86p_x87_phys(f, dst);
  f->reg[p] = reg_of_ext80(r);
  f->tag[p] = (uint8_t)kX86pX87TagValid;
  f->status |= raised;
  return 1;
}

#else

int x86p_x87_ext80_of_st(const X86pX87 *f, int i, X86pExt80 *out) {
  /* The register file is the host's own long double here, so its fields are
     not this encoding and there is nothing to read in place. */
  (void)f;
  (void)i;
  (void)out;
  return 0;
}

int x86p_x87_arith_ext80_fast(X86pX87 *f, X86pX87Op op, int dst, X86pExt80 src, int reverse) {
  (void)f;
  (void)op;
  (void)dst;
  (void)src;
  (void)reverse;
  return 0;
}

#endif /* X86P_X87_BINARY128 */

int x86p_x87_arith_raw(X86pX87 *f, X86pX87Op op, int dst, X86pX87Reg src, int reverse) {
  X86pX87Reg a, r;
  if (!f || !x86p_x87_read(f, dst, &a)) {
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
    X86pX87Reg x = reverse ? src : a;
    X86pX87Reg y = reverse ? a : src;
    if ((unsigned)op >= (unsigned)kX86pX87OpCount) {
      return 0;
    }
    if (f->op_census) {
      x86p_x87_census_note_reg(f, op, x, y);
    }
#if X86P_X87_BINARY128
    const int divide_by_zero = op == kX86pX87Div && reg_is_zero(y) && !reg_is_zero(x) && !reg_is_nan(x);
#else
    const int divide_by_zero = op == kX86pX87Div && y == 0.0L && x != 0.0L && !isnan(x);
#endif
    if (divide_by_zero) {
      /* Divide by zero is a named condition with a defined result -- a signed
         infinity -- not an error to refuse. The guest's own handler is masked
         by default and expects the infinity. */
      f->status |= X86P_X87_ZE;
    }
#if X86P_X87_BINARY128
    {
      /*
       * The ordinary cases -- two operands that are each a normal or a zero,
       * round to nearest, 80-bit precision, a normal or zero result -- as
       * integer work on the encoding. On the game's Dead Zone route that is
       * 99% of the arithmetic performed: measured, by the op census (x87_op_census.c),
       * which asks these same rules rather than guessing at their
       * preconditions.
       *
       * A refusal falls through to exactly the softfloat that ran before, so
       * this decides speed and never an answer. The rules report the status
       * bits themselves -- inexact and C1 -- because a fast path with the
       * right number and the wrong status word would be wrong in the one place
       * a guest can see a rounding.
       */
      X86pExt80 ex;
      X86pExt80 ey;
      X86pExt80 er;
      uint16_t raised = 0u;
      int answered = 0;
      ex.signif = x.signif;
      ex.sign_exp = x.sign_exp;
      ey.signif = y.signif;
      ey.sign_exp = y.sign_exp;
      if (!divide_by_zero) {
        if (op == kX86pX87Mul) {
          answered = x86p_ext80_mul_ordinary(f->control, ex, ey, &er, &raised);
        } else if (op == kX86pX87Add || op == kX86pX87Sub) {
          answered = x86p_ext80_add_ordinary(f->control, ex, ey, op == kX86pX87Sub, &er, &raised);
        }
      }
      if (answered) {
        r = reg_of_ext80(er);
        f->status |= raised;
      } else {
        /* Straight into the softfloat in the format it takes, and straight
           back out. Nothing here widens to binary128: that round trip was 38%
           of this path, measured by tests/bench_x87_arith.cpp. */
        uint16_t status = 0;
        r = x86p_x87_software_arith_raw(f->control, op, x, y, &status);
        f->status |= status;
      }
    }
#elif defined(X86P_X87_HOST_FPU)
    /* One rounding, at the guest's precision, on the unit that defines it. */
    r = host_arith(op, x, y, f->control);
#else
    /* No x87 unit here: two roundings, a measured 1.18% divergence at narrowed
       precision, named by x86p_x87_precision_is_exact() rather than passed off
       as parity. */
    r = x86p_x87_arith_portable(f->control, op, x, y);
#endif
  }
  return x86p_x87_write(f, dst, r);
}

int x86p_x87_arith(X86pX87 *f, X86pX87Op op, int dst, long double src, int reverse) {
  return x86p_x87_arith_raw(f, op, dst, x86p_x87_reg_of(src), reverse);
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
  /* Every FCOM/FUCOM/FICOM form clears C1 as well; a C1 an earlier FSQRT or
     FPREM left must not survive a comparison. */
  f->status &= (uint16_t)~(X86P_X87_C0 | X86P_X87_C1 | X86P_X87_C2 | X86P_X87_C3);
#ifdef X86P_X87_BINARY128
  {
    const X86pF128 x = x86p_f128_of(a), y = x86p_f128_of(other);
    int order;
    if (x86p_f128_is_nan(x) || x86p_f128_is_nan(y)) {
      f->status |= X86P_X87_C0 | X86P_X87_C2 | X86P_X87_C3;
      f->status |= X86P_X87_IE;
      return 1;
    }
    order = x86p_f128_compare(x, y);
    if (order < 0) {
      f->status |= X86P_X87_C0;
    } else if (order == 0) {
      f->status |= X86P_X87_C3;
    }
    return 1;
  }
#else
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
#endif
}

/* ---- formats ------------------------------------------------------------ */

/*
 * Guest memory bits <-> the register's storage, without the detour through the
 * host's widest float. On a native ext80 host the storage IS that type, so
 * these are the existing conversions; on a binary128 host they go straight to
 * and from the ext80 the softfloat already works in.
 */
/*
 * Both directions widen EXACTLY -- every binary32 and binary64 value has an
 * ext80 with the same number in it -- so neither consults the control word nor
 * can raise anything. They had been calling the general softfloat conversion,
 * which exists to make the rounding decision this direction does not have, and
 * that was 4.95% of the browser's guest worker. x87_ext80_widen owns the
 * reassembly now, and tests/test_x87_ext80_widen.cpp holds it to the host
 * x87's own answer over every subnormal and a sweep of both spaces.
 */

X86pX87Reg x86p_x87_reg_from_f32_bits(uint32_t bits) {
#if X86P_X87_BINARY128
  return reg_of_ext80(x86p_ext80_from_f32_bits(bits));
#else
  return x86p_x87_from_f32(bits);
#endif
}

X86pX87Reg x86p_x87_reg_from_f64_bits(uint64_t bits) {
#if X86P_X87_BINARY128
  return reg_of_ext80(x86p_ext80_from_f64_bits(bits));
#else
  return x86p_x87_from_f64(bits);
#endif
}

/*
 * The ordinary case of a float store, without the softfloat.
 *
 * x87_ext80_narrow.h holds what "ordinary" is and why it is worth separating.
 * A refusal falls through to exactly the conversion that ran before, so this
 * decides speed and never an answer: tests/test_x87_ext80_narrow.cpp checks
 * the fast path against the conversion it bypasses, and tests/test_x87_narrow.c
 * checks this entry point itself under all four rounding modes.
 */
#if X86P_X87_BINARY128
static int narrow_ordinary(uint16_t control, X86pX87Reg v, unsigned width, uint64_t *out) {
  X86pExt80 wide;
  if ((control & X86P_X87_RC_MASK) != X86P_X87_RC_NEAREST) {
    return 0;
  }
  wide.signif = v.signif;
  wide.sign_exp = v.sign_exp;
  return x86p_ext80_narrow_nearest(wide, width, out);
}
#endif

uint64_t x86p_x87_reg_to_f32_bits(const X86pX87 *f, X86pX87Reg v) {
#if X86P_X87_BINARY128
  const uint16_t control = f ? f->control : (uint16_t)X86P_X87_CW_INIT;
  uint64_t bits;
  if (narrow_ordinary(control, v, 4u, &bits)) {
    return bits;
  }
  return x86p_x87_software_narrow_raw(control, v, 0);
#else
  return x86p_x87_to_f32(f, v);
#endif
}

uint64_t x86p_x87_reg_to_f64_bits(const X86pX87 *f, X86pX87Reg v) {
#if X86P_X87_BINARY128
  const uint16_t control = f ? f->control : (uint16_t)X86P_X87_CW_INIT;
  uint64_t bits;
  if (narrow_ordinary(control, v, 8u, &bits)) {
    return bits;
  }
  return x86p_x87_software_narrow_raw(control, v, 1);
#else
  return x86p_x87_to_f64(f, v);
#endif
}

long double x86p_x87_from_f32(uint32_t bits) {
#ifdef X86P_X87_BINARY128
  /* Exact, and without __extendsftf2. See x87_binary128.h. */
  return x86p_f128_value(x86p_f128_from_f32(bits));
#else
  float v;
  memcpy(&v, &bits, sizeof v);
  return (long double)v;
#endif
}

long double x86p_x87_from_f64(uint64_t bits) {
#ifdef X86P_X87_BINARY128
  return x86p_f128_value(x86p_f128_from_f64(bits));
#else
  double v;
  memcpy(&v, &bits, sizeof v);
  return (long double)v;
#endif
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
#elif LDBL_MANT_DIG == 113 && LDBL_MAX_EXP == 16384
static uint64_t host_narrow(long double v, uint16_t cw, int is64) {
  return x86p_x87_software_narrow(cw, v, is64);
}
#else
static uint64_t host_narrow(long double v, uint16_t cw, int is64) {
#pragma STDC FENV_ACCESS ON
  uint64_t bits = 0;
#if LDBL_MANT_DIG == DBL_MANT_DIG && LDBL_MAX_EXP == DBL_MAX_EXP
  /* This host already stores the guest value as binary64. Copying its bits
     needs no conversion and cannot depend on the requested rounding mode. */
  if (is64) {
    const double out = (double)v;
    memcpy(&bits, &out, sizeof out);
    return bits;
  }
#endif
  const int save = fegetround();
  int rounding;
  switch (cw & X86P_X87_RC_MASK) {
  case X86P_X87_RC_DOWN:
    rounding = FE_DOWNWARD;
    break;
  case X86P_X87_RC_UP:
    rounding = FE_UPWARD;
    break;
  case X86P_X87_RC_TRUNCATE:
    rounding = FE_TOWARDZERO;
    break;
  default:
    rounding = FE_TONEAREST;
    break;
  }
  /* Keep host state unchanged when it already matches. Never cache it: host
     services and another guest context can change the thread's environment. */
  if (rounding != save) {
    fesetround(rounding);
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
  if (rounding != save) {
    fesetround(save);
  }
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
#if LDBL_MANT_DIG == 113 && LDBL_MAX_EXP == 16384
  return x86p_x87_software_decode(bytes);
#elif X86P_EXACT_LONG_DOUBLE
  /* Read from the caller's object, which is what `value` points at. */
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
#if LDBL_MANT_DIG == 113 && LDBL_MAX_EXP == 16384
  x86p_x87_software_encode(v, bytes);
#elif X86P_EXACT_LONG_DOUBLE
  /* Read from the caller's object, which is what `value` points at. */
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
#if LDBL_MANT_DIG == 113 && LDBL_MAX_EXP == 16384
  return f && x86p_x87_software_integer(f->control, v, width_bytes, out);
#else
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
#endif
}
