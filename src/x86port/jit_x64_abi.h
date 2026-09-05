/*
 * jit_x64_abi.h -- host-call ABI policy for the x86-64 JIT emitter.
 *
 * Generated blocks are ordinary host functions and call ordinary C helpers,
 * so their entry register, nonvolatile-register saves, argument registers,
 * stack alignment, and Windows shadow space are one indivisible contract.
 * Keep that contract here instead of scattering _WIN32 branches through each
 * instruction-family emitter.
 */
#ifndef X86PORT_JIT_X64_ABI_H
#define X86PORT_JIT_X64_ABI_H

#include "emit_x64.h"

typedef enum X86pJitHostAbi {
  kX86pJitHostAbiSystemV = 0,
  kX86pJitHostAbiWin64,
} X86pJitHostAbi;

#if defined(_WIN32)
#define X86P_JIT_HOST_ABI kX86pJitHostAbiWin64
#define X86P_JIT_HOST_ARG0 kX64Rcx
#define X86P_JIT_HOST_ARG1 kX64Rdx
#define X86P_JIT_HOST_ARG2 kX64R8
#define X86P_JIT_HOST_ARG3 kX64R9
#define X86P_JIT_HOST_CALL_FRAME_BYTES 48
#else
#define X86P_JIT_HOST_ABI kX86pJitHostAbiSystemV
#define X86P_JIT_HOST_ARG0 kX64Rdi
#define X86P_JIT_HOST_ARG1 kX64Rsi
#define X86P_JIT_HOST_ARG2 kX64Rdx
#define X86P_JIT_HOST_ARG3 kX64Rcx
#define X86P_JIT_HOST_CALL_FRAME_BYTES 0
#endif

/* Explicit variants are exposed to the emitter test so both ABIs are proven
 * on every host, rather than leaving Win64 byte structure to a Windows-only
 * test that cannot distinguish a bad sequence until it executes. */
static inline X86pHostReg x86p_jit_abi_arg(X86pJitHostAbi abi, unsigned index) {
  static const X86pHostReg system_v[] = {kX64Rdi, kX64Rsi, kX64Rdx, kX64Rcx, kX64R8, kX64R9};
  static const X86pHostReg win64[] = {kX64Rcx, kX64Rdx, kX64R8, kX64R9};
  return (abi == kX86pJitHostAbiWin64 ? win64 : system_v)[index];
}

static inline int x86p_jit_abi_call_frame_bytes(X86pJitHostAbi abi) {
  return abi == kX86pJitHostAbiWin64 ? 48 : 0;
}

static inline int32_t x86p_jit_abi_stack_arg_offset(unsigned index) {
  return (int32_t)(32u + (index - 4u) * 8u);
}

static inline void x86p_jit_abi_emit_arg32_imm(X86pEmit *e, X86pJitHostAbi abi, unsigned index, uint32_t value) {
  if (abi != kX86pJitHostAbiWin64 || index < 4u) {
    x86p_emit_mov_r32_imm32(e, x86p_jit_abi_arg(abi, index), value);
    return;
  }
  x86p_emit_store32_imm(e, kX64Rsp, x86p_jit_abi_stack_arg_offset(index), value);
}

static inline void x86p_jit_abi_emit_arg64_reg(X86pEmit *e, X86pJitHostAbi abi, unsigned index, X86pHostReg value) {
  if (abi != kX86pJitHostAbiWin64 || index < 4u) {
    x86p_emit_mov_r64_r64(e, x86p_jit_abi_arg(abi, index), value);
    return;
  }
  x86p_emit_store64(e, kX64Rsp, x86p_jit_abi_stack_arg_offset(index), value);
}

/* Entry RSP is 8 mod 16. System V needs one push. Win64 also preserves RSI and
 * RDI because this emitter uses them as scratch even though that ABI makes
 * them nonvolatile; three pushes leave RSP aligned. The 48-byte frame contains
 * the mandatory 32-byte home/shadow area, one eight-byte stack argument slot,
 * and alignment padding. */
static inline void x86p_jit_abi_emit_enter(X86pEmit *e, X86pJitHostAbi abi, X86pHostReg cpu_reg) {
  x86p_emit_push_r64(e, cpu_reg);
  if (abi == kX86pJitHostAbiWin64) {
    x86p_emit_push_r64(e, kX64Rsi);
    x86p_emit_push_r64(e, kX64Rdi);
    x86p_emit_alu_r64_imm8(e, kX64Sub, kX64Rsp, 48);
  }
  x86p_emit_mov_r64_r64(e, cpu_reg, x86p_jit_abi_arg(abi, 0));
}

static inline void x86p_jit_abi_emit_leave(X86pEmit *e, X86pJitHostAbi abi, X86pHostReg cpu_reg) {
  if (abi == kX86pJitHostAbiWin64) {
    x86p_emit_alu_r64_imm8(e, kX64Add, kX64Rsp, 48);
    x86p_emit_pop_r64(e, kX64Rdi);
    x86p_emit_pop_r64(e, kX64Rsi);
  }
  x86p_emit_pop_r64(e, cpu_reg);
}

#endif /* X86PORT_JIT_X64_ABI_H */
