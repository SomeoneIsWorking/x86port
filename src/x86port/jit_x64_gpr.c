/* jit_x64_gpr.c -- see jit_x64_gpr.h. */
#include "jit_x64_gpr.h"

#include "cpu.h"
#include "emit_x64.h"

#include <stdint.h>

/* The host registers the cache hands out, in order. Callee-saved under both
   host ABIs and saved by the block's frame (jit_x64_abi.h), and used for
   nothing else in a block but a spill of R12 that a helper call brackets. */
#define GPR_CACHE_SLOTS 3u
static const X86pHostReg kGprCacheReg[GPR_CACHE_SLOTS] = {kX64R12, kX64R13, kX64Rbp};

#define kCcE 0x4u

/* A live set is good only while the emitter has made no call and bound no
   jump since it was built. */
static unsigned flow_epoch(const X86pEmit *e) {
  return e->calls + e->sites_bound;
}

static void refresh(BlockCtx *c) {
  const unsigned now = flow_epoch(c->e);
  if (c->gpr_epoch != now) {
    c->gpr_live = 0u;
    c->gpr_epoch = now;
  }
}

/* The host register caching dword register `r`: 1 and *host when it has one,
   or when `claim` and a slot is free, which gives it that slot. */
static int cache_reg(BlockCtx *c, int r, int claim, X86pHostReg *host) {
  if (!c->gpr_slot[r]) {
    if (!claim || c->gpr_slots >= GPR_CACHE_SLOTS) {
      return 0;
    }
    c->gpr_slot[r] = (uint8_t)++c->gpr_slots;
  }
  *host = kGprCacheReg[c->gpr_slot[r] - 1u];
  return 1;
}

/* The dword register a width-`w` operand names, and the bit offset of the
   operand inside it. */
static int dword_reg(int reg, int w, int *shift) {
  *shift = 0;
  return w == 1 ? x86p_byte_reg(reg, shift) : reg;
}

void gpr_load(BlockCtx *c, X86pHostReg dst, int reg, int w) {
  int shift;
  const int r = dword_reg(reg, w, &shift);
  X86pHostReg host;
  refresh(c);
  if (!cache_reg(c, r, w == 4, &host) || (!(c->gpr_live & (1u << r)) && w != 4)) {
    emit_load_w(c->e, dst, CPU_REG, reg_off_w(reg, w), w);
    return;
  }
  if (!(c->gpr_live & (1u << r))) {
    x86p_emit_load32(c->e, host, CPU_REG, reg_off(r));
    c->gpr_live |= 1u << r;
  }
  x86p_emit_mov_r32_r32(c->e, dst, host);
  if (shift) {
    x86p_emit_shift_r32_imm8(c->e, kX64Shr, dst, (uint8_t)shift);
  }
  if (w < 4) {
    x86p_emit_alu_r32_imm32(c->e, kX64And, dst, x86p_width_mask(w));
  }
}

void gpr_store(BlockCtx *c, int reg, X86pHostReg src, int w) {
  int shift;
  const int r = dword_reg(reg, w, &shift);
  X86pHostReg host;
  refresh(c);
  emit_store_w(c->e, CPU_REG, reg_off_w(reg, w), src, w);
  if (!cache_reg(c, r, w == 4, &host)) {
    return;
  }
  if (w == 4) {
    x86p_emit_mov_r32_r32(c->e, host, src);
    c->gpr_live |= 1u << r;
  } else {
    c->gpr_live &= ~(1u << r);
  }
}

void gpr_store_imm(BlockCtx *c, int reg, uint32_t imm, int w) {
  int shift;
  const int r = dword_reg(reg, w, &shift);
  X86pHostReg host;
  refresh(c);
  emit_store_imm_w(c->e, CPU_REG, reg_off_w(reg, w), imm, w);
  if (!cache_reg(c, r, w == 4, &host)) {
    return;
  }
  if (w == 4) {
    x86p_emit_mov_r32_imm32(c->e, host, imm);
    c->gpr_live |= 1u << r;
  } else {
    c->gpr_live &= ~(1u << r);
  }
}

void gpr_forget(BlockCtx *c) {
  refresh(c);
  c->gpr_live = 0u;
}

void gpr_check(BlockCtx *c) {
#if X86P_JIT_GPR_CHECK
  int r;
  refresh(c);
  for (r = 0; r < 8; r++) {
    X86pHostReg host;
    X86pEmitSite agrees;
    if (!(c->gpr_live & (1u << r)) || !cache_reg(c, r, 0, &host)) {
      continue;
    }
    x86p_emit_alu_r32_mem(c->e, kX64Cmp, host, CPU_REG, reg_off(r));
    agrees = x86p_emit_jcc_rel32(c->e, kCcE);
    x86p_emit_byte(c->e, 0x0Fu); /* ud2 */
    x86p_emit_byte(c->e, 0x0Bu);
    x86p_emit_bind(c->e, agrees);
  }
  /* Those binds join no other path: the set is as good as before them. */
  c->gpr_epoch = flow_epoch(c->e);
#else
  (void)c;
#endif
}
