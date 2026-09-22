/*
 * jit_x64_x87_inline.c -- see jit_x64_x87_inline.h.
 *
 * Register roles inside one inline sequence. Nothing here survives the guest
 * instruction, and no call is made, so every caller-saved register is free
 * except HOSTPTR_REG while a prepared memory operand is still to be read:
 *
 *   RAX  physical index
 *   RCX  tag byte / operand bits under test
 *   RDX  destination register slot:  [RDX + reg0_off()] is ST(dst)
 *   RSI  destination tag slot:       [RSI + tag0_off()] is its tag
 *   RDI  source register slot
 *
 * A tag byte records only whether a register is occupied (x87.h), so a write
 * stores kTagValid and never classifies the value it wrote.
 *
 * THE HOST x87 STACK is empty on entry -- the ABI requires it at every call
 * boundary and these sequences are only ever between them -- and every
 * sequence leaves it empty again: each value it loads is stored with a pop.
 */
#include "jit_x64_x87_inline.h"

#include "cpu.h"
#include "emit_x64.h"
#include "x87.h"

#include <stddef.h>
#include <stdint.h>

#if (defined(__x86_64__) || defined(_M_X64)) && X86P_EXACT_LONG_DOUBLE
/* The host executes x87 itself and the register file holds its ten-byte
   objects at a sixteen-byte stride, which the index scaling below assumes. */
#define X87_INLINE_HOST 1
_Static_assert(sizeof(X86pX87Reg) == 16, "x87 register slots are addressed as index << 4");
#else
#define X87_INLINE_HOST 0
#endif

#if X87_INLINE_HOST

#define kCcE 0x4u
#define kCcNe 0x5u
#define kCcP 0xAu
#define kTagValid ((unsigned)kX86pX87TagValid)
#define kTagEmpty ((unsigned)kX86pX87TagEmpty)

static int32_t x87_field(size_t field) {
  return (int32_t)(offsetof(X86pCpu, x87) + field);
}

static int32_t reg0_off(void) {
  return x87_field(offsetof(X86pX87, reg));
}

static int32_t tag0_off(void) {
  return x87_field(offsetof(X86pX87, tag));
}

static int32_t top_off(void) {
  return x87_field(offsetof(X86pX87, top));
}

static int32_t control_off(void) {
  return x87_field(offsetof(X86pX87, control));
}

static int32_t census_off(void) {
  return x87_field(offsetof(X86pX87, op_census));
}

static void note_slow(X86pEmit *e, X87Inline *fast, unsigned cc) {
  if (fast->nslow >= X87_INLINE_MAX_SLOW) {
    /* A sequence with more guards than this table holds would leave one
       unbound; refuse the whole block rather than emit it. */
    e->overflow = 1;
    return;
  }
  fast->slow[fast->nslow++] = x86p_emit_jcc_rel32(e, cc);
}

/* eax = (TOP + delta) & 7 -- the physical register ST(delta) names. */
static void emit_phys(X86pEmit *e, unsigned delta) {
  x86p_emit_load8_zx(e, kX64Rax, CPU_REG, top_off());
  if (delta & 7u) {
    x86p_emit_alu_r32_imm32(e, kX64Add, kX64Rax, delta & 7u);
    x86p_emit_alu_r32_imm32(e, kX64And, kX64Rax, 7u);
  }
}

/* tag = CPU + eax: [tag + tag0_off()] is that register's tag byte. */
static void emit_tag_slot(X86pEmit *e, X86pHostReg tag) {
  x86p_emit_mov_r32_r32(e, tag, kX64Rax);
  x86p_emit_alu_r64_r64(e, kX64Add, tag, CPU_REG);
}

/* slot = CPU + eax * 16: [slot + reg0_off()] is that register's value. */
static void emit_reg_slot(X86pEmit *e, X86pHostReg slot) {
  x86p_emit_mov_r32_r32(e, slot, kX64Rax);
  x86p_emit_shl_r32_imm8(e, slot, 4u);
  x86p_emit_alu_r64_r64(e, kX64Add, slot, CPU_REG);
}

/* Jump to the slow path when the tag at [tag] is `tag_value`. */
static void guard_tag_is(X86pEmit *e, X87Inline *fast, X86pHostReg tag, unsigned tag_value) {
  x86p_emit_load8_zx(e, kX64Rcx, tag, tag0_off());
  x86p_emit_alu_r32_imm32(e, kX64Cmp, kX64Rcx, tag_value);
  note_slow(e, fast, kCcE);
}

/* Jump to the slow path when the tag at [tag] is anything BUT `tag_value`. */
static void guard_tag_is_not(X86pEmit *e, X87Inline *fast, X86pHostReg tag, unsigned tag_value) {
  x86p_emit_load8_zx(e, kX64Rcx, tag, tag0_off());
  x86p_emit_alu_r32_imm32(e, kX64Cmp, kX64Rcx, tag_value);
  note_slow(e, fast, kCcNe);
}

/*
 * The helpers execute the guest's operation on the host FPU without touching
 * its control word only when the two already agree; otherwise they load the
 * guest's around the instruction. The inline path takes the first case only.
 * The host word is read, never assumed: host code can change it.
 *
 * FNSTCW needs memory. A 16-byte slot is opened for it and closed before any
 * branch, so every guard leaves with the block's own RSP -- the fault stub and
 * the helper sequence both depend on that.
 */
static void guard_host_control(X86pEmit *e, X87Inline *fast) {
  const int32_t slot = X86P_JIT_HOST_CALL_FRAME_BYTES;
  x86p_emit_alu_r64_imm8(e, kX64Sub, kX64Rsp, 16);
  x86p_emit_x87_m(e, 0xD9u, 7u, kX64Rsp, slot); /* fnstcw */
  x86p_emit_load16_zx(e, kX64Rax, kX64Rsp, slot);
  x86p_emit_alu_r64_imm8(e, kX64Add, kX64Rsp, 16);
  x86p_emit_load16_zx(e, kX64Rcx, CPU_REG, control_off());
  x86p_emit_alu_r32_r32(e, kX64Cmp, kX64Rax, kX64Rcx);
  note_slow(e, fast, kCcNe);
}

/* The op census counts inside x86p_x87_arith_raw; while one is armed every
   operation goes there, or the instrument would silently under-report. */
static void guard_census_disarmed(X86pEmit *e, X87Inline *fast) {
  x86p_emit_load32(e, kX64Rax, CPU_REG, census_off());
  x86p_emit_alu_r32_mem(e, kX64Or, kX64Rax, CPU_REG, census_off() + 4);
  note_slow(e, fast, kCcNe);
}

/* The register whose tag byte is at [tag] is now occupied. */
static void emit_occupied(X86pEmit *e, X86pHostReg tag) {
  x86p_emit_store8_imm(e, tag, tag0_off(), (uint8_t)kTagValid);
}

/* One pop: ST(0) becomes empty and TOP moves up. Emitted only where ST(0) is
   known to hold a value, which is x86p_x87_pop's own fault-free case. */
static void emit_pop(X86pEmit *e) {
  emit_phys(e, 0u);
  emit_tag_slot(e, kX64Rcx);
  x86p_emit_store8_imm(e, kX64Rcx, tag0_off(), (uint8_t)kTagEmpty);
  x86p_emit_alu_r32_imm32(e, kX64Add, kX64Rax, 1u);
  x86p_emit_alu_r32_imm32(e, kX64And, kX64Rax, 7u);
  x86p_emit_store8_reg(e, CPU_REG, top_off(), kX64Rax);
}

static void emit_pops(X86pEmit *e, unsigned pops) {
  while (pops--) {
    emit_pop(e);
  }
}

static void fld_ext80(X86pEmit *e, X86pHostReg slot) {
  x86p_emit_x87_m(e, 0xDBu, 5u, slot, reg0_off());
}

static void fstp_ext80(X86pEmit *e, X86pHostReg slot) {
  x86p_emit_x87_m(e, 0xDBu, 7u, slot, reg0_off());
}

/* FLD / FILD of a prepared guest operand. Every width here widens exactly. */
static void fld_guest(X86pEmit *e, int w, int integer) {
  if (integer) {
    x86p_emit_x87_m(e, w == 4 ? 0xDBu : 0xDFu, w == 8 ? 5u : 0u, HOSTPTR_REG, 0);
  } else {
    x86p_emit_x87_m(e, w == 4 ? 0xD9u : 0xDDu, 0u, HOSTPTR_REG, 0);
  }
}

/* ST(0) = ST(0) op ST(1) on the host stack; the ModRM of `fop st(0), st(1)`. */
static uint8_t host_arith_modrm(X86pX87Op op) {
  switch (op) {
  case kX86pX87Add:
    return 0xC1u;
  case kX86pX87Mul:
    return 0xC9u;
  case kX86pX87Sub:
    return 0xE1u;
  case kX86pX87Div:
  default:
    return 0xF1u;
  }
}

/* Compute into the host ST(0) from two operands already loaded as x (ST(0))
   and y (ST(1)), store over ST(dst) at [RDX] and drop y. ST(dst) was guarded
   occupied, so its tag already says so. */
static void finish_arith(X86pEmit *e, X86pX87Op op) {
  x86p_emit_x87_reg(e, 0xD8u, host_arith_modrm(op));
  fstp_ext80(e, kX64Rdx);
  x86p_emit_x87_reg(e, 0xDDu, 0xD8u); /* fstp st(0) */
}

/*
 * Jump to the slow path when the register divisor at [slot] is a zero of
 * either sign -- the helper's `y == 0.0L` -- asked of the host FPU. FUCOMIP
 * writes EFLAGS directly: equal is ZF with PF clear, and unordered (a NaN)
 * sets PF as well. The divisor is dropped again before the branch, so the
 * host stack is empty on both paths, and FSTP leaves EFLAGS alone.
 */
static void guard_register_nonzero(X86pEmit *e, X87Inline *fast, X86pHostReg slot) {
  X86pEmitSite unordered;
  fld_ext80(e, slot);
  x86p_emit_x87_reg(e, 0xD9u, 0xEEu); /* fldz */
  x86p_emit_x87_reg(e, 0xDFu, 0xE9u); /* fucomip st(0), st(1) */
  x86p_emit_x87_reg(e, 0xDDu, 0xD8u); /* fstp st(0) */
  unordered = x86p_emit_jcc_rel32(e, kCcP);
  note_slow(e, fast, kCcE);
  x86p_emit_bind(e, unordered);
}

/*
 * Jump to the slow path when a prepared memory divisor is a zero of either
 * sign, asked of its bits: integer zero, or a float whose exponent and
 * significand are both zero. That is exactly the condition the helper's
 * `y == 0.0L` names after the exact widening.
 */
static void guard_memory_nonzero(X86pEmit *e, X87Inline *fast, int w, int integer) {
  if (integer) {
    if (w == 2) {
      x86p_emit_load16_zx(e, kX64Rcx, HOSTPTR_REG, 0);
    } else {
      x86p_emit_load32(e, kX64Rcx, HOSTPTR_REG, 0);
    }
    x86p_emit_test_r32_r32(e, kX64Rcx, kX64Rcx);
  } else if (w == 4) {
    x86p_emit_load32(e, kX64Rcx, HOSTPTR_REG, 0);
    x86p_emit_alu_r32_imm32(e, kX64And, kX64Rcx, 0x7FFFFFFFu);
  } else {
    x86p_emit_load32(e, kX64Rcx, HOSTPTR_REG, 4);
    x86p_emit_alu_r32_imm32(e, kX64And, kX64Rcx, 0x7FFFFFFFu);
    x86p_emit_alu_r32_mem(e, kX64Or, kX64Rcx, HOSTPTR_REG, 0);
  }
  note_slow(e, fast, kCcE);
}

static void finish_fast(X86pEmit *e, X87Inline *fast) {
  fast->done = x86p_emit_jmp_rel32(e);
  fast->emitted = 1;
}

#endif /* X87_INLINE_HOST */

void x87_inline_begin_slow(X86pEmit *e, X87Inline *fast) {
  unsigned i;
  for (i = 0; fast->emitted && i < fast->nslow; i++) {
    x86p_emit_bind(e, fast->slow[i]);
  }
}

void x87_inline_end(X86pEmit *e, X87Inline *fast) {
  if (fast->emitted) {
    x86p_emit_bind(e, fast->done);
  }
}

void x87_inline_load(BlockCtx *c, const X86pInsn *insn, X87Inline *fast) {
  fast->emitted = 0;
  fast->nslow = 0;
#if X87_INLINE_HOST
  {
    X86pEmit *e = c->e;
    const X86pOperand *o = &insn->operand[0];
    if (o->kind != kX86pOperandMem) {
      /* FLD ST(i): the source is read BEFORE the push renumbers the stack. */
      emit_phys(e, (unsigned)o->reg);
      emit_tag_slot(e, kX64Rsi);
      guard_tag_is(e, fast, kX64Rsi, kTagEmpty);
      emit_reg_slot(e, kX64Rdi);
    }
    emit_phys(e, 7u); /* the slot a push fills: TOP - 1 */
    emit_tag_slot(e, kX64Rsi);
    guard_tag_is_not(e, fast, kX64Rsi, kTagEmpty); /* full: the helper reports the overflow */
    emit_reg_slot(e, kX64Rdx);
    x86p_emit_store8_reg(e, CPU_REG, top_off(), kX64Rax);
    if (o->kind == kX86pOperandMem) {
      fld_guest(e, o->size, insn->x87 == kX86pX87InsnLoadInt);
    } else {
      fld_ext80(e, kX64Rdi);
    }
    fstp_ext80(e, kX64Rdx);
    emit_occupied(e, kX64Rsi);
    finish_fast(e, fast);
  }
#else
  (void)c;
  (void)insn;
#endif
}

void x87_inline_arith(BlockCtx *c, const X86pInsn *insn, X87Inline *fast) {
  fast->emitted = 0;
  fast->nslow = 0;
#if X87_INLINE_HOST
  {
    X86pEmit *e = c->e;
    const X86pOperand *o0 = &insn->operand[0];
    const int two_op = insn->operands == 2;
    const unsigned dst = two_op ? (unsigned)o0->reg : 0u;
    const X86pX87Op op = (X86pX87Op)insn->x87_op;
    const int reverse = insn->x87_reverse != 0;
    const int divide = op == kX86pX87Div;
    const unsigned pops = insn->x87_pops;

    if (o0->kind != kX86pOperandMem) {
      const unsigned src = two_op ? (unsigned)insn->operand[1].reg : (unsigned)o0->reg;
      /* A pop needs ST(0) full after the write, which is certain only when
         ST(0) was one of the two operands. Every encoded P form is. */
      if (pops && src != 0u && dst != 0u) {
        return;
      }
      guard_host_control(e, fast);
      guard_census_disarmed(e, fast);
      emit_phys(e, src);
      emit_tag_slot(e, kX64Rsi);
      guard_tag_is(e, fast, kX64Rsi, kTagEmpty);
      emit_reg_slot(e, kX64Rdi);
      emit_phys(e, dst);
      emit_tag_slot(e, kX64Rsi);
      guard_tag_is(e, fast, kX64Rsi, kTagEmpty);
      emit_reg_slot(e, kX64Rdx);
      if (divide) {
        guard_register_nonzero(e, fast, reverse ? kX64Rdx : kX64Rdi);
      }
      /* x = reverse ? src : ST(dst), y = the other; x on top. */
      fld_ext80(e, reverse ? kX64Rdx : kX64Rdi);
      fld_ext80(e, reverse ? kX64Rdi : kX64Rdx);
      finish_arith(e, op);
      emit_pops(e, pops);
      finish_fast(e, fast);
      return;
    }

    guard_host_control(e, fast);
    guard_census_disarmed(e, fast);
    emit_phys(e, 0u);
    emit_tag_slot(e, kX64Rsi);
    guard_tag_is(e, fast, kX64Rsi, kTagEmpty);
    emit_reg_slot(e, kX64Rdx);
    if (divide) {
      if (reverse) {
        guard_register_nonzero(e, fast, kX64Rdx);
      } else {
        guard_memory_nonzero(e, fast, o0->size, insn->x87_mem_int);
      }
    }
    if (reverse) {
      fld_ext80(e, kX64Rdx);
      fld_guest(e, o0->size, insn->x87_mem_int);
    } else {
      fld_guest(e, o0->size, insn->x87_mem_int);
      fld_ext80(e, kX64Rdx);
    }
    finish_arith(e, op);
    emit_pops(e, pops);
    finish_fast(e, fast);
  }
#else
  (void)c;
  (void)insn;
#endif
}

void x87_inline_store_reg(BlockCtx *c, const X86pInsn *insn, X87Inline *fast) {
  fast->emitted = 0;
  fast->nslow = 0;
#if X87_INLINE_HOST
  {
    X86pEmit *e = c->e;
    emit_phys(e, 0u);
    emit_tag_slot(e, kX64Rsi);
    guard_tag_is(e, fast, kX64Rsi, kTagEmpty);
    emit_reg_slot(e, kX64Rdi);
    emit_phys(e, (unsigned)insn->operand[0].reg);
    emit_tag_slot(e, kX64Rsi);
    emit_reg_slot(e, kX64Rdx);
    fld_ext80(e, kX64Rdi);
    fstp_ext80(e, kX64Rdx);
    emit_occupied(e, kX64Rsi);
    emit_pops(e, insn->x87_pops);
    finish_fast(e, fast);
  }
#else
  (void)c;
  (void)insn;
#endif
}

void x87_inline_store_mem(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip, X87Inline *fast) {
  fast->emitted = 0;
  fast->nslow = 0;
#if X87_INLINE_HOST
  {
    X86pEmit *e = c->e;
    const X86pOperand *o0 = &insn->operand[0];
    const int w = o0->size;
    /* The integer stores keep the helper: their overflow answer is the
       integer indefinite with IE raised, which is not one host instruction. */
    if (insn->x87 != kX86pX87InsnStore || (w != 4 && w != 8)) {
      return;
    }
    guard_host_control(e, fast);
    emit_phys(e, 0u);
    emit_tag_slot(e, kX64Rsi);
    guard_tag_is(e, fast, kX64Rsi, kTagEmpty);
    emit_reg_slot(e, kX64Rdx);
    /* Only now may the access fault: ST(0) holds a value to store. The address
       path uses RAX, RDI, R10 and R11, so RDX survives it. */
    emit_mem_prepare_w(c, o0, insn_eip, w);
    fld_ext80(e, kX64Rdx);
    x86p_emit_x87_m(e, w == 4 ? 0xD9u : 0xDDu, 3u, HOSTPTR_REG, 0); /* fstp m32/m64 */
    emit_pops(e, insn->x87_pops);
    finish_fast(e, fast);
  }
#else
  (void)c;
  (void)insn;
  (void)insn_eip;
#endif
}
