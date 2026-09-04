/*
 * emit_x64.h -- writing x86-64 machine code, byte by byte.
 *
 * The bottom half of the first JIT backend. This file knows nothing about the
 * guest: it encodes host instructions and that is all. Guest meaning lives in
 * jit_x64.c, so the encoder can be tested against a disassembler without a
 * guest program existing.
 *
 * WHY AN ENCODER AT ALL, WHEN THE GUEST IS ALSO x86. Because x86-32 guest code
 * cannot simply be run on an x86-64 host: the guest's registers live in an
 * X86pCpu struct rather than in host registers, its memory is an offset into a
 * host mapping rather than a flat address space, and its flags are the lazy
 * (kind, a, b, r) form this framework's interpreter defines rather than host
 * EFLAGS. Every guest instruction therefore becomes several host ones. The
 * family similarity buys correctness of ARITHMETIC -- a host ADD computes what
 * a guest ADD computes -- and nothing else.
 *
 * OVERFLOW IS RECORDED, NEVER TRUNCATED. An encoder that quietly stops writing
 * when the buffer fills produces a block that ends mid-instruction, and the CPU
 * will happily execute whatever follows. That is unbounded, silent, and lands
 * far from the cause. So every emit checks capacity first, and a failed emit
 * sets a sticky flag that the caller must ask about before publishing anything.
 * The caller checks ONCE, at the end, rather than after each byte -- checking
 * per-emit is what makes people stop checking.
 *
 * THE THREE ENCODING TRAPS, all of which produce valid-looking wrong code:
 *
 *   - RSP and R12 have rm == 100, which the ModRM byte spells "a SIB follows".
 *     `[rsp+8]` written without a SIB decodes as something else entirely.
 *   - RBP and R13 have rm == 101, which at mod == 00 spells "RIP-relative".
 *     `[rbp]` with no displacement is not encodable; it needs an explicit
 *     zero disp8.
 *   - REX.B extends the base register, REX.R the reg field. Swapping them is
 *     an off-by-eight in which register is touched: still executes, still
 *     plausible, corrupts a different register than intended.
 *
 * None of these are caught by reading the bytes back and agreeing with
 * yourself, which is why the test decodes this module's output with Zydis --
 * the same decoder the guest side uses -- and compares against the intent.
 */
#ifndef X86PORT_EMIT_X64_H
#define X86PORT_EMIT_X64_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Host registers, numbered as the encoding numbers them. The low three bits go
 * in ModRM; the fourth bit goes in REX. Do not reorder.
 */
typedef enum X86pHostReg {
  kX64Rax = 0,
  kX64Rcx = 1,
  kX64Rdx = 2,
  kX64Rbx = 3,
  kX64Rsp = 4,
  kX64Rbp = 5,
  kX64Rsi = 6,
  kX64Rdi = 7,
  kX64R8 = 8,
  kX64R9 = 9,
  kX64R10 = 10,
  kX64R11 = 11,
  kX64R12 = 12,
  kX64R13 = 13,
  kX64R14 = 14,
  kX64R15 = 15,
  kX64RegCount /* MUST stay last */
} X86pHostReg;

/*
 * The host ALU operations, by their /r opcode-extension number, which is also
 * the order the one-byte opcodes run in. The number IS the encoding, so this
 * cannot drift from the manual.
 */
typedef enum X86pHostAlu {
  kX64Add = 0,
  kX64Or = 1,
  kX64Adc = 2,
  kX64Sbb = 3,
  kX64And = 4,
  kX64Sub = 5,
  kX64Xor = 6,
  kX64Cmp = 7,
  kX64AluCount /* MUST stay last */
} X86pHostAlu;

/*
 * The output buffer.
 *
 * `overflow` is sticky: once set it stays set, so a caller that emits fifty
 * instructions and checks once at the end cannot miss a failure in the middle.
 */
typedef struct X86pEmit {
  uint8_t *buf;
  size_t cap;
  size_t len;
  int overflow;
  unsigned sites_made;  /* forward jumps created */
  unsigned sites_bound; /* ... and given a destination */
} X86pEmit;

void x86p_emit_init(X86pEmit *e, void *buf, size_t cap);

/* Did every emit fit? Ask this before publishing code. A block that overflowed
   must be DISCARDED, not truncated and run. */
int x86p_emit_ok(const X86pEmit *e);

/* ---- moves ------------------------------------------------------------- */

/* mov r32, imm32 */
void x86p_emit_mov_r32_imm32(X86pEmit *e, X86pHostReg dst, uint32_t imm);

/* mov r64, r64 */
void x86p_emit_mov_r64_r64(X86pEmit *e, X86pHostReg dst, X86pHostReg src);

/* mov r32, r32 */
void x86p_emit_mov_r32_r32(X86pEmit *e, X86pHostReg dst, X86pHostReg src);

/* mov r32, [base + disp] */
void x86p_emit_load32(X86pEmit *e, X86pHostReg dst, X86pHostReg base, int32_t disp);

/* mov [base + disp], r32 */
void x86p_emit_store32(X86pEmit *e, X86pHostReg base, int32_t disp, X86pHostReg src);

/* mov [base + disp], r64 */
void x86p_emit_store64(X86pEmit *e, X86pHostReg base, int32_t disp, X86pHostReg src);

/* mov r64, imm64 -- how the address of a helper function reaches the code. */
void x86p_emit_mov_r64_imm64(X86pEmit *e, X86pHostReg dst, uint64_t imm);

/* lea r64, [base + disp] -- an ADDRESS inside the guest CPU struct, for a
   helper that takes a pointer to part of it. */
void x86p_emit_lea64(X86pEmit *e, X86pHostReg dst, X86pHostReg base, int32_t disp);

/*
 * lea r64, [rip + disp32] -- the address of something else in THIS buffer.
 *
 * `target_off` is an offset from the start of the buffer, and the
 * displacement is computed from the end of the emitted instruction, which is
 * what RIP means at that point. Taking an offset rather than a displacement
 * puts that -1-instruction-length subtraction in one place instead of at every
 * call site.
 *
 * RIP-relative rather than an absolute immediate BECAUSE OF W^X: on a host
 * that maps the code arena twice, the address the translator writes to and the
 * address the block runs at are different, and only the caller knows both. A
 * baked absolute pointer would be the write address, correct on Linux and
 * wrong on Android. A RIP-relative reference resolves against wherever the
 * block actually runs, so it is right in both.
 */
void x86p_emit_lea_rip(X86pEmit *e, X86pHostReg dst, size_t target_off);

/*
 * Raw bytes, for DATA placed in the instruction stream. The caller is
 * responsible for jumping over them: bytes emitted here are not instructions
 * and executing them is undefined.
 */
void x86p_emit_data(X86pEmit *e, const void *p, size_t n);

/* Pad with single-byte NOPs until the buffer length is a multiple of `align`.
   Data emitted after it is then correctly aligned for its type. */
void x86p_emit_align(X86pEmit *e, size_t align);

/* Where the next byte will land, as an offset from the start of the buffer. */
size_t x86p_emit_here(const X86pEmit *e);

/* mov dword [base + disp], imm32 */
void x86p_emit_store32_imm(X86pEmit *e, X86pHostReg base, int32_t disp, uint32_t imm);

/* mov byte [base + disp], imm8 -- for the one-byte fields of X86pFlags. */
void x86p_emit_store8_imm(X86pEmit *e, X86pHostReg base, int32_t disp, uint8_t imm);

/* ---- arithmetic -------------------------------------------------------- */

/* <alu> r32, r32 */
void x86p_emit_alu_r32_r32(X86pEmit *e, X86pHostAlu op, X86pHostReg dst, X86pHostReg src);

/* <alu> r32, imm32 */
void x86p_emit_alu_r32_imm32(X86pEmit *e, X86pHostAlu op, X86pHostReg dst, uint32_t imm);

/* <alu> r64, r64 -- 64-bit, for host pointer arithmetic. Guest values are
   never 64-bit; this is only ever used on addresses. */
void x86p_emit_alu_r64_r64(X86pEmit *e, X86pHostAlu op, X86pHostReg dst, X86pHostReg src);

/* shl r32, imm8 -- the scale of a guest index operand. */
void x86p_emit_shl_r32_imm8(X86pEmit *e, X86pHostReg dst, uint8_t count);

/* sar r32, imm8 -- arithmetic (sign-propagating) right shift. Pairs with shl
   to synthesise MOVSX, and fills the sign word for CDQ/CWDE. */
void x86p_emit_sar_r32_imm8(X86pEmit *e, X86pHostReg dst, uint8_t count);

/* test r32, r32 -- sets flags, writes no result. The idiom for "is this
   register zero", which is how a helper's int return is branched on. */
void x86p_emit_test_r32_r32(X86pEmit *e, X86pHostReg a, X86pHostReg b);

/*
 * cmovcc r32, r32 -- conditional move, using the HOST's condition codes.
 *
 * This is how a guest conditional branch is emitted without a forward jump and
 * therefore without any label patching: compute both candidate addresses, then
 * select. A patched jump needs a fixup list, and a fixup list that is not
 * applied leaves a branch pointing at whatever followed it -- a whole class of
 * bug this avoids rather than manages. `cc` is the host encoding's condition
 * number, which is the same numbering X86pCond uses.
 */
void x86p_emit_cmovcc_r32_r32(X86pEmit *e, unsigned cc, X86pHostReg dst, X86pHostReg src);

/* <alu> r32, [base + disp] */
void x86p_emit_alu_r32_mem(X86pEmit *e, X86pHostAlu op, X86pHostReg dst, X86pHostReg base, int32_t disp);

/*
 * setcc r8 -- materialise a condition as 0 or 1.
 *
 * RESTRICTED TO RAX..RBX by contract, and the restriction is real rather than
 * cautious: without REX, byte-register numbers 4-7 mean AH, CH, DH, BH rather
 * than SPL, BPL, SIL, DIL. Emitting this for RSP would write AH and look
 * entirely plausible in a disassembly. Callers use AL or CL.
 */
void x86p_emit_setcc_r8(X86pEmit *e, unsigned cc, X86pHostReg dst);

/* mov byte [base + disp], r8 -- same RAX..RBX restriction as setcc. */
void x86p_emit_store8_reg(X86pEmit *e, X86pHostReg base, int32_t disp, X86pHostReg src);

/* movzx r32, byte [base + disp] -- a one-byte guest field widened without
   carrying whatever happened to be in the upper bits of the destination. */
void x86p_emit_load8_zx(X86pEmit *e, X86pHostReg dst, X86pHostReg base, int32_t disp);

/*
 * The 16-bit forms, for guest operands of width 2.
 *
 * All three carry the 0x66 operand-size prefix, which must be emitted BEFORE
 * any REX byte: REX has to be the last prefix before the opcode, and a 0x66
 * after it is not a prefix at all. Both orders assemble.
 */
void x86p_emit_load16_zx(X86pEmit *e, X86pHostReg dst, X86pHostReg base, int32_t disp);
void x86p_emit_store16_reg(X86pEmit *e, X86pHostReg base, int32_t disp, X86pHostReg src);
void x86p_emit_store16_imm(X86pEmit *e, X86pHostReg base, int32_t disp, uint16_t imm);

/* <alu> r64, imm8 (sign-extended) -- REX.W 83 /op ib. Only ever an address or
   stack-pointer adjustment; guest values are never 64-bit. */
void x86p_emit_alu_r64_imm8(X86pEmit *e, X86pHostAlu op, X86pHostReg dst, int8_t imm);

/* or word [base + disp], imm16 -- set bits in a 16-bit memory field in place,
   for the x87 status word's stack-fault flags. */
void x86p_emit_or_m16_imm16(X86pEmit *e, X86pHostReg base, int32_t disp, uint16_t imm);

/*
 * x87 instructions, the two shapes the JIT emits them in.
 *
 * `x87_m` is an escape opcode (D8..DF) plus a memory operand whose ModRM.reg
 * field carries the opcode-extension digit -- FLD m32 is D9 /0, FSTP m80 is
 * DB /7. `x87_reg` is a fixed opcode and a fixed second byte for the
 * register forms (FLD ST(i) is D9, C0+i). The caller holds the encoding from
 * the manual; this only lays the bytes down with the right REX for the base.
 */
void x86p_emit_x87_m(X86pEmit *e, uint8_t opcode, unsigned digit, X86pHostReg base, int32_t disp);
void x86p_emit_x87_reg(X86pEmit *e, uint8_t opcode, uint8_t modrm);

/*
 * FORWARD JUMPS, with the destination filled in later.
 *
 * CMOVcc covers selecting between two values, which is why conditional
 * BRANCHES needed no jumps. It cannot cover a guest memory access: a bounds
 * check that fails must not perform the load, and a conditional move performs
 * both sides. So this is the first place a real jump is unavoidable.
 *
 * A site is returned by value and bound by x86p_emit_bind, which computes the
 * displacement from where the jump ends to where the label landed. An UNBOUND
 * site is the failure this interface is shaped to prevent: the emitted jump
 * would carry whatever displacement was left in the buffer, which is a branch
 * into the middle of an unrelated instruction. x86p_emit_sites_bound() reports
 * whether every site created has been bound, and the translator asks before
 * publishing.
 */
typedef struct X86pEmitSite {
  size_t at;  /* offset of the 4-byte displacement */
  size_t end; /* offset just past the jump, which the displacement is relative to */
} X86pEmitSite;

/* jcc rel32, destination unbound. */
X86pEmitSite x86p_emit_jcc_rel32(X86pEmit *e, unsigned cc);

/* jmp rel32, destination unbound. */
X86pEmitSite x86p_emit_jmp_rel32(X86pEmit *e);

/* Point a site at the current end of the buffer. */
void x86p_emit_bind(X86pEmit *e, X86pEmitSite site);

/* Were all sites bound? Ask before publishing; an unbound jump is a branch to
   an arbitrary offset. */
int x86p_emit_sites_bound(const X86pEmit *e);

/* ---- structure --------------------------------------------------------- */

void x86p_emit_push_r64(X86pEmit *e, X86pHostReg r);
void x86p_emit_pop_r64(X86pEmit *e, X86pHostReg r);
void x86p_emit_ret(X86pEmit *e);

/* call r64. Indirect through a register because a direct CALL is a 32-bit
   relative displacement, and translated code is not guaranteed to land within
   2 GB of the helper it calls -- a limit that holds on a developer's machine
   and fails once the code arena moves. */
void x86p_emit_call_r64(X86pEmit *e, X86pHostReg target);

/* A single-byte raw opcode, for the handful of forms with no operands. */
void x86p_emit_byte(X86pEmit *e, uint8_t b);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_EMIT_X64_H */
