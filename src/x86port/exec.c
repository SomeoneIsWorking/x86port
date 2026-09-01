/* exec.c -- see exec.h for why every outcome is named. */
#include "exec.h"

#include <string.h>

static const char *kStatusNames[] = {
    "ok", "decode-failed", "unsupported", "fetch-fault", "memory-fault", "divide-error"};
_Static_assert((int)(sizeof kStatusNames / sizeof kStatusNames[0]) == (int)kX86pStepStatusCount,
               "every X86pStepStatus needs a name");

const char *x86p_step_status_name(X86pStepStatus s) {
  if ((unsigned)s >= (unsigned)kX86pStepStatusCount) {
    return "unknown";
  }
  return kStatusNames[(int)s];
}

/*
 * One step's working state. Passed around instead of a dozen parameters, and
 * it carries `fault` so a memory access deep inside an operand read can report
 * without every helper returning a status nobody would check.
 */
typedef struct Ctx {
  X86pCpu *cpu;
  const X86pMem *mem;
  const X86pInsn *insn;
  uint32_t next_eip; /* where execution goes unless the instruction branches */
  int fault;         /* an X86pStepStatus, or 0 */
  uint32_t fault_addr;
} Ctx;

static uint32_t effective_address(Ctx *c, const X86pOperand *o) {
  uint32_t addr = (uint32_t)o->disp;
  if (o->base >= 0) {
    addr += x86p_reg_read(c->cpu, o->base, 4);
  }
  if (o->index >= 0) {
    addr += x86p_reg_read(c->cpu, o->index, 4) * (uint32_t)o->scale;
  }
  /* Wraps at 32 bits, which is what the hardware does and what a guest with a
     negative displacement off a low base relies on. */
  return addr;
}

static uint32_t read_operand(Ctx *c, const X86pOperand *o) {
  uint32_t v = 0;
  switch (o->kind) {
  case kX86pOperandReg:
    return x86p_reg_read(c->cpu, o->reg, o->size);
  case kX86pOperandImm:
    return o->imm;
  case kX86pOperandMem: {
    uint32_t addr = effective_address(c, o);
    if (!x86p_mem_read(c->mem, addr, o->size, &v)) {
      c->fault = kX86pStepMemoryFault;
      c->fault_addr = addr;
      return 0;
    }
    return v;
  }
  default:
    /* Not reachable: decode refuses an operand shape it cannot model, so an
       instruction carrying one never gets here. Named anyway, because "not
       reachable" is a claim about today's decoder. */
    c->fault = kX86pStepUnsupported;
    return 0;
  }
}

static void write_operand(Ctx *c, const X86pOperand *o, uint32_t value) {
  switch (o->kind) {
  case kX86pOperandReg:
    x86p_reg_write(c->cpu, o->reg, o->size, value);
    return;
  case kX86pOperandMem: {
    uint32_t addr = effective_address(c, o);
    if (!x86p_mem_write(c->mem, addr, o->size, value)) {
      c->fault = kX86pStepMemoryFault;
      c->fault_addr = addr;
    }
    return;
  }
  default:
    /* An immediate destination is not an instruction; refusing is the only
       honest answer, and silently discarding the write would make a
       mis-decoded instruction look like it executed. */
    c->fault = kX86pStepUnsupported;
    return;
  }
}

/* A branch target: an absolute immediate, a relative displacement measured
   from the NEXT instruction, or a register/memory operand for an indirect
   jump. */
static uint32_t branch_target(Ctx *c, const X86pOperand *o) {
  if (o->kind == kX86pOperandImm) {
    return o->relative ? (c->next_eip + o->imm) : o->imm;
  }
  return read_operand(c, o);
}

static uint32_t sign_extend(uint32_t v, int from_bytes) {
  switch (from_bytes) {
  case 1:
    return (uint32_t)(int32_t)(int8_t)(v & 0xFFu);
  case 2:
    return (uint32_t)(int32_t)(int16_t)(v & 0xFFFFu);
  default:
    return v;
  }
}

/* The implicit accumulator/data pair the widening multiplies and divides use.
   Named rather than open-coded so the width relationship is stated once: the
   pair is EAX:EDX at dword width, AX:DX at word, and AH:AL -- one register --
   at byte, which is the case that does not fit the pattern. */
static void mul_result_store(Ctx *c, int w, uint32_t lo, uint32_t hi) {
  if (w == 1) {
    x86p_reg_write(c->cpu, kX86pEax, 2, (lo & 0xFFu) | ((hi & 0xFFu) << 8));
    return;
  }
  x86p_reg_write(c->cpu, kX86pEax, w, lo);
  x86p_reg_write(c->cpu, kX86pEdx, w, hi);
}

static void div_operands(Ctx *c, int w, uint32_t *hi, uint32_t *lo) {
  if (w == 1) {
    uint32_t ax = x86p_reg_read(c->cpu, kX86pEax, 2);
    *hi = (ax >> 8) & 0xFFu;
    *lo = ax & 0xFFu;
    return;
  }
  *hi = x86p_reg_read(c->cpu, kX86pEdx, w);
  *lo = x86p_reg_read(c->cpu, kX86pEax, w);
}

static void div_result_store(Ctx *c, int w, uint32_t quot, uint32_t rem) {
  if (w == 1) {
    x86p_reg_write(c->cpu, kX86pEax, 2, (quot & 0xFFu) | ((rem & 0xFFu) << 8));
    return;
  }
  x86p_reg_write(c->cpu, kX86pEax, w, quot);
  x86p_reg_write(c->cpu, kX86pEdx, w, rem);
}

static void execute(Ctx *c) {
  const X86pInsn *in = c->insn;
  const X86pOperand *o0 = &in->operand[0];
  const X86pOperand *o1 = &in->operand[1];
  X86pCpu *cpu = c->cpu;

  switch ((X86pInsnOp)in->op) {
  case kX86pInsnNop:
    return;

  case kX86pInsnAlu: {
    uint32_t a = read_operand(c, o0);
    uint32_t b = read_operand(c, o1);
    uint32_t r;
    if (c->fault) {
      return;
    }
    r = x86p_alu((X86pAluOp)in->alu, a, b, o0->size, &cpu->flags);
    /* CMP and TEST compute exactly the same thing and discard it. That is
       why they are the same ALU call with a different destination policy,
       rather than two more operations that could drift from SUB and AND. */
    if (in->alu != kX86pAluCmp && in->alu != kX86pAluTest) {
      write_operand(c, o0, r);
    }
    return;
  }

  case kX86pInsnAluUnary: {
    uint32_t a = read_operand(c, o0);
    uint32_t r;
    if (c->fault) {
      return;
    }
    r = x86p_alu_unary((X86pAluUnOp)in->alu, a, o0->size, &cpu->flags);
    write_operand(c, o0, r);
    return;
  }

  case kX86pInsnMov: {
    uint32_t v = read_operand(c, o1);
    if (c->fault) {
      return;
    }
    write_operand(c, o0, v);
    return;
  }

  case kX86pInsnMovzx:
  case kX86pInsnMovsx: {
    /* The widths DIFFER between source and destination -- that is the whole
       instruction. Reading at the destination's width would read too much and
       look right whenever the extra bytes happened to be zero. */
    uint32_t v = read_operand(c, o1);
    if (c->fault) {
      return;
    }
    if (in->op == kX86pInsnMovsx) {
      v = sign_extend(v, o1->size);
    }
    write_operand(c, o0, v);
    return;
  }

  case kX86pInsnLea:
    /* The ADDRESS, never the contents. LEA is the one instruction with a
       memory operand it does not access -- computing it as a read would fault
       on an address the guest never intended to touch. */
    if (o1->kind != kX86pOperandMem) {
      c->fault = kX86pStepUnsupported;
      return;
    }
    write_operand(c, o0, effective_address(c, o1));
    return;

  case kX86pInsnPush: {
    uint32_t v = read_operand(c, o0);
    if (c->fault) {
      return;
    }
    if (o0->size == 1 || o0->size == 2) {
      v = sign_extend(v, o0->size);
    }
    if (!x86p_push32(cpu, c->mem, v)) {
      c->fault = kX86pStepMemoryFault;
      c->fault_addr = cpu->reg[kX86pEsp] - 4u;
    }
    return;
  }

  case kX86pInsnPop: {
    uint32_t v;
    if (!x86p_pop32(cpu, c->mem, &v)) {
      c->fault = kX86pStepMemoryFault;
      c->fault_addr = cpu->reg[kX86pEsp];
      return;
    }
    write_operand(c, o0, v);
    return;
  }

  case kX86pInsnXchg: {
    uint32_t a = read_operand(c, o0);
    uint32_t b = read_operand(c, o1);
    if (c->fault) {
      return;
    }
    write_operand(c, o0, b);
    if (c->fault) {
      return;
    }
    write_operand(c, o1, a);
    return;
  }

  case kX86pInsnJmp: {
    uint32_t t = branch_target(c, o0);
    if (c->fault) {
      return;
    }
    c->next_eip = t;
    return;
  }

  case kX86pInsnJcc: {
    uint32_t t = branch_target(c, o0);
    if (c->fault) {
      return;
    }
    if (x86p_cond((X86pCond)in->cond, &cpu->flags)) {
      c->next_eip = t;
    }
    return;
  }

  case kX86pInsnSetcc:
    write_operand(c, o0, (uint32_t)(x86p_cond((X86pCond)in->cond, &cpu->flags) ? 1 : 0));
    return;

  case kX86pInsnCmovcc: {
    uint32_t v = read_operand(c, o1);
    if (c->fault) {
      return;
    }
    /* The source is read WHETHER OR NOT the move happens, because the hardware
       does and a faulting read must fault either way. Only the write is
       conditional. */
    if (x86p_cond((X86pCond)in->cond, &cpu->flags)) {
      write_operand(c, o0, v);
    }
    return;
  }

  case kX86pInsnCall: {
    uint32_t t = branch_target(c, o0);
    if (c->fault) {
      return;
    }
    if (!x86p_push32(cpu, c->mem, c->next_eip)) {
      c->fault = kX86pStepMemoryFault;
      c->fault_addr = cpu->reg[kX86pEsp] - 4u;
      return;
    }
    c->next_eip = t;
    return;
  }

  case kX86pInsnRet: {
    uint32_t ra;
    if (!x86p_pop32(cpu, c->mem, &ra)) {
      c->fault = kX86pStepMemoryFault;
      c->fault_addr = cpu->reg[kX86pEsp];
      return;
    }
    /* RET imm16 releases the caller's arguments as well. Applied AFTER the
       pop, since the immediate is a count of bytes above the return
       address. */
    if (in->operands >= 1 && o0->kind == kX86pOperandImm) {
      cpu->reg[kX86pEsp] += o0->imm;
    }
    c->next_eip = ra;
    return;
  }

  case kX86pInsnLeave: {
    uint32_t ebp;
    cpu->reg[kX86pEsp] = cpu->reg[kX86pEbp];
    if (!x86p_pop32(cpu, c->mem, &ebp)) {
      c->fault = kX86pStepMemoryFault;
      c->fault_addr = cpu->reg[kX86pEsp];
      return;
    }
    cpu->reg[kX86pEbp] = ebp;
    return;
  }

  case kX86pInsnCdq: {
    /* EDX becomes the sign of EAX. Not "EDX = 0 if positive": written as an
       arithmetic shift so the intent survives. */
    uint32_t eax = x86p_reg_read(cpu, kX86pEax, 4);
    x86p_reg_write(cpu, kX86pEdx, 4, (eax & 0x80000000u) ? 0xFFFFFFFFu : 0u);
    return;
  }

  case kX86pInsnCwde:
    x86p_reg_write(cpu, kX86pEax, 4, sign_extend(x86p_reg_read(cpu, kX86pEax, 2), 2));
    return;

  case kX86pInsnMul:
  case kX86pInsnImul: {
    int w = o0->size;
    uint32_t lo = 0, hi = 0;
    if (in->operands == 1) {
      /* The one-operand form: the accumulator is implicit on both sides. */
      uint32_t a = x86p_reg_read(cpu, kX86pEax, w);
      uint32_t b = read_operand(c, o0);
      if (c->fault) {
        return;
      }
      if (in->op == kX86pInsnMul) {
        x86p_alu_mul(a, b, w, &lo, &hi, &cpu->flags);
      } else {
        x86p_alu_imul(a, b, w, &lo, &hi, &cpu->flags);
      }
      mul_result_store(c, w, lo, hi);
      return;
    }
    if (in->op == kX86pInsnMul) {
      c->fault = kX86pStepUnsupported; /* MUL has no two-operand form */
      return;
    }
    {
      /* IMUL's two- and three-operand forms keep only the low half, in the
         named destination. The flags still report whether the full product
         fit, which is the only way the guest can tell it overflowed. */
      uint32_t a = (in->operands == 3) ? read_operand(c, o1) : read_operand(c, o0);
      uint32_t b = (in->operands == 3) ? read_operand(c, &in->operand[2]) : read_operand(c, o1);
      if (c->fault) {
        return;
      }
      x86p_alu_imul(a, b, o0->size, &lo, &hi, &cpu->flags);
      write_operand(c, o0, lo);
      return;
    }
  }

  case kX86pInsnDiv:
  case kX86pInsnIdiv: {
    int w = o0->size;
    uint32_t hi, lo, d, q = 0, r = 0;
    int ok;
    div_operands(c, w, &hi, &lo);
    d = read_operand(c, o0);
    if (c->fault) {
      return;
    }
    ok = (in->op == kX86pInsnDiv) ? x86p_alu_div(hi, lo, d, w, &q, &r, &cpu->flags)
                                  : x86p_alu_idiv(hi, lo, d, w, &q, &r, &cpu->flags);
    if (!ok) {
      /* A guest-visible exception, delivered as a status. Nothing is written,
         so a caller that emulates the #DE handler sees the machine exactly as
         the faulting instruction found it. */
      c->fault = kX86pStepDivideError;
      return;
    }
    div_result_store(c, w, q, r);
    return;
  }

  case kX86pInsnPushfd:
    if (!x86p_push32(cpu, c->mem, x86p_eflags(&cpu->flags))) {
      c->fault = kX86pStepMemoryFault;
      c->fault_addr = cpu->reg[kX86pEsp] - 4u;
    }
    return;

  case kX86pInsnPopfd: {
    uint32_t v;
    if (!x86p_pop32(cpu, c->mem, &v)) {
      c->fault = kX86pStepMemoryFault;
      c->fault_addr = cpu->reg[kX86pEsp];
      return;
    }
    x86p_flags_set_explicit(&cpu->flags, v);
    return;
  }

  case kX86pInsnUnsupported:
  case kX86pInsnOpCount:
    break;
  }
  c->fault = kX86pStepUnsupported;
}

X86pStepStatus x86p_step(X86pCpu *cpu, const X86pMem *mem, X86pStepReport *report) {
  uint8_t bytes[X86P_MAX_INSN_LEN];
  X86pInsn insn;
  Ctx c;
  uint32_t avail, i;
  X86pStepStatus st;

  if (report) {
    memset(report, 0, sizeof *report);
    report->mnemonic = "?";
  }
  if (!cpu) {
    return kX86pStepFetchFault;
  }
  if (report) {
    report->eip = cpu->eip;
  }

  /*
   * Fetch as much as is mapped, up to the maximum instruction length, rather
   * than demanding all 15 bytes. A perfectly good instruction can end one byte
   * before the end of a mapping, and requiring the full window would fault on
   * code the guest executes -- a fault the guest never took, which is worse
   * than the one it did.
   */
  avail = 0;
  for (i = 0; i < X86P_MAX_INSN_LEN; i++) {
    uint32_t b;
    if (!x86p_mem_read(mem, cpu->eip + i, 1, &b)) {
      break;
    }
    bytes[i] = (uint8_t)b;
    avail++;
  }
  if (avail == 0) {
    if (report) {
      report->status = kX86pStepFetchFault;
      report->fault_addr = cpu->eip;
    }
    return kX86pStepFetchFault;
  }

  if (!x86p_decode(bytes, avail, &insn)) {
    if (report) {
      report->status = kX86pStepDecodeFailed;
      report->fault_addr = cpu->eip;
    }
    return kX86pStepDecodeFailed;
  }
  if (report) {
    report->length = insn.length;
    report->mnemonic = insn.mnemonic;
    report->op = insn.op;
  }
  if (insn.op == kX86pInsnUnsupported) {
    /* NAMED, and EIP is left pointing at it, so the caller can report it, hand
       it to another engine, or add semantics for it. The mnemonic in the
       report is what makes the unmodelled set a ranked work list. */
    if (report) {
      report->status = kX86pStepUnsupported;
    }
    return kX86pStepUnsupported;
  }

  memset(&c, 0, sizeof c);
  c.cpu = cpu;
  c.mem = mem;
  c.insn = &insn;
  c.next_eip = cpu->eip + insn.length;
  execute(&c);

  if (c.fault) {
    /* EIP is NOT advanced. The machine may be partly modified -- x86 itself
       has instructions that fault midway -- but the caller can always say
       which instruction it was, which is the part that matters for a
       divergence report. */
    st = (X86pStepStatus)c.fault;
    if (report) {
      report->status = (uint8_t)st;
      report->fault_addr = c.fault_addr;
    }
    return st;
  }
  cpu->eip = c.next_eip;
  if (report) {
    report->status = kX86pStepOk;
  }
  return kX86pStepOk;
}
