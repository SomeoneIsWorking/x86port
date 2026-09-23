#include "x87_transcendental.h"

#include <float.h>
#include <string.h>

static const char *kNames[] = {"FSQRT",
                               "FSIN",
                               "FCOS",
                               "FSINCOS",
                               "FPTAN",
                               "FPATAN",
                               "FYL2X",
                               "FYL2XP1",
                               "F2XM1",
                               "FSCALE",
                               "FRNDINT",
                               "FXTRACT",
                               "FPREM",
                               "FPREM1",
                               "FABS",
                               "FCHS"};
_Static_assert((int)(sizeof kNames / sizeof kNames[0]) == (int)kX86pX87FnCount, "every X86pX87Fn needs a name");

const char *x86p_x87_fn_name(X86pX87Fn fn) {
  if (fn < 0 || fn >= kX86pX87FnCount) {
    return "?";
  }
  return kNames[fn];
}

#if (defined(__x86_64__) || defined(__i386__)) && LDBL_MANT_DIG == 64 && LDBL_MAX_EXP == 16384
#define HAVE_X87 1
#else
#define HAVE_X87 0
#endif

int x86p_x87_fn_available(void) {
  return 1;
}

#if HAVE_X87
/*
 * Operands cross in the 80-bit format, through memory.
 *
 * FLDT and FSTPT rather than the "t" and "u" register constraints: the
 * constraints describe a stack the compiler is also using, and an instruction
 * that pushes or pops -- which most of these do -- has to be described to it
 * exactly or it allocates around a stack shape that is not the one that
 * happens. Loading and storing explicitly makes each block self-contained, and
 * the cost is irrelevant beside a transcendental.
 *
 * The status word is read with FNSTSW straight after the operation, before
 * the stores: FSTP clears C1, which lost FPREM's quotient bit and FSQRT's
 * rounding direction while the read came last. C1 (which reports rounding direction and stack
 * overflow) and C2 (which reports an incomplete FPREM reduction, and an
 * out-of-range argument to the trigonometric functions) are both things guest
 * code branches on.
 */
#define X87_UNARY(op)                                                                                                  \
  __asm__ volatile("fldt %2\n\t" op "\n\tfnstsw %1\n\tfstpt %0"                                                        \
                   : "=m"(*r0), "=&a"(sw)                                                                              \
                   : "m"(a)                                                                                            \
                   : "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)")

/* Two results, ST(0) then the pushed one. The pushed value ends up ON TOP, so
   it is stored first and the original second. */
#define X87_UNARY2(op)                                                                                                 \
  __asm__ volatile("fldt %3\n\t" op "\n\tfnstsw %2\n\tfstpt %0\n\tfstpt %1"                                            \
                   : "=m"(*r1), "=m"(*r0), "=&a"(sw)                                                                   \
                   : "m"(a)                                                                                            \
                   : "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)")

/* Two operands: ST(1) is loaded first so that ST(0) ends up on top, which is
   the arrangement every two-operand x87 instruction is specified against. */
#define X87_BINARY(op)                                                                                                 \
  __asm__ volatile("fldt %2\n\tfldt %3\n\t" op "\n\tfnstsw %1\n\tfstpt %0"                                             \
                   : "=m"(*r0), "=&a"(sw)                                                                              \
                   : "m"(b), "m"(a)                                                                                    \
                   : "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)")

/* FSCALE and FPREM do NOT pop, so both registers survive and ST(0) is the
   result; the second operand is discarded here because the caller keeps it. */
#define X87_BINARY_NOPOP(op)                                                                                           \
  __asm__ volatile("fldt %2\n\tfldt %3\n\t" op "\n\tfnstsw %1\n\tfstpt %0\n\tfstp %%st(0)"                             \
                   : "=m"(*r0), "=&a"(sw)                                                                              \
                   : "m"(b), "m"(a)                                                                                    \
                   : "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)")
#endif

/*
 * THE GUEST'S CONTROL WORD GOVERNS A TRANSCENDENTAL TOO.
 *
 * The arithmetic path has loaded it for a long time -- x87 rounds once, at the
 * precision the control word selects -- and this path did not, so FYL2X,
 * FSQRT, FSIN and the rest ran at whatever precision and rounding the HOST
 * process happened to be in. The software path beside it has always taken
 * `control`; only the host-FPU arm was left asking a different machine.
 *
 * It is not a rounding curiosity. The value that comes out is usually
 * converted to an integer by the guest's own FISTP, and a result a fraction
 * below an integer becomes the integer below it. Issue #172: a Cg hash table
 * on the Android x86_64 emulator was built with a per-step bit count of ZERO
 * where the desktop computes ten, and the loop that consumes it terminates by
 * subtracting that count from 32. A step of zero never terminates, and the run
 * wedged in one compiled block before any touch event could be pumped.
 */
int x86p_x87_fn(X86pX87Fn fn,
                uint16_t control,
                long double a,
                long double b,
                long double *r0,
                long double *r1,
                int *pushed,
                uint16_t *status) {
#if HAVE_X87
  uint16_t sw = 0u;
  uint16_t host_control = 0u;
  int restore_control = 0;
  long double scratch = 0.0L;

  if (!r0 || !pushed) {
    return 0;
  }
  if (!r1) {
    r1 = &scratch;
  }
  /* Only when it differs, and restored before returning. This framework is a
     library inside someone else's process and must not leave the FPU
     configured for the guest. */
  __asm__ volatile("fnstcw %0" : "=m"(host_control));
  if (host_control != control) {
    __asm__ volatile("fldcw %0" : : "m"(control));
    restore_control = 1;
  }
  *pushed = 0;
  *r1 = 0.0L;

  switch (fn) {
  case kX86pX87FnSqrt:
    X87_UNARY("fsqrt");
    break;
  case kX86pX87FnSin:
    X87_UNARY("fsin");
    break;
  case kX86pX87FnCos:
    X87_UNARY("fcos");
    break;
  case kX86pX87FnAbs:
    X87_UNARY("fabs");
    break;
  case kX86pX87FnChs:
    X87_UNARY("fchs");
    break;
  case kX86pX87Fn2xm1:
    X87_UNARY("f2xm1");
    break;
  case kX86pX87FnRndint:
    X87_UNARY("frndint");
    break;
  case kX86pX87FnSinCos:
    /* ST(0) becomes the sine and the COSINE is pushed above it. */
    X87_UNARY2("fsincos");
    *pushed = 1;
    break;
  case kX86pX87FnPtan:
    /* The tangent replaces ST(0) and a literal 1.0 is pushed -- which is what
       makes FPTAN usable for a division without a second constant. */
    X87_UNARY2("fptan");
    *pushed = 1;
    break;
  case kX86pX87FnXtract:
    X87_UNARY2("fxtract");
    *pushed = 1;
    break;
  case kX86pX87FnPatan:
    X87_BINARY("fpatan");
    break;
  case kX86pX87FnYl2x:
    X87_BINARY("fyl2x");
    break;
  case kX86pX87FnYl2xp1:
    X87_BINARY("fyl2xp1");
    break;
  case kX86pX87FnScale:
    X87_BINARY_NOPOP("fscale");
    break;
  case kX86pX87FnPrem:
    X87_BINARY_NOPOP("fprem");
    break;
  case kX86pX87FnPrem1:
    X87_BINARY_NOPOP("fprem1");
    break;
  case kX86pX87FnCount:
  default:
    if (restore_control) {
      __asm__ volatile("fldcw %0" : : "m"(host_control));
    }
    return 0;
  }

  if (restore_control) {
    __asm__ volatile("fldcw %0" : : "m"(host_control));
  }
  if (status) {
    *status = sw;
  }
  return 1;
#else
  return x86p_x87_fn_software_control(fn, control, a, b, r0, r1, pushed, status);
#endif
}
