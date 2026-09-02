/* x87_state.c -- see x87_state.h. */
#include "x87_state.h"

#include <string.h>

/*
 * The model keeps one BYTE of tag per register; the architecture packs TWO BITS
 * per register into a single word, register 0 in the low pair. The encodings
 * are the same four values, so this is a repacking rather than a translation --
 * but it is the repacking, done twice in opposite directions, that a
 * hand-written save/restore pair gets subtly wrong and only notices when a
 * restored FPU reports a stack fault on a register that was valid.
 */
static uint16_t pack_tags(const X86pX87 *fpu) {
  uint16_t w = 0;
  unsigned i;
  for (i = 0; i < X86P_X87_REGS; i++) {
    w = (uint16_t)(w | ((uint16_t)(fpu->tag[i] & 3u) << (2u * i)));
  }
  return w;
}

static void unpack_tags(X86pX87 *fpu, uint16_t w) {
  unsigned i;
  for (i = 0; i < X86P_X87_REGS; i++) {
    fpu->tag[i] = (uint8_t)((w >> (2u * i)) & 3u);
  }
}

/*
 * The environment's seven dwords. The instruction pointer, opcode and data
 * pointer fields record where the last non-control FPU instruction was, for a
 * handler that wants to report it. Nothing in this framework generates an
 * unmasked exception -- every process it targets runs with all of them masked
 * -- so there is no last-instruction address to report and these are written as
 * zero. That is a statement about what is modelled, not an oversight: a guest
 * that read them would be inspecting an exception frame this engine never
 * produces.
 */
static int write_env(const X86pX87 *fpu, const X86pMem *mem, uint32_t addr) {
  const uint32_t env[7] = {
      (uint32_t)fpu->control,
      (uint32_t)x86p_x87_status(fpu), /* TOP is merged in by that accessor */
      (uint32_t)pack_tags(fpu),
      0u, /* FIP  */
      0u, /* FCS and the opcode */
      0u, /* FDP  */
      0u, /* FDS  */
  };
  unsigned i;
  for (i = 0; i < 7u; i++) {
    if (!x86p_mem_write(mem, addr + i * 4u, 4, env[i])) {
      return 0;
    }
  }
  return 1;
}

int x86p_x87_save_state(X86pX87 *fpu, const X86pMem *mem, uint32_t addr) {
  unsigned i;
  uint8_t bytes[10];

  /* Everything is checked before anything is destroyed: FNSAVE reinitialises
     the unit, so a fault partway through a version that wrote as it went would
     leave a guest with neither its registers nor a usable image. */
  for (i = 0; i < X86P_X87_STATE_BYTES; i++) {
    uint32_t probe;
    if (!x86p_mem_read(mem, addr + i, 1, &probe)) {
      return 0;
    }
  }
  if (!write_env(fpu, mem, addr)) {
    return 0;
  }
  for (i = 0; i < X86P_X87_REGS; i++) {
    unsigned b;
    /* PHYSICAL order, not stack order: the image records register i, and TOP
       in the status word says which of them is ST(0). Storing in stack order
       would round-trip correctly through this same pair and disagree with
       every other implementation, which is the kind of bug a self-consistent
       test cannot see. */
    x86p_x87_to_f80(fpu->reg[i], bytes);
    for (b = 0; b < 10u; b++) {
      if (!x86p_mem_write(mem, addr + 28u + i * 10u + b, 1, bytes[b])) {
        return 0;
      }
    }
  }
  x86p_x87_reset(fpu);
  return 1;
}

int x86p_x87_restore_state(X86pX87 *fpu, const X86pMem *mem, uint32_t addr) {
  uint32_t env[7];
  uint8_t regs[X86P_X87_REGS][10];
  unsigned i;

  for (i = 0; i < 7u; i++) {
    if (!x86p_mem_read(mem, addr + i * 4u, 4, &env[i])) {
      return 0;
    }
  }
  for (i = 0; i < X86P_X87_REGS; i++) {
    unsigned b;
    for (b = 0; b < 10u; b++) {
      uint32_t v;
      if (!x86p_mem_read(mem, addr + 28u + i * 10u + b, 1, &v)) {
        return 0;
      }
      regs[i][b] = (uint8_t)v;
    }
  }

  /* Only now, with the whole image in hand. */
  fpu->control = (uint16_t)env[0];
  /* TOP travels inside the status word and is a separate field in the model,
     so it is split back out here rather than left merged -- the accessor that
     merges it on read has no inverse, and leaving TOP in `status` would make
     every later read report it twice. */
  fpu->status = (uint16_t)(env[1] & ~(uint16_t)0x3800u);
  fpu->top = (uint8_t)((env[1] >> 11) & 7u);
  unpack_tags(fpu, (uint16_t)env[2]);
  for (i = 0; i < X86P_X87_REGS; i++) {
    fpu->reg[i] = x86p_x87_from_f80(regs[i]);
  }
  return 1;
}
