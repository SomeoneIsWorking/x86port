/* emit_x64.c -- see emit_x64.h for the three encoding traps this gets right. */
#include "emit_x64.h"

#include <string.h>

void x86p_emit_init(X86pEmit *e, void *buf, size_t cap) {
  if (!e) {
    return;
  }
  e->buf = (uint8_t *)buf;
  e->cap = buf ? cap : 0u;
  e->len = 0u;
  /* A null buffer is overflowed from the start rather than a special case
     every emit has to remember to test. */
  e->overflow = buf ? 0 : 1;
}

int x86p_emit_ok(const X86pEmit *e) {
  return e && !e->overflow;
}

/* The only place bytes are appended, so the capacity check exists once. */
static void put(X86pEmit *e, uint8_t b) {
  if (!e || e->overflow) {
    return;
  }
  if (e->len >= e->cap) {
    e->overflow = 1;
    return;
  }
  e->buf[e->len++] = b;
}

static void put32(X86pEmit *e, uint32_t v) {
  put(e, (uint8_t)(v & 0xFFu));
  put(e, (uint8_t)((v >> 8) & 0xFFu));
  put(e, (uint8_t)((v >> 16) & 0xFFu));
  put(e, (uint8_t)((v >> 24) & 0xFFu));
}

/*
 * REX, emitted only when it is needed -- except when `w` is set, where it
 * always is.
 *
 * `r` extends the ModRM.reg field, `b` the ModRM.rm/base field. They are
 * separate arguments rather than one "does this use high registers" flag
 * because swapping them is the off-by-eight described in the header: the code
 * still runs and touches the wrong register.
 */
static void rex(X86pEmit *e, int w, X86pHostReg r, X86pHostReg b) {
  uint8_t v = (uint8_t)(0x40u | (w ? 0x08u : 0u) | (((unsigned)r >> 3) << 2) | ((unsigned)b >> 3));
  if (v != 0x40u) {
    put(e, v);
  }
}

/* ModRM for the register-direct form: mod == 11. */
static void modrm_reg(X86pEmit *e, X86pHostReg reg, X86pHostReg rm) {
  put(e, (uint8_t)(0xC0u | (((unsigned)reg & 7u) << 3) | ((unsigned)rm & 7u)));
}

/*
 * ModRM (+ SIB, + displacement) for [base + disp].
 *
 * Both traps are handled here, once, rather than at each call site:
 *
 *  - base's low three bits == 100 (RSP, R12) means "SIB follows" in the rm
 *    field, so a SIB naming base with no index (0x24 | base) is required.
 *  - base's low three bits == 101 (RBP, R13) at mod == 00 means RIP-relative,
 *    so those two registers must use an explicit disp8 of zero even when the
 *    displacement is zero.
 */
static void modrm_mem(X86pEmit *e, X86pHostReg reg, X86pHostReg base, int32_t disp) {
  unsigned rm = (unsigned)base & 7u;
  int need_sib = (rm == 4u);
  int mod;

  if (disp == 0 && rm != 5u) {
    mod = 0;
  } else if (disp >= -128 && disp <= 127) {
    mod = 1;
  } else {
    mod = 2;
  }

  put(e, (uint8_t)(((unsigned)mod << 6) | (((unsigned)reg & 7u) << 3) | rm));
  if (need_sib) {
    /* scale=0, index=100 (none), base=rm */
    put(e, (uint8_t)(0x20u | rm));
  }
  if (mod == 1) {
    put(e, (uint8_t)(uint32_t)(int32_t)disp);
  } else if (mod == 2) {
    put32(e, (uint32_t)disp);
  }
}

/* ---- moves ------------------------------------------------------------- */

void x86p_emit_mov_r32_imm32(X86pEmit *e, X86pHostReg dst, uint32_t imm) {
  rex(e, 0, kX64Rax, dst);
  put(e, (uint8_t)(0xB8u + ((unsigned)dst & 7u)));
  put32(e, imm);
}

void x86p_emit_mov_r64_r64(X86pEmit *e, X86pHostReg dst, X86pHostReg src) {
  rex(e, 1, src, dst);
  put(e, 0x89u);
  modrm_reg(e, src, dst);
}

void x86p_emit_mov_r32_r32(X86pEmit *e, X86pHostReg dst, X86pHostReg src) {
  rex(e, 0, src, dst);
  put(e, 0x89u);
  modrm_reg(e, src, dst);
}

void x86p_emit_load32(X86pEmit *e, X86pHostReg dst, X86pHostReg base, int32_t disp) {
  rex(e, 0, dst, base);
  put(e, 0x8Bu);
  modrm_mem(e, dst, base, disp);
}

void x86p_emit_store32(X86pEmit *e, X86pHostReg base, int32_t disp, X86pHostReg src) {
  rex(e, 0, src, base);
  put(e, 0x89u);
  modrm_mem(e, src, base, disp);
}

void x86p_emit_mov_r64_imm64(X86pEmit *e, X86pHostReg dst, uint64_t imm) {
  rex(e, 1, kX64Rax, dst);
  put(e, (uint8_t)(0xB8u + ((unsigned)dst & 7u)));
  put32(e, (uint32_t)(imm & 0xFFFFFFFFu));
  put32(e, (uint32_t)(imm >> 32));
}

void x86p_emit_lea64(X86pEmit *e, X86pHostReg dst, X86pHostReg base, int32_t disp) {
  rex(e, 1, dst, base);
  put(e, 0x8Du);
  modrm_mem(e, dst, base, disp);
}

void x86p_emit_store32_imm(X86pEmit *e, X86pHostReg base, int32_t disp, uint32_t imm) {
  rex(e, 0, kX64Rax, base);
  put(e, 0xC7u);
  modrm_mem(e, kX64Rax, base, disp); /* /0 */
  put32(e, imm);
}

void x86p_emit_store8_imm(X86pEmit *e, X86pHostReg base, int32_t disp, uint8_t imm) {
  rex(e, 0, kX64Rax, base);
  put(e, 0xC6u);
  modrm_mem(e, kX64Rax, base, disp); /* /0 */
  put(e, imm);
}

/* ---- arithmetic -------------------------------------------------------- */

void x86p_emit_alu_r32_r32(X86pEmit *e, X86pHostAlu op, X86pHostReg dst, X86pHostReg src) {
  /* The r/m32, r32 forms run 0x01, 0x09, 0x11, ... -- opcode = op*8 + 1. The
     arithmetic IS the manual's table, not a lookup that can drift from it. */
  rex(e, 0, src, dst);
  put(e, (uint8_t)(((unsigned)op << 3) | 1u));
  modrm_reg(e, src, dst);
}

void x86p_emit_alu_r32_imm32(X86pEmit *e, X86pHostAlu op, X86pHostReg dst, uint32_t imm) {
  rex(e, 0, kX64Rax, dst);
  put(e, 0x81u);
  put(e, (uint8_t)(0xC0u | (((unsigned)op & 7u) << 3) | ((unsigned)dst & 7u)));
  put32(e, imm);
}

/* ---- structure --------------------------------------------------------- */

void x86p_emit_push_r64(X86pEmit *e, X86pHostReg r) {
  /* PUSH/POP default to 64-bit operand size, so no REX.W -- but a high
     register still needs REX.B. */
  if ((unsigned)r >= 8u) {
    put(e, 0x41u);
  }
  put(e, (uint8_t)(0x50u + ((unsigned)r & 7u)));
}

void x86p_emit_pop_r64(X86pEmit *e, X86pHostReg r) {
  if ((unsigned)r >= 8u) {
    put(e, 0x41u);
  }
  put(e, (uint8_t)(0x58u + ((unsigned)r & 7u)));
}

void x86p_emit_ret(X86pEmit *e) {
  put(e, 0xC3u);
}

void x86p_emit_call_r64(X86pEmit *e, X86pHostReg target) {
  /* No REX.W: CALL r/m64 is already 64-bit in long mode, but a high register
     still needs REX.B. */
  if ((unsigned)target >= 8u) {
    put(e, 0x41u);
  }
  put(e, 0xFFu);
  put(e, (uint8_t)(0xD0u | ((unsigned)target & 7u))); /* /2 */
}

void x86p_emit_byte(X86pEmit *e, uint8_t b) {
  put(e, b);
}
