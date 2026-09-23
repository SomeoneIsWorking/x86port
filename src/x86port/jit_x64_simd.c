#include "jit_x64_internal.h"
#include "simd.h"
#include <stddef.h>

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
