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
/* An SSE2 packed-integer form the host runs as itself: exact integer lanes, so the host
   instruction is the guest's. `ext` is the opcode extension of the shift-by-immediate forms. */
typedef struct IntForm {
  uint8_t prefix;
  uint8_t op;
  uint8_t shift_op;
  uint8_t ext;
} IntForm;

static int int_form(X86pSimdOp op, IntForm *f) {
  static const struct {
    X86pSimdOp simd;
    IntForm form;
  } kForms[] = {
      {kX86pSimdPand, {0x66u, 0xDBu, 0u, 0u}},       {kX86pSimdPandn, {0x66u, 0xDFu, 0u, 0u}},
      {kX86pSimdPor, {0x66u, 0xEBu, 0u, 0u}},        {kX86pSimdPxor, {0x66u, 0xEFu, 0u, 0u}},
      {kX86pSimdPaddb, {0x66u, 0xFCu, 0u, 0u}},      {kX86pSimdPaddw, {0x66u, 0xFDu, 0u, 0u}},
      {kX86pSimdPaddd, {0x66u, 0xFEu, 0u, 0u}},      {kX86pSimdPaddq, {0x66u, 0xD4u, 0u, 0u}},
      {kX86pSimdPaddsb, {0x66u, 0xECu, 0u, 0u}},     {kX86pSimdPaddsw, {0x66u, 0xEDu, 0u, 0u}},
      {kX86pSimdPaddusb, {0x66u, 0xDCu, 0u, 0u}},    {kX86pSimdPaddusw, {0x66u, 0xDDu, 0u, 0u}},
      {kX86pSimdPsubb, {0x66u, 0xF8u, 0u, 0u}},      {kX86pSimdPsubw, {0x66u, 0xF9u, 0u, 0u}},
      {kX86pSimdPsubd, {0x66u, 0xFAu, 0u, 0u}},      {kX86pSimdPsubq, {0x66u, 0xFBu, 0u, 0u}},
      {kX86pSimdPsubsb, {0x66u, 0xE8u, 0u, 0u}},     {kX86pSimdPsubsw, {0x66u, 0xE9u, 0u, 0u}},
      {kX86pSimdPsubusb, {0x66u, 0xD8u, 0u, 0u}},    {kX86pSimdPsubusw, {0x66u, 0xD9u, 0u, 0u}},
      {kX86pSimdPcmpeqb, {0x66u, 0x74u, 0u, 0u}},    {kX86pSimdPcmpeqw, {0x66u, 0x75u, 0u, 0u}},
      {kX86pSimdPcmpeqd, {0x66u, 0x76u, 0u, 0u}},    {kX86pSimdPcmpgtb, {0x66u, 0x64u, 0u, 0u}},
      {kX86pSimdPcmpgtw, {0x66u, 0x65u, 0u, 0u}},    {kX86pSimdPcmpgtd, {0x66u, 0x66u, 0u, 0u}},
      {kX86pSimdPunpcklbw, {0x66u, 0x60u, 0u, 0u}},  {kX86pSimdPunpcklwd, {0x66u, 0x61u, 0u, 0u}},
      {kX86pSimdPunpckldq, {0x66u, 0x62u, 0u, 0u}},  {kX86pSimdPunpckhbw, {0x66u, 0x68u, 0u, 0u}},
      {kX86pSimdPunpckhwd, {0x66u, 0x69u, 0u, 0u}},  {kX86pSimdPunpckhdq, {0x66u, 0x6Au, 0u, 0u}},
      {kX86pSimdPunpcklqdq, {0x66u, 0x6Cu, 0u, 0u}}, {kX86pSimdPunpckhqdq, {0x66u, 0x6Du, 0u, 0u}},
      {kX86pSimdPacksswb, {0x66u, 0x63u, 0u, 0u}},   {kX86pSimdPackuswb, {0x66u, 0x67u, 0u, 0u}},
      {kX86pSimdPackssdw, {0x66u, 0x6Bu, 0u, 0u}},   {kX86pSimdPmullw, {0x66u, 0xD5u, 0u, 0u}},
      {kX86pSimdPmulhw, {0x66u, 0xE5u, 0u, 0u}},     {kX86pSimdPmulhuw, {0x66u, 0xE4u, 0u, 0u}},
      {kX86pSimdPmaddwd, {0x66u, 0xF5u, 0u, 0u}},    {kX86pSimdPmuludq, {0x66u, 0xF4u, 0u, 0u}},
      {kX86pSimdPavgb, {0x66u, 0xE0u, 0u, 0u}},      {kX86pSimdPavgw, {0x66u, 0xE3u, 0u, 0u}},
      {kX86pSimdPminub, {0x66u, 0xDAu, 0u, 0u}},     {kX86pSimdPmaxub, {0x66u, 0xDEu, 0u, 0u}},
      {kX86pSimdPminsw, {0x66u, 0xEAu, 0u, 0u}},     {kX86pSimdPmaxsw, {0x66u, 0xEEu, 0u, 0u}},
      {kX86pSimdPsadbw, {0x66u, 0xF6u, 0u, 0u}},     {kX86pSimdPshufd, {0x66u, 0x70u, 0u, 0u}},
      {kX86pSimdPshuflw, {0xF2u, 0x70u, 0u, 0u}},    {kX86pSimdPshufhw, {0xF3u, 0x70u, 0u, 0u}},
      {kX86pSimdPsrlw, {0x66u, 0xD1u, 0x71u, 2u}},   {kX86pSimdPsraw, {0x66u, 0xE1u, 0x71u, 4u}},
      {kX86pSimdPsllw, {0x66u, 0xF1u, 0x71u, 6u}},   {kX86pSimdPsrld, {0x66u, 0xD2u, 0x72u, 2u}},
      {kX86pSimdPsrad, {0x66u, 0xE2u, 0x72u, 4u}},   {kX86pSimdPslld, {0x66u, 0xF2u, 0x72u, 6u}},
      {kX86pSimdPsrlq, {0x66u, 0xD3u, 0x73u, 2u}},   {kX86pSimdPsllq, {0x66u, 0xF3u, 0x73u, 6u}},
      {kX86pSimdPsrldq, {0x66u, 0u, 0x73u, 3u}},     {kX86pSimdPslldq, {0x66u, 0u, 0x73u, 7u}},
  };
  for (size_t i = 0; i < sizeof kForms / sizeof kForms[0]; i++) {
    if (kForms[i].simd == op) {
      *f = kForms[i].form;
      return 1;
    }
  }
  return 0;
}

static int shuffles(X86pSimdOp op) {
  return op == kX86pSimdPshufd || op == kX86pSimdPshuflw || op == kX86pSimdPshufhw;
}

/* The admitted integer forms: xmm, xmm/m128; xmm, xmm/m128, imm8 for the shuffles; xmm, imm8 for
   the shifts that have an immediate form. */
static int int_is_emittable(const X86pInsn *insn) {
  IntForm f;
  const X86pOperand *d = &insn->operand[0], *s = &insn->operand[1];
  if (!int_form((X86pSimdOp)insn->simd, &f) || !xmm(d)) {
    return 0;
  }
  if (shuffles((X86pSimdOp)insn->simd)) {
    return insn->operands == 3 && (xmm(s) || vector_mem(s)) && insn->operand[2].kind == kX86pOperandImm;
  }
  if (insn->operands != 2) {
    return 0;
  }
  if (s->kind == kX86pOperandImm) {
    return f.shift_op != 0u;
  }
  return f.op != 0u && (xmm(s) || vector_mem(s));
}

int simd_bits_is_emittable(const X86pInsn *insn) {
  if (int_is_emittable(insn)) {
    return 1;
  }
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
  IntForm f;
  if (int_is_emittable(insn) && int_form((X86pSimdOp)insn->simd, &f)) {
    x86p_emit_movups_load(c->e, kX64Xmm0, CPU_REG, dst);
    if (s->kind == kX86pOperandImm) {
      x86p_emit_sse_rr(c->e, f.prefix, f.shift_op, f.ext, kX64Xmm0, 1, (uint8_t)s->imm);
    } else {
      const int imm = shuffles((X86pSimdOp)insn->simd);
      x86p_emit_movups_load(c->e, kX64Xmm1, src_base, src_disp);
      x86p_emit_sse_rr(c->e, f.prefix, f.op, kX64Xmm0, kX64Xmm1, imm, imm ? (uint8_t)insn->operand[2].imm : 0u);
    }
    x86p_emit_movups_store(c->e, CPU_REG, dst, kX64Xmm0);
    return;
  }
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
