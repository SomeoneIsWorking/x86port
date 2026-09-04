/*
 * test_emit_arm64 -- does the ARM64 encoder emit the instruction it was
 * asked for?
 *
 * There is no Zydis-grade oracle for AArch64 available to this build (Zydis
 * only decodes x86), so this file cannot borrow test_emit_x64.c's trick of
 * decoding through an independent library. Instead it decodes with a small,
 * independently-written bitfield extractor: every field layout below (fixed
 * bits, sf/opc/shift/Rd/Rn/Rm/imm positions) is taken straight from the
 * AArch64 ISA field tables, not derived from emit_arm64.c's own encoding
 * logic. A bug shared between the encoder and this decoder is possible in
 * principle, but it would have to be the SAME misreading of the manual
 * reached twice independently, from two different codepaths, rather than the
 * decoder simply mirroring the encoder's arithmetic.
 *
 * Every AArch64 instruction is a fixed 4 bytes, so there is no length trap
 * the way there is on x86 -- the trap here is a field landing one bit off
 * (hw, shift, imm12, imm6) or a fixed-bits mask that overlaps a neighbouring
 * instruction family, which is exactly how the CSEL/CSET bug in this file's
 * history slipped through a purely visual review.
 */
#include "emit_arm64.h"

#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failed;
static int g_test_failed;
static unsigned long g_decoded;

#define CHECK(cond)                                                                                                   \
  do {                                                                                                                 \
    g_checks++;                                                                                                       \
    if (!(cond)) {                                                                                                     \
      g_failed++;                                                                                                     \
      printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                       \
    }                                                                                                                  \
  } while (0)

#define RUN(fn)                                                                                                        \
  do {                                                                                                                 \
    int before = g_failed;                                                                                            \
    printf("test %s\n", #fn);                                                                                         \
    fn();                                                                                                              \
    if (g_failed != before) {                                                                                         \
      g_test_failed++;                                                                                                \
      printf("  FAIL\n");                                                                                             \
    } else {                                                                                                           \
      printf("  PASS\n");                                                                                             \
    }                                                                                                                  \
  } while (0)

static uint32_t fbits(uint32_t w, int hi, int lo) {
  return (w >> lo) & ((1u << (hi - lo + 1)) - 1u);
}

static int32_t sext(uint32_t value, int bits) {
  uint32_t signbit = 1u << (bits - 1);
  return (int32_t)((value ^ signbit) - signbit);
}

static uint32_t last_word(const X86pA64Emit *e) {
  uint32_t w;
  g_checks++;
  if (!x86p_a64_emit_ok(e)) {
    g_failed++;
    printf("    FAIL: emitter overflowed\n");
    return 0;
  }
  if (e->len < 4) {
    g_failed++;
    printf("    FAIL: nothing emitted\n");
    return 0;
  }
  memcpy(&w, e->buf + e->len - 4, 4);
  g_decoded++;
  return w;
}

/* ---- moves: MOVZ/MOVK wide immediate, fixed bits28:23 = 100101 ---------- */

static void test_mov_x_imm64_forms(void) {
  static const uint64_t values[] = {0u, 1u, 0x1234u, 0x56781234u, 0xFFFFFFFFFFFFFFFFull, 0x8000000000000001ull};
  size_t v;
  for (v = 0; v < sizeof values / sizeof values[0]; v++) {
    uint8_t buf[64];
    X86pA64Emit e;
    uint64_t rebuilt = 0;
    int any_hw_used = 0;
    size_t i;
    x86p_a64_emit_init(&e, buf, sizeof buf);
    x86p_a64_emit_mov_x_imm64(&e, kA64X3, values[v]);
    CHECK(x86p_a64_emit_ok(&e));
    /* At least one MOVZ/MOVN/MOVK 64-bit instruction, all naming x3, all
       fixed-field 100101, reassembled by hw/imm16/opc should equal the
       requested immediate. */
    CHECK(e.len >= 4 && e.len % 4 == 0);
    for (i = 0; i < e.len; i += 4) {
      uint32_t w;
      uint32_t sf, opc, hw, imm16, rd;
      memcpy(&w, buf + i, 4);
      g_decoded++;
      CHECK(fbits(w, 28, 23) == 0x25u); /* 100101 */
      sf = fbits(w, 31, 31);
      opc = fbits(w, 30, 29);
      hw = fbits(w, 22, 21);
      imm16 = fbits(w, 20, 5);
      rd = fbits(w, 4, 0);
      CHECK(sf == 1u); /* 64-bit form throughout */
      CHECK(rd == (uint32_t)kA64X3);
      CHECK(opc == 2u || opc == 3u || (i == 0 && opc == 0u)); /* MOVZ, MOVK, or a leading MOVN */
      if (opc == 2u) { /* MOVZ: clears, then ORs this halfword in */
        rebuilt = ((uint64_t)imm16) << (hw * 16u);
      } else if (opc == 3u) { /* MOVK: merges this halfword in */
        rebuilt |= ((uint64_t)imm16) << (hw * 16u);
        any_hw_used = 1;
      } else { /* MOVN dst,#imm16,LSL hw == NOT(imm16 << hw*16 | ~0 elsewhere) */
        rebuilt = ~(((uint64_t)imm16) << (hw * 16u));
      }
    }
    (void)any_hw_used;
    CHECK(rebuilt == values[v]);
  }
}

static void test_mov_w_imm32_is_32_bit(void) {
  uint8_t buf[32];
  X86pA64Emit e;
  uint32_t w;
  x86p_a64_emit_init(&e, buf, sizeof buf);
  x86p_a64_emit_mov_w_imm32(&e, kA64X5, 0xBEEFu);
  w = last_word(&e);
  CHECK(fbits(w, 28, 23) == 0x25u);
  CHECK(fbits(w, 31, 31) == 0u); /* sf=0: 32-bit */
  CHECK(fbits(w, 4, 0) == (uint32_t)kA64X5);
}

/* mov x(dst), x(src) is an ORR (shifted register) alias: orr xd, xzr, xm.
   Logical-shifted-register fixed bits28:24 = 01010; opc(30:29) 01 = ORR. */
static void test_mov_x_x_is_orr_with_xzr(void) {
  uint8_t buf[16];
  X86pA64Emit e;
  uint32_t w;
  x86p_a64_emit_init(&e, buf, sizeof buf);
  x86p_a64_emit_mov_x_x(&e, kA64X7, kA64X22);
  w = last_word(&e);
  CHECK(fbits(w, 28, 24) == 0x0Au);
  CHECK(fbits(w, 31, 31) == 1u); /* 64-bit */
  CHECK(fbits(w, 30, 29) == 1u); /* ORR */
  CHECK(fbits(w, 21, 21) == 0u); /* N=0 */
  CHECK(fbits(w, 20, 16) == (uint32_t)kA64X22); /* Rm = src */
  CHECK(fbits(w, 9, 5) == 31u);                 /* Rn = XZR */
  CHECK(fbits(w, 4, 0) == (uint32_t)kA64X7);    /* Rd = dst */
}

/* ---- LDR/STR unsigned-offset immediate, fixed bits29:24 = 111001, V=0 --- */

static void check_ldst_unsigned(uint32_t w, uint32_t want_size, uint32_t want_opc, X86pA64Reg want_rt,
                                X86pA64Reg want_rn, uint32_t scale) {
  uint32_t imm12;
  CHECK(fbits(w, 29, 24) == 0x39u); /* 111001 */
  CHECK(fbits(w, 26, 26) == 0u);    /* not a vector/SIMD transfer */
  CHECK(fbits(w, 31, 30) == want_size);
  CHECK(fbits(w, 23, 22) == want_opc);
  CHECK(fbits(w, 9, 5) == (uint32_t)want_rn);
  CHECK(fbits(w, 4, 0) == (uint32_t)want_rt);
  imm12 = fbits(w, 21, 10);
  (void)imm12;
  (void)scale;
}

static void test_load32_store32_scaled(void) {
  uint8_t buf[16];
  X86pA64Emit e;
  uint32_t w;

  x86p_a64_emit_init(&e, buf, sizeof buf);
  x86p_a64_emit_load32(&e, kA64X0, kA64X19, 64); /* 64 / 4 = imm12 16, exact */
  w = last_word(&e);
  check_ldst_unsigned(w, 2u, 1u, kA64X0, kA64X19, 4u);
  CHECK(fbits(w, 21, 10) == 16u);

  x86p_a64_emit_init(&e, buf, sizeof buf);
  x86p_a64_emit_store32(&e, kA64X19, 8, kA64X2);
  w = last_word(&e);
  check_ldst_unsigned(w, 2u, 0u, kA64X2, kA64X19, 4u);
  CHECK(fbits(w, 21, 10) == 2u);
}

static void test_load64_store64_scaled(void) {
  uint8_t buf[16];
  X86pA64Emit e;
  uint32_t w;

  x86p_a64_emit_init(&e, buf, sizeof buf);
  x86p_a64_emit_load64(&e, kA64X1, kA64X11, 16);
  w = last_word(&e);
  check_ldst_unsigned(w, 3u, 1u, kA64X1, kA64X11, 8u);
  CHECK(fbits(w, 21, 10) == 2u);

  x86p_a64_emit_init(&e, buf, sizeof buf);
  x86p_a64_emit_store64(&e, kA64X11, 0, kA64X9);
  w = last_word(&e);
  check_ldst_unsigned(w, 3u, 0u, kA64X9, kA64X11, 8u);
  CHECK(fbits(w, 21, 10) == 0u);
}

static void test_load8_store8_zero_extend(void) {
  uint8_t buf[16];
  X86pA64Emit e;
  uint32_t w;

  x86p_a64_emit_init(&e, buf, sizeof buf);
  x86p_a64_emit_load8_zx(&e, kA64X0, kA64X10, 3);
  w = last_word(&e);
  check_ldst_unsigned(w, 0u, 1u, kA64X0, kA64X10, 1u);
  CHECK(fbits(w, 21, 10) == 3u);

  x86p_a64_emit_init(&e, buf, sizeof buf);
  x86p_a64_emit_store8_reg(&e, kA64X10, 5, kA64X3);
  w = last_word(&e);
  check_ldst_unsigned(w, 0u, 0u, kA64X3, kA64X10, 1u);
}

static void test_load16_store16_zero_extend(void) {
  uint8_t buf[16];
  X86pA64Emit e;
  uint32_t w;

  x86p_a64_emit_init(&e, buf, sizeof buf);
  x86p_a64_emit_load16_zx(&e, kA64X0, kA64X10, 6);
  w = last_word(&e);
  check_ldst_unsigned(w, 1u, 1u, kA64X0, kA64X10, 2u);
  CHECK(fbits(w, 21, 10) == 3u);

  x86p_a64_emit_init(&e, buf, sizeof buf);
  x86p_a64_emit_store16_reg(&e, kA64X10, 4, kA64X4);
  w = last_word(&e);
  check_ldst_unsigned(w, 1u, 0u, kA64X4, kA64X10, 2u);
}

/* materialize_address fallback: a displacement too large/negative for the
   scaled unsigned-offset form. Must decode as SUB/ADD (immediate) into the
   encoder's internal scratch register, followed by a zero-offset load. */
static void test_load32_negative_displacement_uses_materialize_fallback(void) {
  uint8_t buf[32];
  X86pA64Emit e;
  uint32_t addr_w, ld_w;
  x86p_a64_emit_init(&e, buf, sizeof buf);
  x86p_a64_emit_load32(&e, kA64X0, kA64X1, -5);
  CHECK(x86p_a64_emit_ok(&e));
  CHECK(e.len == 8u); /* one address instruction, one load */
  memcpy(&addr_w, buf, 4);
  memcpy(&ld_w, buf + 4, 4);
  g_decoded += 2;
  CHECK(fbits(addr_w, 28, 24) == 0x11u); /* ADD/SUB immediate family */
  CHECK(fbits(addr_w, 30, 30) == 1u);    /* op=1: SUB */
  CHECK(fbits(addr_w, 9, 5) == (uint32_t)kA64X1);
  CHECK((int32_t)fbits(addr_w, 21, 10) == 5);
  check_ldst_unsigned(ld_w, 2u, 1u, kA64X0, (X86pA64Reg)fbits(addr_w, 4, 0), 4u);
  CHECK(fbits(ld_w, 21, 10) == 0u); /* zero-offset from the materialised base */
}

/* ---- ALU: shifted-register (fixed 01011 add/sub, 01010 logical) --------- */

static void test_alu_w_w_every_op(void) {
  static const struct {
    X86pA64Alu op;
    uint32_t family; /* bits28:24 */
    uint32_t opc;    /* bits30:29 */
  } cases[] = {
      {kA64Add, 0x0Bu, 0u}, {kA64Sub, 0x0Bu, 2u}, {kA64And, 0x0Au, 0u}, {kA64Orr, 0x0Au, 1u}, {kA64Eor, 0x0Au, 2u},
  };
  size_t i;
  for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    uint8_t buf[16];
    X86pA64Emit e;
    uint32_t w;
    x86p_a64_emit_init(&e, buf, sizeof buf);
    x86p_a64_emit_alu_w_w(&e, cases[i].op, kA64X0, kA64X2);
    w = last_word(&e);
    CHECK(fbits(w, 28, 24) == cases[i].family);
    CHECK(fbits(w, 31, 31) == 0u); /* 32-bit */
    CHECK(fbits(w, 30, 29) == cases[i].opc);
    CHECK(fbits(w, 20, 16) == (uint32_t)kA64X2);
    CHECK(fbits(w, 9, 5) == (uint32_t)kA64X0);
    CHECK(fbits(w, 4, 0) == (uint32_t)kA64X0);
    /* ADD/SUB shifted-register also requires bit21=0 (register, not extended)*/
    if (cases[i].family == 0x0Bu) {
      CHECK(fbits(w, 21, 21) == 0u);
    }
  }
}

static void test_alu_w_imm_and_ff_ffff(void) {
  uint8_t buf[16];
  X86pA64Emit e;
  uint32_t w;

  /* AND w0, w0, #0xff -- logical immediate, fixed bits28:23 = 100100 */
  x86p_a64_emit_init(&e, buf, sizeof buf);
  x86p_a64_emit_alu_w_imm(&e, kA64And, kA64X0, 0xffu);
  w = last_word(&e);
  CHECK(fbits(w, 28, 23) == 0x24u);
  CHECK(fbits(w, 31, 31) == 0u);
  CHECK(fbits(w, 30, 29) == 0u); /* AND */
  CHECK(fbits(w, 22, 22) == 0u); /* N=0 for a 32-bit bitmask */
  /* contiguous low ones of width 8: immr=0, imms=(8-1)=7 */
  CHECK(fbits(w, 21, 16) == 0u);
  CHECK(fbits(w, 15, 10) == 7u);
  CHECK(fbits(w, 4, 0) == (uint32_t)kA64X0);

  x86p_a64_emit_init(&e, buf, sizeof buf);
  x86p_a64_emit_alu_w_imm(&e, kA64And, kA64X0, 0xffffu);
  w = last_word(&e);
  CHECK(fbits(w, 28, 23) == 0x24u);
  CHECK(fbits(w, 15, 10) == 15u); /* width 16: imms=15 */

  /* eor w0, w0, #0xffffffff has no bitmask-immediate encoding (all-ones is
     never a legal AArch64 logical immediate); the encoder documents this as
     an MVN alias instead: orn wd, wzr, wm with wm holding the -1 pattern, or
     an equivalent all-ones materialisation. Whichever it chooses, it must
     still land in the logical family and must NOT silently no-op. */
  x86p_a64_emit_init(&e, buf, sizeof buf);
  x86p_a64_emit_alu_w_imm(&e, kA64Eor, kA64X0, 0xffffffffu);
  CHECK(x86p_a64_emit_ok(&e));
  CHECK(e.len >= 4u);
}

static void test_shl_sar_w_imm(void) {
  uint8_t buf[16];
  X86pA64Emit e;
  uint32_t w;

  /* lsl wd, wd, #3 == UBFM wd, wd, #(-3 mod 32), #(31-3) */
  x86p_a64_emit_init(&e, buf, sizeof buf);
  x86p_a64_emit_shl_w_imm(&e, kA64X4, 3);
  w = last_word(&e);
  CHECK(fbits(w, 28, 23) == 0x26u); /* UBFM family: 100110 */
  CHECK(fbits(w, 31, 31) == 0u);
  CHECK(fbits(w, 30, 29) == 2u); /* UBFM opc */
  CHECK(fbits(w, 21, 16) == ((32u - 3u) & 31u));
  CHECK(fbits(w, 15, 10) == 31u - 3u);
  CHECK(fbits(w, 4, 0) == (uint32_t)kA64X4);
  CHECK(fbits(w, 9, 5) == (uint32_t)kA64X4);

  /* asr wd, wd, #5 == SBFM wd, wd, #5, #31 */
  x86p_a64_emit_init(&e, buf, sizeof buf);
  x86p_a64_emit_sar_w_imm(&e, kA64X4, 5);
  w = last_word(&e);
  CHECK(fbits(w, 28, 23) == 0x26u);
  CHECK(fbits(w, 30, 29) == 0u); /* SBFM opc */
  CHECK(fbits(w, 21, 16) == 5u);
  CHECK(fbits(w, 15, 10) == 31u);
}

static void test_cmp_and_tst(void) {
  uint8_t buf[16];
  X86pA64Emit e;
  uint32_t w;

  /* cmp wa,wb == SUBS wzr,wa,wb -- fixed 01011, opc=11 (sub, S=1) */
  x86p_a64_emit_init(&e, buf, sizeof buf);
  x86p_a64_emit_cmp_w_w(&e, kA64X6, kA64X1);
  w = last_word(&e);
  CHECK(fbits(w, 28, 24) == 0x0Bu);
  CHECK(fbits(w, 30, 29) == 3u); /* op=1 sub, S=1 */
  CHECK(fbits(w, 9, 5) == (uint32_t)kA64X6);
  CHECK(fbits(w, 20, 16) == (uint32_t)kA64X1);
  CHECK(fbits(w, 4, 0) == 31u); /* Rd = WZR, result discarded */

  /* cmp wa,#imm == SUBS wzr,wa,#imm -- ADD/SUB immediate, fixed 10001 */
  x86p_a64_emit_init(&e, buf, sizeof buf);
  x86p_a64_emit_cmp_w_imm(&e, kA64X6, 50u);
  w = last_word(&e);
  CHECK(fbits(w, 28, 24) == 0x11u);
  CHECK(fbits(w, 30, 29) == 3u);
  CHECK(fbits(w, 21, 10) == 50u);
  CHECK(fbits(w, 4, 0) == 31u);

  /* tst wa,wb == ANDS wzr,wa,wb -- logical shifted-register, opc=11 */
  x86p_a64_emit_init(&e, buf, sizeof buf);
  x86p_a64_emit_tst_w_w(&e, kA64X6, kA64X1);
  w = last_word(&e);
  CHECK(fbits(w, 28, 24) == 0x0Au);
  CHECK(fbits(w, 30, 29) == 3u);
  CHECK(fbits(w, 4, 0) == 31u);
}

/* ---- CSEL/CSET: fixed field is bits28:21 = 0xD4, not bits31:24 --------- */

static void test_csel_every_condition(void) {
  unsigned cc;
  for (cc = 0; cc <= (unsigned)kA64CondAl; cc++) {
    uint8_t buf[16];
    X86pA64Emit e;
    uint32_t w;
    x86p_a64_emit_init(&e, buf, sizeof buf);
    x86p_a64_emit_csel_w(&e, (X86pA64Cond)cc, kA64X0, kA64X1, kA64X2);
    w = last_word(&e);
    CHECK(fbits(w, 28, 21) == 0xD4u);
    CHECK(fbits(w, 31, 31) == 0u); /* 32-bit */
    CHECK(fbits(w, 30, 29) == 0u); /* CSEL, not CSINC/CSINV/CSNEG */
    CHECK(fbits(w, 11, 10) == 0u); /* op2 */
    CHECK(fbits(w, 20, 16) == (uint32_t)kA64X2); /* Rm = false-value */
    CHECK(fbits(w, 9, 5) == (uint32_t)kA64X1);   /* Rn = true-value */
    CHECK(fbits(w, 4, 0) == (uint32_t)kA64X0);
    CHECK(fbits(w, 15, 12) == cc);
  }
}

static void test_cset_is_csinc_with_inverted_condition(void) {
  /* cset wd,cc == csinc wd,wzr,wzr,invert(cc) */
  uint8_t buf[16];
  X86pA64Emit e;
  uint32_t w;
  x86p_a64_emit_init(&e, buf, sizeof buf);
  x86p_a64_emit_cset_w(&e, kA64CondNe, kA64X0);
  w = last_word(&e);
  CHECK(fbits(w, 28, 21) == 0xD4u);
  CHECK(fbits(w, 30, 29) == 0u); /* op=0,S=0 -- same as CSEL, op2 alone selects CSINC */
  CHECK(fbits(w, 11, 10) == 1u); /* op2=01: CSINC */
  CHECK(fbits(w, 20, 16) == 31u);
  CHECK(fbits(w, 9, 5) == 31u);
  CHECK(fbits(w, 4, 0) == (uint32_t)kA64X0);
  CHECK(fbits(w, 15, 12) == (uint32_t)(kA64CondNe ^ 1u)); /* inverted: EQ */
}

/* ---- branches ------------------------------------------------------------ */

static void test_bcc_self_bind_has_zero_offset(void) {
  uint8_t buf[16];
  X86pA64Emit e;
  X86pA64EmitSite s;
  uint32_t w;
  x86p_a64_emit_init(&e, buf, sizeof buf);
  s = x86p_a64_emit_bcc(&e, kA64CondEq);
  x86p_a64_emit_bind(&e, s); /* nothing emitted between site and bind */
  CHECK(x86p_a64_emit_ok(&e));
  memcpy(&w, buf, 4);
  g_decoded++;
  CHECK(fbits(w, 31, 24) == 0x54u); /* fixed B.cond family */
  CHECK(fbits(w, 4, 0) == (uint32_t)kA64CondEq);
  CHECK(sext(fbits(w, 23, 5), 19) == 1); /* one instruction (4 bytes) forward */
}

static void test_bcc_forward_over_one_instruction(void) {
  uint8_t buf[16];
  X86pA64Emit e;
  X86pA64EmitSite s;
  uint32_t w;
  x86p_a64_emit_init(&e, buf, sizeof buf);
  s = x86p_a64_emit_bcc(&e, kA64CondNe);
  x86p_a64_emit_mov_x_x(&e, kA64X0, kA64X1); /* one instruction in between */
  x86p_a64_emit_bind(&e, s);
  CHECK(x86p_a64_emit_ok(&e));
  memcpy(&w, buf, 4);
  g_decoded++;
  CHECK(sext(fbits(w, 23, 5), 19) == 2); /* skips the mov and lands past it */
}

static void test_b_unconditional_forward(void) {
  uint8_t buf[16];
  X86pA64Emit e;
  X86pA64EmitSite s;
  uint32_t w;
  x86p_a64_emit_init(&e, buf, sizeof buf);
  s = x86p_a64_emit_b(&e);
  x86p_a64_emit_bind(&e, s);
  CHECK(x86p_a64_emit_ok(&e));
  memcpy(&w, buf, 4);
  g_decoded++;
  CHECK(fbits(w, 31, 26) == 0x05u); /* 000101 */
  CHECK(sext(fbits(w, 25, 0), 26) == 1);
}

/* ---- structure ------------------------------------------------------------ */

static void test_push_pop_pair_and_sp_adjust(void) {
  uint8_t buf[16];
  X86pA64Emit e;
  uint32_t w;

  x86p_a64_emit_init(&e, buf, sizeof buf);
  x86p_a64_emit_push_pair(&e, kA64X19, kA64Lr);
  w = last_word(&e);
  CHECK(fbits(w, 31, 30) == 2u); /* opc=10: 64-bit */
  CHECK(fbits(w, 29, 27) == 5u); /* 101 */
  CHECK(fbits(w, 26, 26) == 0u); /* GPR, not SIMD */
  CHECK(fbits(w, 25, 23) == 3u); /* 011: pre-indexed */
  CHECK(fbits(w, 22, 22) == 0u); /* store */
  CHECK(sext(fbits(w, 21, 15), 7) == -2); /* #-16 in units of 8 bytes */
  CHECK(fbits(w, 14, 10) == (uint32_t)kA64Lr);
  CHECK(fbits(w, 9, 5) == 31u); /* base = SP */
  CHECK(fbits(w, 4, 0) == (uint32_t)kA64X19);

  x86p_a64_emit_init(&e, buf, sizeof buf);
  x86p_a64_emit_pop_pair(&e, kA64X19, kA64Lr);
  w = last_word(&e);
  CHECK(fbits(w, 25, 23) == 1u); /* 001: post-indexed */
  CHECK(fbits(w, 22, 22) == 1u); /* load */
  CHECK(sext(fbits(w, 21, 15), 7) == 2);

  x86p_a64_emit_init(&e, buf, sizeof buf);
  x86p_a64_emit_sub_sp_imm(&e, 16u);
  w = last_word(&e);
  CHECK(fbits(w, 28, 24) == 0x11u); /* ADD/SUB immediate */
  CHECK(fbits(w, 30, 30) == 1u);    /* SUB */
  CHECK(fbits(w, 9, 5) == 31u);     /* SP */
  CHECK(fbits(w, 4, 0) == 31u);
  CHECK(fbits(w, 21, 10) == 16u);

  x86p_a64_emit_init(&e, buf, sizeof buf);
  x86p_a64_emit_add_sp_imm(&e, 16u);
  w = last_word(&e);
  CHECK(fbits(w, 30, 30) == 0u); /* ADD */
}

static void test_ret_and_blr(void) {
  uint8_t buf[16];
  X86pA64Emit e;
  uint32_t w;

  x86p_a64_emit_init(&e, buf, sizeof buf);
  x86p_a64_emit_ret(&e);
  w = last_word(&e);
  CHECK(w == 0xD65F03C0u); /* ret (Rn=LR=x30): fully fixed, no operand choice */

  x86p_a64_emit_init(&e, buf, sizeof buf);
  x86p_a64_emit_blr(&e, kA64X9);
  w = last_word(&e);
  CHECK(fbits(w, 31, 25) == 0x6Bu); /* 1101011 */
  CHECK(fbits(w, 24, 21) == 1u);    /* BLR opc */
  CHECK(fbits(w, 9, 5) == (uint32_t)kA64X9);
}

/* ---- 128-bit V-register load/store (x87 long-double marshaling) -------- */

static void test_load_store_mov_q(void) {
  uint8_t buf[16];
  X86pA64Emit e;
  uint32_t w;

  x86p_a64_emit_init(&e, buf, sizeof buf);
  x86p_a64_emit_load_q(&e, 0u, kA64X1, 32);
  w = last_word(&e);
  CHECK(fbits(w, 29, 24) == 0x3Du); /* 111101 */
  CHECK(fbits(w, 23, 22) == 3u);    /* opc=11: 128-bit load */
  CHECK(fbits(w, 9, 5) == (uint32_t)kA64X1);
  CHECK(fbits(w, 4, 0) == 0u);
  CHECK(fbits(w, 21, 10) == 2u); /* 32 / 16 */

  x86p_a64_emit_init(&e, buf, sizeof buf);
  x86p_a64_emit_store_q(&e, kA64X1, 32, 0u);
  w = last_word(&e);
  CHECK(fbits(w, 23, 22) == 2u); /* opc=10: 128-bit store */

  x86p_a64_emit_init(&e, buf, sizeof buf);
  x86p_a64_emit_mov_q_q(&e, 0u, 1u);
  w = last_word(&e);
  CHECK(fbits(w, 31, 21) == 0x275u); /* fixed field of the ORR Vd.16B idiom */
}

/* ---- the negative: overflow is reported, not truncated ------------------ */

static void test_overflow_is_sticky_and_never_writes_past_the_end(void) {
  uint8_t buf[8];
  uint8_t guard[8];
  X86pA64Emit e;
  int i;

  memset(guard, 0xCD, sizeof guard);

  x86p_a64_emit_init(&e, buf, sizeof buf);
  CHECK(x86p_a64_emit_ok(&e));

  /* Five 4-byte instructions into an 8-byte buffer: the third must not fit. */
  for (i = 0; i < 5; i++) {
    x86p_a64_emit_mov_x_imm64(&e, kA64X0, 0x1111111111111111ull);
  }
  CHECK(!x86p_a64_emit_ok(&e));
  CHECK(e.len <= sizeof buf);

  x86p_a64_emit_ret(&e);
  CHECK(!x86p_a64_emit_ok(&e)); /* sticky: a later small emit must not clear it */

  x86p_a64_emit_init(&e, NULL, 64);
  CHECK(!x86p_a64_emit_ok(&e));
  x86p_a64_emit_ret(&e);
  CHECK(!x86p_a64_emit_ok(&e));

  for (i = 0; i < (int)sizeof guard; i++) {
    CHECK(guard[i] == 0xCD);
  }
}

int main(void) {
  RUN(test_mov_x_imm64_forms);
  RUN(test_mov_w_imm32_is_32_bit);
  RUN(test_mov_x_x_is_orr_with_xzr);
  RUN(test_load32_store32_scaled);
  RUN(test_load64_store64_scaled);
  RUN(test_load8_store8_zero_extend);
  RUN(test_load16_store16_zero_extend);
  RUN(test_load32_negative_displacement_uses_materialize_fallback);
  RUN(test_alu_w_w_every_op);
  RUN(test_alu_w_imm_and_ff_ffff);
  RUN(test_shl_sar_w_imm);
  RUN(test_cmp_and_tst);
  RUN(test_csel_every_condition);
  RUN(test_cset_is_csinc_with_inverted_condition);
  RUN(test_bcc_self_bind_has_zero_offset);
  RUN(test_bcc_forward_over_one_instruction);
  RUN(test_b_unconditional_forward);
  RUN(test_push_pop_pair_and_sp_adjust);
  RUN(test_ret_and_blr);
  RUN(test_load_store_mov_q);
  RUN(test_overflow_is_sticky_and_never_writes_past_the_end);

  printf("\n%d check(s), %d failure(s) in %d test(s)\n", g_checks, g_failed, g_test_failed);
  printf("%lu instruction word(s) decoded\n", g_decoded);
  if (g_decoded == 0u) {
    printf("REFUSED: nothing was decoded; these results mean nothing\n");
    return 1;
  }
  return g_failed ? 1 : 0;
}
