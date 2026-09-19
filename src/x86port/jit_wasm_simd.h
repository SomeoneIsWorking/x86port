#ifndef X86PORT_JIT_WASM_SIMD_H
#define X86PORT_JIT_WASM_SIMD_H

#include "cpu.h"
#include "decode.h"
#include "simd.h"

struct X86pWasmLower;

/* Source lanes are captured by emitted code before this arithmetic ABI is
 * entered. No instruction object, guest address, or decoder crosses it. */
uint32_t x86p_wasm_simd_arithmetic(X86pCpu *cpu,
                                   uint32_t destination,
                                   uint32_t operation,
                                   uint32_t b0,
                                   uint32_t b1,
                                   uint32_t b2,
                                   uint32_t b3,
                                   uint32_t immediate);
int x86p_wasm_simd_accepts(const X86pInsn *insn);
void x86p_wasm_simd_lower(struct X86pWasmLower *lower, const X86pInsn *insn, uint32_t pc);

/*
 * WHAT THE GUEST'S MXCSR SAYS, COUNTED OVER OPERATIONS.
 *
 * TEMPORARY. This exists to answer one question before any SSE instruction is
 * lowered to WebAssembly SIMD, and it comes out again once the answer is
 * written down. The question is the one that killed a whole plan in this
 * project's issue #162: wasm's f32x4 arithmetic is round-to-nearest-even and
 * has no flush-to-zero, so an inline lowering is only valid for the modes the
 * host can produce -- and whether the guest ever asks for another one is a fact
 * about the running game, not something to reason out.
 *
 * COUNTED PER OPERATION AND NOT PER LDMXCSR, because a control word set once
 * and used a billion times and one set a billion times and never used give the
 * same event count and opposite answers.
 *
 * `by_mode` is indexed by x86p_wasm_simd_mode_index below, so a single reader
 * can print every bucket including the empty ones -- a mode that never occurs
 * has to be visibly zero rather than absent, or "nearest, always" cannot be
 * told from "the counter never looked".
 */
enum {
  /* rounding control (2 bits) then flush-to-zero then denormals-are-zero */
  kX86pWasmSimdModeCount = 16
};

typedef struct X86pWasmSimdCensus {
  uint64_t operations; /* the denominator: every call to the helper */
  uint64_t by_mode[kX86pWasmSimdModeCount];
  uint64_t by_op[kX86pSimdOpCount];
} X86pWasmSimdCensus;

/* The bucket an MXCSR word falls in: (RC << 2) | (FTZ << 1) | DAZ. */
unsigned x86p_wasm_simd_mode_index(uint32_t mxcsr);

/* The name of a bucket, for a report that prints every one of them. */
const char *x86p_wasm_simd_mode_name(unsigned index);

/* A snapshot. Never fails; a run that executed no SSE arithmetic reports a
   zero denominator, which is the answer "this route asked nothing of SIMD"
   rather than "the mode is nearest". */
void x86p_wasm_simd_census(X86pWasmSimdCensus *out);

#endif
