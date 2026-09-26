/*
 * emit_arm64.h -- writing AArch64 machine code, instruction by instruction.
 *
 * The ARM64 counterpart of emit_x64.h: this file knows nothing about the
 * guest, only how to encode host instructions. jit_arm64.c is the guest
 * meaning; this is the alphabet.
 *
 * REGISTERS ARE FIXED-WIDTH 32-BIT INSTRUCTIONS, NOT VARIABLE-LENGTH BYTES.
 * Unlike x86, there is no REX/ModRM/SIB puzzle -- every AArch64 instruction is
 * four bytes, and the traps here are different: an immediate that does not fit
 * the field width, a PC-relative branch that must be four-byte aligned and
 * within its signed range, and SP's dual identity (SP in a load/store base but
 * the zero register XZR/WZR everywhere else a register field this width can
 * name it).
 *
 * THE SAME OVERFLOW DISCIPLINE AS emit_x64.h: every emit checks capacity
 * first, a failed emit sets a sticky flag, and the caller checks once at the
 * end. A block that overflowed is discarded, not truncated and run.
 */
#ifndef X86PORT_EMIT_ARM64_H
#define X86PORT_EMIT_ARM64_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * General registers, numbered as AArch64 encodes them (0-30), plus the two
 * special encodings the ISA overloads onto number 31: SP in a load/store
 * base or an ADD/SUB immediate, XZR/WZR (hard-wired zero) everywhere else.
 * This encoder only ever uses 31 as SP (kA64Sp) for the host stack, and never
 * needs XZR as an explicit operand -- CMP/TST are emitted as their own
 * instructions rather than as ANDS/SUBS-with-XZR aliases, so the ambiguity
 * never has to be resolved by a caller.
 */
typedef enum X86pA64Reg {
  kA64X0 = 0,
  kA64X1 = 1,
  kA64X2 = 2,
  kA64X3 = 3,
  kA64X4 = 4,
  kA64X5 = 5,
  kA64X6 = 6,
  kA64X7 = 7,
  kA64X8 = 8,
  kA64X9 = 9,
  kA64X10 = 10,
  kA64X11 = 11,
  kA64X12 = 12,
  kA64X13 = 13,
  kA64X14 = 14,
  kA64X15 = 15,
  /* X16/X17 are the platform's IP0/IP1 veneer registers -- avoided as general
     scratch so an emitted BLR is never mistaken for a linker stub by a tool
     walking the code. */
  kA64X18 = 18, /* platform-reserved on Darwin; avoided, listed for completeness */
  kA64X19 = 19, /* callee-saved: this backend's CPU_REG */
  kA64X20 = 20,
  kA64X21 = 21,
  kA64X22 = 22,
  kA64X23 = 23,
  kA64X24 = 24,
  kA64X25 = 25,
  kA64X26 = 26,
  kA64X27 = 27,
  kA64X28 = 28,
  kA64Fp = 29, /* X29, frame pointer */
  kA64Lr = 30, /* X30, link register */
  kA64Sp = 31, /* valid only as a load/store base or ADD/SUB(imm) operand */
  kA64RegCount
} X86pA64Reg;

/*
 * The host ALU operations this backend needs, by their AArch64 mnemonic
 * family. Unlike x86's single opcode-extension numbering, AArch64 ADD/SUB and
 * AND/ORR/EOR are genuinely different instruction encodings, so this is a
 * dispatch tag for the emitter rather than a wire-format number.
 */
typedef enum X86pA64Alu { kA64Add = 0, kA64Sub = 1, kA64And = 2, kA64Orr = 3, kA64Eor = 4, kA64AluCount } X86pA64Alu;

/*
 * AArch64 condition codes, numbered as the instruction set encodes them
 * (AArch64 ARM, C1.2.4). X86pCond's own numbering is unrelated; jit_arm64.c
 * is the one place that translates between them, the same seam
 * x86p_emit_cmovcc_r32_r32's `cc` parameter is for the x64 backend.
 */
typedef enum X86pA64Cond {
  kA64CondEq = 0x0,
  kA64CondNe = 0x1,
  kA64CondCs = 0x2, /* HS/CS: unsigned >= */
  kA64CondCc = 0x3, /* LO/CC: unsigned < */
  kA64CondMi = 0x4,
  kA64CondPl = 0x5,
  kA64CondVs = 0x6,
  kA64CondVc = 0x7,
  kA64CondHi = 0x8,
  kA64CondLs = 0x9,
  kA64CondGe = 0xA,
  kA64CondLt = 0xB,
  kA64CondGt = 0xC,
  kA64CondLe = 0xD,
  kA64CondAl = 0xE
} X86pA64Cond;

/* The output buffer. Same shape and discipline as X86pEmit in emit_x64.h. */
typedef struct X86pA64Emit {
  uint8_t *buf;
  size_t cap;
  size_t len;
  int overflow;
  unsigned sites_made;
  unsigned sites_bound;
} X86pA64Emit;

void x86p_a64_emit_init(X86pA64Emit *e, void *buf, size_t cap);
int x86p_a64_emit_ok(const X86pA64Emit *e);

/* ---- moves --------------------------------------------------------------- */

/* mov w(dst), #imm -- exact for any 32-bit pattern via up to two MOVZ/MOVK. */
void x86p_a64_emit_mov_w_imm32(X86pA64Emit *e, X86pA64Reg dst, uint32_t imm);

/* mov x(dst), #imm -- exact for any 64-bit pattern via up to four MOVZ/MOVK.
   How the address of a helper function or a code-cache constant reaches the
   emitted code. */
void x86p_a64_emit_mov_x_imm64(X86pA64Emit *e, X86pA64Reg dst, uint64_t imm);

/* mov x(dst), x(src) -- the full 64-bit register (ORR alias, XZR is never an
   operand here so mov-from-XZR is never how this encodes a zero). */
void x86p_a64_emit_mov_x_x(X86pA64Emit *e, X86pA64Reg dst, X86pA64Reg src);

/* mov w(dst), w(src) -- the 32-bit form; also zeroes the upper 32 bits of the
   64-bit register, as every AArch64 32-bit write does. */
void x86p_a64_emit_mov_w_w(X86pA64Emit *e, X86pA64Reg dst, X86pA64Reg src);

/* ldr w(dst), [x(base), #disp] */
void x86p_a64_emit_load32(X86pA64Emit *e, X86pA64Reg dst, X86pA64Reg base, int32_t disp);
/* str w(src), [x(base), #disp] */
void x86p_a64_emit_store32(X86pA64Emit *e, X86pA64Reg base, int32_t disp, X86pA64Reg src);
/* ldr x(dst), [x(base), #disp] */
void x86p_a64_emit_load64(X86pA64Emit *e, X86pA64Reg dst, X86pA64Reg base, int32_t disp);
/* str x(src), [x(base), #disp] */
void x86p_a64_emit_store64(X86pA64Emit *e, X86pA64Reg base, int32_t disp, X86pA64Reg src);

/* ldrb w(dst), [x(base), #disp] -- zero-extended. */
void x86p_a64_emit_load8_zx(X86pA64Emit *e, X86pA64Reg dst, X86pA64Reg base, int32_t disp);
/* strb w(src), [x(base), #disp] */
void x86p_a64_emit_store8_reg(X86pA64Emit *e, X86pA64Reg base, int32_t disp, X86pA64Reg src);
/* strb w(zr-built-from-imm), [x(base), #disp] -- materialises imm in a
   scratch register first; there is no store-immediate on AArch64. */
void x86p_a64_emit_store8_imm(X86pA64Emit *e, X86pA64Reg base, int32_t disp, uint8_t imm);

/* ldrh w(dst), [x(base), #disp] -- zero-extended. */
void x86p_a64_emit_load16_zx(X86pA64Emit *e, X86pA64Reg dst, X86pA64Reg base, int32_t disp);
/* strh w(src), [x(base), #disp] */
void x86p_a64_emit_store16_reg(X86pA64Emit *e, X86pA64Reg base, int32_t disp, X86pA64Reg src);
void x86p_a64_emit_store16_imm(X86pA64Emit *e, X86pA64Reg base, int32_t disp, uint16_t imm);
void x86p_a64_emit_store32_imm(X86pA64Emit *e, X86pA64Reg base, int32_t disp, uint32_t imm);

/* add x(dst), x(base), #disp -- an ADDRESS inside the guest CPU struct, for a
   helper that takes a pointer to part of it. `disp` is folded through one or
   two ADD/MOVK-style immediate emits when it does not fit a single 12-bit
   (optionally shifted) field. */
void x86p_a64_emit_lea64(X86pA64Emit *e, X86pA64Reg dst, X86pA64Reg base, int32_t disp);

/* ---- arithmetic ------------------------------------------------------------ */

/* <alu> w(dst), w(dst), w(src) -- 32-bit, flags NOT set (plain ADD/SUB/AND/
   ORR/EOR, never the S-suffixed form). */
void x86p_a64_emit_alu_w_w(X86pA64Emit *e, X86pA64Alu op, X86pA64Reg dst, X86pA64Reg src);
/* <alu> w(dst), w(dst), #imm */
void x86p_a64_emit_alu_w_imm(X86pA64Emit *e, X86pA64Alu op, X86pA64Reg dst, uint32_t imm);
/* <alu> x(dst), x(dst), x(src) -- 64-bit, for host pointer arithmetic only. */
void x86p_a64_emit_alu_x_x(X86pA64Emit *e, X86pA64Alu op, X86pA64Reg dst, X86pA64Reg src);
/* add/sub x(dst), x(dst), #imm (0..4095) -- host pointer/stack adjustment. */
void x86p_a64_emit_alu_x_imm(X86pA64Emit *e, X86pA64Alu op, X86pA64Reg dst, uint32_t imm);

/* lsl w(dst), w(dst), #count */
void x86p_a64_emit_shl_w_imm(X86pA64Emit *e, X86pA64Reg dst, uint8_t count);
/* asr w(dst), w(dst), #count -- arithmetic (sign-propagating) right shift. */
void x86p_a64_emit_sar_w_imm(X86pA64Emit *e, X86pA64Reg dst, uint8_t count);
/* lsr w(dst), w(dst), #count */
void x86p_a64_emit_lsr_w_imm(X86pA64Emit *e, X86pA64Reg dst, uint8_t count);
/* lsl/lsr/asr w(dst), w(src), w(count) -- LSLV/LSRV/ASRV: the count is taken
   modulo 32 by the hardware. */
typedef enum X86pA64Shift { kA64Lsl = 0, kA64Lsr = 1, kA64Asr = 2 } X86pA64Shift;
void x86p_a64_emit_shift_w_w(X86pA64Emit *e, X86pA64Shift op, X86pA64Reg dst, X86pA64Reg src, X86pA64Reg count);
/* dst = src >> count, 64-bit logical. */
void x86p_a64_emit_lsr_x_imm(X86pA64Emit *e, X86pA64Reg dst, X86pA64Reg src, uint8_t count);

/* cmp w(a), w(b) -- SUBS with the result discarded; sets NZCV. */
void x86p_a64_emit_cmp_w_w(X86pA64Emit *e, X86pA64Reg a, X86pA64Reg b);
void x86p_a64_emit_cmn_w_w(X86pA64Emit *e, X86pA64Reg a, X86pA64Reg b);
/* cmp w(a), #imm */
void x86p_a64_emit_cmp_w_imm(X86pA64Emit *e, X86pA64Reg a, uint32_t imm);
/* cmp x(a), x(b) -- SUBS XZR, Xa, Xb: the 64-bit compare. */
void x86p_a64_emit_cmp_x_x(X86pA64Emit *e, X86pA64Reg a, X86pA64Reg b);
/* subs x(dst), x(dst), #imm (imm <= 4095): a decrement that sets NZCV. */
void x86p_a64_emit_subs_x_imm(X86pA64Emit *e, X86pA64Reg dst, uint32_t imm);
/* tst w(a), w(b) -- ANDS with the result discarded; sets NZCV (V and C to 0). */
void x86p_a64_emit_tst_w_w(X86pA64Emit *e, X86pA64Reg a, X86pA64Reg b);

/* csel w(dst), w(a), w(b), cc -- dst = cc ? a : b. The AArch64 analogue of
   x86p_emit_cmovcc_r32_r32: select without a branch, using NZCV a preceding
   CMP/TST already set. */
void x86p_a64_emit_csel_w(X86pA64Emit *e, X86pA64Cond cc, X86pA64Reg dst, X86pA64Reg a, X86pA64Reg b);

/* cset w(dst), cc -- materialise a condition as 0 or 1. */
void x86p_a64_emit_cset_w(X86pA64Emit *e, X86pA64Cond cc, X86pA64Reg dst);

/* <alu> w(dst), w(dst), w([base, #disp]) -- load then ALU; there is no
   memory operand on AArch64 data-processing instructions. */
void x86p_a64_emit_alu_w_mem(X86pA64Emit *e, X86pA64Alu op, X86pA64Reg dst, X86pA64Reg base, int32_t disp);

/* ---- floating point (for the x87 helper-call marshaling only) ------------ */

/* ldr q(dst 0..7), [x(base), #disp] -- the 128-bit `long double` slot AAPCS64
   passes in a V register. Only V0 and V1 are ever used by this backend. */
void x86p_a64_emit_load_q(X86pA64Emit *e, unsigned vreg, X86pA64Reg base, int32_t disp);
void x86p_a64_emit_store_q(X86pA64Emit *e, X86pA64Reg base, int32_t disp, unsigned vreg);
/* mov v(dst).16b, v(src).16b -- move a 128-bit value between V registers. */
void x86p_a64_emit_mov_q_q(X86pA64Emit *e, unsigned vdst, unsigned vsrc);

/* ---- scalar double arithmetic (the x87 inline paths) ------------------------ */

/* The four binary64 operations, d(dst) = d(a) <op> d(b), in the host's
   current FPCR rounding mode (round-to-nearest-even; nothing here changes it). */
typedef enum X86pA64FpOp { kA64FAdd = 0, kA64FSub = 1, kA64FMul = 2, kA64FDiv = 3 } X86pA64FpOp;
void x86p_a64_emit_fop_d(X86pA64Emit *e, X86pA64FpOp op, unsigned dst, unsigned a, unsigned b);
/* fneg d(dst), d(src) -- flips the sign bit only. */
void x86p_a64_emit_fneg_d(X86pA64Emit *e, unsigned dst, unsigned src);
/* fcmp d(a), d(b) -- NZCV from an ordered compare: EQ equal, MI less than. */
void x86p_a64_emit_fcmp_d(X86pA64Emit *e, unsigned a, unsigned b);
/* fmov d(dst), x(src) / fmov x(dst), d(src) -- the 64 bits, unconverted. */
void x86p_a64_emit_fmov_d_x(X86pA64Emit *e, unsigned dst, X86pA64Reg src);
void x86p_a64_emit_fmov_x_d(X86pA64Emit *e, X86pA64Reg dst, unsigned src);
/* fmov d(dst), d(src) -- a register copy. */
void x86p_a64_emit_fmov_d_d(X86pA64Emit *e, unsigned dst, unsigned src);
/* fmov d(dst), xzr -- +0.0. */
void x86p_a64_emit_fmov_d_zero(X86pA64Emit *e, unsigned dst);
/* fmov s(dst), w(src) / fmov w(dst), s(src) -- the 32 bits, unconverted. */
void x86p_a64_emit_fmov_s_w(X86pA64Emit *e, unsigned dst, X86pA64Reg src);
void x86p_a64_emit_fmov_w_s(X86pA64Emit *e, X86pA64Reg dst, unsigned src);
/* fcvt d(dst), s(src) (exact) and fcvt s(dst), d(src) (rounds per FPCR). */
void x86p_a64_emit_fcvt_d_s(X86pA64Emit *e, unsigned dst, unsigned src);
void x86p_a64_emit_fcvt_s_d(X86pA64Emit *e, unsigned dst, unsigned src);
/* ucvtf d(dst), x(src) -- the unsigned 64-bit integer, rounded per FPCR. */
void x86p_a64_emit_ucvtf_d_x(X86pA64Emit *e, unsigned dst, X86pA64Reg src);

/* ---- bit fields ---------------------------------------------------------------- */

/* ubfx w(dst), w(src), #lsb, #width / the 64-bit form: the field, zero-extended. */
void x86p_a64_emit_ubfx_w(X86pA64Emit *e, X86pA64Reg dst, X86pA64Reg src, unsigned lsb, unsigned width);
void x86p_a64_emit_ubfx_x(X86pA64Emit *e, X86pA64Reg dst, X86pA64Reg src, unsigned lsb, unsigned width);
/* lsl x(dst), x(src), #count (count 1..63). */
void x86p_a64_emit_lsl_x_imm(X86pA64Emit *e, X86pA64Reg dst, X86pA64Reg src, unsigned count);
/* lsl w(dst), w(src), #count (count 1..31), source and destination apart. */
void x86p_a64_emit_lsl_w_w_imm(X86pA64Emit *e, X86pA64Reg dst, X86pA64Reg src, unsigned count);
/* <add|orr> x(dst), x(a), x(b), lsl #shift and the 32-bit forms. Only kA64Add
   and kA64Orr are accepted; anything else overflows the emitter. */
void x86p_a64_emit_alu_x_x_lsl(
    X86pA64Emit *e, X86pA64Alu op, X86pA64Reg dst, X86pA64Reg a, X86pA64Reg b, unsigned shift);
void x86p_a64_emit_alu_w_w_lsl(
    X86pA64Emit *e, X86pA64Alu op, X86pA64Reg dst, X86pA64Reg a, X86pA64Reg b, unsigned shift);
/* eor w(dst), w(a), w(b), lsr #shift (shift 0..31). */
void x86p_a64_emit_eor_w_w_lsr(X86pA64Emit *e, X86pA64Reg dst, X86pA64Reg a, X86pA64Reg b, unsigned shift);
/* tst w(a), #1 -- ANDS WZR, Wa, #1: Z is set when bit 0 is clear. */
void x86p_a64_emit_tst_w_bit0(X86pA64Emit *e, X86pA64Reg a);
/* orr x(dst), x(dst), #(1 << bit) -- one set bit, bit 0..63. */
void x86p_a64_emit_orr_x_bit(X86pA64Emit *e, X86pA64Reg dst, unsigned bit);

/* ---- loads and stores of the 128-bit x87 slot ------------------------------------ */

/* ldr q(t), [x(base)] / str q(t), [x(base)] with no displacement, for a slot
   whose address was computed into a register. */
void x86p_a64_emit_load_q_at(X86pA64Emit *e, unsigned vreg, X86pA64Reg base);
void x86p_a64_emit_store_q_at(X86pA64Emit *e, X86pA64Reg base, unsigned vreg);

/* ---- forward branches ------------------------------------------------------ */

typedef struct X86pA64EmitSite {
  size_t at;  /* offset of the instruction whose immediate is unresolved */
  size_t end; /* == at + 4; kept for the same shape as X86pEmitSite */
} X86pA64EmitSite;

/* b.cc <unbound> */
X86pA64EmitSite x86p_a64_emit_bcc(X86pA64Emit *e, X86pA64Cond cc);
/* b <unbound> */
X86pA64EmitSite x86p_a64_emit_b(X86pA64Emit *e);
/* cbz/cbnz w(reg) and x(reg) <unbound>: to the site when the register is (not)
   zero. */
X86pA64EmitSite x86p_a64_emit_cbz_w(X86pA64Emit *e, X86pA64Reg reg);
X86pA64EmitSite x86p_a64_emit_cbnz_w(X86pA64Emit *e, X86pA64Reg reg);
X86pA64EmitSite x86p_a64_emit_cbz_x(X86pA64Emit *e, X86pA64Reg reg);
X86pA64EmitSite x86p_a64_emit_cbnz_x(X86pA64Emit *e, X86pA64Reg reg);
/* tbz/tbnz x(reg), #bit <unbound> (bit 0..63): to the site when the bit is
   clear (tbz) or set (tbnz). Reach is +-32KB. */
X86pA64EmitSite x86p_a64_emit_tbz(X86pA64Emit *e, X86pA64Reg reg, unsigned bit);
X86pA64EmitSite x86p_a64_emit_tbnz(X86pA64Emit *e, X86pA64Reg reg, unsigned bit);
/* bl <unbound> -- a direct call to code in the same buffer: a routine the
   block emits once and its instructions share. LR is overwritten. */
X86pA64EmitSite x86p_a64_emit_bl(X86pA64Emit *e);
void x86p_a64_emit_bind(X86pA64Emit *e, X86pA64EmitSite site);
/* Bind `site` to the code at offset `target` (<= the current length), which
   may lie before it: a backward branch. */
void x86p_a64_emit_bind_to(X86pA64Emit *e, X86pA64EmitSite site, size_t target);
int x86p_a64_emit_sites_bound(const X86pA64Emit *e);

/* ---- structure -------------------------------------------------------------- */

/* stp x(a), x(b), [sp, #-16]! -- push a pair, pre-indexed; keeps SP 16-byte
   aligned, which AArch64 requires at every load/store through SP. */
void x86p_a64_emit_push_pair(X86pA64Emit *e, X86pA64Reg a, X86pA64Reg b);
/* ldp x(a), x(b), [sp], #16 -- pop a pair, post-indexed. */
void x86p_a64_emit_pop_pair(X86pA64Emit *e, X86pA64Reg a, X86pA64Reg b);
/* sub sp, sp, #imm (16-byte aligned) */
void x86p_a64_emit_sub_sp_imm(X86pA64Emit *e, uint32_t imm);
/* add sp, sp, #imm */
void x86p_a64_emit_add_sp_imm(X86pA64Emit *e, uint32_t imm);

/* ret (via LR) */
void x86p_a64_emit_ret(X86pA64Emit *e);

/* blr x(target) -- indirect call-with-link. Every helper call in this
   backend goes through a register, exactly as x86p_emit_call_r64 documents:
   the target is not guaranteed to be within a direct branch's reach. */
void x86p_a64_emit_blr(X86pA64Emit *e, X86pA64Reg target);

/* br x(target) -- indirect jump, LR untouched: a chained block transfer
   enters its successor in the frame the dispatcher's call opened. */
void x86p_a64_emit_br(X86pA64Emit *e, X86pA64Reg target);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_EMIT_ARM64_H */
