/*
 * jit_arm64.c -- x86-32 guest basic block to AArch64 host code.
 *
 * The AArch64 counterpart of jit_x64.c: same translatable set, same shared
 * semantic authorities (x86p_alu, x86p_cond, x86p_flag_cf, ...), same named
 * refusal discipline -- see jit_x64.h for why arithmetic is called rather
 * than inlined, and jit_x64_internal.h/jit_arm64_internal.h for the register
 * role rationale. This file differs from jit_x64.c only in WHICH host
 * instructions each guest instruction becomes; the guest-facing contract
 * (X86pJitBlock, X86pJitExit, can-emit predicates) is identical by
 * construction, because both files implement the one public header,
 * jit_x64.h.
 *
 * REGISTER ROLES, AND WHY THEY DIFFER FROM jit_x64.c'S CHOICES. The x64
 * backend reuses two of its SysV argument registers (RDI, RCX) as role
 * registers (ADDR_TMP, CARRY_REG) during narrow windows, then reuses them as
 * plain call arguments elsewhere -- safe there because the file states each
 * window precisely. This backend does not need that trade: AAPCS64 gives it
 * eight argument/scratch registers (X0..X7) it never assigns a role to, so
 * every role register (X10..X15, plus CPU_REG=X19) stays live across every
 * helper call without exception, and a call's arguments are simply whichever
 * of X0..X7 the callee's signature puts them in. See jit_arm64_internal.h.
 */
#include "jit_x64.h"

#include "alu.h"
#include "cond.h"
#include "decode.h"
#include "emit_arm64.h"
#include "flags.h"
#include "jit_arm64_internal.h"
#include "jit_arm64_x87.h"
#include "simd.h"
#include "string_ops.h"
#include "three_dnow.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* The host register roles (CPU_REG, HOSTPTR_REG, ...) and MAX_INSNS live in
   jit_arm64_internal.h -- the x87 emission unit needs the same definitions. */

#define WORST_CASE_INSN_BYTES X86P_JIT_WORST_CASE_INSN_BYTES
#define EPILOGUE_BYTES X86P_JIT_EPILOGUE_BYTES

int x86p_jit_available(void) {
#if defined(__aarch64__)
  return 1;
#else
  return 0;
#endif
}

static void say(char *buf, unsigned len, const char *fmt, ...) {
  va_list ap;
  if (!buf || len == 0) {
    return;
  }
  va_start(ap, fmt);
  vsnprintf(buf, len, fmt, ap);
  va_end(ap);
}

/* ---- where things live in X86pCpu ---------------------------------------
 * Identical to jit_x64.c: offsets are taken from the real struct, never
 * written down as constants. */
static int32_t reg_off(int index) {
  return (int32_t)(offsetof(X86pCpu, reg) + (size_t)index * sizeof(uint32_t));
}

static int32_t eip_off(void) {
  return (int32_t)offsetof(X86pCpu, eip);
}

static int32_t flags_off(void) {
  return (int32_t)offsetof(X86pCpu, flags);
}

static int32_t flag_off(size_t field) {
  return (int32_t)(offsetof(X86pCpu, flags) + field);
}

#define FLAG_A flag_off(offsetof(X86pFlags, a))
#define FLAG_B flag_off(offsetof(X86pFlags, b))
#define FLAG_R flag_off(offsetof(X86pFlags, r))
#define FLAG_KIND flag_off(offsetof(X86pFlags, kind))
#define FLAG_W flag_off(offsetof(X86pFlags, w))
#define FLAG_CARRY_IN flag_off(offsetof(X86pFlags, carry_in))

/* ---- can this instruction be emitted? ------------------------------------
 * Every predicate below is a pure shape check over X86pInsn/X86pOperand, with
 * no host-register content at all -- identical to jit_x64.c by necessity,
 * since the translatable SET must be the same set on every backend. */

static int operand_is_reg32(const X86pOperand *o) {
  return o->kind == kX86pOperandReg && o->size == 4;
}

static int operand_is_imm(const X86pOperand *o) {
  return o->kind == kX86pOperandImm;
}

static int operand_is_mem32(const X86pOperand *o) {
  return o->kind == kX86pOperandMem && o->size == 4;
}

static int operand_writable(const X86pOperand *o) {
  return operand_is_reg32(o) || operand_is_mem32(o);
}

static int is_relative_branch(const X86pInsn *insn) {
  if (insn->op != (uint8_t)kX86pInsnJmp && insn->op != (uint8_t)kX86pInsnJcc) {
    return 0;
  }
  return insn->operands >= 1 && insn->operand[0].kind == kX86pOperandImm && insn->operand[0].relative;
}

static int is_indirect_branch(const X86pInsn *insn) {
  if (insn->op != (uint8_t)kX86pInsnJmp) {
    return 0;
  }
  return insn->operands == 1 && insn->operand[0].kind != kX86pOperandImm;
}

static int mov_operand_ok(const X86pOperand *o, int w, int for_write) {
  if (o->kind == kX86pOperandImm) {
    return !for_write;
  }
  if (o->kind == kX86pOperandMem) {
    return o->size == w;
  }
  if (o->kind != kX86pOperandReg) {
    return 0;
  }
  if (o->size != w) {
    return 0;
  }
  return w != 1 || (o->reg >= 0 && o->reg < 8);
}

static int mov_is_emittable(const X86pInsn *insn) {
  int w;
  if (insn->operands != 2) {
    return 0;
  }
  w = insn->operand[0].size;
  if (w != 1 && w != 2 && w != 4) {
    return 0;
  }
  if (insn->operand[0].kind == kX86pOperandMem && insn->operand[1].kind == kX86pOperandMem) {
    return 0;
  }
  return mov_operand_ok(&insn->operand[0], w, 1) && mov_operand_ok(&insn->operand[1], w, 0);
}

static int movx_is_emittable(const X86pInsn *insn) {
  const X86pOperand *dst;
  const X86pOperand *src;
  if (insn->operands != 2) {
    return 0;
  }
  dst = &insn->operand[0];
  src = &insn->operand[1];
  if (dst->kind != kX86pOperandReg || (dst->size != 2 && dst->size != 4)) {
    return 0;
  }
  if (src->size != 1 && src->size != 2) {
    return 0;
  }
  if (src->size >= dst->size) {
    return 0;
  }
  if (src->kind == kX86pOperandReg) {
    return src->size != 1 || (src->reg >= 0 && src->reg < 8);
  }
  return src->kind == kX86pOperandMem;
}

/* The x87 predicates (x87_load_is_emittable, ...) live in jit_arm64_x87.c. */

static int can_emit(const X86pInsn *insn) {
  int i;
  for (i = 0; i < insn->operands && i < X86P_MAX_OPERANDS; i++) {
    if (insn->operand[i].kind == kX86pOperandMem && insn->operand[i].addr16) {
      return 0;
    }
  }
  if (is_relative_branch(insn)) {
    return 1;
  }
  if (is_indirect_branch(insn)) {
    return operand_is_reg32(&insn->operand[0]) || operand_is_mem32(&insn->operand[0]);
  }
  switch (insn->op) {
  case kX86pInsnNop:
    return 1;
  case kX86pInsnMov:
    return mov_is_emittable(insn);
  case kX86pInsnMovzx:
  case kX86pInsnMovsx:
    return movx_is_emittable(insn);
  case kX86pInsnXchg:
    return insn->operands == 2 && insn->operand[0].size == 4 && insn->operand[1].size == 4 &&
           ((operand_is_reg32(&insn->operand[0]) &&
             (operand_is_reg32(&insn->operand[1]) || operand_is_mem32(&insn->operand[1]))) ||
            (operand_is_mem32(&insn->operand[0]) && operand_is_reg32(&insn->operand[1])));
  case kX86pInsnSetcc:
    return insn->cond < (uint8_t)kX86pCondCount && insn->operands == 1 && mov_operand_ok(&insn->operand[0], 1, 1);
  case kX86pInsnAlu:
    if (insn->alu >= (uint8_t)kX86pAluShl && insn->alu <= (uint8_t)kX86pAluSar) {
      return insn->operands == 2 && insn->operand[0].kind == kX86pOperandReg &&
             mov_operand_ok(&insn->operand[0], insn->operand[0].size, 1) &&
             (insn->operand[1].kind == kX86pOperandImm ||
              (insn->operand[1].kind == kX86pOperandReg && insn->operand[1].reg == kX86pEcx &&
               insn->operand[1].size == 1));
    }
    if (insn->alu > (uint8_t)kX86pAluTest) {
      return 0;
    }
    if (insn->alu == (uint8_t)kX86pAluAdc || insn->alu == (uint8_t)kX86pAluSbb) {
      return insn->operands == 2 && insn->operand[0].kind == kX86pOperandReg &&
             mov_operand_ok(&insn->operand[0], insn->operand[0].size, 1) && insn->operand[1].kind != kX86pOperandMem &&
             mov_operand_ok(&insn->operand[1], insn->operand[0].size, 0);
    }
    return mov_is_emittable(insn);
  case kX86pInsnPush:
    return insn->operands == 1 && (operand_is_reg32(&insn->operand[0]) || operand_is_mem32(&insn->operand[0]) ||
                                   operand_is_imm(&insn->operand[0]));
  case kX86pInsnPop:
    return insn->operands == 1 && operand_writable(&insn->operand[0]);
  case kX86pInsnPushfd:
  case kX86pInsnPopfd:
    return insn->operands == 0;
  case kX86pInsnCall:
    return insn->operands == 1 && (operand_is_imm(&insn->operand[0]) || operand_is_reg32(&insn->operand[0]) ||
                                   operand_is_mem32(&insn->operand[0]));
  case kX86pInsnRet:
    return insn->operands == 0 || (insn->operands == 1 && operand_is_imm(&insn->operand[0]));
  case kX86pInsnLeave:
    return insn->operands == 0;
  case kX86pInsnCdq:
    return insn->operands == 0;
  case kX86pInsnDiv:
  case kX86pInsnIdiv:
    return insn->operands == 1 && (operand_is_reg32(&insn->operand[0]) || operand_is_mem32(&insn->operand[0]));
  case kX86pInsnMul:
    return insn->operands == 1 && (operand_is_reg32(&insn->operand[0]) || operand_is_mem32(&insn->operand[0]));
  case kX86pInsnImul:
    return (insn->operands == 2 || (insn->operands == 3 && operand_is_imm(&insn->operand[2]))) &&
           operand_is_reg32(&insn->operand[0]) &&
           (operand_is_reg32(&insn->operand[1]) || operand_is_mem32(&insn->operand[1]));
  case kX86pInsnString:
    return x86p_string_is_supported((X86pStringOp)insn->str, (X86pRepKind)insn->rep, insn->str_width);
  case kX86pInsnAluUnary:
    if (insn->alu > (uint8_t)kX86pAluDec) {
      return 0;
    }
    return insn->operands == 1 && insn->operand[0].kind != kX86pOperandImm &&
           mov_operand_ok(&insn->operand[0], insn->operand[0].size, 1);
  case kX86pInsnLea:
    return insn->operands == 2 && operand_is_reg32(&insn->operand[0]) && insn->operand[1].kind == kX86pOperandMem;
  case kX86pInsnX87:
    return x87_load_is_emittable(insn) || x87_arith_is_emittable(insn) || x87_store_reg_is_emittable(insn) ||
           x87_store_mem_is_emittable(insn) || x87_compare_mem_is_emittable(insn) || x87_constant_is_emittable(insn) ||
           x87_status_ax_is_emittable(insn) || x87_clear_exceptions_is_emittable(insn);
  default:
    return 0;
  }
}

static int alu_writes_dest(uint8_t op) {
  return op != (uint8_t)kX86pAluCmp && op != (uint8_t)kX86pAluTest;
}

/* ---- calling a host helper ------------------------------------------------
 * The AAPCS64 counterpart of x64's repeated "mov r64,imm64; call r64"
 * sequence: the target address is materialised into X9 -- the encoder's own
 * second scratch, never a role register and never live across more than this
 * one call -- and BLR'd immediately. Arguments must already be in X0..X7. */
static void emit_call(X86pA64Emit *e, void *fn) {
  x86p_a64_emit_mov_x_imm64(e, kA64X9, (uint64_t)(uintptr_t)fn);
  x86p_a64_emit_blr(e, kA64X9);
}

/* "cmp DST, [base+disp]" has no single AArch64 instruction: load the operand
   into the encoder's other scratch (X8) and compare. Only ever the second of
   a two-instruction sequence with nothing live in X8 across it. */
static void emit_cmp_w_mem(X86pA64Emit *e, X86pA64Reg a, X86pA64Reg base, int32_t disp) {
  x86p_a64_emit_load32(e, kA64X8, base, disp);
  x86p_a64_emit_cmp_w_w(e, a, kA64X8);
}

/* ---- guest memory ---------------------------------------------------------
 * base + index*scale + disp, WITHOUT the segment base -- what LEA computes. */
static void emit_address_parts(X86pA64Emit *e, const X86pOperand *o) {
  int have_base = (o->base >= 0);
  if (have_base) {
    x86p_a64_emit_load32(e, EA_REG, CPU_REG, reg_off(o->base));
  } else {
    x86p_a64_emit_mov_w_imm32(e, EA_REG, 0u);
  }
  if (o->index >= 0) {
    unsigned shift = 0u;
    switch (o->scale) {
    case 2:
      shift = 1u;
      break;
    case 4:
      shift = 2u;
      break;
    case 8:
      shift = 3u;
      break;
    default:
      shift = 0u;
      break;
    }
    x86p_a64_emit_load32(e, ADDR_TMP, CPU_REG, reg_off(o->index));
    if (shift) {
      x86p_a64_emit_shl_w_imm(e, ADDR_TMP, (uint8_t)shift);
    }
    x86p_a64_emit_alu_w_w(e, kA64Add, EA_REG, ADDR_TMP);
  }
  if (o->disp != 0) {
    x86p_a64_emit_alu_w_imm(e, kA64Add, EA_REG, (uint32_t)o->disp);
  }
}

/* The linear address an ACCESS uses: the offset plus the segment base (FS/GS
   only -- see cpu.h on why the flat model is a contract). */
static void emit_effective_address(X86pA64Emit *e, const X86pOperand *o) {
  emit_address_parts(e, o);
  if (o->seg == (uint8_t)kX86pSegFs) {
    x86p_a64_emit_alu_w_mem(e, kA64Add, EA_REG, CPU_REG, (int32_t)offsetof(X86pCpu, fs_base));
  } else if (o->seg == (uint8_t)kX86pSegGs) {
    x86p_a64_emit_alu_w_mem(e, kA64Add, EA_REG, CPU_REG, (int32_t)offsetof(X86pCpu, gs_base));
  }
}

/*
 * Bounds-check EA_REG and leave the host address in HOSTPTR_REG.
 *
 * ONE unsigned compare covers both ends, exactly as jit_x64.c's -- CondHi
 * ("unsigned greater than", the AArch64 name for the same condition x64's
 * `ja` tests here) catches underflow and overflow together.
 */
static X86pA64EmitSite emit_bounds_check(X86pA64Emit *e, const MemPlan *plan, uint32_t insn_eip, int w) {
  x86p_a64_emit_mov_w_imm32(e, FAULTPC_REG, insn_eip);
  x86p_a64_emit_mov_w_w(e, ADDR_TMP, EA_REG);
  if (plan->lo != 0u) {
    x86p_a64_emit_alu_w_imm(e, kA64Sub, ADDR_TMP, plan->lo);
  }
  if (plan->size < (uint32_t)w) {
    return x86p_a64_emit_b(e);
  }
  x86p_a64_emit_cmp_w_imm(e, ADDR_TMP, plan->size - (uint32_t)w);
  return x86p_a64_emit_bcc(e, kA64CondHi);
}

/* HOSTPTR_REG = host + (EA - lo). ADDR_TMP already holds the offset, and
   writing a W register zero-extends into the full X register, so the 64-bit
   add gets a clean offset. */
static void emit_host_pointer(X86pA64Emit *e, const MemPlan *plan) {
  x86p_a64_emit_mov_x_imm64(e, HOSTPTR_REG, plan->host);
  x86p_a64_emit_alu_x_x(e, kA64Add, HOSTPTR_REG, ADDR_TMP);
}

/* ---- emitting -------------------------------------------------------------
 * BlockCtx (per-block emission state) lives in jit_arm64_internal.h. */

static void note_fault(BlockCtx *c, X86pA64EmitSite site) {
  if (c->nfaults < sizeof c->faults / sizeof c->faults[0]) {
    c->faults[c->nfaults++] = site;
    return;
  }
  c->e->overflow = 1;
}

static void note_divide_fault(BlockCtx *c, X86pA64EmitSite site) {
  if (c->ndivide_faults < sizeof c->divide_faults / sizeof c->divide_faults[0]) {
    c->divide_faults[c->ndivide_faults++] = site;
    return;
  }
  c->e->overflow = 1;
}

void emit_mem_prepare_w(BlockCtx *c, const X86pOperand *o, uint32_t insn_eip, int w) {
  emit_effective_address(c->e, o);
  note_fault(c, emit_bounds_check(c->e, &c->plan, insn_eip, w));
  emit_host_pointer(c->e, &c->plan);
}

static void emit_mem_prepare(BlockCtx *c, const X86pOperand *o, uint32_t insn_eip) {
  emit_mem_prepare_w(c, o, insn_eip, 4);
}

/* Where a guest register operand of width `w` lives, as a byte offset --
   identical to jit_x64.c's reg_off_w, arch-independent. */
static int32_t reg_off_w(int index, int w) {
  if (w == 1) {
    int shift = 0;
    int r = x86p_byte_reg(index, &shift);
    return reg_off(r) + shift / 8;
  }
  return reg_off(index);
}

static void emit_load_w(X86pA64Emit *e, X86pA64Reg dst, X86pA64Reg base, int32_t disp, int w) {
  if (w == 1) {
    x86p_a64_emit_load8_zx(e, dst, base, disp);
  } else if (w == 2) {
    x86p_a64_emit_load16_zx(e, dst, base, disp);
  } else {
    x86p_a64_emit_load32(e, dst, base, disp);
  }
}

static void emit_store_w(X86pA64Emit *e, X86pA64Reg base, int32_t disp, X86pA64Reg src, int w) {
  if (w == 1) {
    x86p_a64_emit_store8_reg(e, base, disp, src);
  } else if (w == 2) {
    x86p_a64_emit_store16_reg(e, base, disp, src);
  } else {
    x86p_a64_emit_store32(e, base, disp, src);
  }
}

static void emit_store_imm_w(X86pA64Emit *e, X86pA64Reg base, int32_t disp, uint32_t imm, int w) {
  if (w == 1) {
    x86p_a64_emit_store8_imm(e, base, disp, (uint8_t)(imm & 0xFFu));
  } else if (w == 2) {
    x86p_a64_emit_store16_imm(e, base, disp, (uint16_t)(imm & 0xFFFFu));
  } else {
    x86p_a64_emit_store32_imm(e, base, disp, imm);
  }
}

/* Which host ALU opcode computes a guest ALU op, and which flag kind it
   records -- identical mapping to jit_x64.c's inline_alu_shape, targeting
   X86pA64Alu. CMP maps to a plain Sub with writes_dest=0, exactly as x64
   maps it to Sub rather than exposing a distinct "compare" opcode: the guest
   destination is simply never written back. */
static int inline_alu_shape(uint8_t alu, X86pA64Alu *host, X86pFlagKind *kind, int *writes_dest) {
  *writes_dest = 1;
  switch (alu) {
  case kX86pAluAdd:
    *host = kA64Add;
    *kind = kX86pFlagsAdd;
    return 1;
  case kX86pAluSub:
    *host = kA64Sub;
    *kind = kX86pFlagsSub;
    return 1;
  case kX86pAluCmp:
    *host = kA64Sub;
    *kind = kX86pFlagsSub;
    *writes_dest = 0;
    return 1;
  case kX86pAluOr:
    *host = kA64Orr;
    *kind = kX86pFlagsLogic;
    return 1;
  case kX86pAluAnd:
    *host = kA64And;
    *kind = kX86pFlagsLogic;
    return 1;
  case kX86pAluTest:
    *host = kA64And;
    *kind = kX86pFlagsLogic;
    *writes_dest = 0;
    return 1;
  case kX86pAluXor:
    *host = kA64Eor;
    *kind = kX86pFlagsLogic;
    return 1;
  default:
    return 0;
  }
}

/* DEAD FLAG STORE ELIMINATION -- identical logic to jit_x64.c's
   flag_write_is_dead: a pure decode-level scan with no host-register content
   at all, so it is copied verbatim rather than re-derived. See jit_x64.c for
   the full rationale and the safety argument for the carry-in. */
static int flag_write_is_dead(const X86pMem *mem,
                              uint32_t pc,
                              uint32_t eip,
                              X86pJitBoundaryFn boundary,
                              void *boundary_user,
                              uint32_t count,
                              size_t code_len,
                              size_t code_cap) {
  int step;
  for (step = 0; step < 8; step++) {
    uint8_t bytes[X86P_MAX_INSN_LEN];
    uint32_t avail = 0;
    uint32_t i;
    X86pInsn insn;

    if (count + 1u + (uint32_t)step >= MAX_INSNS) {
      return 0;
    }
    if (code_len + (size_t)(step + 2) * WORST_CASE_INSN_BYTES + EPILOGUE_BYTES > code_cap) {
      return 0;
    }

    for (i = 0; i < (uint32_t)X86P_MAX_INSN_LEN; i++) {
      uint32_t byte;
      if (!x86p_mem_read(mem, pc + i, 1, &byte)) {
        break;
      }
      bytes[i] = (uint8_t)byte;
      avail++;
    }
    if (avail == 0 || !x86p_decode(bytes, avail, &insn)) {
      return 0;
    }
    if (pc != eip && boundary && boundary(pc, boundary_user)) {
      return 0;
    }
    if (!can_emit(&insn)) {
      return 0;
    }

    if (insn.op == (uint8_t)kX86pInsnAlu) {
      X86pA64Alu host;
      X86pFlagKind kind;
      int writes_dest;
      if (inline_alu_shape(insn.alu, &host, &kind, &writes_dest) && insn.operand[0].kind != kX86pOperandMem &&
          insn.operand[1].kind != kX86pOperandMem) {
        return 1;
      }
      return 0;
    }
    if (insn.op == (uint8_t)kX86pInsnAluUnary) {
      if (insn.alu == (uint8_t)kX86pAluNeg && insn.operand[0].kind != kX86pOperandMem) {
        return 1;
      }
      if (insn.alu == (uint8_t)kX86pAluNot && insn.operand[0].kind != kX86pOperandMem) {
        pc += insn.length;
        continue;
      }
      return 0;
    }
    if (insn.op == (uint8_t)kX86pInsnNop) {
      pc += insn.length;
      continue;
    }
    if (insn.op == (uint8_t)kX86pInsnMov && insn.operand[0].kind == kX86pOperandReg &&
        insn.operand[1].kind != kX86pOperandMem) {
      pc += insn.length;
      continue;
    }
    return 0;
  }
  return 0;
}

/*
 * Store `carry_in`: the CF the flag state held BEFORE this operation
 * overwrites it -- see jit_x64.c's emit_compute_carry_in for the full
 * rationale. The derivations mirror x86p_flag_cf exactly, at w == 4.
 */
static int emit_compute_carry_in(X86pA64Emit *e, int last_kind) {
  switch (last_kind) {
  case kX86pFlagsNone:
  case kX86pFlagsLogic:
    x86p_a64_emit_mov_w_imm32(e, CARRY_REG, 0u);
    return 0;
  case kX86pFlagsAdd:
    /* CF = r < a, unsigned */
    x86p_a64_emit_load32(e, CARRY_REG, CPU_REG, FLAG_R);
    emit_cmp_w_mem(e, CARRY_REG, CPU_REG, FLAG_A);
    x86p_a64_emit_cset_w(e, kA64CondCc, CARRY_REG);
    return 0;
  case kX86pFlagsExplicit:
    /* A real EFLAGS word, which ADC and SBB leave behind: CF is bit 0. */
    x86p_a64_emit_load32(e, CARRY_REG, CPU_REG, FLAG_A);
    x86p_a64_emit_alu_w_imm(e, kA64And, CARRY_REG, X86P_CF);
    return 0;
  case kX86pFlagsInc:
  case kX86pFlagsDec:
    /* PRESERVED: the carry the state already holds IS the carry. */
    x86p_a64_emit_load8_zx(e, CARRY_REG, CPU_REG, FLAG_CARRY_IN);
    return 0;
  case kX86pFlagsSub:
    /* CF = a < b, unsigned */
    x86p_a64_emit_load32(e, CARRY_REG, CPU_REG, FLAG_A);
    emit_cmp_w_mem(e, CARRY_REG, CPU_REG, FLAG_B);
    x86p_a64_emit_cset_w(e, kA64CondCc, CARRY_REG);
    return 0;
  default:
    /* Unknown predecessor: ask the one authority. Once per block. */
    x86p_a64_emit_lea64(e, kA64X0, CPU_REG, flags_off());
    emit_call(e, (void *)&x86p_flag_cf);
    x86p_a64_emit_mov_w_w(e, CARRY_REG, kA64X0);
    return 1;
  }
}

static uint32_t width_mask(int w) {
  return (w == 1) ? 0xFFu : ((w == 2) ? 0xFFFFu : 0xFFFFFFFFu);
}

/* The non-memory side of an ALU operand, at width `w`. */
static void emit_read_alu_src(BlockCtx *c, X86pA64Reg dst, const X86pOperand *o, int w) {
  if (o->kind == kX86pOperandImm) {
    x86p_a64_emit_mov_w_imm32(c->e, dst, o->imm & width_mask(w));
    return;
  }
  emit_load_w(c->e, dst, CPU_REG, reg_off_w(o->reg, w), w);
}

/*
 * The inlined ALU form: native host arithmetic plus the lazy tuple written
 * directly, with no call at all -- see jit_x64.c's emit_alu_inline for the
 * ordering rationale (carry-in computed before the memory operand's bounds
 * check, stored only after). X0/X1 hold the two operand values (a, b); X2
 * holds the native result -- all three inside the X0..X7 pool, which no role
 * register ever occupies.
 */
static void emit_alu_inline(BlockCtx *c,
                            const X86pInsn *insn,
                            X86pA64Alu host,
                            X86pFlagKind kind,
                            int writes_dest,
                            int last_kind,
                            int flags_dead,
                            uint32_t insn_eip) {
  const X86pOperand *dst = &insn->operand[0];
  const X86pOperand *src = &insn->operand[1];
  const int w = dst->size;

  if (!flags_dead) {
    c->flag_helper_calls += (unsigned)emit_compute_carry_in(c->e, last_kind);
  }

  if (dst->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, dst, insn_eip, w);
    if (!flags_dead) {
      x86p_a64_emit_store8_reg(c->e, CPU_REG, FLAG_CARRY_IN, CARRY_REG);
    }
    emit_load_w(c->e, kA64X0, HOSTPTR_REG, 0, w);
    emit_read_alu_src(c, kA64X1, src, w);
  } else if (src->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, src, insn_eip, w);
    if (!flags_dead) {
      x86p_a64_emit_store8_reg(c->e, CPU_REG, FLAG_CARRY_IN, CARRY_REG);
    }
    emit_load_w(c->e, kA64X0, CPU_REG, reg_off_w(dst->reg, w), w);
    emit_load_w(c->e, kA64X1, HOSTPTR_REG, 0, w);
  } else {
    if (!flags_dead) {
      x86p_a64_emit_store8_reg(c->e, CPU_REG, FLAG_CARRY_IN, CARRY_REG);
    }
    emit_load_w(c->e, kA64X0, CPU_REG, reg_off_w(dst->reg, w), w);
    emit_read_alu_src(c, kA64X1, src, w);
  }

  x86p_a64_emit_mov_w_w(c->e, kA64X2, kA64X0);
  x86p_a64_emit_alu_w_w(c->e, host, kA64X2, kA64X1); /* r */
  if (w != 4) {
    x86p_a64_emit_alu_w_imm(c->e, kA64And, kA64X2, width_mask(w));
  }

  if (!flags_dead) {
    x86p_a64_emit_store32(c->e, CPU_REG, FLAG_A, kA64X0);
    x86p_a64_emit_store32(c->e, CPU_REG, FLAG_B, kA64X1);
    x86p_a64_emit_store32(c->e, CPU_REG, FLAG_R, kA64X2);
    x86p_a64_emit_store8_imm(c->e, CPU_REG, FLAG_KIND, (uint8_t)kind);
    x86p_a64_emit_store8_imm(c->e, CPU_REG, FLAG_W, (uint8_t)w);
  }

  if (writes_dest) {
    if (dst->kind == kX86pOperandMem) {
      emit_store_w(c->e, HOSTPTR_REG, 0, kA64X2, w);
    } else {
      emit_store_w(c->e, CPU_REG, reg_off_w(dst->reg, w), kA64X2, w);
    }
  }
}

/* PUSH / POP: the stack is ordinary guest memory, same fault path and same
   ordering rules as jit_x64.c -- see there for why. */
static void emit_lea(BlockCtx *c, const X86pInsn *insn) {
  emit_address_parts(c->e, &insn->operand[1]);
  x86p_a64_emit_store32(c->e, CPU_REG, reg_off(insn->operand[0].reg), EA_REG);
}

static void emit_leave(BlockCtx *c, uint32_t insn_eip) {
  x86p_a64_emit_load32(c->e, EA_REG, CPU_REG, reg_off(kX86pEbp));
  x86p_a64_emit_store32(c->e, CPU_REG, reg_off(kX86pEsp), EA_REG);
  note_fault(c, emit_bounds_check(c->e, &c->plan, insn_eip, 4));
  emit_host_pointer(c->e, &c->plan);
  x86p_a64_emit_load32(c->e, kA64X0, HOSTPTR_REG, 0);
  x86p_a64_emit_alu_w_imm(c->e, kA64Add, EA_REG, 4u);
  x86p_a64_emit_store32(c->e, CPU_REG, reg_off(kX86pEsp), EA_REG);
  x86p_a64_emit_store32(c->e, CPU_REG, reg_off(kX86pEbp), kA64X0);
}

static void emit_cdq(X86pA64Emit *e) {
  x86p_a64_emit_load32(e, kA64X0, CPU_REG, reg_off(kX86pEax));
  x86p_a64_emit_sar_w_imm(e, kA64X0, 31u);
  x86p_a64_emit_store32(e, CPU_REG, reg_off(kX86pEdx), kA64X0);
}

/* This helper owns only one already-decoded operation's value semantics --
   identical to jit_x64.c's jit_div32. */
static int jit_div32(X86pCpu *cpu, uint32_t divisor, uint32_t signed_divide) {
  uint32_t quotient = 0u;
  uint32_t remainder = 0u;

  int ok = signed_divide
               ? x86p_alu_idiv(cpu->reg[kX86pEdx], cpu->reg[kX86pEax], divisor, 4, &quotient, &remainder, &cpu->flags)
               : x86p_alu_div(cpu->reg[kX86pEdx], cpu->reg[kX86pEax], divisor, 4, &quotient, &remainder, &cpu->flags);
  if (!ok) {
    return 0;
  }
  cpu->reg[kX86pEax] = quotient;
  cpu->reg[kX86pEdx] = remainder;
  return 1;
}

static void emit_div32(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip, int signed_divide) {
  const X86pOperand *divisor = &insn->operand[0];
  X86pA64EmitSite failed;

  if (divisor->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, divisor, insn_eip, 4);
    x86p_a64_emit_load32(c->e, kA64X1, HOSTPTR_REG, 0);
  } else {
    x86p_a64_emit_load32(c->e, kA64X1, CPU_REG, reg_off(divisor->reg));
  }
  x86p_a64_emit_mov_x_x(c->e, kA64X0, CPU_REG);
  x86p_a64_emit_mov_w_imm32(c->e, kA64X2, (uint32_t)signed_divide);
  emit_call(c->e, (void *)&jit_div32);
  x86p_a64_emit_tst_w_w(c->e, kA64X0, kA64X0);
  x86p_a64_emit_mov_w_imm32(c->e, FAULTPC_REG, insn_eip);
  failed = x86p_a64_emit_bcc(c->e, kA64CondEq);
  note_divide_fault(c, failed);
}

/* The shipping emitter calls the canonical widening-multiply semantics after
   capturing the explicit operand -- identical to jit_x64.c's jit_mul32. */
static void jit_mul32(X86pCpu *cpu, uint32_t operand) {
  uint32_t low = 0u;
  uint32_t high = 0u;

  x86p_alu_mul(cpu->reg[kX86pEax], operand, 4, &low, &high, &cpu->flags);
  cpu->reg[kX86pEax] = low;
  cpu->reg[kX86pEdx] = high;
}

static void emit_mul32(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  const X86pOperand *operand = &insn->operand[0];

  if (operand->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, operand, insn_eip, 4);
    x86p_a64_emit_load32(c->e, kA64X1, HOSTPTR_REG, 0);
  } else {
    x86p_a64_emit_load32(c->e, kA64X1, CPU_REG, reg_off(operand->reg));
  }
  x86p_a64_emit_mov_x_x(c->e, kA64X0, CPU_REG);
  emit_call(c->e, (void *)&jit_mul32);
}

static void jit_imul32(X86pCpu *cpu, uint32_t destination, uint32_t left, uint32_t right) {
  uint32_t low = 0u;
  uint32_t high = 0u;

  x86p_alu_imul(left, right, 4, &low, &high, &cpu->flags);
  cpu->reg[destination] = low;
}

static void emit_imul32(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  const X86pOperand *destination = &insn->operand[0];
  const X86pOperand *source = &insn->operand[1];

  if (insn->operands == 2) {
    if (source->kind == kX86pOperandMem) {
      emit_mem_prepare_w(c, source, insn_eip, 4);
      x86p_a64_emit_load32(c->e, kA64X3, HOSTPTR_REG, 0);
    } else {
      x86p_a64_emit_load32(c->e, kA64X3, CPU_REG, reg_off(source->reg));
    }
    x86p_a64_emit_load32(c->e, kA64X2, CPU_REG, reg_off(destination->reg));
  } else {
    if (source->kind == kX86pOperandMem) {
      emit_mem_prepare_w(c, source, insn_eip, 4);
      x86p_a64_emit_load32(c->e, kA64X2, HOSTPTR_REG, 0);
    } else {
      x86p_a64_emit_load32(c->e, kA64X2, CPU_REG, reg_off(source->reg));
    }
    x86p_a64_emit_mov_w_imm32(c->e, kA64X3, insn->operand[2].imm);
  }
  x86p_a64_emit_mov_x_x(c->e, kA64X0, CPU_REG);
  x86p_a64_emit_mov_w_imm32(c->e, kA64X1, destination->reg);
  emit_call(c->e, (void *)&jit_imul32);
}

static int jit_string(X86pCpu *cpu, const X86pMem *mem, uint32_t operation, uint32_t repeat, uint32_t width) {
  X86pInsn insn;

  memset(&insn, 0, sizeof insn);
  insn.op = (uint8_t)kX86pInsnString;
  insn.str = (uint8_t)operation;
  insn.rep = (uint8_t)repeat;
  insn.str_width = (uint8_t)width;
  return x86p_string_execute(cpu, mem, &insn, NULL) == kX86pStringOk;
}

static void emit_string(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  X86pA64EmitSite failed;

  x86p_a64_emit_mov_x_x(c->e, kA64X0, CPU_REG);
  x86p_a64_emit_mov_x_imm64(c->e, kA64X1, (uint64_t)(uintptr_t)c->mem);
  x86p_a64_emit_mov_w_imm32(c->e, kA64X2, insn->str);
  x86p_a64_emit_mov_w_imm32(c->e, kA64X3, insn->rep);
  x86p_a64_emit_mov_w_imm32(c->e, kA64X4, insn->str_width);
  emit_call(c->e, (void *)&jit_string);
  x86p_a64_emit_tst_w_w(c->e, kA64X0, kA64X0);
  x86p_a64_emit_mov_w_imm32(c->e, FAULTPC_REG, insn_eip);
  failed = x86p_a64_emit_bcc(c->e, kA64CondEq);
  note_fault(c, failed);
}

/* Push whatever is in X0. The one implementation of the stack store, shared
   by PUSH and by CALL's return address. */
static void emit_push_x0(BlockCtx *c, uint32_t insn_eip) {
  x86p_a64_emit_load32(c->e, EA_REG, CPU_REG, reg_off(kX86pEsp));
  x86p_a64_emit_alu_w_imm(c->e, kA64Sub, EA_REG, 4u);
  note_fault(c, emit_bounds_check(c->e, &c->plan, insn_eip, 4));
  emit_host_pointer(c->e, &c->plan);
  x86p_a64_emit_store32(c->e, HOSTPTR_REG, 0, kA64X0);
  x86p_a64_emit_store32(c->e, CPU_REG, reg_off(kX86pEsp), EA_REG);
}

static void emit_push(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  const X86pOperand *o = &insn->operand[0];

  if (o->kind == kX86pOperandImm) {
    x86p_a64_emit_mov_w_imm32(c->e, kA64X0, x86p_sign_extend(o->imm, o->size));
  } else if (o->kind == kX86pOperandMem) {
    emit_mem_prepare(c, o, insn_eip);
    x86p_a64_emit_load32(c->e, kA64X0, HOSTPTR_REG, 0);
  } else {
    x86p_a64_emit_load32(c->e, kA64X0, CPU_REG, reg_off(o->reg));
  }

  emit_push_x0(c, insn_eip);
}

/* Pop whatever is at [ESP] into X0 and advance ESP past it, the one
   implementation of the stack load shared by POP and POPFD (mirrors
   emit_push_x0 above for PUSH). The caller decides where X0 ends up. */
static void emit_pop_x0(BlockCtx *c, uint32_t insn_eip) {
  x86p_a64_emit_load32(c->e, EA_REG, CPU_REG, reg_off(kX86pEsp));
  note_fault(c, emit_bounds_check(c->e, &c->plan, insn_eip, 4));
  emit_host_pointer(c->e, &c->plan);
  x86p_a64_emit_load32(c->e, kA64X0, HOSTPTR_REG, 0);

  x86p_a64_emit_mov_w_w(c->e, kA64X1, EA_REG);
  x86p_a64_emit_alu_w_imm(c->e, kA64Add, kA64X1, 4u);
  x86p_a64_emit_store32(c->e, CPU_REG, reg_off(kX86pEsp), kA64X1);
}

static void emit_pop(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  const X86pOperand *o = &insn->operand[0];

  emit_pop_x0(c, insn_eip);

  if (o->kind == kX86pOperandMem) {
    emit_mem_prepare(c, o, insn_eip);
    x86p_a64_emit_store32(c->e, HOSTPTR_REG, 0, kA64X0);
    return;
  }
  x86p_a64_emit_store32(c->e, CPU_REG, reg_off(o->reg), kA64X0);
}

/*
 * PUSHFD and POPFD are where the two representations of EFLAGS meet: the six
 * arithmetic flags are derived from the lazy (kind, a, b, r) record, and DF is
 * held apart because nothing computes it (see X86pCpu::df). Both halves cross
 * through one call each, matching exec.c's interpreter path and jit_x64.c's
 * identical jit_pushfd_value/jit_popfd_apply exactly rather than reproducing
 * that merge as a second authority here.
 */
static uint32_t jit_pushfd_value(X86pCpu *cpu) {
  return x86p_eflags(&cpu->flags) | (cpu->df ? X86P_DF : 0u);
}

static void jit_popfd_apply(X86pCpu *cpu, uint32_t v) {
  x86p_flags_set_explicit(&cpu->flags, v);
  cpu->df = (v & X86P_DF) ? 1u : 0u;
}

static void emit_pushfd(BlockCtx *c, uint32_t insn_eip) {
  x86p_a64_emit_mov_x_x(c->e, kA64X0, CPU_REG);
  emit_call(c->e, (void *)&jit_pushfd_value);
  /* The return is a uint32_t (W0); emit_push_x0's store32 reads only W0, so
     whatever the ABI left in X0's upper half is irrelevant here. */
  emit_push_x0(c, insn_eip);
}

static void emit_popfd(BlockCtx *c, uint32_t insn_eip) {
  emit_pop_x0(c, insn_eip); /* X0 = popped value, already the second AAPCS64 arg */
  x86p_a64_emit_mov_w_w(c->e, kA64X1, kA64X0);
  x86p_a64_emit_mov_x_x(c->e, kA64X0, CPU_REG);
  emit_call(c->e, (void *)&jit_popfd_apply);
}

/* INC, DEC, NEG and NOT -- see jit_x64.c's emit_alu_unary_inline for the CF
   preservation rationale. Returns the flag kind recorded, or -1 for NOT. */
static int emit_alu_unary_inline(BlockCtx *c, const X86pInsn *insn, int last_kind, int flags_dead, uint32_t insn_eip) {
  const X86pOperand *o = &insn->operand[0];
  const int w = o->size;
  const int is_mem = (o->kind == kX86pOperandMem);
  X86pFlagKind kind;

  if (insn->alu == (uint8_t)kX86pAluNot) {
    if (is_mem) {
      emit_mem_prepare_w(c, o, insn_eip, w);
      emit_load_w(c->e, kA64X0, HOSTPTR_REG, 0, w);
    } else {
      emit_load_w(c->e, kA64X0, CPU_REG, reg_off_w(o->reg, w), w);
    }
    x86p_a64_emit_alu_w_imm(c->e, kA64Eor, kA64X0, 0xFFFFFFFFu);
    if (w != 4) {
      x86p_a64_emit_alu_w_imm(c->e, kA64And, kA64X0, width_mask(w));
    }
    if (is_mem) {
      emit_store_w(c->e, HOSTPTR_REG, 0, kA64X0, w);
    } else {
      emit_store_w(c->e, CPU_REG, reg_off_w(o->reg, w), kA64X0, w);
    }
    return -1;
  }

  if (!flags_dead) {
    c->flag_helper_calls += (unsigned)emit_compute_carry_in(c->e, last_kind);
  }

  if (is_mem) {
    emit_mem_prepare_w(c, o, insn_eip, w);
    if (!flags_dead) {
      x86p_a64_emit_store8_reg(c->e, CPU_REG, FLAG_CARRY_IN, CARRY_REG);
    }
    emit_load_w(c->e, kA64X0, HOSTPTR_REG, 0, w);
  } else {
    if (!flags_dead) {
      x86p_a64_emit_store8_reg(c->e, CPU_REG, FLAG_CARRY_IN, CARRY_REG);
    }
    emit_load_w(c->e, kA64X0, CPU_REG, reg_off_w(o->reg, w), w);
  }

  if (insn->alu == (uint8_t)kX86pAluNeg) {
    /* 0 - a, recorded as the SUB it is, so CF falls out of the borrow. */
    x86p_a64_emit_mov_w_imm32(c->e, kA64X1, 0u);
    x86p_a64_emit_alu_w_w(c->e, kA64Sub, kA64X1, kA64X0);
    kind = kX86pFlagsSub;
  } else {
    x86p_a64_emit_mov_w_w(c->e, kA64X1, kA64X0);
    if (insn->alu == (uint8_t)kX86pAluInc) {
      x86p_a64_emit_alu_w_imm(c->e, kA64Add, kA64X1, 1u);
      kind = kX86pFlagsInc;
    } else {
      x86p_a64_emit_alu_w_imm(c->e, kA64Sub, kA64X1, 1u);
      kind = kX86pFlagsDec;
    }
  }
  if (w != 4) {
    x86p_a64_emit_alu_w_imm(c->e, kA64And, kA64X1, width_mask(w));
  }

  /* NEG's operands are (0, a); INC and DEC's are (a, 1). X0 holds the operand
     `a` throughout -- reused below whichever branch ran -- and X1 holds the
     result. */
  if (!flags_dead) {
    if (kind == kX86pFlagsSub) {
      x86p_a64_emit_store32_imm(c->e, CPU_REG, FLAG_A, 0u);
      x86p_a64_emit_store32(c->e, CPU_REG, FLAG_B, kA64X0);
    } else {
      x86p_a64_emit_store32(c->e, CPU_REG, FLAG_A, kA64X0);
      x86p_a64_emit_store32_imm(c->e, CPU_REG, FLAG_B, 1u);
    }
    x86p_a64_emit_store32(c->e, CPU_REG, FLAG_R, kA64X1);
    x86p_a64_emit_store8_imm(c->e, CPU_REG, FLAG_KIND, (uint8_t)kind);
    x86p_a64_emit_store8_imm(c->e, CPU_REG, FLAG_W, (uint8_t)w);
  }

  if (is_mem) {
    emit_store_w(c->e, HOSTPTR_REG, 0, kA64X1, w);
  } else {
    emit_store_w(c->e, CPU_REG, reg_off_w(o->reg, w), kA64X1, w);
  }
  return (int)kind;
}

/* The shift / ADC / SBB path: register-only (can_emit keeps memory operands
   away from them), calling x86p_alu(op, a, b, w, &flags) -> result in X0. */
static void emit_alu(X86pA64Emit *e, const X86pInsn *insn) {
  const X86pOperand *dst = &insn->operand[0];
  const X86pOperand *src = &insn->operand[1];
  const int w = dst->size;

  emit_load_w(e, kA64X1, CPU_REG, reg_off_w(dst->reg, w), w); /* a */
  if (src->kind == kX86pOperandImm) {
    x86p_a64_emit_mov_w_imm32(e, kA64X2, src->imm & width_mask(w)); /* b */
  } else {
    /* At the SOURCE's own width: a shift's count is CL, one byte. */
    emit_load_w(e, kA64X2, CPU_REG, reg_off_w(src->reg, src->size), src->size);
  }
  x86p_a64_emit_mov_w_imm32(e, kA64X3, (uint32_t)w);
  x86p_a64_emit_lea64(e, kA64X4, CPU_REG, flags_off());
  x86p_a64_emit_mov_w_imm32(e, kA64X0, (uint32_t)insn->alu);
  emit_call(e, (void *)&x86p_alu);
  if (alu_writes_dest(insn->alu)) {
    emit_store_w(e, CPU_REG, reg_off_w(dst->reg, w), kA64X0, w);
  }
}

static void emit_mov(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  const X86pOperand *dst = &insn->operand[0];
  const X86pOperand *src = &insn->operand[1];
  const int w = dst->size;

  if (dst->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, dst, insn_eip, w);
    if (src->kind == kX86pOperandImm) {
      emit_store_imm_w(c->e, HOSTPTR_REG, 0, src->imm, w);
      return;
    }
    emit_load_w(c->e, kA64X0, CPU_REG, reg_off_w(src->reg, w), w);
    emit_store_w(c->e, HOSTPTR_REG, 0, kA64X0, w);
    return;
  }

  if (src->kind == kX86pOperandImm) {
    emit_store_imm_w(c->e, CPU_REG, reg_off_w(dst->reg, w), src->imm, w);
    return;
  }
  if (src->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, src, insn_eip, w);
    emit_load_w(c->e, kA64X0, HOSTPTR_REG, 0, w);
  } else {
    emit_load_w(c->e, kA64X0, CPU_REG, reg_off_w(src->reg, w), w);
  }
  emit_store_w(c->e, CPU_REG, reg_off_w(dst->reg, w), kA64X0, w);
}

static void emit_xchg32(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  const X86pOperand *first = &insn->operand[0];
  const X86pOperand *second = &insn->operand[1];
  const X86pOperand *memory = first->kind == kX86pOperandMem ? first : second;
  const X86pOperand *reg = first->kind == kX86pOperandReg ? first : second;

  if (memory->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, memory, insn_eip, 4);
    x86p_a64_emit_load32(c->e, kA64X0, HOSTPTR_REG, 0);
    x86p_a64_emit_load32(c->e, kA64X1, CPU_REG, reg_off(reg->reg));
    x86p_a64_emit_store32(c->e, HOSTPTR_REG, 0, kA64X1);
    x86p_a64_emit_store32(c->e, CPU_REG, reg_off(reg->reg), kA64X0);
    return;
  }

  x86p_a64_emit_load32(c->e, kA64X0, CPU_REG, reg_off(first->reg));
  x86p_a64_emit_load32(c->e, kA64X1, CPU_REG, reg_off(second->reg));
  x86p_a64_emit_store32(c->e, CPU_REG, reg_off(first->reg), kA64X1);
  x86p_a64_emit_store32(c->e, CPU_REG, reg_off(second->reg), kA64X0);
}

/* MOVZX / MOVSX -- see jit_x64.c's emit_movx for why the widened load plus an
   optional sign-fill pair is enough and needs no new emitter. */
static void emit_movx(BlockCtx *c, const X86pInsn *insn, int is_signed, uint32_t insn_eip) {
  const X86pOperand *dst = &insn->operand[0];
  const X86pOperand *src = &insn->operand[1];
  const int sw = src->size; /* 1 or 2 */
  const int dw = dst->size; /* 2 or 4 */

  if (src->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, src, insn_eip, sw);
    emit_load_w(c->e, kA64X0, HOSTPTR_REG, 0, sw);
  } else {
    emit_load_w(c->e, kA64X0, CPU_REG, reg_off_w(src->reg, sw), sw);
  }

  if (is_signed) {
    const uint8_t fill = (uint8_t)(32 - 8 * sw);
    x86p_a64_emit_shl_w_imm(c->e, kA64X0, fill);
    x86p_a64_emit_sar_w_imm(c->e, kA64X0, fill);
  }

  emit_store_w(c->e, CPU_REG, reg_off_w(dst->reg, dw), kA64X0, dw);
}

/*
 * Prologue: save X19 (CPU_REG) and X30 (LR), park the X86pCpu pointer.
 *
 * Unlike x86-64's CALL, which pushes the return address for free, AArch64's
 * BLR clobbers X30 on every helper call this backend emits -- so LR must be
 * saved and restored alongside CPU_REG rather than left to the caller's own
 * frame. STP/LDP through SP also satisfies AArch64's requirement that SP be
 * 16-byte aligned at every load/store through it.
 */
static void emit_prologue(X86pA64Emit *e) {
  x86p_a64_emit_push_pair(e, CPU_REG, kA64Lr);
  x86p_a64_emit_mov_x_x(e, CPU_REG, kA64X0);
}

static void emit_epilogue(X86pA64Emit *e, uint32_t next_eip, X86pJitExit exit) {
  x86p_a64_emit_store32_imm(e, CPU_REG, eip_off(), next_eip);
  x86p_a64_emit_mov_w_imm32(e, kA64X0, (uint32_t)exit);
  x86p_a64_emit_pop_pair(e, CPU_REG, kA64Lr);
  x86p_a64_emit_ret(e);
}

static void emit_epilogue_from(X86pA64Emit *e, X86pA64Reg eip_reg, X86pJitExit exit) {
  x86p_a64_emit_store32(e, CPU_REG, eip_off(), eip_reg);
  x86p_a64_emit_mov_w_imm32(e, kA64X0, (uint32_t)exit);
  x86p_a64_emit_pop_pair(e, CPU_REG, kA64Lr);
  x86p_a64_emit_ret(e);
}

/*
 * A conditional branch, emitted WITHOUT a forward jump -- see jit_x64.c's
 * emit_jcc for why CSEL replaces a patched jump here exactly as CMOVcc does
 * there. `mov`/`cset` do not disturb NZCV, so the ordering only has to keep
 * the TST that reads x86p_cond's return value adjacent to the CSEL.
 */
static void emit_condition_value(X86pA64Emit *e, uint8_t cond) {
  x86p_a64_emit_mov_w_imm32(e, kA64X0, (uint32_t)cond);
  x86p_a64_emit_lea64(e, kA64X1, CPU_REG, flags_off());
  emit_call(e, (void *)&x86p_cond);
}

static void emit_jcc(X86pA64Emit *e, uint8_t cond, uint32_t target, uint32_t fallthrough) {
  emit_condition_value(e, cond);
  x86p_a64_emit_tst_w_w(e, kA64X0, kA64X0);
  x86p_a64_emit_mov_w_imm32(e, kA64X0, fallthrough);
  x86p_a64_emit_mov_w_imm32(e, kA64X1, target);
  x86p_a64_emit_csel_w(e, kA64CondNe, kA64X0, kA64X1, kA64X0);
  emit_epilogue_from(e, kA64X0, kX86pJitExitBlockEnd);
}

/* SETcc materialises the canonical condition evaluator's 0/1 result without
   touching guest flags. A memory destination computes the condition first,
   then preserves it in CARRY_REG while the shared address/bounds path uses
   X0. */
static void emit_setcc(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  const X86pOperand *dst = &insn->operand[0];

  emit_condition_value(c->e, insn->cond);
  if (dst->kind == kX86pOperandMem) {
    x86p_a64_emit_mov_w_w(c->e, CARRY_REG, kA64X0);
    emit_mem_prepare_w(c, dst, insn_eip, 1);
    x86p_a64_emit_store8_reg(c->e, HOSTPTR_REG, 0, CARRY_REG);
    return;
  }
  x86p_a64_emit_store8_reg(c->e, CPU_REG, reg_off_w(dst->reg, 1), kA64X0);
}

/* CALL and RET -- see jit_x64.c's emit_call_rel for why these END the block
   with a plain successor EIP rather than refusing it. */
static void emit_call_rel(BlockCtx *c, uint32_t return_eip, uint32_t target, uint32_t insn_eip) {
  x86p_a64_emit_mov_w_imm32(c->e, kA64X0, return_eip);
  emit_push_x0(c, insn_eip);
  emit_epilogue(c->e, target, kX86pJitExitBlockEnd);
}

/* The indirect forms. TARGET_REG is read before anything else touches
   memory, because CALL [ESP+4] must take its target from the stack as it
   stands and not from the stack after the return address has been pushed. */
static void emit_read_branch_target(BlockCtx *c, const X86pOperand *o, uint32_t insn_eip) {
  if (o->kind == kX86pOperandMem) {
    emit_mem_prepare(c, o, insn_eip);
    x86p_a64_emit_load32(c->e, TARGET_REG, HOSTPTR_REG, 0);
    return;
  }
  x86p_a64_emit_load32(c->e, TARGET_REG, CPU_REG, reg_off(o->reg));
}

static void emit_jmp_indirect(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  emit_read_branch_target(c, &insn->operand[0], insn_eip);
  emit_epilogue_from(c->e, TARGET_REG, kX86pJitExitBlockEnd);
}

static void emit_call_indirect(BlockCtx *c, const X86pInsn *insn, uint32_t return_eip, uint32_t insn_eip) {
  emit_read_branch_target(c, &insn->operand[0], insn_eip);
  x86p_a64_emit_mov_w_imm32(c->e, kA64X0, return_eip);
  emit_push_x0(c, insn_eip);
  emit_epilogue_from(c->e, TARGET_REG, kX86pJitExitBlockEnd);
}

/* `release` is RET imm16's argument count, applied AFTER the pop because the
   immediate counts bytes ABOVE the return address. */
static void emit_ret(BlockCtx *c, uint32_t release, uint32_t insn_eip) {
  x86p_a64_emit_load32(c->e, EA_REG, CPU_REG, reg_off(kX86pEsp));
  note_fault(c, emit_bounds_check(c->e, &c->plan, insn_eip, 4));
  emit_host_pointer(c->e, &c->plan);
  x86p_a64_emit_load32(c->e, kA64X0, HOSTPTR_REG, 0);

  x86p_a64_emit_mov_w_w(c->e, kA64X1, EA_REG);
  x86p_a64_emit_alu_w_imm(c->e, kA64Add, kA64X1, 4u + release);
  x86p_a64_emit_store32(c->e, CPU_REG, reg_off(kX86pEsp), kA64X1);

  emit_epilogue_from(c->e, kA64X0, kX86pJitExitBlockEnd);
}

/* ---- translation ----------------------------------------------------------
 * Identical control flow to jit_x64.c's translator -- see there for the full
 * rationale behind each decision. This is the same function with every emit
 * call retargeted to the AArch64 encoder. */

X86pJitStatus x86p_jit_translate(const X86pMem *mem,
                                 uint32_t eip,
                                 void *code,
                                 size_t code_cap,
                                 X86pJitBlock *out,
                                 char *reason,
                                 unsigned reason_len) {
  return x86p_jit_translate_bounded(mem, eip, code, code_cap, NULL, NULL, out, reason, reason_len);
}

X86pJitStatus x86p_jit_translate_bounded(const X86pMem *mem,
                                         uint32_t eip,
                                         void *code,
                                         size_t code_cap,
                                         X86pJitBoundaryFn boundary,
                                         void *boundary_user,
                                         X86pJitBlock *out,
                                         char *reason,
                                         unsigned reason_len) {
  X86pA64Emit e;
  BlockCtx ctx;
  uint32_t pc = eip;
  uint32_t count = 0;
  X86pJitExit exit = kX86pJitExitBlockEnd;
  const char *stopper = NULL;
  int terminated = 0;
  int last_kind = -1;

  if (!mem || !out || !code) {
    say(reason, reason_len, "null argument");
    return kX86pJitOutOfSpace;
  }
  memset(out, 0, sizeof *out);

  if (!x86p_jit_available()) {
    say(reason, reason_len, "no ARM64 backend in this build; host is not ARM64");
    return kX86pJitOutOfSpace;
  }

  x86p_a64_emit_init(&e, code, code_cap);
  memset(&ctx, 0, sizeof ctx);
  ctx.e = &e;
  ctx.mem = mem;
  ctx.plan.host = (uint64_t)(uintptr_t)mem->host;
  ctx.plan.lo = mem->lo;
  ctx.plan.size = mem->size;
  emit_prologue(&e);

  for (;;) {
    uint8_t bytes[X86P_MAX_INSN_LEN];
    X86pInsn insn;
    uint32_t avail;
    uint32_t i;

    if (count >= MAX_INSNS) {
      break;
    }
    if (e.len + WORST_CASE_INSN_BYTES + EPILOGUE_BYTES > code_cap) {
      break;
    }

    avail = 0;
    for (i = 0; i < (uint32_t)X86P_MAX_INSN_LEN; i++) {
      uint32_t byte;
      if (!x86p_mem_read(mem, pc + i, 1, &byte)) {
        break;
      }
      bytes[i] = (uint8_t)byte;
      avail++;
    }
    if (avail == 0) {
      if (count == 0) {
        say(reason, reason_len, "guest EIP %08X is not mapped", pc);
        return kX86pJitFetchFault;
      }
      break;
    }

    if (!x86p_decode(bytes, avail, &insn)) {
      if (count == 0) {
        say(reason, reason_len, "the bytes at %08X are not an instruction", pc);
        return kX86pJitDecodeFailed;
      }
      break;
    }

    if (pc != eip && boundary && boundary(pc, boundary_user)) {
      stopper = "consumer interception point";
      break;
    }

    if (!can_emit(&insn)) {
      if (count == 0) {
        say(reason, reason_len, "%s at %08X has no JIT emitter in this build", insn.mnemonic, pc);
        return kX86pJitUnsupportedAtEntry;
      }
      exit = kX86pJitExitUnsupported;
      stopper = insn.mnemonic;
      break;
    }

    if (insn.op == (uint8_t)kX86pInsnCall || insn.op == (uint8_t)kX86pInsnRet || is_indirect_branch(&insn)) {
      uint32_t next = pc + insn.length;
      if (is_indirect_branch(&insn)) {
        emit_jmp_indirect(&ctx, &insn, pc);
      } else if (insn.op != (uint8_t)kX86pInsnCall) {
        emit_ret(&ctx, insn.operands == 1 ? insn.operand[0].imm : 0u, pc);
      } else if (insn.operand[0].kind == kX86pOperandImm) {
        emit_call_rel(&ctx, next, next + insn.operand[0].imm, pc);
      } else {
        emit_call_indirect(&ctx, &insn, next, pc);
      }
      pc = next;
      count++;
      terminated = 1;
      break;
    }

    if (is_relative_branch(&insn)) {
      uint32_t next = pc + insn.length;
      uint32_t target = next + insn.operand[0].imm;
      if (insn.op == (uint8_t)kX86pInsnJmp) {
        emit_epilogue(&e, target, kX86pJitExitBlockEnd);
      } else {
        emit_jcc(&e, insn.cond, target, next);
      }
      pc = next;
      count++;
      terminated = 1;
      break;
    }

    switch (insn.op) {
    case kX86pInsnNop:
      break;
    case kX86pInsnMov:
      emit_mov(&ctx, &insn, pc);
      break;
    case kX86pInsnMovzx:
      emit_movx(&ctx, &insn, 0, pc);
      break;
    case kX86pInsnMovsx:
      emit_movx(&ctx, &insn, 1, pc);
      break;
    case kX86pInsnXchg:
      emit_xchg32(&ctx, &insn, pc);
      break;
    case kX86pInsnSetcc:
      emit_setcc(&ctx, &insn, pc);
      break;
    case kX86pInsnAluUnary: {
      int dead = flag_write_is_dead(mem, pc + insn.length, eip, boundary, boundary_user, count, e.len, code_cap);
      int k = emit_alu_unary_inline(&ctx, &insn, last_kind, dead, pc);
      if (k >= 0 && !dead) {
        last_kind = k;
      }
      break;
    }
    case kX86pInsnLea:
      emit_lea(&ctx, &insn);
      break;
    case kX86pInsnLeave:
      emit_leave(&ctx, pc);
      break;
    case kX86pInsnCdq:
      emit_cdq(&e);
      break;
    case kX86pInsnDiv:
      emit_div32(&ctx, &insn, pc, 0);
      break;
    case kX86pInsnIdiv:
      emit_div32(&ctx, &insn, pc, 1);
      break;
    case kX86pInsnMul:
      emit_mul32(&ctx, &insn, pc);
      last_kind = -1;
      break;
    case kX86pInsnImul:
      emit_imul32(&ctx, &insn, pc);
      last_kind = -1;
      break;
    case kX86pInsnString:
      emit_string(&ctx, &insn, pc);
      if (insn.str == (uint8_t)kX86pStringScas || insn.str == (uint8_t)kX86pStringCmps) {
        last_kind = -1;
      }
      break;
    case kX86pInsnX87:
      if (x87_arith_is_emittable(&insn)) {
        emit_x87_arith(&ctx, &insn, pc);
      } else if (x87_compare_mem_is_emittable(&insn)) {
        emit_x87_compare_mem(&ctx, &insn, pc);
      } else if (x87_store_reg_is_emittable(&insn)) {
        emit_x87_store_reg(&ctx, &insn);
      } else if (x87_store_mem_is_emittable(&insn)) {
        emit_x87_store_mem(&ctx, &insn, pc);
      } else if (x87_constant_is_emittable(&insn)) {
        emit_x87_constant(&ctx, &insn);
      } else if (x87_status_ax_is_emittable(&insn)) {
        emit_x87_status_ax(&ctx);
      } else if (x87_clear_exceptions_is_emittable(&insn)) {
        emit_x87_clear_exceptions(&ctx);
      } else {
        emit_x87_load(&ctx, &insn, pc);
      }
      break;
    case kX86pInsnPush:
      emit_push(&ctx, &insn, pc);
      break;
    case kX86pInsnPop:
      emit_pop(&ctx, &insn, pc);
      break;
    case kX86pInsnPushfd:
      emit_pushfd(&ctx, pc);
      break;
    case kX86pInsnPopfd:
      emit_popfd(&ctx, pc);
      /* x86p_flags_set_explicit unconditionally records Explicit, exactly
         like ADC/SBB below -- the next carry-in is statically known rather
         than worth a helper call to ask. */
      last_kind = (int)kX86pFlagsExplicit;
      break;
    case kX86pInsnAlu: {
      X86pA64Alu host;
      X86pFlagKind kind;
      int writes_dest;
      if (inline_alu_shape(insn.alu, &host, &kind, &writes_dest)) {
        int dead = flag_write_is_dead(mem, pc + insn.length, eip, boundary, boundary_user, count, e.len, code_cap);
        emit_alu_inline(&ctx, &insn, host, kind, writes_dest, last_kind, dead, pc);
        if (!dead) {
          last_kind = (int)kind;
        }
      } else {
        emit_alu(&e, &insn);
        if (insn.alu >= (uint8_t)kX86pAluShl && insn.alu <= (uint8_t)kX86pAluSar) {
          last_kind = -1;
        } else {
          last_kind = (int)kX86pFlagsExplicit;
        }
      }
      break;
    }
    default:
      say(reason, reason_len, "internal: %s passed can_emit but has no emitter", insn.mnemonic);
      return kX86pJitOutOfSpace;
    }

    pc += insn.length;
    count++;
  }

  if (!terminated) {
    emit_epilogue(&e, pc, exit);
  }

  /*
   * The shared fault stub, AFTER the normal return so it is never fallen
   * into. FAULTPC_REG holds the guest EIP of whichever access failed, set
   * immediately before each bounds check.
   */
  if (ctx.nfaults) {
    unsigned f;
    for (f = 0; f < ctx.nfaults; f++) {
      x86p_a64_emit_bind(&e, ctx.faults[f]);
    }
    x86p_a64_emit_store32(&e, CPU_REG, eip_off(), FAULTPC_REG);
    x86p_a64_emit_mov_w_imm32(&e, kA64X0, (uint32_t)kX86pJitExitMemoryFault);
    x86p_a64_emit_pop_pair(&e, CPU_REG, kA64Lr);
    x86p_a64_emit_ret(&e);
  }

  if (ctx.ndivide_faults) {
    unsigned f;
    for (f = 0; f < ctx.ndivide_faults; f++) {
      x86p_a64_emit_bind(&e, ctx.divide_faults[f]);
    }
    x86p_a64_emit_store32(&e, CPU_REG, eip_off(), FAULTPC_REG);
    x86p_a64_emit_mov_w_imm32(&e, kA64X0, (uint32_t)kX86pJitExitDivideError);
    x86p_a64_emit_pop_pair(&e, CPU_REG, kA64Lr);
    x86p_a64_emit_ret(&e);
  }

  if (!x86p_a64_emit_sites_bound(&e)) {
    say(reason, reason_len, "internal: %u jump site(s) left unbound at %08X", e.sites_made - e.sites_bound, eip);
    return kX86pJitOutOfSpace;
  }

  if (!x86p_a64_emit_ok(&e)) {
    say(reason, reason_len, "code buffer of %zu byte(s) too small for the block at %08X", code_cap, eip);
    return kX86pJitOutOfSpace;
  }
  if (count == 0) {
    say(reason, reason_len, "translated 0 instructions at %08X", eip);
    return kX86pJitUnsupportedAtEntry;
  }

  out->entry = code;
  out->guest_eip = eip;
  out->guest_len = pc - eip;
  out->insns = count;
  out->host_bytes = e.len;
  out->stopper = stopper;
  out->flag_helper_calls = ctx.flag_helper_calls;
  out->ends_in_branch = terminated;
  return kX86pJitOk;
}

X86pJitExit x86p_jit_enter(const X86pJitBlock *b, X86pCpu *cpu) {
  uint32_t (*fn)(X86pCpu *);
  if (!b || !b->entry || !cpu) {
    return kX86pJitExitUnsupported;
  }
  *(void **)&fn = b->entry;
  return (X86pJitExit)fn(cpu);
}

int x86p_jit_emits_natively(const X86pInsn *insn) {
  return can_emit(insn);
}

int x86p_jit_can_translate(const X86pInsn *insn) {
  return can_emit(insn);
}

const char *x86p_jit_exit_name(X86pJitExit e) {
  switch (e) {
  case kX86pJitExitBlockEnd:
    return "block end";
  case kX86pJitExitUnsupported:
    return "unsupported instruction";
  case kX86pJitExitMemoryFault:
    return "guest memory fault";
  case kX86pJitExitDivideError:
    return "divide error";
  case kX86pJitExitInterrupt:
    return "software interrupt";
  case kX86pJitExitProtectionFault:
    return "general-protection fault";
  case kX86pJitExitBoundRange:
    return "bound range exceeded";
  case kX86pJitExitCount:
  default:
    return "?";
  }
}

const char *x86p_jit_status_name(X86pJitStatus s) {
  switch (s) {
  case kX86pJitOk:
    return "ok";
  case kX86pJitFetchFault:
    return "fetch fault";
  case kX86pJitDecodeFailed:
    return "decode failed";
  case kX86pJitUnsupportedAtEntry:
    return "unsupported at entry";
  case kX86pJitOutOfSpace:
    return "out of space";
  case kX86pJitStatusCount:
  default:
    return "?";
  }
}
