/*
 * x87_transcendental.h -- the x87 functions, evaluated on an x87 unit.
 *
 * FSIN is not sinl(). The x87 unit computes it with its own polynomial to its
 * own precision, and the C library computes it with a different one; they
 * agree to about eighteen digits and differ below that. For a guest that
 * accumulates transforms across a frame, "differ below that" is drift, and the
 * test that would catch it is the one nobody writes.
 *
 * So these do not approximate the instruction -- they ARE the instruction.
 * Each one loads its operands in the 80-bit format, executes the real x87
 * opcode, and stores the 80-bit result back. That is exact by construction on
 * any host with an x87 unit, which is every x86 host this framework's JIT
 * targets.
 *
 * ON A HOST WITHOUT ONE, THEY REFUSE. x86p_x87_fn returns 0 and the engine
 * names the instruction rather than substituting a library call whose low bits
 * are different. A port that needs these on ARM needs a decision about what
 * "correct" means there, and silently picking libm would be making that
 * decision by accident.
 */
#ifndef X86PORT_X87_TRANSCENDENTAL_H
#define X86PORT_X87_TRANSCENDENTAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The functions, named by the instruction rather than by the mathematics:
 * FPATAN is not atan, it is atan(ST1/ST0) with a defined quadrant rule, and
 * FYL2X is not a logarithm, it is ST1*log2(ST0). Naming them after the maths
 * is how a caller ends up passing the operands the other way round.
 */
typedef enum X86pX87Fn {
  kX86pX87FnSqrt = 0, /* FSQRT    ST0 <- sqrt(ST0)                        */
  kX86pX87FnSin,      /* FSIN     ST0 <- sin(ST0)                        */
  kX86pX87FnCos,      /* FCOS     ST0 <- cos(ST0)                        */
  kX86pX87FnSinCos,   /* FSINCOS  ST0 <- sin, then PUSH cos              */
  kX86pX87FnPtan,     /* FPTAN    ST0 <- tan(ST0), then PUSH 1.0         */
  kX86pX87FnPatan,    /* FPATAN   ST1 <- atan(ST1/ST0), pop             */
  kX86pX87FnYl2x,     /* FYL2X    ST1 <- ST1 * log2(ST0), pop           */
  kX86pX87FnYl2xp1,   /* FYL2XP1  ST1 <- ST1 * log2(ST0 + 1), pop       */
  kX86pX87Fn2xm1,     /* F2XM1    ST0 <- 2^ST0 - 1                       */
  kX86pX87FnScale,    /* FSCALE   ST0 <- ST0 * 2^trunc(ST1), no pop     */
  kX86pX87FnRndint,   /* FRNDINT  ST0 <- round(ST0) per the control word */
  kX86pX87FnXtract,   /* FXTRACT  ST0 <- exponent, then PUSH significand */
  kX86pX87FnPrem,     /* FPREM    ST0 <- partial remainder ST0 mod ST1  */
  kX86pX87FnPrem1,    /* FPREM1   ... IEEE-754's rounding rule instead   */
  kX86pX87FnAbs,      /* FABS                                            */
  kX86pX87FnChs,      /* FCHS                                            */
  kX86pX87FnCount     /* MUST stay last */
} X86pX87Fn;

/*
 * Evaluate on the host's x87 unit.
 *
 * `a` is ST(0) and `b` is ST(1) for the two-operand forms. `r0` receives the
 * value that ends up in ST(0) and `r1` the SECOND result for the two that
 * produce one (FSINCOS, FPTAN, FXTRACT); `pushed` says whether there is one,
 * so a caller cannot mistake a stale `r1` for a value.
 *
 * `status` receives the condition-code bits the instruction wrote, which is
 * how FPREM reports that its reduction is incomplete and how the comparisons
 * report unordered. Returns 0 if this host has no x87 unit, in which case
 * nothing is written and the caller must refuse the instruction by name.
 */
int x86p_x87_fn(
    X86pX87Fn fn, long double a, long double b, long double *r0, long double *r1, int *pushed, uint16_t *status);

/* Whether this build can evaluate them at all -- that is, whether the host has
   an x87 unit to run them on. Asked by the engine before claiming coverage. */
int x86p_x87_fn_available(void);

const char *x86p_x87_fn_name(X86pX87Fn fn);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_X87_TRANSCENDENTAL_H */
