/* x87_exec.c -- see x87_exec.h for why addressing is not duplicated here. */
#include "x87_exec.h"

#include "cond.h"
#include "flags.h"
#include "x87_memory.h"
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
static void memory_status(Ctx *c, X86pX87MemoryStatus status) {
  if (status == kX86pX87MemoryFault) {
    fault(c);
  } else if (status == kX86pX87MemoryUnsupported) {
    c->status = kX86pX87ExecUnsupported;
  }
}
static long double read_value(Ctx *c, int size, int integer) {
  long double value = 0;
  memory_status(c, x86p_x87_read_value(c->mem, c->addr, (unsigned)size, integer, &value));
  return value;
}
static long double read_float(Ctx *c, int size) {
  return read_value(c, size, 0);
}
static long double read_integer(Ctx *c, int size) {
  return read_value(c, size, 1);
}
static void write_float(Ctx *c, int size, long double value) {
  memory_status(c, x86p_x87_write_value(c->fpu, c->mem, c->addr, (unsigned)size, 0, value));
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
    long double value;
    if (!c->has_mem) {
      c->status = kX86pX87ExecUnsupported;
      return;
    }
    if (!x86p_x87_get(f, 0, &value)) {
      return;
    }
    memory_status(c, x86p_x87_write_value(f, c->mem, c->addr, o0->size, 1, value));
    if (c->status == kX86pX87ExecOk) {
      do_pops(c, in->x87_pops);
    }
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
      x86p_x87_compare_register(f, o0->reg, in->x87_pops);
      return;
    } else if (in->operands == 0) {
      /* FCOMPP compares ST(0) with ST(1) implicitly, then pops both. */
      x86p_x87_compare_register(f, 1, in->x87_pops);
      return;
    } else {
      c->status = kX86pX87ExecUnsupported;
      return;
    }
    x86p_x87_compare(f, other);
    do_pops(c, in->x87_pops);
    return;
  }

  case kX86pX87InsnExchange:
    x86p_x87_exchange(f, (in->operands >= 1 && o0->kind == kX86pOperandSt) ? o0->reg : 1);
    return;
  case kX86pX87InsnChangeSign:
  case kX86pX87InsnAbs:
    x86p_x87_sign(f, in->x87 == kX86pX87InsnAbs);
    return;

  case kX86pX87InsnConstZero:
  case kX86pX87InsnConstOne:
  case kX86pX87InsnConstPi:
  case kX86pX87InsnConstLog2E:
  case kX86pX87InsnConstLog2T:
  case kX86pX87InsnConstLn2:
  case kX86pX87InsnConstLog102:
    (void)x86p_x87_push_constant(f, (X86pX87Insn)in->x87);
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
      x86p_x87_free(f, o0->reg);
    }
    return;

  case kX86pX87InsnInit:
    x86p_x87_reset(f);
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
    x86p_x87_clear_exceptions(f);
    return;

  case kX86pX87InsnTest:
    x86p_x87_test(f);
    return;

  case kX86pX87InsnCompareInt: {
    const int index = in->operands == 2                                   ? in->operand[1].reg
                      : (in->operands == 1 && o0->kind == kX86pOperandSt) ? o0->reg
                                                                          : 1;
    if (x86p_x87_compare_flags(f, &c->cpu->flags, index)) {
      do_pops(c, in->x87_pops);
    }
    return;
  }

  case kX86pX87InsnFn:
    if (!x86p_x87_apply_fn(f, (X86pX87Fn)in->x87_fn)) {
      c->status = kX86pX87ExecUnsupported;
    }
    return;

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
