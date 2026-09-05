/* x87 transcendental evaluation. Intel hosts use their x87 instructions;
 * other hosts use pinned Bochs/SoftFloat extended-precision math. The software
 * path is tolerance-tested against x87, not claimed bit-identical to its
 * microcode. CPU storage still has the precision reported by
 * x86p_x87_precision_is_exact(). Unmasked exception delivery is not modelled.
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
 * Evaluate using the host-specific implementation.
 *
 * `a` is ST(0) and `b` is ST(1) for the two-operand forms. `r0` receives the
 * value that ends up in ST(0) and `r1` the SECOND result for the two that
 * produce one (FSINCOS, FPTAN, FXTRACT); `pushed` says whether there is one,
 * so a caller cannot mistake a stale `r1` for a value.
 *
 * `status` receives the condition-code bits the instruction wrote, which is
 * how FPREM reports that its reduction is incomplete and how the comparisons
 * report unordered. Returns 0 for unsupported functions; the caller must
 * refuse the instruction by name. Software FXTRACT remains unsupported.
 */
int x86p_x87_fn(
    X86pX87Fn fn, long double a, long double b, long double *r0, long double *r1, int *pushed, uint16_t *status);

/* Whether this build has a numerical implementation. Individual operation
   support is still checked by x86p_x87_fn. */
int x86p_x87_fn_available(void);

const char *x86p_x87_fn_name(X86pX87Fn fn);

int x86p_x87_fn_software(
    X86pX87Fn fn, long double a, long double b, long double *r0, long double *r1, int *pushed, uint16_t *status);

int x86p_x87_fn_software_control(X86pX87Fn fn,
                                 uint16_t control,
                                 long double a,
                                 long double b,
                                 long double *r0,
                                 long double *r1,
                                 int *pushed,
                                 uint16_t *status);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_X87_TRANSCENDENTAL_H */
