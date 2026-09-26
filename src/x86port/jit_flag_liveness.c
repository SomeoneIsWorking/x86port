/* Dead flag store elimination for the native backends: see jit_flag_liveness.h. */
#include "jit_flag_liveness.h"

#include "alu.h"

/* An ALU operation that rewrites every flag from its own operands, reading
   none: ADC and SBB write them all too, but read CF first. */
static int alu_rewrites_all_flags(uint8_t alu) {
  switch (alu) {
  case kX86pAluAdd:
  case kX86pAluSub:
  case kX86pAluCmp:
  case kX86pAluOr:
  case kX86pAluAnd:
  case kX86pAluTest:
  case kX86pAluXor:
    return 1;
  default:
    return 0;
  }
}

int x86p_jit_flag_write_is_dead(const X86pJitFlagScan *scan, uint32_t pc) {
  int step;
  for (step = 0; step < 8; step++) {
    uint8_t bytes[X86P_MAX_INSN_LEN];
    uint32_t avail = 0;
    uint32_t i;
    X86pInsn insn;

    /* Would the emit loop still be running when it reached this instruction? */
    if (scan->count + 1u + (uint32_t)step >= scan->max_insns) {
      return 0;
    }
    if (scan->code_len + (size_t)(step + 2) * X86P_JIT_WORST_CASE_INSN_BYTES + X86P_JIT_EPILOGUE_BYTES >
        scan->code_cap) {
      return 0;
    }

    for (i = 0; i < (uint32_t)X86P_MAX_INSN_LEN; i++) {
      uint32_t byte;
      if (!x86p_mem_read(scan->mem, pc + i, 1, &byte)) {
        break;
      }
      bytes[i] = (uint8_t)byte;
      avail++;
    }
    if (avail == 0 || !x86p_decode(bytes, avail, &insn)) {
      return 0;
    }
    if (pc != scan->eip && scan->boundary && scan->boundary(pc, scan->boundary_user)) {
      return 0;
    }
    /* A later flag write is a valid killer only if the emit loop will reach
       and translate it. Shape-only checks below are intentionally narrower
       than the complete translation gate, so consulting them first could
       discard flags on the strength of an instruction the block then refuses. */
    if (!scan->can_emit(&insn)) {
      return 0;
    }

    if (insn.op == (uint8_t)kX86pInsnAlu) {
      const int registers_only = insn.operand[0].kind != kX86pOperandMem && insn.operand[1].kind != kX86pOperandMem;
      if (alu_rewrites_all_flags(insn.alu) && registers_only) {
        return 1;
      }
      if (x86p_alu_is_shift(insn.alu) && insn.operand[0].kind != kX86pOperandMem &&
          insn.operand[1].kind == kX86pOperandImm && (insn.operand[1].imm & 0x1Fu) != 0u) {
        return 1; /* a nonzero constant count rewrites the whole tuple */
      }
      return 0; /* CL shift / rotate / ADC / SBB / memory ALU: may keep, read CF, or fault */
    }
    if (insn.op == (uint8_t)kX86pInsnAluUnary) {
      if (insn.alu == (uint8_t)kX86pAluNeg && insn.operand[0].kind != kX86pOperandMem) {
        return 1; /* NEG rewrites every flag */
      }
      if (insn.alu == (uint8_t)kX86pAluNot && insn.operand[0].kind != kX86pOperandMem) {
        pc += insn.length; /* NOT writes no flags -- transparent */
        continue;
      }
      return 0; /* INC / DEC preserve (read) CF; memory forms can fault */
    }
    if (insn.op == (uint8_t)kX86pInsnNop || insn.op == (uint8_t)kX86pInsnLea) {
      pc += insn.length; /* LEA computes an address: no access, no flags */
      continue;
    }
    if (insn.op == (uint8_t)kX86pInsnMov && insn.operand[0].kind == kX86pOperandReg &&
        insn.operand[1].kind != kX86pOperandMem) {
      pc += insn.length; /* reg <- reg/imm : no flags, no fault */
      continue;
    }
    return 0;
  }
  return 0;
}
