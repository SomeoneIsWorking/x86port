/* x87_exec.c -- see x87_exec.h for why addressing is not duplicated here. */
#include "x87_exec.h"

#include "cond.h"
#include "flags.h"
#include "x87_state.h"
#include "x87_transcendental.h"

#include <math.h>
#include <string.h>

static const char *kStatusNames[] = {"ok", "memory-fault", "unsupported"};
_Static_assert((int)(sizeof kStatusNames / sizeof kStatusNames[0]) == (int)kX86pX87ExecStatusCount,
               "every X86pX87ExecStatus needs a name");

const char *x86p_x87_exec_status_name(X86pX87ExecStatus s) {
  if ((unsigned)s >= (unsigned)kX86pX87ExecStatusCount) {
    return "unknown";
  }
  return kStatusNames[(int)s];
}

/*
 * One instruction's working state, for the same reason exec.c has one: a fault
 * raised inside an operand read has to reach the caller without every helper
 * returning a status that somebody forgets to check.
 */
typedef struct Ctx {
  X86pCpu *cpu;
  X86pX87 *fpu;
  const X86pMem *mem;
  const X86pInsn *insn;
  int has_mem;
  uint32_t addr;
  X86pX87ExecStatus status;
  uint32_t fault_addr;
} Ctx;

static void fault(Ctx *c) {
  c->status = kX86pX87ExecMemoryFault;
  c->fault_addr = c->addr;
}

/* ---- the source operand ------------------------------------------------- */

/*
 * The value an arithmetic or load instruction reads.
 *
 * ITS WIDTH SAYS WHICH FORMAT IT IS, and the three are genuinely different
 * encodings, not the same number at three sizes: 4 and 8 bytes are IEEE single
 * and double, 10 bytes is x87's own extended format with its explicit leading
 * mantissa bit. An integer form (FILD, FIADD) reads a two's-complement integer
 * instead, which is why that is a separate function rather than another width
 * in this one.
 */
static long double read_float(Ctx *c, int size) {
  switch (size) {
  case 4: {
    uint32_t bits = 0;
    if (!x86p_mem_read(c->mem, c->addr, 4, &bits)) {
      fault(c);
      return 0.0L;
    }
    return x86p_x87_from_f32(bits);
  }
  case 8: {
    uint8_t b[8];
    uint64_t bits = 0;
    int i;
    if (!x86p_mem_read_bytes(c->mem, c->addr, b, 8)) {
      fault(c);
      return 0.0L;
    }
    for (i = 7; i >= 0; i--) {
      bits = (bits << 8) | b[i];
    }
    return x86p_x87_from_f64(bits);
  }
  case 10: {
    uint8_t b[10];
    if (!x86p_mem_read_bytes(c->mem, c->addr, b, 10)) {
      fault(c);
      return 0.0L;
    }
    return x86p_x87_from_f80(b);
  }
  default:
    c->status = kX86pX87ExecUnsupported;
    return 0.0L;
  }
}

static long double read_integer(Ctx *c, int size) {
  uint32_t v = 0;
  switch (size) {
  case 2:
    if (!x86p_mem_read(c->mem, c->addr, 2, &v)) {
      fault(c);
      return 0.0L;
    }
    return (long double)(int16_t)(uint16_t)v;
  case 4:
    if (!x86p_mem_read(c->mem, c->addr, 4, &v)) {
      fault(c);
      return 0.0L;
    }
    return (long double)(int32_t)v;
  case 8: {
    uint8_t b[8];
    uint64_t bits = 0;
    int i;
    if (!x86p_mem_read_bytes(c->mem, c->addr, b, 8)) {
      fault(c);
      return 0.0L;
    }
    for (i = 7; i >= 0; i--) {
      bits = (bits << 8) | b[i];
    }
    return (long double)(int64_t)bits;
  }
  default:
    c->status = kX86pX87ExecUnsupported;
    return 0.0L;
  }
}

static void write_float(Ctx *c, int size, long double v) {
  switch (size) {
  case 4: {
    uint32_t bits = x86p_x87_to_f32(c->fpu, v);
    if (!x86p_mem_write(c->mem, c->addr, 4, bits)) {
      fault(c);
    }
    return;
  }
  case 8: {
    uint64_t bits = x86p_x87_to_f64(c->fpu, v);
    uint8_t b[8];
    int i;
    for (i = 0; i < 8; i++) {
      b[i] = (uint8_t)(bits & 0xFFu);
      bits >>= 8;
    }
    if (!x86p_mem_write_bytes(c->mem, c->addr, b, 8)) {
      fault(c);
    }
    return;
  }
  case 10: {
    uint8_t b[10];
    x86p_x87_to_f80(v, b);
    if (!x86p_mem_write_bytes(c->mem, c->addr, b, 10)) {
      fault(c);
    }
    return;
  }
  default:
    c->status = kX86pX87ExecUnsupported;
    return;
  }
}

/*
 * The second operand of an arithmetic instruction, which comes from one of
 * three places and is the part most easily got wrong:
 *
 *   FADD m32          -- memory, and ST(0) is the destination
 *   FADD ST(0), ST(i) -- a stack position, destination named explicitly
 *   FADDP ST(i), ST(0)-- likewise, and then a pop
 *
 * Returns 0 without touching *src or *dst when a named stack position is
 * empty, which is a stack fault and not a zero to compute with.
 */
static int arith_operands(Ctx *c, int *dst, long double *src) {
  const X86pInsn *in = c->insn;
  if (c->has_mem) {
    const X86pOperand *o = &in->operand[0];
    *dst = 0; /* a memory form always accumulates into ST(0) */
    *src = in->x87_mem_int ? read_integer(c, o->size) : read_float(c, o->size);
    return c->status == kX86pX87ExecOk;
  }
  if (in->operands == 2 && in->operand[0].kind == kX86pOperandSt && in->operand[1].kind == kX86pOperandSt) {
    *dst = in->operand[0].reg;
    if (!x86p_x87_get(c->fpu, in->operand[1].reg, src)) {
      return 0;
    }
    return 1;
  }
  if (in->operands == 1 && in->operand[0].kind == kX86pOperandSt) {
    /* The one-operand register form is `FADD ST(0), ST(i)` written short. */
    *dst = 0;
    if (!x86p_x87_get(c->fpu, in->operand[0].reg, src)) {
      return 0;
    }
    return 1;
  }
  c->status = kX86pX87ExecUnsupported;
  return 0;
}

/* Pop `n` values off, discarding them. The pop suffix is part of the
   operation; running it as a separate step is how the count gets lost. */
static void do_pops(Ctx *c, int n) {
  int i;
  for (i = 0; i < n; i++) {
    x86p_x87_pop(c->fpu, NULL);
  }
}

static void execute(Ctx *c) {
  const X86pInsn *in = c->insn;
  X86pX87 *f = c->fpu;
  const X86pOperand *o0 = &in->operand[0];

  switch ((X86pX87Insn)in->x87) {
  case kX86pX87InsnLoad: {
    long double v;
    if (c->has_mem) {
      v = read_float(c, o0->size);
      if (c->status != kX86pX87ExecOk) {
        return;
      }
    } else if (o0->kind == kX86pOperandSt) {
      /* FLD ST(i) reads BEFORE the push, because the push moves TOP and would
         renumber the very position being read. */
      if (!x86p_x87_get(f, o0->reg, &v)) {
        return;
      }
    } else {
      c->status = kX86pX87ExecUnsupported;
      return;
    }
    x86p_x87_push(f, v);
    return;
  }

  case kX86pX87InsnLoadInt: {
    long double v;
    if (!c->has_mem) {
      c->status = kX86pX87ExecUnsupported;
      return;
    }
    v = read_integer(c, o0->size);
    if (c->status != kX86pX87ExecOk) {
      return;
    }
    x86p_x87_push(f, v);
    return;
  }

  case kX86pX87InsnStore: {
    long double v;
    if (!x86p_x87_get(f, 0, &v)) {
      return;
    }
    if (c->has_mem) {
      write_float(c, o0->size, v);
      if (c->status != kX86pX87ExecOk) {
        return; /* and NOTHING is popped: a store that faulted did not happen */
      }
    } else if (o0->kind == kX86pOperandSt) {
      x86p_x87_set(f, o0->reg, v);
    } else {
      c->status = kX86pX87ExecUnsupported;
      return;
    }
    do_pops(c, in->x87_pops);
    return;
  }

  case kX86pX87InsnStoreInt: {
    long double v;
    int64_t n;
    if (!c->has_mem) {
      c->status = kX86pX87ExecUnsupported;
      return;
    }
    if (!x86p_x87_get(f, 0, &v)) {
      return;
    }
    if (!x86p_x87_to_int(f, v, o0->size, &n)) {
      /* Out of range is an invalid operation with a defined result -- the
         "integer indefinite" value -- not something to refuse. */
      f->status |= X86P_X87_IE;
      n = (o0->size == 2) ? INT64_C(-32768) : (o0->size == 4 ? INT64_C(-2147483648) : INT64_MIN);
    }
    if (o0->size == 8) {
      uint8_t b[8];
      uint64_t bits = (uint64_t)n;
      int i;
      for (i = 0; i < 8; i++) {
        b[i] = (uint8_t)(bits & 0xFFu);
        bits >>= 8;
      }
      if (!x86p_mem_write_bytes(c->mem, c->addr, b, 8)) {
        fault(c);
        return;
      }
    } else if (!x86p_mem_write(c->mem, c->addr, o0->size, (uint32_t)(uint64_t)n)) {
      fault(c);
      return;
    }
    do_pops(c, in->x87_pops);
    return;
  }

  case kX86pX87InsnArith: {
    int dst = 0;
    long double src = 0.0L;
    if (!arith_operands(c, &dst, &src)) {
      return;
    }
    x86p_x87_arith(f, (X86pX87Op)in->x87_op, dst, src, in->x87_reverse);
    do_pops(c, in->x87_pops);
    return;
  }

  case kX86pX87InsnCompare: {
    long double other = 0.0L;
    if (c->has_mem) {
      other = in->x87_mem_int ? read_integer(c, o0->size) : read_float(c, o0->size);
      if (c->status != kX86pX87ExecOk) {
        return;
      }
    } else if (in->operands >= 1 && o0->kind == kX86pOperandSt) {
      if (!x86p_x87_get(f, o0->reg, &other)) {
        return;
      }
    } else if (in->operands == 0) {
      /* FCOMPP compares ST(0) with ST(1) implicitly, then pops both. */
      if (!x86p_x87_get(f, 1, &other)) {
        return;
      }
    } else {
      c->status = kX86pX87ExecUnsupported;
      return;
    }
    x86p_x87_compare(f, other);
    do_pops(c, in->x87_pops);
    return;
  }

  case kX86pX87InsnExchange: {
    long double a, b;
    int i = (in->operands >= 1 && o0->kind == kX86pOperandSt) ? o0->reg : 1;
    if (!x86p_x87_get(f, 0, &a) || !x86p_x87_get(f, i, &b)) {
      return;
    }
    x86p_x87_set(f, 0, b);
    x86p_x87_set(f, i, a);
    return;
  }

  case kX86pX87InsnChangeSign:
  case kX86pX87InsnAbs: {
    long double v;
    if (!x86p_x87_get(f, 0, &v)) {
      return;
    }
    /* Sign manipulation, NOT arithmetic: it does not round, does not consult
       precision control, and works on a NaN. `0 - v` would be a different
       operation with different results for zero and for NaN. */
    if (in->x87 == kX86pX87InsnAbs) {
      v = (v < 0.0L || (v == 0.0L && signbit(v))) ? -v : v;
    } else {
      v = -v;
    }
    x86p_x87_set(f, 0, v);
    return;
  }

  case kX86pX87InsnConstZero:
    x86p_x87_push(f, 0.0L);
    return;
  case kX86pX87InsnConstOne:
    x86p_x87_push(f, 1.0L);
    return;
  case kX86pX87InsnConstPi:
    /* The hardware's own 80-bit constant, written to the full 64-bit
       significand. A `double`-precision literal here would be a different
       number in the last eleven bits, and every angle computed from it would
       drift. */
    x86p_x87_push(f, 3.14159265358979323846264338327950288L);
    return;

  case kX86pX87InsnStoreStatus: {
    uint16_t sw = x86p_x87_status(f);
    /* FNSTSW AX is the common form and the reason the FPU state lives on the
       CPU: the status word lands in a general-purpose register, and the guest
       then branches on it with SAHF or TEST. */
    if (c->has_mem) {
      if (!x86p_mem_write(c->mem, c->addr, 2, sw)) {
        fault(c);
      }
      return;
    }
    if (in->operands >= 1 && o0->kind == kX86pOperandReg) {
      x86p_reg_write(c->cpu, o0->reg, 2, sw);
      return;
    }
    x86p_reg_write(c->cpu, kX86pEax, 2, sw);
    return;
  }

  case kX86pX87InsnLoadControl: {
    uint32_t v = 0;
    if (!c->has_mem) {
      c->status = kX86pX87ExecUnsupported;
      return;
    }
    if (!x86p_mem_read(c->mem, c->addr, 2, &v)) {
      fault(c);
      return;
    }
    /* The CRT and this era's D3D drivers really do change precision and
       rounding here, so it is stored rather than ignored -- ignoring it makes
       every subsequent result subtly wrong with no other symptom. */
    f->control = (uint16_t)v;
    return;
  }

  case kX86pX87InsnStoreControl:
    if (!c->has_mem) {
      c->status = kX86pX87ExecUnsupported;
      return;
    }
    if (!x86p_mem_write(c->mem, c->addr, 2, f->control)) {
      fault(c);
    }
    return;

  case kX86pX87InsnFree:
    if (in->operands >= 1 && o0->kind == kX86pOperandSt) {
      int p = (f->top + o0->reg) & (X86P_X87_REGS - 1);
      f->tag[p] = (uint8_t)kX86pX87TagEmpty;
    }
    return;

  case kX86pX87InsnInit:
    x86p_x87_reset(f);
    return;

  /*
   * The four remaining hardware constants. Written to the full 64-bit
   * significand for the same reason FLDPI is: a `double` literal is a
   * different number in the last eleven bits, and everything computed from it
   * drifts.
   */
  case kX86pX87InsnConstLog2E:
    x86p_x87_push(f, 1.44269504088896340735992468100189214L);
    return;
  case kX86pX87InsnConstLog2T:
    x86p_x87_push(f, 3.32192809488736234787031942948939018L);
    return;
  case kX86pX87InsnConstLn2:
    x86p_x87_push(f, 0.693147180559945309417232121458176568L);
    return;
  case kX86pX87InsnConstLog102:
    x86p_x87_push(f, 0.301029995663981195213738894724493027L);
    return;

  case kX86pX87InsnCmov: {
    /* Reads the INTEGER flags, which is the point of the instruction: it lets
       an FCOMI result be acted on without a branch. */
    long double v;
    if (!x86p_cond((X86pCond)c->insn->cond, &c->cpu->flags)) {
      return;
    }
    if (!x86p_x87_get(f, c->insn->operand[1].reg, &v)) {
      return; /* x86p_x87_get has already set the stack-fault flags */
    }
    (void)x86p_x87_set(f, 0, v);
    return;
  }

  case kX86pX87InsnSaveState:
    if (!x86p_x87_save_state(f, c->mem, c->addr)) {
      fault(c);
    }
    return;

  case kX86pX87InsnRestoreState:
    if (!x86p_x87_restore_state(f, c->mem, c->addr)) {
      fault(c);
    }
    return;

  case kX86pX87InsnWait:
    /* WAIT checks for a pending unmasked exception. Every process this targets
       runs with all of them masked, so there is never one pending and there is
       nothing to do -- but it is a NAMED arm rather than a fall-through, so a
       build that starts modelling exceptions has somewhere to put the check. */
    return;

  case kX86pX87InsnClearExc:
    /* The exception flags and the busy bit; the condition codes and TOP are
       not touched. Clearing the whole word would silently reset TOP to zero
       and every subsequent ST(i) would name a different register. */
    f->status &= (uint16_t)~0x80FFu;
    return;

  case kX86pX87InsnTest: {
    long double v = 0.0L;
    if (!x86p_x87_get(f, 0, &v)) {
      /* x86p_x87_get has already set the stack-fault and invalid-operation
         flags; the instruction produces no result, exactly as the other arms
         here do. */
      return;
    }
    /* C3, C2, C0 encode the three-way result, exactly as FCOM does. */
    f->status &= (uint16_t)~(X86P_X87_C0 | X86P_X87_C2 | X86P_X87_C3);
    if (v > 0.0L) {
      /* all three clear */
    } else if (v < 0.0L) {
      f->status |= X86P_X87_C0;
    } else if (v == 0.0L) {
      f->status |= X86P_X87_C3;
    } else {
      f->status |= (uint16_t)(X86P_X87_C0 | X86P_X87_C2 | X86P_X87_C3); /* unordered */
    }
    return;
  }

  case kX86pX87InsnCompareInt: {
    /*
     * FCOMI and friends write EFLAGS rather than the x87 condition codes,
     * which is the whole point of them: no FNSTSW, no SAHF, just a branch. ZF,
     * PF and CF carry the same three-way encoding, with PF as UNORDERED.
     */
    long double x = 0.0L;
    long double y = 0.0L;
    uint32_t e = X86P_EFLAGS_FIXED;
    int i = (in->operands >= 1 && o0->kind == kX86pOperandSt) ? o0->reg : 1;
    if (!x86p_x87_get(f, 0, &x) || !x86p_x87_get(f, i, &y)) {
      /* x86p_x87_get has already set the stack-fault and invalid-operation
         flags; the instruction produces no result, exactly as the other arms
         here do. */
      return;
    }
    if (!(x == x) || !(y == y)) {
      e |= X86P_ZF | X86P_PF | X86P_CF;
    } else if (x > y) {
      /* all three clear */
    } else if (x < y) {
      e |= X86P_CF;
    } else {
      e |= X86P_ZF;
    }
    x86p_flags_set_explicit(&c->cpu->flags, e);
    for (i = 0; i < (int)in->x87_pops; i++) {
      long double dropped;
      (void)x86p_x87_pop(f, &dropped);
    }
    return;
  }

  case kX86pX87InsnFn: {
    /*
     * Evaluated on the host's own x87 unit -- see x87_transcendental.h. Not
     * approximated with libm: FSIN and sinl() agree to about eighteen digits
     * and differ below that, and a guest accumulating transforms across a
     * frame turns "differ below that" into drift.
     */
    long double a = 0.0L;
    long double b = 0.0L;
    long double r0 = 0.0L;
    long double r1 = 0.0L;
    int pushed = 0;
    uint16_t sw = 0u;
    X86pX87Fn fn = (X86pX87Fn)in->x87_fn;
    int two_operand = (fn == kX86pX87FnPatan || fn == kX86pX87FnYl2x || fn == kX86pX87FnYl2xp1 ||
                       fn == kX86pX87FnScale || fn == kX86pX87FnPrem || fn == kX86pX87FnPrem1);

    if (!x86p_x87_get(f, 0, &a)) {
      /* x86p_x87_get has already set the stack-fault and invalid-operation
         flags; the instruction produces no result, exactly as the other arms
         here do. */
      return;
    }
    if (two_operand && !x86p_x87_get(f, 1, &b)) {
      /* x86p_x87_get has already set the stack-fault and invalid-operation
         flags; the instruction produces no result, exactly as the other arms
         here do. */
      return;
    }
    if (!x86p_x87_fn(fn, a, b, &r0, &r1, &pushed, &sw)) {
      /* No x87 unit on this host. Refused by name rather than substituted. */
      c->status = kX86pX87ExecUnsupported;
      return;
    }
    /* C1 and C2 are guest-visible: C2 reports an incomplete FPREM reduction
       and an out-of-range trigonometric argument, and guest code loops on it. */
    f->status &= (uint16_t)~(X86P_X87_C0 | X86P_X87_C1 | X86P_X87_C2 | X86P_X87_C3);
    f->status |= (uint16_t)(sw & (X86P_X87_C0 | X86P_X87_C1 | X86P_X87_C2 | X86P_X87_C3));

    if (fn == kX86pX87FnPatan || fn == kX86pX87FnYl2x || fn == kX86pX87FnYl2xp1) {
      /* These consume BOTH registers and leave one result: pop, then replace. */
      long double dropped;
      (void)x86p_x87_pop(f, &dropped);
      x86p_x87_set(f, 0, r0);
    } else {
      x86p_x87_set(f, 0, r0);
      if (pushed) {
        x86p_x87_push(f, r1);
      }
    }
    return;
  }

  case kX86pX87InsnCount:
  default:
    c->status = kX86pX87ExecUnsupported;
    return;
  }
}

X86pX87ExecStatus x86p_x87_execute(
    X86pCpu *cpu, const X86pMem *mem, const X86pInsn *insn, int has_mem, uint32_t mem_addr, uint32_t *fault_addr) {
  Ctx c;
  if (!cpu || !insn || insn->op != kX86pInsnX87) {
    return kX86pX87ExecUnsupported;
  }
  memset(&c, 0, sizeof c);
  c.cpu = cpu;
  c.fpu = &cpu->x87;
  c.mem = mem;
  c.insn = insn;
  c.has_mem = has_mem;
  c.addr = mem_addr;
  c.status = kX86pX87ExecOk;
  execute(&c);
  if (c.status == kX86pX87ExecMemoryFault && fault_addr) {
    *fault_addr = c.fault_addr;
  }
  return c.status;
}
