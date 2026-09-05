#include "jit_x64_internal.h"
#include "simd.h"
#include "simd_packed.h"
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
  if (insn->simd == kX86pSimdMovlps || insn->simd == kX86pSimdMovhps || insn->simd == kX86pSimdMovhlps ||
      insn->simd == kX86pSimdMovlhps) {
    const int32_t source_half = insn->simd == kX86pSimdMovhlps || (memory_dst && insn->simd == kX86pSimdMovhps) ? 8 : 0;
    const int32_t dest_half = insn->simd == kX86pSimdMovlhps || (memory_src && insn->simd == kX86pSimdMovhps) ? 8 : 0;
    const int32_t from = memory_src ? 0 : source + source_half;
    const int32_t to = memory_dst ? 0 : dst + dest_half;
    x86p_emit_load32(c->e, kX64Rax, memory_src ? HOSTPTR_REG : CPU_REG, from);
    x86p_emit_load32(c->e, kX64Rcx, memory_src ? HOSTPTR_REG : CPU_REG, from + 4);
    x86p_emit_store32(c->e, memory_dst ? HOSTPTR_REG : CPU_REG, to, kX64Rax);
    x86p_emit_store32(c->e, memory_dst ? HOSTPTR_REG : CPU_REG, to + 4, kX64Rcx);
    return;
  }
  if (insn->simd == kX86pSimdMovss) {
    x86p_emit_load32(c->e, kX64Rsi, memory_src ? HOSTPTR_REG : CPU_REG, memory_src ? 0 : source);
    x86p_emit_store32(c->e, memory_dst ? HOSTPTR_REG : CPU_REG, memory_dst ? 0 : dst, kX64Rsi);
    if (memory_src) {
      x86p_emit_mov_r32_imm32(c->e, kX64Rsi, 0);
      for (int32_t lane = 4; lane < 16; lane += 4) {
        x86p_emit_store32(c->e, CPU_REG, dst + lane, kX64Rsi);
      }
    }
    return;
  }
  void (*arithmetic)(void *, const void *) = insn->simd == kX86pSimdAddps   ? x86p_simd_addps
                                             : insn->simd == kX86pSimdSubps ? x86p_simd_subps
                                             : insn->simd == kX86pSimdMulps ? x86p_simd_mulps
                                             : insn->simd == kX86pSimdDivps ? x86p_simd_divps
                                                                            : NULL;
  if (arithmetic) {
    x86p_emit_lea64(c->e, X86P_JIT_HOST_ARG0, CPU_REG, dst);
    x86p_emit_lea64(c->e, X86P_JIT_HOST_ARG1, memory_src ? HOSTPTR_REG : CPU_REG, memory_src ? 0 : source);
    x86p_emit_mov_r64_imm64(c->e, kX64Rax, (uint64_t)(uintptr_t)arithmetic);
    x86p_emit_call_r64(c->e, kX64Rax);
    return;
  }
  if (insn->simd == kX86pSimdShufps) {
    const int lanes[4] = {kX64Rax, kX64Rcx, kX64Rdx, kX64Rsi};
    /* Read all selected lanes before writing: either input may alias XMMd. */
    for (int32_t lane = 0; lane < 4; lane++) {
      const int32_t selected = (int32_t)((insn->operand[2].imm >> (lane * 2)) & 3u) * 4;
      x86p_emit_load32(c->e,
                       lanes[lane],
                       lane < 2 || !memory_src ? CPU_REG : HOSTPTR_REG,
                       (lane < 2     ? dst
                        : memory_src ? 0
                                     : source) +
                           selected);
    }
    for (int32_t lane = 0; lane < 4; lane++) {
      x86p_emit_store32(c->e, CPU_REG, dst + lane * 4, lanes[lane]);
    }
    return;
  }
  for (int32_t lane = 0; lane < 16; lane += 4) {
    x86p_emit_load32(c->e, kX64Rsi, memory_src ? HOSTPTR_REG : CPU_REG, memory_src ? lane : source + lane);
    if (insn->simd != kX86pSimdMovaps) {
      x86p_emit_load32(c->e, kX64Rdx, CPU_REG, dst + lane);
      if (insn->simd == kX86pSimdAndnps) {
        x86p_emit_alu_r32_imm32(c->e, kX64Xor, kX64Rdx, UINT32_MAX);
      }
      x86p_emit_alu_r32_r32(c->e,
                            insn->simd == kX86pSimdOrps    ? kX64Or
                            : insn->simd == kX86pSimdXorps ? kX64Xor
                                                           : kX64And,
                            kX64Rsi,
                            kX64Rdx);
    }
    x86p_emit_store32(c->e, memory_dst ? HOSTPTR_REG : CPU_REG, memory_dst ? lane : dst + lane, kX64Rsi);
  }
}
