/* WASM owns SIMD operand access, whole-access guards and movement. Arithmetic
 * calls the same lane owners as native execution, with explicit source values. */
#include "jit_wasm_simd.h"

#include "diagnostic.h"
#include "jit_wasm_internal.h"
#include "simd_internal.h"
#include "x87.h"

static const X86pWasmLocal lanes[] = {kX86pWasmLocalA, kX86pWasmLocalB, kX86pWasmLocalR, kX86pWasmLocalCarry};

uint32_t x86p_wasm_simd_arithmetic(X86pCpu *cpu,
                                   uint32_t destination,
                                   uint32_t operation,
                                   uint32_t b0,
                                   uint32_t b1,
                                   uint32_t b2,
                                   uint32_t b3,
                                   uint32_t immediate) {
  const X86pSimdOp op = (X86pSimdOp)operation;
  const int scalar_result = op == kX86pSimdCvtss2si || op == kX86pSimdCvttss2si;
  X86pVec a = {.bytes = scalar_result ? 4u : 16u}, b = {.bytes = 16u}, r = {0};
  memcpy(a.b, cpu->xmm[destination], sizeof a.b);
  vec_set_u32(&b, 0, b0);
  vec_set_u32(&b, 1, b1);
  vec_set_u32(&b, 2, b2);
  vec_set_u32(&b, 3, b3);
  if (!x86p_simd_int(op, &a, &b, (uint8_t)immediate, &r) &&
      !x86p_simd_float(op, &a, &b, (uint8_t)immediate, &r, &cpu->flags)) {
    x86p_diagnostic_fatalf("wasm.simd", "arithmetic admitted unsupported operation %u", operation);
  }
  if (!scalar_result && op != kX86pSimdComiss && op != kX86pSimdUcomiss) {
    memcpy(cpu->xmm[destination], r.b, sizeof r.b);
  }
  return vec_u32(&r, 0);
}

static int xmm(const X86pOperand *o) {
  return o->kind == kX86pOperandXmm && o->reg >= 0 && o->reg < 8;
}

static int mem(const X86pOperand *o, unsigned bytes) {
  return o->kind == kX86pOperandMem && !o->addr16 && o->size == bytes;
}

static int gpr(const X86pOperand *o) {
  return o->kind == kX86pOperandReg && o->reg >= 0 && o->reg < 8 && o->size == 4;
}

static int scalar(X86pSimdOp op) {
  return (op >= kX86pSimdAddss && op <= kX86pSimdSqrtss) || op == kX86pSimdCmpss || op == kX86pSimdComiss ||
         op == kX86pSimdUcomiss;
}

static int immediate_op(X86pSimdOp op) {
  return op == kX86pSimdPextrw || op == kX86pSimdPinsrw || op == kX86pSimdPshufd || op == kX86pSimdCmpps ||
         op == kX86pSimdCmpss || op == kX86pSimdShufps;
}

int x86p_wasm_simd_accepts(const X86pInsn *insn) {
  const X86pSimdOp op = (X86pSimdOp)insn->simd;
  const X86pOperand *d = &insn->operand[0], *s = &insn->operand[1];
  int i;
  /* MMX/3DNow touch raw ext80 aliases which binary128 CPU state cannot hold. */
  for (i = 0; i < insn->operands; ++i) {
    if (insn->operand[i].kind == kX86pOperandMmx) {
      return 0;
    }
  }
  if (op == kX86pSimdEmms || op == kX86pSimdFence) {
    return insn->operands == 0;
  }
  if (op == kX86pSimdPrefetch) {
    return insn->operands == 1 && d->kind == kX86pOperandMem;
  }
  if (op == kX86pSimdLdmxcsr || op == kX86pSimdStmxcsr) {
    return insn->operands == 1 && mem(d, 4);
  }
  if (insn->operands != (immediate_op(op) ? 3 : 2) || (immediate_op(op) && insn->operand[2].kind != kX86pOperandImm)) {
    return 0;
  }
  switch (op) {
  case kX86pSimdMovq:
    return (xmm(d) && (xmm(s) || mem(s, 8))) || (mem(d, 8) && xmm(s));
  case kX86pSimdMovd:
    return (xmm(d) && (gpr(s) || mem(s, 4))) || ((gpr(d) || mem(d, 4)) && xmm(s));
  case kX86pSimdMovaps:
    return (xmm(d) && (xmm(s) || mem(s, 16))) || (mem(d, 16) && xmm(s));
  case kX86pSimdMovss:
    return (xmm(d) && (xmm(s) || mem(s, 4))) || (mem(d, 4) && xmm(s));
  case kX86pSimdMovlps:
  case kX86pSimdMovhps:
    return (xmm(d) && mem(s, 8)) || (mem(d, 8) && xmm(s));
  case kX86pSimdMovhlps:
  case kX86pSimdMovlhps:
    return xmm(d) && xmm(s);
  case kX86pSimdMovmskps:
  case kX86pSimdPmovmskb:
  case kX86pSimdPextrw:
    return gpr(d) && xmm(s);
  case kX86pSimdPinsrw:
    return xmm(d) && (gpr(s) || mem(s, 2));
  case kX86pSimdCvtsi2ss:
    return xmm(d) && (gpr(s) || mem(s, 4));
  case kX86pSimdCvtss2si:
  case kX86pSimdCvttss2si:
    return gpr(d) && (xmm(s) || mem(s, 4));
  default:
    if (op >= kX86pSimdPsllw && op <= kX86pSimdPsrad && s->kind == kX86pOperandImm) {
      return xmm(d);
    }
    return ((op >= kX86pSimdPand && op <= kX86pSimdPsadbw) || op == kX86pSimdPshufd ||
            (op >= kX86pSimdAddps && op <= kX86pSimdUnpckhps)) &&
           xmm(d) && (xmm(s) || mem(s, scalar(op) ? 4u : 16u));
  }
}

static void advance_address(X86pWasmLower *l) {
  x86p_wasm_local_get(l->e, kX86pWasmLocalAddr);
  x86p_wasm_i32_const(l->e, 4);
  x86p_wasm_i32_op(l->e, kWasmI32Add);
  x86p_wasm_local_set(l->e, kX86pWasmLocalAddr);
}

/* The complete source range is admitted before any register/flag mutation.
 * Capturing all lanes also preserves every source/destination alias case. */
static void read_source(X86pWasmLower *l, const X86pOperand *s, unsigned bytes, unsigned first_lane, uint32_t pc) {
  unsigned lane;
  if (s->kind == kX86pOperandMem) {
    x86p_wasm_state_guard(&l->state, s, pc, (int)bytes, kX86pMemRead);
  }
  for (lane = 0; lane < 4; ++lane) {
    if (lane * 4u >= bytes) {
      x86p_wasm_i32_const(l->e, 0);
    } else if (s->kind == kX86pOperandMem) {
      x86p_wasm_state_load_mem(&l->state, bytes < 4u ? (int)bytes : 4);
    } else if (xmm(s)) {
      x86p_wasm_state_load_xmm_lane(&l->state, (unsigned)s->reg, first_lane + lane);
    } else if (lane == 0) {
      x86p_wasm_push_operand(l, s, 4);
    } else {
      x86p_wasm_i32_const(l->e, 0);
    }
    x86p_wasm_local_set(l->e, lanes[lane]);
    if (s->kind == kX86pOperandMem && (lane + 1u) * 4u < bytes) {
      advance_address(l);
    }
  }
}

static void store_lanes(X86pWasmLower *l, const X86pOperand *d, unsigned bytes, unsigned first_lane, uint32_t pc) {
  unsigned lane;
  if (d->kind == kX86pOperandMem) {
    x86p_wasm_state_guard(&l->state, d, pc, (int)bytes, kX86pMemWrite);
  }
  for (lane = 0; lane * 4u < bytes; ++lane) {
    if (d->kind == kX86pOperandMem) {
      x86p_wasm_state_store_mem(&l->state, 4, lanes[lane]);
      if ((lane + 1u) * 4u < bytes) {
        advance_address(l);
      }
    } else if (xmm(d)) {
      x86p_wasm_state_store_xmm_lane(&l->state, (unsigned)d->reg, first_lane + lane, lanes[lane]);
    } else {
      x86p_wasm_state_store_reg(&l->state, d->reg, 4, lanes[lane]);
    }
  }
}

static void move(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  const X86pSimdOp op = (X86pSimdOp)insn->simd;
  const X86pOperand *d = &insn->operand[0], *s = &insn->operand[1];
  const unsigned bytes = op == kX86pSimdMovd || op == kX86pSimdMovss ? 4u : op == kX86pSimdMovaps ? 16u : 8u;
  const unsigned from = op == kX86pSimdMovhlps || (op == kX86pSimdMovhps && d->kind == kX86pOperandMem) ? 2u : 0u;
  const unsigned to = op == kX86pSimdMovlhps || (op == kX86pSimdMovhps && s->kind == kX86pOperandMem) ? 2u : 0u;
  const int zero_high =
      xmm(d) && (op == kX86pSimdMovd || op == kX86pSimdMovq || (op == kX86pSimdMovss && s->kind == kX86pOperandMem));
  read_source(l, s, bytes, from, pc);
  store_lanes(l, d, zero_high ? 16u : bytes, to, pc);
}

static void extract_mask(X86pWasmLower *l, const X86pInsn *insn) {
  const unsigned stride = insn->simd == kX86pSimdMovmskps ? 32u : 8u;
  unsigned i;
  x86p_wasm_i32_const(l->e, 0);
  for (i = 0; i < 128u / stride; ++i) {
    x86p_wasm_state_load_xmm_lane(&l->state, (unsigned)insn->operand[1].reg, i * stride / 32u);
    x86p_wasm_i32_const(l->e, (int32_t)((i * stride) % 32u + stride - 1u));
    x86p_wasm_i32_op(l->e, kWasmI32ShrU);
    x86p_wasm_i32_const(l->e, 1);
    x86p_wasm_i32_op(l->e, kWasmI32And);
    x86p_wasm_i32_const(l->e, (int32_t)i);
    x86p_wasm_i32_op(l->e, kWasmI32Shl);
    x86p_wasm_i32_op(l->e, kWasmI32Or);
  }
  x86p_wasm_local_set(l->e, kX86pWasmLocalR);
  x86p_wasm_state_store_reg(&l->state, insn->operand[0].reg, 4, kX86pWasmLocalR);
}

static void word_transfer(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  const unsigned word = insn->operand[2].imm & 7u, shift = (word & 1u) * 16u;
  const X86pOperand *d = &insn->operand[0], *s = &insn->operand[1];
  if (insn->simd == kX86pSimdPextrw) {
    x86p_wasm_state_load_xmm_lane(&l->state, (unsigned)s->reg, word / 2u);
    x86p_wasm_i32_const(l->e, (int32_t)shift);
    x86p_wasm_i32_op(l->e, kWasmI32ShrU);
    x86p_wasm_i32_const(l->e, 0xffff);
    x86p_wasm_i32_op(l->e, kWasmI32And);
    x86p_wasm_local_set(l->e, kX86pWasmLocalR);
    x86p_wasm_state_store_reg(&l->state, d->reg, 4, kX86pWasmLocalR);
  } else {
    read_source(l, s, 2, 0, pc);
    x86p_wasm_state_load_xmm_lane(&l->state, (unsigned)d->reg, word / 2u);
    x86p_wasm_i32_const(l->e, (int32_t)~(0xffffu << shift));
    x86p_wasm_i32_op(l->e, kWasmI32And);
    x86p_wasm_local_get(l->e, lanes[0]);
    x86p_wasm_i32_const(l->e, 0xffff);
    x86p_wasm_i32_op(l->e, kWasmI32And);
    x86p_wasm_i32_const(l->e, (int32_t)shift);
    x86p_wasm_i32_op(l->e, kWasmI32Shl);
    x86p_wasm_i32_op(l->e, kWasmI32Or);
    x86p_wasm_local_set(l->e, kX86pWasmLocalR);
    x86p_wasm_state_store_xmm_lane(&l->state, (unsigned)d->reg, word / 2u, kX86pWasmLocalR);
  }
}

void x86p_wasm_simd_lower(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  const X86pSimdOp op = (X86pSimdOp)insn->simd;
  const X86pOperand *d = &insn->operand[0], *s = &insn->operand[1];
  unsigned i, bytes;
  if (op == kX86pSimdFence || op == kX86pSimdPrefetch) {
    return;
  }
  if (op == kX86pSimdEmms) {
    x86p_wasm_state_x87_addr(&l->state);
    x86p_wasm_call_import(l, kX86pWasmImportX87Emms);
    return;
  }
  if (op == kX86pSimdStmxcsr || op == kX86pSimdLdmxcsr) {
    x86p_wasm_state_guard(&l->state, d, pc, 4, op == kX86pSimdStmxcsr ? kX86pMemWrite : kX86pMemRead);
    if (op == kX86pSimdStmxcsr) {
      x86p_wasm_state_load_mxcsr(&l->state);
      x86p_wasm_local_set(l->e, kX86pWasmLocalR);
      x86p_wasm_state_store_mem(&l->state, 4, kX86pWasmLocalR);
    } else {
      x86p_wasm_state_load_mem(&l->state, 4);
      x86p_wasm_local_set(l->e, kX86pWasmLocalR);
      x86p_wasm_state_store_mxcsr(&l->state, kX86pWasmLocalR);
    }
    return;
  }
  if (op <= kX86pSimdMovlhps) {
    move(l, insn, pc);
    return;
  }
  if (op == kX86pSimdMovmskps || op == kX86pSimdPmovmskb) {
    extract_mask(l, insn);
    return;
  }
  if (op == kX86pSimdPextrw || op == kX86pSimdPinsrw) {
    word_transfer(l, insn, pc);
    return;
  }
  bytes = op == kX86pSimdCvtsi2ss || op == kX86pSimdCvtss2si || op == kX86pSimdCvttss2si ||
                  (scalar(op) && s->kind == kX86pOperandMem)
              ? 4u
              : 16u;
  read_source(l, s, bytes, 0, pc);
  x86p_wasm_state_cpu(&l->state);
  x86p_wasm_i32_const(l->e, xmm(d) ? d->reg : 0);
  x86p_wasm_i32_const(l->e, (int32_t)op);
  for (i = 0; i < 4; ++i) {
    x86p_wasm_local_get(l->e, lanes[i]);
  }
  x86p_wasm_i32_const(l->e, insn->operands == 3 ? (int32_t)insn->operand[2].imm : 0);
  x86p_wasm_call_import(l, kX86pWasmImportSimdArithmetic);
  x86p_wasm_local_set(l->e, kX86pWasmLocalR);
  if (gpr(d)) {
    x86p_wasm_state_store_reg(&l->state, d->reg, 4, kX86pWasmLocalR);
  }
  if (op == kX86pSimdComiss || op == kX86pSimdUcomiss) {
    l->last_kind = kX86pFlagsExplicit;
  }
}
