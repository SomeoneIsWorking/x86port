/*
 * emit_arm64.c -- the AArch64 encoder. See emit_arm64.h for the contract.
 *
 * Every instruction here is one fixed 32-bit little-endian word, so unlike
 * emit_x64.c there is no variable-length prefix puzzle -- the risk here is a
 * wrong field width or an immediate that silently does not fit. Encodings
 * are built from the documented AArch64 bit layouts and cross-checked against
 * known-good disassembly of the same instruction (noted at each base
 * constant) rather than trusted from memory alone.
 */
#include "emit_arm64.h"

#include <string.h>

void x86p_a64_emit_init(X86pA64Emit *e, void *buf, size_t cap) {
  e->buf = (uint8_t *)buf;
  e->cap = buf ? cap : 0u;
  e->len = 0;
  /* A null buffer is overflowed from the start rather than a special case
     every emit has to remember to test (matches emit_x64.c's discipline). */
  e->overflow = buf ? 0 : 1;
  e->sites_made = 0;
  e->sites_bound = 0;
}

int x86p_a64_emit_ok(const X86pA64Emit *e) {
  return !e->overflow;
}

static void put32(X86pA64Emit *e, uint32_t word) {
  if (e->overflow || e->len + 4 > e->cap) {
    e->overflow = 1;
    return;
  }
  memcpy(e->buf + e->len, &word, 4);
  e->len += 4;
}

/* ---- moves ----------------------------------------------------------------- */

/* MOVZ/MOVK/MOVN, "move wide immediate": sf|opc(2)|100101|hw(2)|imm16|Rd. */
static void move_wide(X86pA64Emit *e, int sf, unsigned opc, unsigned hw, uint16_t imm16, X86pA64Reg rd) {
  uint32_t word = ((uint32_t)sf << 31) | ((uint32_t)opc << 29) | (0x25u << 23) | ((uint32_t)hw << 21) |
                   ((uint32_t)imm16 << 5) | (uint32_t)rd;
  put32(e, word);
}

void x86p_a64_emit_mov_w_imm32(X86pA64Emit *e, X86pA64Reg dst, uint32_t imm) {
  uint16_t lo = (uint16_t)(imm & 0xFFFFu);
  uint16_t hi = (uint16_t)((imm >> 16) & 0xFFFFu);
  if (hi == 0) {
    move_wide(e, 0, 2u /* MOVZ */, 0, lo, dst);
    return;
  }
  if (lo == 0) {
    move_wide(e, 0, 2u, 1, hi, dst);
    return;
  }
  move_wide(e, 0, 2u, 0, lo, dst);
  move_wide(e, 0, 3u /* MOVK */, 1, hi, dst);
}

void x86p_a64_emit_mov_x_imm64(X86pA64Emit *e, X86pA64Reg dst, uint64_t imm) {
  uint16_t chunk[4];
  int i;
  int wrote = 0;
  chunk[0] = (uint16_t)(imm & 0xFFFFu);
  chunk[1] = (uint16_t)((imm >> 16) & 0xFFFFu);
  chunk[2] = (uint16_t)((imm >> 32) & 0xFFFFu);
  chunk[3] = (uint16_t)((imm >> 48) & 0xFFFFu);
  for (i = 0; i < 4; i++) {
    if (chunk[i] == 0 && !(i == 3 && !wrote) && !(imm == 0 && i == 0)) {
      continue;
    }
    if (!wrote) {
      move_wide(e, 1, 2u, (unsigned)i, chunk[i], dst);
      wrote = 1;
    } else {
      move_wide(e, 1, 3u, (unsigned)i, chunk[i], dst);
    }
  }
  if (!wrote) {
    /* imm == 0: MOVZ dst, #0 */
    move_wide(e, 1, 2u, 0, 0, dst);
  }
}

/* MOV (register) is the ORR-with-zero-register alias: logical (shifted
   register), opc=01 (ORR), N=0, shift=0, Rn=31 (XZR/WZR -- valid here since
   this position is never SP). */
static void orr_shifted_reg(X86pA64Emit *e, int sf, unsigned opc, X86pA64Reg rd, X86pA64Reg rn, X86pA64Reg rm) {
  uint32_t word = ((uint32_t)sf << 31) | (opc << 29) | (0x0Au << 24) | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) |
                   (uint32_t)rd;
  put32(e, word);
}

void x86p_a64_emit_mov_x_x(X86pA64Emit *e, X86pA64Reg dst, X86pA64Reg src) {
  orr_shifted_reg(e, 1, 1u, dst, (X86pA64Reg)31, src);
}

void x86p_a64_emit_mov_w_w(X86pA64Emit *e, X86pA64Reg dst, X86pA64Reg src) {
  orr_shifted_reg(e, 0, 1u, dst, (X86pA64Reg)31, src);
}

/* ---- loads and stores -------------------------------------------------------- */

/*
 * The scaled-unsigned-offset immediate class covers base+[0, 4095*size]. A
 * displacement outside that (negative, or a struct that has grown past the
 * range) falls back to materialising the full address in the encoder's
 * reserved scratch register (X8, never a role register -- see
 * jit_arm64_internal.h) and loading/storing at [X8, #0].
 */
#define A64_SCRATCH kA64X8

static int fits_scaled_unsigned(int32_t disp, int size_bytes, uint32_t *out_imm) {
  if (disp < 0) {
    return 0;
  }
  if ((disp % size_bytes) != 0) {
    return 0;
  }
  {
    uint32_t scaled = (uint32_t)disp / (uint32_t)size_bytes;
    if (scaled > 4095u) {
      return 0;
    }
    *out_imm = scaled;
    return 1;
  }
}

static void add_sub_imm(X86pA64Emit *e, int sf, int is_sub, X86pA64Reg rd, X86pA64Reg rn, uint32_t imm12,
                        unsigned shift12) {
  uint32_t word = ((uint32_t)sf << 31) | ((uint32_t)(is_sub ? 1 : 0) << 30) | (0u << 29) | (0x22u << 23) |
                   ((uint32_t)shift12 << 22) | ((imm12 & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
  put32(e, word);
}

/* x(dst) = x(base) + disp, for any 32-bit signed disp. Small magnitudes use
   one or two ADD/SUB-immediate instructions (imm12, optionally <<12); a
   value that needs more precision than that is materialised as a 64-bit
   immediate (sign-extended) and added as a register. */
static void materialize_address(X86pA64Emit *e, X86pA64Reg dst, X86pA64Reg base, int32_t disp) {
  uint32_t mag;
  int neg;
  if (disp == 0) {
    /*
     * NOT x86p_a64_emit_mov_x_x (ORR dst,XZR,base): ORR's register field
     * treats 31 as XZR unconditionally, so "mov dst,SP" via that alias
     * silently produces dst=0 instead of the stack pointer -- SP's dual
     * identity (kA64Reg's own header comment) applies to a load/store base
     * or an ADD/SUB immediate operand, NOT to a plain register-register ALU
     * instruction. ADD dst,base,#0 is the one instruction class that reads
     * 31 as SP here, so it is the only correct way to copy a possibly-SP
     * base with zero displacement. Measured: every `lea64(dst, kA64Sp, 0)`
     * call (the x87 backend's scratch-slot address, its only SP-based
     * caller) silently materialized dst=0 and corrupted every long-double
     * marshaled through it, with no compile-time or encoder-test signal --
     * task 2's bitfield-decoder suite checked each instruction's bit layout
     * in isolation, never this address-of-SP case at runtime.
     */
    add_sub_imm(e, 1, 0, dst, base, 0u, 0);
    return;
  }
  neg = disp < 0;
  mag = neg ? (uint32_t)(-(int64_t)disp) : (uint32_t)disp;
  if (mag <= 0xFFFu) {
    add_sub_imm(e, 1, neg, dst, base, mag, 0);
    return;
  }
  if ((mag & 0xFFFu) == 0 && (mag >> 12) <= 0xFFFu) {
    add_sub_imm(e, 1, neg, dst, base, mag >> 12, 1);
    return;
  }
  x86p_a64_emit_mov_x_imm64(e, A64_SCRATCH == dst ? kA64X9 : A64_SCRATCH, (uint64_t)(int64_t)disp);
  {
    X86pA64Reg tmp = (A64_SCRATCH == dst) ? kA64X9 : A64_SCRATCH;
    /* x(dst) = x(base) + x(tmp), 64-bit shifted-register ADD. */
    uint32_t word = (1u << 31) | (0u << 30) | (0u << 29) | (0x0Bu << 24) | ((uint32_t)tmp << 16) |
                     ((uint32_t)base << 5) | (uint32_t)dst;
    put32(e, word);
  }
}

static void ldst_unsigned(X86pA64Emit *e, uint32_t base_word, X86pA64Reg rt, X86pA64Reg rn, int32_t disp,
                          int size_bytes) {
  uint32_t imm;
  if (fits_scaled_unsigned(disp, size_bytes, &imm)) {
    put32(e, base_word | (imm << 10) | ((uint32_t)rn << 5) | (uint32_t)rt);
    return;
  }
  materialize_address(e, A64_SCRATCH, rn, disp);
  put32(e, base_word | (0u << 10) | ((uint32_t)A64_SCRATCH << 5) | (uint32_t)rt);
}

/* Base words, verified against known disassembly (e.g. `ldr w0,[x0]` =
   0xB9400000, `str x0,[x0]` = 0xF9000000). */
#define LDR_W_BASE 0xB9400000u
#define STR_W_BASE 0xB9000000u
#define LDR_X_BASE 0xF9400000u
#define STR_X_BASE 0xF9000000u
#define LDRB_BASE 0x39400000u
#define STRB_BASE 0x39000000u
#define LDRH_BASE 0x79400000u
#define STRH_BASE 0x79000000u

void x86p_a64_emit_load32(X86pA64Emit *e, X86pA64Reg dst, X86pA64Reg base, int32_t disp) {
  ldst_unsigned(e, LDR_W_BASE, dst, base, disp, 4);
}
void x86p_a64_emit_store32(X86pA64Emit *e, X86pA64Reg base, int32_t disp, X86pA64Reg src) {
  ldst_unsigned(e, STR_W_BASE, src, base, disp, 4);
}
void x86p_a64_emit_load64(X86pA64Emit *e, X86pA64Reg dst, X86pA64Reg base, int32_t disp) {
  ldst_unsigned(e, LDR_X_BASE, dst, base, disp, 8);
}
void x86p_a64_emit_store64(X86pA64Emit *e, X86pA64Reg base, int32_t disp, X86pA64Reg src) {
  ldst_unsigned(e, STR_X_BASE, src, base, disp, 8);
}
void x86p_a64_emit_load8_zx(X86pA64Emit *e, X86pA64Reg dst, X86pA64Reg base, int32_t disp) {
  ldst_unsigned(e, LDRB_BASE, dst, base, disp, 1);
}
void x86p_a64_emit_store8_reg(X86pA64Emit *e, X86pA64Reg base, int32_t disp, X86pA64Reg src) {
  ldst_unsigned(e, STRB_BASE, src, base, disp, 1);
}
void x86p_a64_emit_load16_zx(X86pA64Emit *e, X86pA64Reg dst, X86pA64Reg base, int32_t disp) {
  ldst_unsigned(e, LDRH_BASE, dst, base, disp, 2);
}
void x86p_a64_emit_store16_reg(X86pA64Emit *e, X86pA64Reg base, int32_t disp, X86pA64Reg src) {
  ldst_unsigned(e, STRH_BASE, src, base, disp, 2);
}

void x86p_a64_emit_store8_imm(X86pA64Emit *e, X86pA64Reg base, int32_t disp, uint8_t imm) {
  x86p_a64_emit_mov_w_imm32(e, kA64X9, imm);
  x86p_a64_emit_store8_reg(e, base, disp, kA64X9);
}
void x86p_a64_emit_store16_imm(X86pA64Emit *e, X86pA64Reg base, int32_t disp, uint16_t imm) {
  x86p_a64_emit_mov_w_imm32(e, kA64X9, imm);
  x86p_a64_emit_store16_reg(e, base, disp, kA64X9);
}
void x86p_a64_emit_store32_imm(X86pA64Emit *e, X86pA64Reg base, int32_t disp, uint32_t imm) {
  x86p_a64_emit_mov_w_imm32(e, kA64X9, imm);
  x86p_a64_emit_store32(e, base, disp, kA64X9);
}

void x86p_a64_emit_lea64(X86pA64Emit *e, X86pA64Reg dst, X86pA64Reg base, int32_t disp) {
  materialize_address(e, dst, base, disp);
}

/* ---- arithmetic ------------------------------------------------------------- */

static void alu_shifted_reg(X86pA64Emit *e, int sf, X86pA64Alu op, X86pA64Reg dst, X86pA64Reg src) {
  uint32_t word;
  switch (op) {
  case kA64Add:
    word = ((uint32_t)sf << 31) | (0u << 30) | (0u << 29) | (0x0Bu << 24) | ((uint32_t)src << 16) |
           ((uint32_t)dst << 5) | (uint32_t)dst;
    break;
  case kA64Sub:
    word = ((uint32_t)sf << 31) | (1u << 30) | (0u << 29) | (0x0Bu << 24) | ((uint32_t)src << 16) |
           ((uint32_t)dst << 5) | (uint32_t)dst;
    break;
  case kA64And:
    word = ((uint32_t)sf << 31) | (0u << 29) | (0x0Au << 24) | ((uint32_t)src << 16) | ((uint32_t)dst << 5) |
           (uint32_t)dst;
    break;
  case kA64Orr:
    word = ((uint32_t)sf << 31) | (1u << 29) | (0x0Au << 24) | ((uint32_t)src << 16) | ((uint32_t)dst << 5) |
           (uint32_t)dst;
    break;
  case kA64Eor:
  default:
    word = ((uint32_t)sf << 31) | (2u << 29) | (0x0Au << 24) | ((uint32_t)src << 16) | ((uint32_t)dst << 5) |
           (uint32_t)dst;
    break;
  }
  put32(e, word);
}

void x86p_a64_emit_alu_w_w(X86pA64Emit *e, X86pA64Alu op, X86pA64Reg dst, X86pA64Reg src) {
  alu_shifted_reg(e, 0, op, dst, src);
}
void x86p_a64_emit_alu_x_x(X86pA64Emit *e, X86pA64Alu op, X86pA64Reg dst, X86pA64Reg src) {
  alu_shifted_reg(e, 1, op, dst, src);
}

/* AND/ORR/EOR (immediate), restricted to the one shape this backend needs: a
   contiguous run of `n` one-bits at the bottom of the word (n in 1..31), no
   rotation. That covers every mask this JIT ever ANDs with (0xFF, 0xFFFF)
   and is a single documented case of the general bitmask-immediate encoding
   (N=0, immr=0, imms=n-1) rather than the full decoder. */
static int contiguous_lsb_ones(uint32_t v, unsigned *n_out) {
  unsigned n = 0;
  if (v == 0 || v == 0xFFFFFFFFu) {
    return 0;
  }
  while (n < 32 && (v & (1u << n))) {
    n++;
  }
  if (n >= 32 || (v >> n) != 0) {
    return 0;
  }
  *n_out = n;
  return 1;
}

static void logical_imm(X86pA64Emit *e, int sf, unsigned opc, X86pA64Reg dst, unsigned n_ones) {
  uint32_t word = ((uint32_t)sf << 31) | (opc << 29) | (0x24u << 23) | (0u << 22) /* N */ | (0u << 16) /* immr */ |
                   (((uint32_t)n_ones - 1u) << 10) /* imms */ | ((uint32_t)dst << 5) | (uint32_t)dst;
  put32(e, word);
}

void x86p_a64_emit_alu_w_imm(X86pA64Emit *e, X86pA64Alu op, X86pA64Reg dst, uint32_t imm) {
  if (op == kA64Add || op == kA64Sub) {
    if (imm <= 0xFFFu) {
      add_sub_imm(e, 0, op == kA64Sub, dst, dst, imm, 0);
      return;
    }
    if ((imm & 0xFFFu) == 0 && (imm >> 12) <= 0xFFFu) {
      add_sub_imm(e, 0, op == kA64Sub, dst, dst, imm >> 12, 1);
      return;
    }
    x86p_a64_emit_mov_w_imm32(e, kA64X9, imm);
    x86p_a64_emit_alu_w_w(e, op, dst, kA64X9);
    return;
  }
  /* AND/ORR/EOR immediate. */
  {
    unsigned n;
    unsigned opc = (op == kA64And) ? 0u : (op == kA64Orr) ? 1u : 2u;
    if (op == kA64And && contiguous_lsb_ones(imm, &n)) {
      logical_imm(e, 0, 0u, dst, n);
      return;
    }
    if (op == kA64Eor && imm == 0xFFFFFFFFu) {
      /* Bitwise NOT: MVN dst, dst == ORN dst, XZR, dst (N=1 ORR variant). */
      uint32_t word = (0u << 31) | (1u << 29) | (0x0Au << 24) | (1u << 21) /* N */ | ((uint32_t)dst << 16) |
                       (31u << 5) | (uint32_t)dst;
      put32(e, word);
      return;
    }
    (void)opc;
    x86p_a64_emit_mov_w_imm32(e, kA64X9, imm);
    x86p_a64_emit_alu_w_w(e, op, dst, kA64X9);
  }
}

void x86p_a64_emit_alu_x_imm(X86pA64Emit *e, X86pA64Alu op, X86pA64Reg dst, uint32_t imm) {
  if (imm <= 0xFFFu) {
    add_sub_imm(e, 1, op == kA64Sub, dst, dst, imm, 0);
    return;
  }
  if ((imm & 0xFFFu) == 0 && (imm >> 12) <= 0xFFFu) {
    add_sub_imm(e, 1, op == kA64Sub, dst, dst, imm >> 12, 1);
    return;
  }
  x86p_a64_emit_mov_x_imm64(e, kA64X9, imm);
  x86p_a64_emit_alu_x_x(e, op, dst, kA64X9);
}

/* Bitfield move (UBFM/SBFM), used for LSL/ASR by immediate. */
static void bitfield(X86pA64Emit *e, unsigned opc, X86pA64Reg dst, X86pA64Reg src, unsigned immr, unsigned imms) {
  uint32_t word = (0u << 31) | (opc << 29) | (0x26u << 23) | (0u << 22) /* N */ | ((immr & 0x3Fu) << 16) |
                   ((imms & 0x3Fu) << 10) | ((uint32_t)src << 5) | (uint32_t)dst;
  put32(e, word);
}

void x86p_a64_emit_shl_w_imm(X86pA64Emit *e, X86pA64Reg dst, uint8_t count) {
  /* LSL Wd, Wn, #s == UBFM Wd, Wn, #(-s mod 32), #(31-s). */
  unsigned s = count & 31u;
  bitfield(e, 2u /* UBFM */, dst, dst, (32u - s) & 31u, 31u - s);
}

void x86p_a64_emit_sar_w_imm(X86pA64Emit *e, X86pA64Reg dst, uint8_t count) {
  /* ASR Wd, Wn, #s == SBFM Wd, Wn, #s, #31. */
  unsigned s = count & 31u;
  bitfield(e, 0u /* SBFM */, dst, dst, s, 31u);
}

void x86p_a64_emit_cmp_w_w(X86pA64Emit *e, X86pA64Reg a, X86pA64Reg b) {
  uint32_t word = (0u << 31) | (1u << 30) | (1u << 29) | (0x0Bu << 24) | ((uint32_t)b << 16) | ((uint32_t)a << 5) |
                   31u;
  put32(e, word);
}

void x86p_a64_emit_cmp_w_imm(X86pA64Emit *e, X86pA64Reg a, uint32_t imm) {
  if (imm <= 0xFFFu) {
    uint32_t word = (0u << 31) | (1u << 30) | (1u << 29) | (0x22u << 23) | (0u << 22) | ((imm & 0xFFFu) << 10) |
                     ((uint32_t)a << 5) | 31u;
    put32(e, word);
    return;
  }
  x86p_a64_emit_mov_w_imm32(e, kA64X9, imm);
  x86p_a64_emit_cmp_w_w(e, a, kA64X9);
}

void x86p_a64_emit_tst_w_w(X86pA64Emit *e, X86pA64Reg a, X86pA64Reg b) {
  uint32_t word = (0u << 31) | (3u << 29) | (0x0Au << 24) | ((uint32_t)b << 16) | ((uint32_t)a << 5) | 31u;
  put32(e, word);
}

/* Conditional select family: sf|op|S|11010100(0xD4 at bits28:21)|Rm|cond|
   op2|Rn|Rd. The fixed field sits at bits28:21, NOT bits31:24 -- it overlaps
   sf/op/S at the top and reaches one bit past where a naive top-byte-only
   placement would put it. */
#define A64_CSEL_BASE 0x1A800000u

void x86p_a64_emit_csel_w(X86pA64Emit *e, X86pA64Cond cc, X86pA64Reg dst, X86pA64Reg a, X86pA64Reg b) {
  uint32_t word = A64_CSEL_BASE | ((uint32_t)b << 16) | ((uint32_t)cc << 12) | (0u << 10) /* op2=00: CSEL */ |
                   ((uint32_t)a << 5) | (uint32_t)dst;
  put32(e, word);
}

void x86p_a64_emit_cset_w(X86pA64Emit *e, X86pA64Cond cc, X86pA64Reg dst) {
  /* CSET Wd, cc == CSINC Wd, WZR, WZR, invert(cc). */
  unsigned inv = (unsigned)cc ^ 1u;
  uint32_t word = A64_CSEL_BASE | (31u << 16) | (inv << 12) | (1u << 10) /* op2=01: CSINC */ | (31u << 5) |
                   (uint32_t)dst;
  put32(e, word);
}

void x86p_a64_emit_alu_w_mem(X86pA64Emit *e, X86pA64Alu op, X86pA64Reg dst, X86pA64Reg base, int32_t disp) {
  x86p_a64_emit_load32(e, kA64X9, base, disp);
  x86p_a64_emit_alu_w_w(e, op, dst, kA64X9);
}

/* ---- floating point --------------------------------------------------------- */

/* ldr q(t), [x(n), #disp] -- SIMD&FP 128-bit, unsigned offset (scaled by 16).
   size(31:30)=00, opc(23:22)=11 selects the 128-bit (Q) variant of this
   otherwise-shared load/store-immediate class; base = 0x3DC00000. */
void x86p_a64_emit_load_q(X86pA64Emit *e, unsigned vreg, X86pA64Reg base, int32_t disp) {
  uint32_t imm;
  if (disp >= 0 && (disp % 16) == 0 && ((uint32_t)disp / 16u) <= 4095u) {
    imm = (uint32_t)disp / 16u;
    put32(e, 0x3DC00000u | (imm << 10) | ((uint32_t)base << 5) | vreg);
    return;
  }
  materialize_address(e, A64_SCRATCH, base, disp);
  put32(e, 0x3DC00000u | ((uint32_t)A64_SCRATCH << 5) | vreg);
}

void x86p_a64_emit_store_q(X86pA64Emit *e, X86pA64Reg base, int32_t disp, unsigned vreg) {
  uint32_t imm;
  if (disp >= 0 && (disp % 16) == 0 && ((uint32_t)disp / 16u) <= 4095u) {
    imm = (uint32_t)disp / 16u;
    put32(e, 0x3D800000u | (imm << 10) | ((uint32_t)base << 5) | vreg);
    return;
  }
  materialize_address(e, A64_SCRATCH, base, disp);
  put32(e, 0x3D800000u | ((uint32_t)A64_SCRATCH << 5) | vreg);
}

void x86p_a64_emit_mov_q_q(X86pA64Emit *e, unsigned vdst, unsigned vsrc) {
  /* ORR Vd.16B, Vn.16B, Vn.16B (the canonical 128-bit register-move idiom). */
  uint32_t word = 0x4EA01C00u | (vsrc << 16) | (vsrc << 5) | vdst;
  put32(e, word);
}

/* ---- forward branches -------------------------------------------------------- */

X86pA64EmitSite x86p_a64_emit_bcc(X86pA64Emit *e, X86pA64Cond cc) {
  X86pA64EmitSite site;
  site.at = e->len;
  /* B.cond, imm19 left as 0 until bound. base = 0x54000000 | cond. */
  put32(e, 0x54000000u | (uint32_t)cc);
  site.end = e->len;
  e->sites_made++;
  return site;
}

X86pA64EmitSite x86p_a64_emit_b(X86pA64Emit *e) {
  X86pA64EmitSite site;
  site.at = e->len;
  put32(e, 0x14000000u);
  site.end = e->len;
  e->sites_made++;
  return site;
}

void x86p_a64_emit_bind(X86pA64Emit *e, X86pA64EmitSite site) {
  int64_t delta;
  uint32_t word;
  if (e->overflow || site.at + 4 > e->len) {
    e->overflow = 1;
    return;
  }
  memcpy(&word, e->buf + site.at, 4);
  delta = (int64_t)e->len - (int64_t)site.at;
  if ((delta & 3) != 0) {
    e->overflow = 1;
    return;
  }
  if ((word & 0xFC000000u) == 0x14000000u) {
    /* B: imm26, +-128MB. */
    int64_t off = delta / 4;
    if (off < -(1 << 25) || off >= (1 << 25)) {
      e->overflow = 1;
      return;
    }
    word = 0x14000000u | ((uint32_t)off & 0x03FFFFFFu);
  } else {
    /* B.cond: imm19, +-1MB, at bits 23:5, condition kept in bits 3:0. */
    int64_t off = delta / 4;
    if (off < -(1 << 18) || off >= (1 << 18)) {
      e->overflow = 1;
      return;
    }
    word = (word & 0xFF00001Fu) | (((uint32_t)off & 0x7FFFFu) << 5);
  }
  memcpy(e->buf + site.at, &word, 4);
  e->sites_bound++;
}

int x86p_a64_emit_sites_bound(const X86pA64Emit *e) {
  return e->sites_made == e->sites_bound;
}

/* ---- structure --------------------------------------------------------------- */

void x86p_a64_emit_push_pair(X86pA64Emit *e, X86pA64Reg a, X86pA64Reg b) {
  /* STP Xa, Xb, [SP, #-16]! -- base 0xA9800000, imm7 = -16/8 = -2. */
  uint32_t imm7 = (uint32_t)(-2) & 0x7Fu;
  put32(e, 0xA9800000u | (imm7 << 15) | ((uint32_t)b << 10) | (31u << 5) | (uint32_t)a);
}

void x86p_a64_emit_pop_pair(X86pA64Emit *e, X86pA64Reg a, X86pA64Reg b) {
  /* LDP Xa, Xb, [SP], #16 -- base 0xA8C00000, imm7 = 16/8 = 2. */
  uint32_t imm7 = 2u & 0x7Fu;
  put32(e, 0xA8C00000u | (imm7 << 15) | ((uint32_t)b << 10) | (31u << 5) | (uint32_t)a);
}

void x86p_a64_emit_sub_sp_imm(X86pA64Emit *e, uint32_t imm) {
  add_sub_imm(e, 1, 1, kA64Sp, kA64Sp, imm, 0);
}

void x86p_a64_emit_add_sp_imm(X86pA64Emit *e, uint32_t imm) {
  add_sub_imm(e, 1, 0, kA64Sp, kA64Sp, imm, 0);
}

void x86p_a64_emit_ret(X86pA64Emit *e) {
  put32(e, 0xD65F0000u | ((uint32_t)kA64Lr << 5));
}

void x86p_a64_emit_blr(X86pA64Emit *e, X86pA64Reg target) {
  put32(e, 0xD63F0000u | ((uint32_t)target << 5));
}
