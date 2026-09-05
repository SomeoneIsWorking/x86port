/* exec.c -- see exec.h for why every outcome is named. */
#include "exec.h"

#include "bcd.h"
#include "bit_ops.h"
#include "cpuid.h"
#include "privilege.h"
#include "simd.h"
#include "string_ops.h"

#include "x87_exec.h"

#include <string.h>

static const char *kStatusNames[] = {"ok",
                                     "decode-failed",
                                     "unsupported",
                                     "fetch-fault",
                                     "memory-fault",
                                     "divide-error",
                                     "interrupt",
                                     "protection-fault",
                                     "bound-range"};
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
  /*
   * A 16-bit address wraps within its sixteen bits, and it does so BEFORE the
   * segment base is added -- which is the whole observable difference between
   * [BX+disp] and [EBX+disp], and the reason the decoder records the address
   * size rather than letting the two share a path.
   */
  if (o->addr16) {
    addr &= 0xFFFFu;
  }
  /*
   * The segment base, for the two segments that have one.
   *
   * ES, CS, SS and DS are flat in every 32-bit Win32 process, so there is
   * nothing to add and nothing to load; FS is how a guest reaches its TEB.
   * Written as a switch on a value the DECODER already resolved, so the fast
   * path costs a compare rather than an indexed load on every access.
   */
  if (o->seg == (uint8_t)kX86pSegFs) {
    addr += c->cpu->fs_base;
  } else if (o->seg == (uint8_t)kX86pSegGs) {
    addr += c->cpu->gs_base;
  }
  /* Wraps at 32 bits, which is what the hardware does and what a guest with a
     negative displacement off a low base relies on. */
  return addr;
}

/*
 * Where a far transfer goes: either both halves are in the instruction
 * (ptr16:32) or both are adjacent in memory (m16:32), offset first.
 *
 * Returns 0 having set a fault. A far transfer that could not resolve its
 * destination must not fall through to a near one -- the stack effects differ
 * -- so there is no "best effort" return here.
 */
static int far_target(Ctx *c, const X86pOperand *o, uint32_t *offset, uint16_t *selector) {
  uint32_t addr, off = 0, sel = 0;
  if (o->kind == kX86pOperandFarPtr) {
    *offset = o->imm;
    *selector = o->selector;
    return 1;
  }
  if (o->kind != kX86pOperandMem) {
    c->fault = kX86pStepUnsupported;
    return 0;
  }
  addr = effective_address(c, o);
  if (!x86p_mem_read(c->mem, addr, 4, &off) || !x86p_mem_read(c->mem, addr + 4u, 2, &sel)) {
    c->fault = kX86pStepMemoryFault;
    c->fault_addr = addr;
    return 0;
  }
  *offset = off;
  *selector = (uint16_t)sel;
  return 1;
}

static uint32_t read_operand(Ctx *c, const X86pOperand *o) {
  uint32_t v = 0;
  switch (o->kind) {
  case kX86pOperandReg:
    return x86p_reg_read(c->cpu, o->reg, o->size);
  case kX86pOperandSeg:
    /* Zero-extended: MOV r32, Sreg writes the selector into the low sixteen
       bits and clears the rest, it does not preserve them. */
    return c->cpu->seg[o->reg];
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
  case kX86pOperandSeg:
    /* The selector only. Loading a segment register also reloads its hidden
       descriptor on real hardware; in a flat process the descriptor never
       changes, which is why this framework can hold a selector and be right --
       and why cpu.h states the flat model as a contract rather than leaving it
       implied. */
    c->cpu->seg[o->reg] = (uint16_t)value;
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

  case kX86pInsnX87: {
    /*
     * The FPU is a separate state machine and lives in its own module. What
     * stays here is the ADDRESSING: effective-address computation is this
     * file's job, so the address is resolved once and handed over rather than
     * computed a second time next to the floating-point semantics.
     */
    int has_mem = 0;
    uint32_t addr = 0;
    uint32_t fault_addr = 0;
    X86pX87ExecStatus s;
    int i;
    for (i = 0; i < in->operands; i++) {
      if (in->operand[i].kind == kX86pOperandMem) {
        has_mem = 1;
        addr = effective_address(c, &in->operand[i]);
        break;
      }
    }
    s = x86p_x87_execute(cpu, c->mem, in, has_mem, addr, &fault_addr);
    if (s == kX86pX87ExecMemoryFault) {
      c->fault = kX86pStepMemoryFault;
      c->fault_addr = fault_addr;
    } else if (s == kX86pX87ExecUnsupported) {
      /* An x87 instruction this build decodes but does not run. Reported as
         Unsupported, which is the same outcome an unmodelled integer
         instruction gets -- and the mnemonic in the report is what keeps the
         remaining work a ranked list rather than a guess. */
      c->fault = kX86pStepUnsupported;
    }
    return;
  }

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
      v = x86p_sign_extend(v, o1->size);
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
      v = x86p_sign_extend(v, o0->size);
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

  case kX86pInsnCallFar: {
    /* Push CS, then the return offset, then load both. The order is the one
       the hardware uses and the one RETF unwinds, so it is not free to be
       convenient: CS ends up at the higher address. */
    uint32_t off = 0;
    uint16_t sel = 0;
    if (!far_target(c, o0, &off, &sel)) {
      return;
    }
    if (!x86p_push32(cpu, c->mem, cpu->seg[kX86pSegCs]) || !x86p_push32(cpu, c->mem, c->next_eip)) {
      c->fault = kX86pStepMemoryFault;
      c->fault_addr = cpu->reg[kX86pEsp] - 4u;
      return;
    }
    cpu->seg[kX86pSegCs] = sel;
    c->next_eip = off;
    return;
  }

  case kX86pInsnJmpFar: {
    uint32_t off = 0;
    uint16_t sel = 0;
    if (!far_target(c, o0, &off, &sel)) {
      return;
    }
    cpu->seg[kX86pSegCs] = sel;
    c->next_eip = off;
    return;
  }

  case kX86pInsnRetf: {
    uint32_t ra = 0, sel = 0;
    if (!x86p_pop32(cpu, c->mem, &ra) || !x86p_pop32(cpu, c->mem, &sel)) {
      c->fault = kX86pStepMemoryFault;
      c->fault_addr = cpu->reg[kX86pEsp];
      return;
    }
    /* Like the near form, RETF imm16 releases the caller's arguments after
       both words are off the stack. */
    if (in->operands >= 1 && o0->kind == kX86pOperandImm) {
      cpu->reg[kX86pEsp] += o0->imm;
    }
    cpu->seg[kX86pSegCs] = (uint16_t)sel;
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
    x86p_reg_write(cpu, kX86pEax, 4, x86p_sign_extend(x86p_reg_read(cpu, kX86pEax, 2), 2));
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

  case kX86pInsnSimd: {
    uint32_t fault = 0u;
    switch (x86p_simd_execute(cpu, c->mem, in, &fault)) {
    case kX86pSimdOk:
      return;
    case kX86pSimdFault:
      c->fault = kX86pStepMemoryFault;
      c->fault_addr = fault;
      return;
    case kX86pSimdUnsupported:
    case kX86pSimdStatusCount:
    default:
      /* NAMED, not swallowed: the approximation instructions are refused on
         purpose and a caller must be able to say which one it met. */
      c->fault = kX86pStepUnsupported;
      return;
    }
  }

  case kX86pInsnCld:
    cpu->df = 0u;
    return;

  case kX86pInsnStd:
    cpu->df = 1u;
    return;

  case kX86pInsnString: {
    uint32_t fault = 0u;
    switch (x86p_string_execute(cpu, c->mem, in, &fault)) {
    case kX86pStringOk:
      return;
    case kX86pStringFault:
      c->fault = kX86pStepMemoryFault;
      c->fault_addr = fault;
      return;
    case kX86pStringUnsupported:
    case kX86pStringStatusCount:
    default:
      c->fault = kX86pStepUnsupported;
      return;
    }
  }

  /*
   * PUSHFD and POPFD are where the two representations of EFLAGS meet: the six
   * arithmetic flags are derived from the lazy (kind, a, b, r) record, and DF
   * is held apart because nothing computes it. Both halves have to cross here
   * or a guest that saves and restores its flags loses its direction flag --
   * silently, and only visibly in a string loop that then runs backwards.
   */
  case kX86pInsnPushfd:
    if (!x86p_push32(cpu, c->mem, x86p_eflags(&cpu->flags) | (cpu->df ? X86P_DF : 0u))) {
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
    cpu->df = (v & X86P_DF) ? 1u : 0u;
    return;
  }

    /* ---- the decimal adjustments, the bit tests, and the rest of the integer
       tail. Defined behavior follows the ISA; undefined flags use the semantic
       owners' deterministic policies. */

  case kX86pInsnBcd: {
    uint16_t ax = (uint16_t)x86p_reg_read(cpu, kX86pEax, 2);
    uint32_t f = x86p_eflags(&cpu->flags);
    /* AAM and AAD carry the base as an immediate; the others have no operand
       and read whatever this is, which is why it defaults to ten rather than
       to zero -- AAM with a base of zero is a divide error, and inventing one
       for DAA would turn a correct instruction into a fault. */
    const uint8_t imm = (in->operands > 0 && o0->kind == kX86pOperandImm) ? (uint8_t)o0->imm : 10u;
    if (!x86p_bcd_apply((X86pBcdOp)in->bcd, &ax, &f, imm)) {
      c->fault = kX86pStepDivideError;
      return;
    }
    x86p_reg_write(cpu, kX86pEax, 2, ax);
    x86p_flags_set_explicit(&cpu->flags, f);
    return;
  }

  case kX86pInsnBit: {
    uint32_t f = x86p_eflags(&cpu->flags);
    uint32_t value, updated;
    unsigned bit;
    const int w = o0->size;

    if (o1->kind == kX86pOperandImm) {
      /* An immediate offset is taken modulo the operand width and never
         leaves the operand. */
      bit = (unsigned)o1->imm & (unsigned)(w * 8 - 1);
      value = read_operand(c, o0);
      if (c->fault) {
        return;
      }
      updated = x86p_bit_apply((X86pBitOp)in->bit, value, bit, &f);
      if (in->bit != (uint8_t)kX86pBitTest) {
        write_operand(c, o0, updated);
      }
    } else if (o0->kind == kX86pOperandMem) {
      /*
       * BIT-STRING ADDRESSING. With a register offset and a memory operand the
       * offset is SIGNED and unbounded: the instruction addresses a bit in a
       * string that may start whole operands before or after the address the
       * modrm computed. A model that masked the offset to the operand width
       * instead -- the obvious simplification -- reads the right bit only for
       * offsets under 32, and a guest walking a bitmap with BT [base], eax is
       * exactly the code that exceeds that.
       */
      const int32_t off = (int32_t)read_operand(c, o1);
      const int32_t stride = (int32_t)(int)w * 8;
      int32_t unit = off / stride;
      int32_t rem = off % stride;
      X86pOperand adjusted;
      if (rem < 0) { /* C truncates toward zero; the bit index must not go negative */
        unit -= 1;
        rem += stride;
      }
      adjusted = *o0;
      adjusted.disp = (int32_t)((uint32_t)o0->disp + (uint32_t)(unit * (int32_t)w));
      bit = (unsigned)rem;
      value = read_operand(c, &adjusted);
      if (c->fault) {
        return;
      }
      updated = x86p_bit_apply((X86pBitOp)in->bit, value, bit, &f);
      if (in->bit != (uint8_t)kX86pBitTest) {
        write_operand(c, &adjusted, updated);
      }
    } else {
      bit = (unsigned)read_operand(c, o1) & (unsigned)(w * 8 - 1);
      value = read_operand(c, o0);
      updated = x86p_bit_apply((X86pBitOp)in->bit, value, bit, &f);
      if (in->bit != (uint8_t)kX86pBitTest) {
        write_operand(c, o0, updated);
      }
    }
    if (c->fault) {
      return;
    }
    x86p_flags_set_explicit(&cpu->flags, f);
    return;
  }

  case kX86pInsnShld:
  case kX86pInsnShrd: {
    const X86pOperand *o2 = &in->operand[2];
    uint32_t f = x86p_eflags(&cpu->flags);
    uint32_t dst = read_operand(c, o0);
    uint32_t src = read_operand(c, o1);
    uint32_t r = 0;
    unsigned count;
    int defined = 0;
    if (c->fault) {
      return;
    }
    count = (unsigned)read_operand(c, o2);
    if (c->fault) {
      return;
    }
    if (!x86p_double_shift(in->op == (uint8_t)kX86pInsnShld, dst, src, count, o0->size, &r, &f, &defined)) {
      /* Either a masked count of zero -- which writes nothing at all, flags
         included -- or a count past the operand width, which the SDM leaves
         undefined. Neither writes a result here; refusing to invent one is
         the point of x86p_double_shift's `defined` output. */
      if (!defined) {
        c->fault = kX86pStepUnsupported;
      }
      return;
    }
    write_operand(c, o0, r);
    if (c->fault) {
      return;
    }
    x86p_flags_set_explicit(&cpu->flags, f);
    return;
  }

  case kX86pInsnSahf:
    x86p_cpu_sahf(cpu);
    return;
  case kX86pInsnLahf:
    x86p_cpu_lahf(cpu);
    return;

  case kX86pInsnStc:
    x86p_flags_set_explicit(&cpu->flags, x86p_eflags(&cpu->flags) | X86P_CF);
    return;

  case kX86pInsnClc:
    x86p_flags_set_explicit(&cpu->flags, x86p_eflags(&cpu->flags) & ~(uint32_t)X86P_CF);
    return;

  case kX86pInsnCmc:
    x86p_flags_set_explicit(&cpu->flags, x86p_eflags(&cpu->flags) ^ X86P_CF);
    return;

  case kX86pInsnSalc:
    /* Undocumented in the SDM and present on every x86: "set AL from carry".
       Modelled because it decodes, and an instruction that decodes will be
       reached by something eventually. */
    x86p_reg_write(cpu, kX86pEax, 1, x86p_flag_cf(&cpu->flags) ? 0xFFu : 0x00u);
    return;

  case kX86pInsnXlat: {
    const uint32_t al = x86p_reg_read(cpu, kX86pEax, 1);
    uint32_t addr = cpu->reg[kX86pEbx] + al;
    uint32_t v = 0;
    /* The segment is whatever prefix the encoding carried; Zydis reports it on
       the implicit memory operand, which is the only place it exists. */
    if (in->operands > 0 && o0->kind == kX86pOperandMem) {
      if (o0->seg == (uint8_t)kX86pSegFs) {
        addr += cpu->fs_base;
      } else if (o0->seg == (uint8_t)kX86pSegGs) {
        addr += cpu->gs_base;
      }
    }
    if (!x86p_mem_read(c->mem, addr, 1, &v)) {
      c->fault = kX86pStepMemoryFault;
      c->fault_addr = addr;
      return;
    }
    x86p_reg_write(cpu, kX86pEax, 1, v);
    return;
  }

  case kX86pInsnPushad: {
    /* ESP is pushed as it was BEFORE the instruction, so the saved value is
       the frame the guest had, not the one four bytes into this push. */
    static const int kOrder[] = {kX86pEax, kX86pEcx, kX86pEdx, kX86pEbx, -1, kX86pEbp, kX86pEsi, kX86pEdi};
    const uint32_t esp0 = cpu->reg[kX86pEsp];
    unsigned i;
    for (i = 0; i < 8u; i++) {
      const uint32_t v = (kOrder[i] < 0) ? esp0 : cpu->reg[kOrder[i]];
      if (!x86p_push32(cpu, c->mem, v)) {
        c->fault = kX86pStepMemoryFault;
        c->fault_addr = cpu->reg[kX86pEsp] - 4u;
        return;
      }
    }
    return;
  }

  case kX86pInsnPopad: {
    /* The reverse order, and the saved ESP is DISCARDED rather than loaded --
       loading it would undo the eight pops this instruction just performed. */
    static const int kOrder[] = {kX86pEdi, kX86pEsi, kX86pEbp, -1, kX86pEbx, kX86pEdx, kX86pEcx, kX86pEax};
    unsigned i;
    for (i = 0; i < 8u; i++) {
      uint32_t v;
      if (!x86p_pop32(cpu, c->mem, &v)) {
        c->fault = kX86pStepMemoryFault;
        c->fault_addr = cpu->reg[kX86pEsp];
        return;
      }
      if (kOrder[i] >= 0) {
        cpu->reg[kOrder[i]] = v;
      }
    }
    return;
  }

  case kX86pInsnEnter: {
    const uint32_t alloc = (uint32_t)o0->imm & 0xFFFFu;
    const unsigned level = (unsigned)o1->imm & 0x1Fu;
    uint32_t frame;
    unsigned i;
    if (!x86p_push32(cpu, c->mem, cpu->reg[kX86pEbp])) {
      c->fault = kX86pStepMemoryFault;
      c->fault_addr = cpu->reg[kX86pEsp] - 4u;
      return;
    }
    frame = cpu->reg[kX86pEsp];
    /* The nesting level copies the enclosing frames' pointers into the new
       frame. No C compiler emits a non-zero level, but the instruction has
       one and a decoder that reaches ENTER can reach ENTER 8,3. */
    for (i = 1; i < level; i++) {
      uint32_t v;
      cpu->reg[kX86pEbp] -= 4u;
      if (!x86p_mem_read(c->mem, cpu->reg[kX86pEbp], 4, &v) || !x86p_push32(cpu, c->mem, v)) {
        c->fault = kX86pStepMemoryFault;
        c->fault_addr = cpu->reg[kX86pEbp];
        return;
      }
    }
    if (level > 0u && !x86p_push32(cpu, c->mem, frame)) {
      c->fault = kX86pStepMemoryFault;
      c->fault_addr = cpu->reg[kX86pEsp] - 4u;
      return;
    }
    cpu->reg[kX86pEbp] = frame;
    cpu->reg[kX86pEsp] -= alloc;
    return;
  }

  case kX86pInsnLoop:
  case kX86pInsnLoope:
  case kX86pInsnLoopne: {
    const int condition = in->op == kX86pInsnLoope ? 1 : in->op == kX86pInsnLoopne ? -1 : 0;
    const int take = x86p_cpu_loop(cpu, in->address_width, condition);
    if (take) {
      const uint32_t t = branch_target(c, o0);
      if (c->fault) {
        return;
      }
      c->next_eip = t;
    }
    return;
  }

  case kX86pInsnJecxz:
    if (cpu->reg[kX86pEcx] == 0u) {
      const uint32_t t = branch_target(c, o0);
      if (c->fault) {
        return;
      }
      c->next_eip = t;
    }
    return;

    /* ---- traps, faults, and the instructions a ring-3 process may not run.
       None of these is a gap: each has a defined outcome and reports it. */

  case kX86pInsnInt3:
  case kX86pInsnInt1:
  case kX86pInsnInto:
  case kX86pInsnInt: {
    static const uint8_t kFixedVector[] = {3u, 1u, 4u};
    uint8_t vector;
    if (in->op == (uint8_t)kX86pInsnInt) {
      vector = (uint8_t)o0->imm;
    } else {
      vector = kFixedVector[in->op == (uint8_t)kX86pInsnInt3 ? 0 : (in->op == (uint8_t)kX86pInsnInt1 ? 1 : 2)];
    }
    if (in->op == (uint8_t)kX86pInsnInto && !x86p_flag_of(&cpu->flags)) {
      return; /* INTO with OF clear is a no-op, not a trap */
    }
    /* EIP still points AT the instruction, which is what a handler that wants
       to resume or report needs; advancing first would name the wrong site. */
    c->fault = kX86pStepInterrupt;
    cpu->trap_vector = vector;
    return;
  }

  case kX86pInsnHlt:
  case kX86pInsnWbinvd:
  case kX86pInsnCli:
  case kX86pInsnSti:
  case kX86pInsnPortIo:
    /* privilege.h owns the decision and the reasoning; asking it here rather
       than repeating the list keeps one authority for "may a ring-3 process do
       this". The assert is the exhaustiveness check: a kind added to one and
       not the other is a bug this catches at the first execution. */
    c->fault = x86p_insn_is_privileged(in) ? kX86pStepProtectionFault : kX86pStepUnsupported;
    return;

  case kX86pInsnBound: {
    /*
     * BOUND is an ordinary user-mode instruction, and the only one here whose
     * fault is conditional: it reads a PAIR of bounds from memory and traps
     * only when the index is outside them. The comparison is SIGNED.
     */
    const int32_t index = (int32_t)read_operand(c, o0);
    X86pOperand upper = *o1;
    int32_t lo, hi;
    if (c->fault) {
      return;
    }
    upper.size = o0->size;
    lo = (int32_t)x86p_sign_extend(read_operand(c, &upper), o0->size);
    upper.disp = (int32_t)((uint32_t)upper.disp + (uint32_t)o0->size);
    hi = (int32_t)x86p_sign_extend(read_operand(c, &upper), o0->size);
    if (c->fault) {
      return;
    }
    if (index < lo || index > hi) {
      c->fault = kX86pStepBoundRange;
    }
    return;
  }

  case kX86pInsnArpl: {
    /* Adjust the requested privilege level: if the destination's RPL is lower
       than the source's, raise it and say so in ZF. Legal at ring 3, and gone
       from 64-bit mode -- its opcode is now a REX prefix. */
    const uint32_t dst = read_operand(c, o0);
    const uint32_t src = read_operand(c, o1);
    if (c->fault) {
      return;
    }
    if ((dst & 3u) < (src & 3u)) {
      write_operand(c, o0, (dst & ~3u) | (src & 3u));
      x86p_flags_set_explicit(&cpu->flags, x86p_eflags(&cpu->flags) | X86P_ZF);
    } else {
      x86p_flags_set_explicit(&cpu->flags, x86p_eflags(&cpu->flags) & ~(uint32_t)X86P_ZF);
    }
    return;
  }

  case kX86pInsnSldt:
    /* The LDT selector. A flat Win32 process has no LDT of its own, and cpu.h
       states that flat model as a contract rather than leaving it implied --
       so this stores the selector the CPU holds, which is zero unless
       something set it. */
    write_operand(c, o0, cpu->ldtr);
    return;

  case kX86pInsnLfp: {
    /* LES/LDS/LFS/LGS/LSS: a 32-bit offset and a 16-bit selector, adjacent in
       memory. The selector is the HIGH half, whatever the operand size says. */
    X86pOperand part = *o1;
    uint32_t offset, selector;
    /* The decoded operand is the whole six-byte structure, so neither half can
       be read through it directly -- its size is the size of the pair. */
    part.size = o0->size;
    offset = read_operand(c, &part);
    if (c->fault) {
      return;
    }
    part.size = 2;
    part.disp = (int32_t)((uint32_t)o1->disp + (uint32_t)o0->size);
    selector = read_operand(c, &part);
    if (c->fault) {
      return;
    }
    write_operand(c, o0, offset);
    cpu->seg[in->seg_dest] = (uint16_t)selector;
    return;
  }

  case kX86pInsnIretd: {
    /* Pop EIP, CS and EFLAGS -- the frame an interrupt pushed. Legal at ring 3
       for a same-privilege return, which is the only kind this flat model has.
       No task switch, no stack switch: both need a descriptor table, and this
       process has neither. */
    uint32_t eip_v, cs_v, fl_v;
    if (!x86p_pop32(cpu, c->mem, &eip_v) || !x86p_pop32(cpu, c->mem, &cs_v) || !x86p_pop32(cpu, c->mem, &fl_v)) {
      c->fault = kX86pStepMemoryFault;
      c->fault_addr = cpu->reg[kX86pEsp];
      return;
    }
    cpu->seg[kX86pSegCs] = (uint16_t)cs_v;
    x86p_flags_set_explicit(&cpu->flags, fl_v);
    cpu->df = (fl_v & X86P_DF) ? 1u : 0u;
    c->next_eip = eip_v;
    return;
  }

  case kX86pInsnCpuid:
    x86p_cpu_cpuid(cpu);
    return;

  case kX86pInsnRdtsc:
    x86p_cpu_rdtsc(cpu);
    return;

  case kX86pInsnUnsupported:
  case kX86pInsnOpCount:
    break;
  }
  c->fault = kX86pStepUnsupported;
}

X86pStepStatus x86p_step(X86pCpu *cpu, const X86pMem *mem, X86pStepReport *report) {
  return x86p_step_cached(cpu, mem, NULL, report);
}

X86pStepStatus x86p_step_cached(X86pCpu *cpu, const X86pMem *mem, X86pDecodeCache *cache, X86pStepReport *report) {
  uint8_t bytes[X86P_MAX_INSN_LEN];
  X86pInsn insn;
  uint32_t avail;
  uint32_t fault_addr = 0u;
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
  avail = x86p_mem_readable_span(mem, cpu->eip, X86P_MAX_INSN_LEN);
  if (avail == 0 || !x86p_mem_read_bytes(mem, cpu->eip, bytes, avail)) {
    if (report) {
      report->status = kX86pStepFetchFault;
      report->fault_addr = cpu->eip;
    }
    return kX86pStepFetchFault;
  }

  if (!x86p_decode_cached(cache, cpu->eip, bytes, avail, &insn)) {
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

  st = x86p_execute_decoded(cpu, mem, &insn, &fault_addr);
  if (report) {
    report->status = (uint8_t)st;
    if (st != kX86pStepOk) {
      report->fault_addr = fault_addr;
    }
  }
  return st;
}

X86pStepStatus x86p_execute_decoded(X86pCpu *cpu, const X86pMem *mem, const X86pInsn *insn, uint32_t *fault_addr) {
  Ctx c;

  if (fault_addr) {
    *fault_addr = 0u;
  }
  if (!cpu || !insn) {
    return kX86pStepFetchFault;
  }
  if (insn->op == (uint8_t)kX86pInsnUnsupported) {
    /* Checked, not assumed. Advancing EIP past an instruction with no
       semantics would leave a machine that looks like it made progress. */
    return kX86pStepUnsupported;
  }

  memset(&c, 0, sizeof c);
  c.cpu = cpu;
  c.mem = mem;
  c.insn = insn;
  c.next_eip = cpu->eip + insn->length;
  execute(&c);

  if (c.fault) {
    /* EIP is NOT advanced. The machine may be partly modified -- x86 itself
       has instructions that fault midway -- but the caller can always say
       which instruction it was, which is the part that matters for a
       divergence report. */
    if (fault_addr) {
      *fault_addr = c.fault_addr;
    }
    return (X86pStepStatus)c.fault;
  }
  cpu->eip = c.next_eip;
  return kX86pStepOk;
}
