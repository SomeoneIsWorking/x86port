#include "diagnostic.h"
#include "jit_x64_internal.h"
#include "simd.h"
#include <stddef.h>
#include <string.h>

static int xmm(const X86pOperand *o) {
  return o->kind == kX86pOperandXmm && o->reg >= 0 && o->reg < 8;
}
static int vector_mem(const X86pOperand *o) {
  return o->kind == kX86pOperandMem && o->size == 16 && !o->addr16;
}
int simd_bits_is_emittable(const X86pInsn *insn) {
  const X86pOperand *d = &insn->operand[0], *s = &insn->operand[1];
  if (insn->simd == kX86pSimdEmms) {
    return insn->operands == 0;
  }
  if (insn->simd == kX86pSimdShufps) {
    return insn->operands == 3 && xmm(d) && (xmm(s) || vector_mem(s)) && insn->operand[2].kind == kX86pOperandImm;
  }
  if (insn->operands != 2) {
    return 0;
  }
  if (insn->simd == kX86pSimdMovhlps || insn->simd == kX86pSimdMovlhps) {
    return xmm(d) && xmm(s);
  }
  if (insn->simd == kX86pSimdMovlps || insn->simd == kX86pSimdMovhps) {
    const int dm = d->kind == kX86pOperandMem && d->size == 8 && !d->addr16;
    const int sm = s->kind == kX86pOperandMem && s->size == 8 && !s->addr16;
    return (xmm(d) && sm) || (dm && xmm(s));
  }
  if (insn->simd == kX86pSimdMovss) {
    const int dm = d->kind == kX86pOperandMem && d->size == 4 && !d->addr16;
    const int sm = s->kind == kX86pOperandMem && s->size == 4 && !s->addr16;
    return (xmm(d) && (xmm(s) || sm)) || (dm && xmm(s));
  }
  if (insn->simd == kX86pSimdMovaps) {
    return (xmm(d) && (xmm(s) || vector_mem(s))) || (vector_mem(d) && xmm(s));
  }
  return (insn->simd == kX86pSimdAddps || insn->simd == kX86pSimdSubps || insn->simd == kX86pSimdMulps ||
          insn->simd == kX86pSimdDivps || insn->simd == kX86pSimdAndps || insn->simd == kX86pSimdAndnps ||
          insn->simd == kX86pSimdOrps || insn->simd == kX86pSimdXorps) &&
         xmm(d) && (xmm(s) || vector_mem(s));
}
/* The host register form of a packed guest operation, or 0 for one
   simd_bits_is_emittable does not admit. */
static int packed_op(X86pSimdOp op) {
  switch (op) {
  case kX86pSimdAddps:
    return kX64Addps;
  case kX86pSimdSubps:
    return kX64Subps;
  case kX86pSimdMulps:
    return kX64Mulps;
  case kX86pSimdDivps:
    return kX64Divps;
  case kX86pSimdAndps:
    return kX64Andps;
  case kX86pSimdAndnps:
    return kX64Andnps;
  case kX86pSimdOrps:
    return kX64Orps;
  case kX86pSimdXorps:
    return kX64Xorps;
  case kX86pSimdMovhlps:
    return kX64Movhlps;
  case kX86pSimdMovlhps:
    return kX64Movlhps;
  default:
    return 0;
  }
}

void emit_simd_bits(BlockCtx *c, const X86pInsn *insn, uint32_t pc) {
  if (insn->simd == kX86pSimdEmms) {
    x86p_emit_lea64(c->e, X86P_JIT_HOST_ARG0, CPU_REG, (int32_t)offsetof(X86pCpu, x87));
    x86p_emit_mov_r64_imm64(c->e, kX64Rax, (uint64_t)(uintptr_t)&x86p_x87_emms);
    x86p_emit_call_r64(c->e, kX64Rax);
    return;
  }
  const X86pOperand *d = &insn->operand[0], *s = &insn->operand[1];
  const int memory_dst = d->kind == kX86pOperandMem;
  const int memory_src = s->kind == kX86pOperandMem;
  const int32_t dst = (int32_t)offsetof(X86pCpu, xmm) + d->reg * 16;
  const int32_t source = (int32_t)offsetof(X86pCpu, xmm) + s->reg * 16;
  if (memory_dst || memory_src) {
    emit_mem_prepare_w(c,
                       memory_dst ? d : s,
                       pc,
                       insn->simd == kX86pSimdMovss                                     ? 4
                       : insn->simd == kX86pSimdMovlps || insn->simd == kX86pSimdMovhps ? 8
                                                                                        : 16);
  }
  /*
   * A guest XMM register is written and read as a whole: sixteen-byte host
   * moves and host register forms, never lane stores. A sixteen-byte load of
   * a slot just written by narrower stores cannot be forwarded from them and
   * waits for them to reach the cache; on the Dead Zone route those stalls
   * were the hottest instructions in translated code. Every form here moves
   * bits without interpreting them, so the host instruction is the guest's.
   * A store to guest memory still reads the slot narrowly, which forwards.
   */
  const X86pHostReg src_base = memory_src ? HOSTPTR_REG : CPU_REG;
  const int32_t src_disp = memory_src ? 0 : source;
  if (memory_dst) {
    if (insn->simd == kX86pSimdMovaps) {
      x86p_emit_movups_load(c->e, kX64Xmm0, CPU_REG, source);
      x86p_emit_movups_store(c->e, HOSTPTR_REG, 0, kX64Xmm0);
      return;
    }
    /* MOVSS m32, MOVLPS m64 and MOVHPS m64. */
    const int32_t from = source + (insn->simd == kX86pSimdMovhps ? 8 : 0);
    const int32_t words = insn->simd == kX86pSimdMovss ? 1 : 2;
    for (int32_t k = 0; k < words; k++) {
      x86p_emit_load32(c->e, kX64Rsi, CPU_REG, from + 4 * k);
      x86p_emit_store32(c->e, HOSTPTR_REG, 4 * k, kX64Rsi);
    }
    return;
  }
  if (insn->simd == kX86pSimdMovaps) {
    x86p_emit_movups_load(c->e, kX64Xmm0, src_base, src_disp);
    x86p_emit_movups_store(c->e, CPU_REG, dst, kX64Xmm0);
    return;
  }
  if (insn->simd == kX86pSimdMovss && memory_src) {
    x86p_emit_movss_load(c->e, kX64Xmm0, HOSTPTR_REG, 0); /* zeroes lanes 1-3 */
    x86p_emit_movups_store(c->e, CPU_REG, dst, kX64Xmm0);
    return;
  }
  x86p_emit_movups_load(c->e, kX64Xmm0, CPU_REG, dst);
  if (insn->simd == kX86pSimdMovlps || insn->simd == kX86pSimdMovhps) {
    x86p_emit_half_load(c->e, insn->simd == kX86pSimdMovlps ? kX64Movlps : kX64Movhps, kX64Xmm0, HOSTPTR_REG, 0);
  } else {
    x86p_emit_movups_load(c->e, kX64Xmm1, src_base, src_disp);
    /*
     * The arithmetic forms are the host instruction itself. x86p_simd_addps
     * and its siblings compute `a[i] op b[i]` on host binary32 in the host's
     * default environment, which is exactly what these do; neither reads the
     * guest MXCSR (simd_float.c states why that holds for every process this
     * framework targets). Operand order is the guest's -- the destination
     * first -- so when both lanes are NaN the result is the destination's, as
     * on the hardware the guest was written for.
     */
    switch (insn->simd) {
    case kX86pSimdMovss:
      x86p_emit_movss_rr(c->e, kX64Xmm0, kX64Xmm1);
      break;
    case kX86pSimdShufps:
      x86p_emit_shufps(c->e, kX64Xmm0, kX64Xmm1, (uint8_t)insn->operand[2].imm);
      break;
    default:
      if (!packed_op((X86pSimdOp)insn->simd)) {
        c->e->overflow = 1; /* a form the predicate never admits: refuse the block */
        return;
      }
      x86p_emit_packed_ps(c->e, (X86pHostPacked)packed_op((X86pSimdOp)insn->simd), kX64Xmm0, kX64Xmm1);
      break;
    }
  }
  x86p_emit_movups_store(c->e, CPU_REG, dst, kX64Xmm0);
}

/*
 * Every other SIMD form runs its semantics owner, x86p_simd_execute_host, from
 * the block: the block computes and bounds-checks the memory operand, and the
 * helper reads the operands, computes and writes back. The shape word carries
 * the operand kinds and registers, which is all the owner reads of an operand
 * besides its memory, so the helper needs no decoded instruction at run time.
 */
enum { kShapeKindBits = 4, kShapeRegBits = 3 };

/* MMX registers alias the x87 file; SIMD does not keep the x87 mirror, so the
   block has flushed it and the helper finds the register file complete. */
static int call_operand_ok(const X86pOperand *o) {
  switch (o->kind) {
  case kX86pOperandXmm:
  case kX86pOperandMmx:
    return o->reg >= 0 && o->reg < 8;
  case kX86pOperandReg:
    return o->reg >= 0 && o->reg < 8 && o->size == 4;
  case kX86pOperandMem:
    return !o->addr16;
  case kX86pOperandImm:
    return 1;
  default:
    return 0;
  }
}

int simd_call_is_emittable(const X86pInsn *insn) {
  const X86pSimdOp op = (X86pSimdOp)insn->simd;
  int memories = 0;
  int i;
  if (!x86p_simd_is_implemented(op) || op == kX86pSimdMaskmovdqu || op == kX86pSimdPf || insn->operands > 3) {
    return 0;
  }
  for (i = 0; i < insn->operands; i++) {
    if (!call_operand_ok(&insn->operand[i])) {
      return 0;
    }
    memories += insn->operand[i].kind == kX86pOperandMem;
  }
  return memories <= 1;
}

static uint32_t shape_operand(const X86pOperand *o) {
  return (uint32_t)o->kind | ((uint32_t)(o->reg < 0 ? 0 : o->reg) << kShapeKindBits);
}

static void unshape_operand(uint32_t bits, X86pOperand *o) {
  o->kind = (uint8_t)(bits & ((1u << kShapeKindBits) - 1u));
  o->reg = (int8_t)((bits >> kShapeKindBits) & ((1u << kShapeRegBits) - 1u));
  o->size = 4;
}

static void simd_call(X86pCpu *cpu, uint8_t *memory, uint32_t shape, uint32_t immediate) {
  const uint32_t per = kShapeKindBits + kShapeRegBits;
  X86pInsn insn;
  int i;
  memset(&insn, 0, sizeof insn);
  insn.op = (uint8_t)kX86pInsnSimd;
  insn.simd = (uint8_t)(shape & 0xFFu);
  insn.operands = (uint8_t)((shape >> 8) & 3u);
  for (i = 0; i < insn.operands; i++) {
    unshape_operand(shape >> (10u + per * (uint32_t)i), &insn.operand[i]);
    if (insn.operand[i].kind == kX86pOperandImm) {
      insn.operand[i].imm = immediate;
      insn.operand[i].size = 1;
    }
  }
  if (x86p_simd_execute_host(cpu, memory, &insn) != kX86pSimdOk) {
    x86p_diagnostic_fatalf("jit.x64.simd", "%s was admitted and refused", x86p_simd_op_name((X86pSimdOp)insn.simd));
  }
}

void emit_simd_call(BlockCtx *c, const X86pInsn *insn, uint32_t pc) {
  const uint32_t per = kShapeKindBits + kShapeRegBits;
  uint32_t shape = (uint32_t)insn->simd | ((uint32_t)insn->operands << 8);
  uint32_t immediate = 0u;
  const X86pOperand *memory = NULL;
  int i;
  for (i = 0; i < insn->operands; i++) {
    const X86pOperand *o = &insn->operand[i];
    shape |= shape_operand(o) << (10u + per * (uint32_t)i);
    if (o->kind == kX86pOperandImm) {
      immediate = o->imm;
    } else if (o->kind == kX86pOperandMem) {
      memory = o;
    }
  }
  if (memory && x86p_simd_memory_bytes(insn) > 0u) {
    emit_mem_prepare_w(c, memory, pc, (int)x86p_simd_memory_bytes(insn));
    x86p_emit_mov_r64_r64(c->e, X86P_JIT_HOST_ARG1, HOSTPTR_REG);
  } else {
    x86p_emit_mov_r32_imm32(c->e, X86P_JIT_HOST_ARG1, 0u);
  }
  x86p_emit_mov_r64_r64(c->e, X86P_JIT_HOST_ARG0, CPU_REG);
  x86p_emit_mov_r32_imm32(c->e, X86P_JIT_HOST_ARG2, shape);
  x86p_emit_mov_r32_imm32(c->e, X86P_JIT_HOST_ARG3, immediate);
  x86p_emit_mov_r64_imm64(c->e, kX64Rax, (uint64_t)(uintptr_t)&simd_call);
  x86p_emit_call_r64(c->e, kX64Rax);
}
