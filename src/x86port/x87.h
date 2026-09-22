/*
 * x87.h -- the floating-point stack.
 *
 * 93,730 instructions in the shipped corpus, 4.32%, and the whole of what the
 * integer engine could not run. Ranked, as everything here is: FLD 25185,
 * FSTP 21115, FMUL 12120, FNSTSW 6370, FADD 5636, FCOMP 4943.
 *
 * IT IS A STACK, AND THAT IS THE TRAP. `ST(0)` is not a register, it is a
 * position: the eight physical registers rotate under a TOP pointer, so FLD
 * decrements TOP and then writes, while FSTP writes and then increments. An
 * implementation that treats ST(i) as register i works for exactly as long as
 * TOP happens to be zero, which is until the first unbalanced push -- and then
 * every subsequent value is read from the wrong place, with no crash and no
 * obviously wrong number.
 *
 * TOP is also observable: FNSTSW reports it in bits 11-13, and compilers use
 * the whole status word. So it is modelled explicitly rather than normalised
 * away by shuffling an array.
 *
 * PRECISION, AND WHERE THIS IS HONEST ABOUT ITS LIMITS. x87 computes in 80-bit
 * extended precision and rounds ONCE, to whatever precision the control word
 * selects. On an x86 host that is reproduced exactly, by loading the guest's
 * control word into the real FPU and executing the real instruction -- see
 * x86p_x87_arith. Computing in extended and rounding afterwards is two
 * roundings and is NOT the same: measured against this host's FPU over
 * 1,572,864 operations, the two-step form differs on 0.4% of results at
 * PC=double/nearest and 3.8% at PC=double/up, and agrees exactly only at
 * PC=extended, where there is no second rounding to do.
 *
 * Binary128 hosts store ext80 values losslessly and delegate arithmetic and
 * narrowing to software ext80 operations, preserving one guest rounding step.
 * x86p_x87_values_are_supported() describes that numeric capability separately
 * from x86p_x87_precision_is_exact(), which describes the native object layout
 * required for raw register/MMX aliasing. Binary64 hosts still have a precision
 * limitation; only the explicitly approved Darwin path admits their values.
 */
#ifndef X86PORT_X87_H
#define X86PORT_X87_H

#include "flags.h"
#include "x87_binary128.h"
#include "x87_transcendental.h"
#include <float.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define X86P_X87_REGS 8

/* Status-word bits that matter to a guest. C0-C3 carry comparison results and
   are read through FNSTSW; the exception-summary, stack-fault, and individual
   exception bits are cleared by FNCLEX; B is the architectural busy bit. */
#define X86P_X87_IE 0x0001u /* invalid operation */
#define X86P_X87_DE 0x0002u /* denormal operand */
#define X86P_X87_ZE 0x0004u /* zero divide */
#define X86P_X87_OE 0x0008u /* overflow */
#define X86P_X87_UE 0x0010u /* underflow */
#define X86P_X87_PE 0x0020u /* precision */
#define X86P_X87_SF 0x0040u /* stack fault */
#define X86P_X87_ES 0x0080u /* exception summary */
#define X86P_X87_C0 0x0100u
#define X86P_X87_C1 0x0200u
#define X86P_X87_C2 0x0400u
#define X86P_X87_C3 0x4000u
#define X86P_X87_B 0x8000u /* busy */
#define X86P_X87_FNCLEX_MASK                                                                                           \
  (X86P_X87_B | X86P_X87_ES | X86P_X87_SF | X86P_X87_PE | X86P_X87_UE | X86P_X87_OE | X86P_X87_ZE | X86P_X87_DE |      \
   X86P_X87_IE)
#define X86P_X87_TOP_SHIFT 11

/* Control-word fields. Rounding control selects nearest/down/up/truncate;
   precision control rounds each result to single, double or extended. Guests
   really do change both -- the CRT sets PC, and Direct3D drivers of this era
   were notorious for it -- so neither is assumed. */
#define X86P_X87_RC_MASK 0x0C00u
#define X86P_X87_RC_NEAREST 0x0000u
#define X86P_X87_RC_DOWN 0x0400u
#define X86P_X87_RC_UP 0x0800u
#define X86P_X87_RC_TRUNCATE 0x0C00u
#define X86P_X87_PC_MASK 0x0300u
#define X86P_X87_PC_SINGLE 0x0000u
#define X86P_X87_PC_DOUBLE 0x0200u
#define X86P_X87_PC_EXTENDED 0x0300u
/* What the hardware and the CRT start from: all exceptions masked, round to
   nearest, extended precision. */
#define X86P_X87_CW_INIT 0x037Fu

/* Public C ABI: the C++ adapter must not narrow this enum independently.
 * NOLINTNEXTLINE(performance-enum-size) */
typedef enum X86pX87Tag {
  kX86pX87TagValid = 0,
  kX86pX87TagZero = 1,
  kX86pX87TagSpecial = 2,
  kX86pX87TagEmpty = 3 /* the encoding's own numbering; FFREE writes it */
} X86pX87Tag;

/*
 * WHAT A REGISTER IS MADE OF, which is not the same question on every host.
 *
 * On a host whose `long double` is the x87 format -- x86 and x86-64 -- that
 * type IS the register, and everything below is an identity.
 *
 * On a binary128 host, notably WebAssembly, it is not. WebAssembly has no
 * register wider than 64 bits, so every pass, return and copy of a binary128
 * is memory traffic, and the arithmetic is ext80 software anyway, so the file
 * converted into and out of a format nothing computes in. Measured by
 * tests/bench_x87_arith.cpp: 38% of the arithmetic path, against 38% for the
 * arithmetic itself. There the storage is the guest's own encoding, which is
 * also the format the softfloat takes, so the conversion is a field copy.
 *
 * The two halves are the architectural ones: `signif` is the explicit 64-bit
 * significand, `sign_exp` the sign bit and the 15-bit exponent. Storing them
 * is MORE faithful than binary128, not less -- an unnormal or a pseudo-NaN
 * round-trips, which is why the WASM backend no longer has to refuse raw
 * 80-bit loads and stores.
 */
#if X86P_X87_BINARY128
typedef struct X86pX87Reg {
  uint64_t signif;
  uint16_t sign_exp;
} X86pX87Reg;
#else
typedef long double X86pX87Reg;
#endif

/* The arithmetic operations, as a count usable before the enum below names
   them; the static assertion after that enum is what keeps the two equal. */
#define X86P_X87_OPS 4

/*
 * WHICH ARITHMETIC A RUN ACTUALLY PERFORMS, off by default.
 *
 * The static op mix of a binary says nothing about a route: a multiply inside
 * a skinning loop and a divide in a menu weigh the same there. This counts
 * what a run executes, and beside each total the number of operations an
 * inline encoding-level path could have taken -- `x86p_ext80_mul_ordinary`'s
 * own preconditions, asked through the same predicates it asks, so the two
 * cannot drift.
 *
 * `ordinary_measured` is 0 on a host whose register file is not the ext80
 * encoding, where those preconditions cannot be asked of the storage. A
 * reader must print that rather than a row of zeroes, which would read like a
 * run whose arithmetic an inline path could never touch.
 */
typedef struct X86pX87OpCensus {
  uint64_t total[X86P_X87_OPS];
  /* What one of the rules in x87_ext80_arith.h actually TAKES, asked by
     calling it rather than by repeating its preconditions here. */
  uint64_t taken[X86P_X87_OPS];
  /*
   * WHY the rest were not, because the two answers want different work: a
   * control word the rules do not handle is a property of the guest's mode and
   * no wider rule helps, while a subnormal, unnormal, infinity or NaN is where
   * the softfloat earns its place. What is left -- the total less both refusal
   * columns, less `taken` -- is an operand shape the rules are written for that
   * no rule answered: an operation with no rule at all, an exponent leaving the
   * normal range, an exact cancellation.
   */
  uint64_t refused_control[X86P_X87_OPS];
  uint64_t refused_other[X86P_X87_OPS];
  /*
   * THE GUEST'S MODE, WHICH IS MEASURABLE ON EVERY HOST, and which decides
   * whether an encoding-level rule can reach this title at all.
   *
   * `refused_control` above is the same question asked only where the register
   * file is the ext80 encoding; on a host with a real x87 unit the column is
   * zero and a reader cannot tell "the mode is fine" from "the mode was never
   * looked at". These two are indexed by the control word's own fields --
   * precision control in bits 8-9 and rounding control in bits 10-11 -- so
   * they are counted the same way whatever the storage is.
   *
   * A title that runs at PC=53 (index 2), which is what the MSVC CRT sets, is
   * one an 80-bit-only rule cannot answer however ordinary its operands are.
   */
  uint64_t by_precision[4];
  uint64_t by_rounding[4];
  int ordinary_measured;
} X86pX87OpCensus;

typedef struct X86pX87 {
  X86pX87Reg reg[X86P_X87_REGS]; /* PHYSICAL registers; ST(i) is reg[(top+i)&7] */
  uint8_t tag[X86P_X87_REGS];
  uint8_t top;
  uint16_t control;
  uint16_t status; /* C0-C3 and the exception flags; TOP is merged in on read */
  /* NULL unless a consumer armed the census; the branch is one predictable
     test on a path that costs tens of nanoseconds. */
  X86pX87OpCensus *op_census;
} X86pX87;

/* `census` may be NULL, which disarms it. The counters belong to the caller
   and are not cleared here. */
void x86p_x87_set_op_census(X86pX87 *f, X86pX87OpCensus *census);

/*
 * THE MMX REGISTERS ARE THESE REGISTERS.
 *
 * MMn is the 64-bit mantissa field of PHYSICAL register n -- not of ST(n), and
 * not of a separate file. That aliasing is architectural and observable: a
 * guest that uses MMX and then executes FLD without EMMS reads a value with an
 * exponent of all ones, and one that reverses the order reads the mantissa of
 * whatever float was there. Modelling MMX as its own array would be a lie
 * about the machine, and the code this framework runs is an Alchemy math
 * library that interleaves 3DNow! with x87 on purpose.
 *
 * Writing MMn sets the sign and exponent field to all ones, which is what the
 * hardware does and what makes the register read back as a NaN to x87. It also
 * marks every tag VALID and does NOT change TOP -- MMX is not a stack.
 *
 * Both return 0 on a host whose `long double` is not x87's format, where there
 * is no mantissa field to alias onto and the caller must refuse by name rather
 * than invent one.
 */
int x86p_x87_mmx_read(const X86pX87 *f, int n, uint64_t *out);
int x86p_x87_mmx_write(X86pX87 *f, int n, uint64_t v);

/* EMMS and FEMMS: every register becomes empty again, so subsequent x87 code
   sees a fresh stack. TOP is unchanged; the tags are what x87 reads. */
void x86p_x87_emms(X86pX87 *f);

/* Reset to the state a process starts in: empty stack, CW_INIT, clear status. */
void x86p_x87_reset(X86pX87 *f);

/*
 * WHETHER THIS HOST'S `long double` IS THE TEN-BYTE x87 OBJECT.
 *
 * One definition, because more than one translation unit decides from it what
 * a register may be read as: the tag classifier reads the significand and
 * exponent fields straight out of the object, and the arithmetic executes the
 * real instruction. x86p_x87_precision_is_exact() below returns exactly this.
 */
#if LDBL_MANT_DIG == 64 && LDBL_MAX_EXP == 16384
#define X86P_EXACT_LONG_DOUBLE 1
#else
#define X86P_EXACT_LONG_DOUBLE 0
#endif

/* Whether host long double has the native x87 object layout. This controls
   raw register-byte/MMX alias access, independently of numeric support. */
int x86p_x87_precision_is_exact(void);

/* Value helpers support native ext80 or lossless binary128 storage with
   software ext80 arithmetic. This does not admit raw MMX alias operations. */
int x86p_x87_values_are_supported(void);

/*
 * Stack access by POSITION, which is the only correct way to name a value
 * here. Returns 0 and leaves *out alone when ST(i) is empty -- reading an
 * empty register is a guest bug the engine must be able to report, not a zero
 * to carry forward.
 */
int x86p_x87_get(const X86pX87 *f, int i, long double *out);
int x86p_x87_set(X86pX87 *f, int i, long double v);

/* Push and pop. Push onto a full stack, and pop from an empty one, set the
   stack-fault and invalid bits and return 0 -- the guest is entitled to find
   out, and silently wrapping TOP is how an interpreter drifts from hardware
   with no symptom for thousands of instructions. */
int x86p_x87_push(X86pX87 *f, long double v);
int x86p_x87_pop(X86pX87 *f, long double *out);

/* How many registers are occupied, for diagnostics and for a divergence report
   that has to say the stacks were different depths. */
int x86p_x87_depth(const X86pX87 *f);

/* The status word as FNSTSW reports it, with TOP merged into bits 11-13. */
uint16_t x86p_x87_status(const X86pX87 *f);

/* FNCLEX clears only the busy, summary, stack-fault, and six exception bits.
   It does not change TOP, condition codes, control, tags, or register data. */
void x86p_x87_clear_exceptions(X86pX87 *f);

/* Round a value to the precision the control word currently selects. Applied
   to every arithmetic result, because that is what the hardware does -- a
   guest that sets single precision and gets extended-precision results
   diverges slowly and everywhere. */
long double x86p_x87_round(const X86pX87 *f, long double v);

/*
 * The arithmetic. Each takes ST(dst) and a value, writes the result into
 * ST(dst) with the current rounding and precision applied, and returns 1; 0
 * means a stack register that was empty, with the fault bits set.
 *
 * `reverse` swaps the operands, which is FSUBR/FDIVR -- a separate flag rather
 * than four more entry points, because the reverse forms differ ONLY in
 * operand order and duplicating them is how one of them acquires a bug.
 */
/* Public C ABI; width is checked in both C and the C++ adapter.
 * NOLINTNEXTLINE(performance-enum-size) */
typedef enum X86pX87Op {
  kX86pX87Add = 0,
  kX86pX87Sub,
  kX86pX87Mul,
  kX86pX87Div,
  kX86pX87OpCount /* MUST stay last */
} X86pX87Op;

_Static_assert((int)kX86pX87OpCount == X86P_X87_OPS, "the op census has one counter per arithmetic operation");

int x86p_x87_arith(X86pX87 *f, X86pX87Op op, int dst, long double src, int reverse);

/*
 * THE SAME OPERATIONS IN THE STORAGE TYPE, which on a binary128 host is where
 * the work actually gets done.
 *
 * These are not a second implementation. On every host the `long double`
 * entry points above are thin wrappers that convert their argument and call
 * these, so there is one stack discipline, one tag classifier and one
 * arithmetic path. What the raw forms let a caller avoid is converting a value
 * into a format that is then immediately converted back -- which is what the
 * WASM backend was doing on every x87 instruction.
 *
 * On a native ext80 host X86pX87Reg IS long double and the conversions below
 * are identities, so these cost exactly what the wrappers cost.
 */
int x86p_x87_get_raw(const X86pX87 *f, int i, X86pX87Reg *out);
int x86p_x87_set_raw(X86pX87 *f, int i, X86pX87Reg v);
int x86p_x87_push_raw(X86pX87 *f, X86pX87Reg v);
int x86p_x87_pop_raw(X86pX87 *f, X86pX87Reg *out);
int x86p_x87_arith_raw(X86pX87 *f, X86pX87Op op, int dst, X86pX87Reg src, int reverse);

/* The edge conversions. On a native ext80 host both are the identity. */
X86pX87Reg x86p_x87_reg_from_long_double(long double v);
long double x86p_x87_reg_to_long_double(X86pX87Reg v);

/* The guest's ten bytes, which on a binary128 host the storage already is --
   so an 80-bit load or store becomes a copy with nothing to round. */
X86pX87Reg x86p_x87_reg_from_f80(const uint8_t bytes[10]);
void x86p_x87_reg_to_f80(X86pX87Reg v, uint8_t bytes[10]);

/* Guest memory bits straight to and from the storage type, with no detour
   through the host's widest float. */
X86pX87Reg x86p_x87_reg_from_f32_bits(uint32_t bits);
X86pX87Reg x86p_x87_reg_from_f64_bits(uint64_t bits);
uint64_t x86p_x87_reg_to_f32_bits(const X86pX87 *f, X86pX87Reg v);
uint64_t x86p_x87_reg_to_f64_bits(const X86pX87 *f, X86pX87Reg v);

/*
 * What an x87 INSTRUCTION does, as the decoder classifies it. Distinct from
 * X86pX87Op above, which is only the four arithmetic operations: an
 * instruction also says where its operands come from and what it leaves the
 * stack looking like.
 *
 * The constants are named individually rather than carried as a value, because
 * FLDPI is not "load 3.14159..." -- it loads the 80-bit constant the hardware
 * holds, and a literal written out in C source is a different number in the
 * last few bits.
 */
/* Public C ABI; width is checked in both C and the C++ adapter.
 * NOLINTNEXTLINE(performance-enum-size) */
typedef enum X86pX87Insn {
  kX86pX87InsnLoad = 0,     /* FLD: push a float from memory or ST(i) */
  kX86pX87InsnLoadInt,      /* FILD: push an integer from memory */
  kX86pX87InsnStore,        /* FST/FSTP */
  kX86pX87InsnStoreInt,     /* FIST/FISTP */
  kX86pX87InsnArith,        /* FADD/FSUB/FMUL/FDIV and their R and P forms */
  kX86pX87InsnCompare,      /* FCOM/FCOMP/FCOMPP/FUCOM... */
  kX86pX87InsnExchange,     /* FXCH */
  kX86pX87InsnChangeSign,   /* FCHS */
  kX86pX87InsnAbs,          /* FABS */
  kX86pX87InsnConstZero,    /* FLDZ */
  kX86pX87InsnConstOne,     /* FLD1 */
  kX86pX87InsnConstPi,      /* FLDPI */
  kX86pX87InsnStoreStatus,  /* FNSTSW */
  kX86pX87InsnLoadControl,  /* FLDCW */
  kX86pX87InsnStoreControl, /* FNSTCW */
  kX86pX87InsnFree,         /* FFREE */
  kX86pX87InsnInit,         /* FNINIT */
  /*
   * The functions evaluated on the host's own x87 unit. `x87_fn` says which.
   *
   * One instruction kind rather than sixteen, because what the DECODER has to
   * say about them is identical -- they take no memory operand and their
   * operands are the top of the stack -- and what differs is which opcode runs,
   * which is x87_transcendental.c's business.
   */
  kX86pX87InsnFn,
  kX86pX87InsnWait,        /* FWAIT/WAIT: with exceptions masked, nothing to do */
  kX86pX87InsnClearExc,    /* FNCLEX */
  kX86pX87InsnTest,        /* FTST: compare ST(0) with +0.0 */
  kX86pX87InsnCompareInt,  /* FCOMI/FCOMIP/FUCOMI/FUCOMIP: into EFLAGS */
  kX86pX87InsnConstLog2E,  /* FLDL2E */
  kX86pX87InsnConstLog2T,  /* FLDL2T */
  kX86pX87InsnConstLn2,    /* FLDLN2 */
  kX86pX87InsnConstLog102, /* FLDLG2 */
  /* FCMOVB/E/BE/U and their N forms: move ST(i) into ST(0) when an EFLAGS
     condition holds. The condition is an ordinary X86pCond in `cond` -- these
     read the INTEGER flags, which is the whole point of the instruction: it
     exists so that an FCOMI can be branched on without a jump. */
  kX86pX87InsnCmov,
  kX86pX87InsnSaveState,    /* FNSAVE/FSAVE: the whole unit to 108 bytes, then FNINIT */
  kX86pX87InsnRestoreState, /* FRSTOR: and back */
  kX86pX87InsnCount         /* MUST stay last */
} X86pX87Insn;

/* Push one of FLD1/FLDZ/FLDPI/FLDL2E/FLDL2T/FLDLG2/FLDLN2's exact extended
   constants. Returns 0 for a non-constant kind or a full stack. */
int x86p_x87_push_constant(X86pX87 *f, X86pX87Insn instruction);

const char *x86p_x87_insn_name(X86pX87Insn insn);

/*
 * The PORTABLE arithmetic path: compute in the host's widest type, then round
 * to the guest's precision. On an x86 host x86p_x87_arith does not use this --
 * it executes the real instruction under the guest's control word, which
 * rounds once instead of twice -- but this is what a host without an x87 unit
 * is left with, and it is exposed so the divergence can be MEASURED rather
 * than assumed small. At PC=extended the two agree exactly; at a narrowed
 * precision they do not, and the test suite prints by how much.
 */
long double x86p_x87_arith_portable(uint16_t control, X86pX87Op op, long double x, long double y);

/* Compare ST(0) against a value and set C0/C2/C3 as FCOM does. An unordered
   result (either operand a NaN) sets all three, which is a distinct outcome
   from equal and from less-than, and guests branch on it. */
int x86p_x87_compare(X86pX87 *f, long double other);

/* Name an operation, for traces. Never null. */
const char *x86p_x87_op_name(X86pX87Op op);

/*
 * Conversion to and from the in-memory formats. The 80-bit form is the one
 * worth care: it is the only format that stores the mantissa's leading bit
 * explicitly, and the layout is 64 mantissa bits then 15 exponent bits then a
 * sign, little-endian, in ten bytes.
 */
long double x86p_x87_from_f32(uint32_t bits);
long double x86p_x87_from_f64(uint64_t bits);
uint32_t x86p_x87_to_f32(const X86pX87 *f, long double v);
uint64_t x86p_x87_to_f64(const X86pX87 *f, long double v);
long double x86p_x87_from_f80(const uint8_t bytes[10]);
void x86p_x87_to_f80(long double v, uint8_t bytes[10]);

/* Integer conversion, honouring the rounding mode -- FIST rounds, it does not
   truncate, unless the control word says truncate. Returns 0 when the value
   does not fit, which is a guest-visible invalid operation. */
int x86p_x87_to_int(const X86pX87 *f, long double v, int width_bytes, int64_t *out);

int x86p_x87_apply_fn(X86pX87 *f, X86pX87Fn fn);

long double x86p_x87_integer_value(uint64_t bits, unsigned width);
uint64_t x86p_x87_to_i16(X86pX87 *f, long double value);
uint64_t x86p_x87_to_i32(X86pX87 *f, long double value);
uint64_t x86p_x87_to_i64(X86pX87 *f, long double value);

void x86p_x87_compare_register(X86pX87 *f, int index, unsigned pops);
void x86p_x87_exchange(X86pX87 *f, int index);
void x86p_x87_sign(X86pX87 *f, int absolute);
void x86p_x87_test(X86pX87 *f);
int x86p_x87_compare_flags(X86pX87 *f, X86pFlags *flags, int index);
void x86p_x87_free(X86pX87 *f, int index);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_X87_H */
