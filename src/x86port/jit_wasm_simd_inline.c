/*
 * jit_wasm_simd_inline.c -- the packed SSE forms, emitted.
 * See jit_wasm_simd_inline.h for the contract and for what made it safe.
 */
#include "jit_wasm_simd_inline.h"

#include "jit_wasm_internal.h"
#include "simd.h"

/*
 * The operand order a guest operation wants its two v128 sources pushed in.
 *
 * Almost every one is (destination, source), because that is what x86 means by
 * `op dst, src`. ANDNPS is the exception: it computes `~dst & src`, and the
 * host instruction that expresses that is `v128.andnot`, which computes
 * `a & ~b`. Pushing the source first and the destination second makes the two
 * agree. A comment would not have been enough here -- getting it backwards
 * produces a plausible wrong answer rather than a failure -- so the order is a
 * field of the row and the test drives ANDNPS with distinct operands.
 */
typedef struct PackedForm {
  X86pWasmSimdOp host;
  int source_first;
} PackedForm;

/*
 * Which guest operations this unit emits, and as what.
 *
 * ONLY WHERE THE HOST INSTRUCTION IS UNCONDITIONALLY THE SAME FUNCTION.
 * The four bitwise rows are exact on every input. The four arithmetic rows are
 * IEEE 754 binary32 in the default environment, which is what simd_packed.c's
 * `a[i] op b[i]` compiles to -- see the header on why no MXCSR guard is
 * emitted. MINPS and MAXPS are deliberately absent: WebAssembly's f32x4.min
 * and f32x4.max differ from x86 on NaN and on signed zero, and while
 * f32x4.pmin / f32x4.pmax are defined to match, nothing on the measured route
 * runs either, so they stay on the helper rather than being added untested.
 * SQRTPS, the compares, the converts and every scalar `*ss` form are likewise
 * left alone.
 */
static int packed_form(X86pSimdOp op, PackedForm *out) {
  switch (op) {
  case kX86pSimdAddps:
    out->host = kWasmF32x4Add;
    out->source_first = 0;
    return 1;
  case kX86pSimdSubps:
    out->host = kWasmF32x4Sub;
    out->source_first = 0;
    return 1;
  case kX86pSimdMulps:
    out->host = kWasmF32x4Mul;
    out->source_first = 0;
    return 1;
  case kX86pSimdDivps:
    out->host = kWasmF32x4Div;
    out->source_first = 0;
    return 1;
  case kX86pSimdAndps:
    out->host = kWasmV128And;
    out->source_first = 0;
    return 1;
  case kX86pSimdOrps:
    out->host = kWasmV128Or;
    out->source_first = 0;
    return 1;
  case kX86pSimdXorps:
    out->host = kWasmV128Xor;
    out->source_first = 0;
    return 1;
  case kX86pSimdAndnps:
    out->host = kWasmV128AndNot;
    out->source_first = 1;
    return 1;
  default:
    return 0;
  }
}

static int xmm_operand(const X86pOperand *o) {
  return o->kind == kX86pOperandXmm && o->reg >= 0 && o->reg < 8;
}

static int vector_memory(const X86pOperand *o) {
  return o->kind == kX86pOperandMem && o->size == 16u && !o->addr16;
}

/* Push the second operand as one v128. The bounds check for a memory operand
   has already run, so kX86pWasmLocalAddr holds its linear-memory address. */
static void push_source(X86pWasmLower *l, const X86pOperand *s) {
  if (xmm_operand(s)) {
    x86p_wasm_state_load_xmm(&l->state, (unsigned)s->reg);
    return;
  }
  x86p_wasm_state_load_mem_v128(&l->state);
}

/*
 * SHUFPS's selector, as the sixteen byte indices i8x16.shuffle takes.
 *
 * The immediate picks four 32-bit lanes: the low two results come from the
 * destination and the high two from the source. i8x16.shuffle indexes a
 * 32-byte concatenation of its two operands, so a lane of the destination is
 * four consecutive bytes from 0..15 and a lane of the source four from 16..31.
 *
 * THE SELECTION IS A DECODE-TIME CONSTANT, which is the whole reason this is
 * worth emitting: it becomes part of the instruction, and the fourth-largest
 * packed operation on the measured route stops computing anything at run time.
 */
static void shuffle_lanes(uint32_t immediate, uint8_t lanes[16]) {
  unsigned lane;
  for (lane = 0; lane < 4u; lane++) {
    const unsigned selected = (immediate >> (lane * 2u)) & 3u;
    const unsigned base = (lane < 2u ? 0u : 16u) + selected * 4u;
    unsigned byte;
    for (byte = 0; byte < 4u; byte++) {
      lanes[lane * 4u + byte] = (uint8_t)(base + byte);
    }
  }
}

/*
 * Whether this instruction's form is one the emission below can take.
 *
 * The destination must be a register, because every form here writes all
 * sixteen bytes of one. A memory source must be sixteen bytes AND the mapping
 * must be the contiguous one: the sparse mapping is reached only through the
 * checked imports, which take and return i32, so there is no v128 load of it.
 */
static int form_is_emittable(const X86pWasmLower *l, const X86pInsn *insn) {
  const X86pOperand *d = &insn->operand[0], *s = &insn->operand[1];
  if (!xmm_operand(d)) {
    return 0;
  }
  if (!xmm_operand(s) && !(vector_memory(s) && x86p_wasm_state_memory_is_direct(&l->state))) {
    return 0;
  }
  return 1;
}

int x86p_wasm_simd_inline(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  const X86pSimdOp op = (X86pSimdOp)insn->simd;
  const X86pOperand *d = &insn->operand[0], *s = &insn->operand[1];
  const int shuffle = op == kX86pSimdShufps;
  PackedForm form = {kWasmV128And, 0};

  if (shuffle) {
    if (insn->operands != 3 || insn->operand[2].kind != kX86pOperandImm) {
      return 0;
    }
  } else {
    if (insn->operands != 2 || !packed_form(op, &form)) {
      return 0;
    }
  }
  if (!form_is_emittable(l, insn)) {
    return 0;
  }

  /*
   * From here the instruction WILL be emitted, so the guard may run: it can
   * leave the block on a fault, and a caller that then lowered the instruction
   * again would emit a second one after the return.
   */
  if (!xmm_operand(s)) {
    x86p_wasm_state_guard(&l->state, s, pc, 16, kX86pMemRead);
  }

  /* The store's address goes beneath its value, so it is pushed first. Both
     operands are then read before anything is written, which is what makes a
     source that aliases the destination -- `shufps xmm0, xmm0, imm` -- come out
     right without a temporary. */
  x86p_wasm_state_xmm_addr(&l->state, (unsigned)d->reg);
  if (shuffle) {
    uint8_t lanes[16];
    shuffle_lanes(insn->operand[2].imm, lanes);
    x86p_wasm_state_load_xmm(&l->state, (unsigned)d->reg);
    push_source(l, s);
    x86p_wasm_v128_shuffle(l->e, lanes);
  } else if (form.source_first) {
    push_source(l, s);
    x86p_wasm_state_load_xmm(&l->state, (unsigned)d->reg);
    x86p_wasm_v128_op(l->e, form.host);
  } else {
    x86p_wasm_state_load_xmm(&l->state, (unsigned)d->reg);
    push_source(l, s);
    x86p_wasm_v128_op(l->e, form.host);
  }
  x86p_wasm_state_store_v128(&l->state);
  l->simd_inline++;
  return 1;
}
