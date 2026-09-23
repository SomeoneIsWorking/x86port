/*
 * jit_x64_x87_inline.c -- see jit_x64_x87_inline.h.
 *
 * Register roles inside one inline sequence. Nothing here survives the guest
 * instruction, and no call is made, so every caller-saved register is free
 * except HOSTPTR_REG while a prepared memory operand is still to be read:
 *
 *   RAX  physical index
 *   RCX  tag byte / operand bits under test / a slot the mirror reloads
 *   RDX  destination register slot:  [RDX + reg0_off()] is ST(dst)
 *   RSI  destination tag slot:       [RSI + tag0_off()] is its tag
 *   RDI  source register slot, for a source the mirror does not hold
 *
 * A tag byte records only whether a register is occupied (x87.h), so a write
 * stores kTagValid and never classifies the value it wrote.
 *
 * THE HOST x87 STACK holds the mirror (jit_x64_x87_inline.h) and nothing else,
 * at most X87_MIRROR_MAX deep: one host register always stays free, because a
 * write-through duplicates the value it stores and the divisor guard pushes a
 * zero. Every guard is emitted before the sequence changes the host stack or
 * any guest state, so a slow path starts from the guest state the helper
 * expects.
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
#define X87_MIRROR_MAX 7u

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
 *
 * The host word is the one this block was translated under, as an immediate:
 * x86p_jit_enter and the engine's run refuse the block under any other, and no
 * host call can change it within a run (jit_x64.h, x86p_jit_host_state).
 * Reading it here instead cost an FNSTCW and a reload through the stack at
 * every x87 operation, about 9% of translated-code samples on the Dead Zone
 * route.
 */
static void guard_host_control(BlockCtx *c, X87Inline *fast) {
  x86p_emit_load16_zx(c->e, kX64Rcx, CPU_REG, control_off());
  x86p_emit_alu_r32_imm32(c->e, kX64Cmp, kX64Rcx, c->host_state);
  note_slow(c->e, fast, kCcNe);
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
   known to hold a value, which is x86p_x87_pop's own fault-free case. The host
   side of the pop is the caller's. */
static void emit_pop(X86pEmit *e) {
  emit_phys(e, 0u);
  emit_tag_slot(e, kX64Rcx);
  x86p_emit_store8_imm(e, kX64Rcx, tag0_off(), (uint8_t)kTagEmpty);
  x86p_emit_alu_r32_imm32(e, kX64Add, kX64Rax, 1u);
  x86p_emit_alu_r32_imm32(e, kX64And, kX64Rax, 7u);
  x86p_emit_store8_reg(e, CPU_REG, top_off(), kX64Rax);
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

static void host_fld_st(X86pEmit *e, unsigned i) {
  x86p_emit_x87_reg(e, 0xD9u, (uint8_t)(0xC0u + i));
}

static void host_pop(X86pEmit *e) {
  x86p_emit_x87_reg(e, 0xDDu, 0xD8u); /* fstp st(0) */
}

/* Store host ST(i) into the guest register at [slot]; the host stack is left
   as it was. x87 has no non-popping ten-byte store. */
static void write_through(X86pEmit *e, unsigned host_index, X86pHostReg slot) {
  host_fld_st(e, host_index);
  fstp_ext80(e, slot);
}

/*
 * Host ST(0..depth-1) = guest ST(0..depth-1), read from the register file into
 * an empty host stack. The loads are the block's shared loader
 * (x87_cache_emit_loader), called with TOP in EAX: a reload inline was about
 * 25 bytes per register at every rebuild, which put a single x87 instruction
 * past twice the per-instruction code budget.
 */
static void mirror_load(BlockCtx *c, unsigned depth) {
  c->x87_depth = depth;
  if (depth == 0u) {
    return;
  }
  if (c->nx87_loads >= sizeof c->x87_loads / sizeof c->x87_loads[0]) {
    c->e->overflow = 1; /* an unbound call would jump anywhere; refuse the block */
    return;
  }
  x86p_emit_load8_zx(c->e, kX64Rax, CPU_REG, top_off());
  c->x87_load_depth[c->nx87_loads] = (uint8_t)depth;
  c->x87_loads[c->nx87_loads++] = x86p_emit_call_rel32(c->e);
}

/* Guest ST(0..depth-1) mirrored. The host stack can only grow at its top, so
   a deeper mirror is rebuilt from the register file, which is complete. */
static void mirror_ensure(BlockCtx *c, unsigned depth) {
  if (c->x87_depth >= depth) {
    return;
  }
  x87_cache_flush(c);
  mirror_load(c, depth);
}

/* Room for one push: a full mirror lets go of its deepest value. FFREE leaves
   that host register empty, which is where the host's own push lands. */
static void mirror_make_room(BlockCtx *c) {
  if (c->x87_depth == X87_MIRROR_MAX) {
    x86p_emit_x87_reg(c->e, 0xDDu, (uint8_t)(0xC0u + X87_MIRROR_MAX - 1u)); /* ffree st(6) */
    c->x87_depth--;
  }
}

/*
 * The ModRM.reg field of a host arithmetic form. `fop st(0), src` (D8, and the
 * memory forms) names SUB/SUBR and DIV/DIVR as ST(0) - src and src - ST(0);
 * `fop st(i), st(0)` (DC, DE) swaps the two encodings, so its field for those
 * operations is the other one of the pair.
 */
static unsigned st0_field(X86pX87Op op, int reverse) {
  switch (op) {
  case kX86pX87Add:
    return 0u;
  case kX86pX87Mul:
    return 1u;
  case kX86pX87Sub:
    return reverse ? 5u : 4u;
  case kX86pX87Div:
  default:
    return reverse ? 7u : 6u;
  }
}

static unsigned sti_field(X86pX87Op op, int reverse) {
  const unsigned field = st0_field(op, reverse);
  return field >= 4u ? field ^ 1u : field;
}

/*
 * Jump to the slow path when mirrored ST(i) is a zero of either sign -- the
 * helper's `y == 0.0L` -- asked of the host FPU. FUCOMIP writes EFLAGS
 * directly and pops the zero again: equal is ZF with PF clear, and unordered
 * (a NaN) sets PF as well.
 */
static void guard_mirrored_nonzero(X86pEmit *e, X87Inline *fast, unsigned i) {
  X86pEmitSite unordered;
  x86p_emit_x87_reg(e, 0xD9u, 0xEEu);                     /* fldz */
  x86p_emit_x87_reg(e, 0xDFu, (uint8_t)(0xE8u + i + 1u)); /* fucomip st(0), st(i+1) */
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

static void finish_fast(BlockCtx *c, X87Inline *fast) {
  fast->done = x86p_emit_jmp_rel32(c->e);
  fast->depth = c->x87_depth;
  fast->emitted = 1;
}

#endif /* X87_INLINE_HOST */

uint32_t x87_inline_host_control(void) {
#if X87_INLINE_HOST
  uint16_t control = 0u;
  __asm__ volatile("fnstcw %0" : "=m"(control));
  return control;
#else
  return 0u;
#endif
}

static void begin(X87Inline *fast) {
  fast->emitted = 0;
  fast->nslow = 0;
  fast->depth = 0;
}

void x87_cache_flush(BlockCtx *c) {
#if X87_INLINE_HOST
  while (c->x87_depth) {
    host_pop(c->e);
    c->x87_depth--;
  }
#else
  (void)c;
#endif
}

void x87_cache_discard(X86pEmit *e) {
#if X87_INLINE_HOST
  x86p_emit_byte(e, 0x0Fu); /* emms: every host x87 register empty */
  x86p_emit_byte(e, 0x77u);
#else
  (void)e;
#endif
}

/*
 * One entry per depth, deepest first, each loading one register and falling
 * into the next: entry k loads ST(k-1) and then everything above it, so the
 * last value loaded is ST(0). EAX holds TOP; RCX is the only other register
 * touched, and the caller's RDX, RSI, RDI and HOSTPTR_REG survive.
 */
void x87_cache_emit_loader(BlockCtx *c) {
#if X87_INLINE_HOST
  size_t entry[X87_MIRROR_MAX + 1u];
  unsigned deepest = 0u;
  unsigned i;
  unsigned k;
  for (i = 0; i < c->nx87_loads; i++) {
    deepest = c->x87_load_depth[i] > deepest ? c->x87_load_depth[i] : deepest;
  }
  for (k = deepest; k > 0u; k--) {
    entry[k] = x86p_emit_here(c->e);
    x86p_emit_mov_r32_r32(c->e, kX64Rcx, kX64Rax);
    if (k > 1u) {
      x86p_emit_alu_r32_imm32(c->e, kX64Add, kX64Rcx, k - 1u);
      x86p_emit_alu_r32_imm32(c->e, kX64And, kX64Rcx, 7u);
    }
    x86p_emit_shl_r32_imm8(c->e, kX64Rcx, 4u);
    x86p_emit_alu_r64_r64(c->e, kX64Add, kX64Rcx, CPU_REG);
    fld_ext80(c->e, kX64Rcx);
  }
  if (deepest) {
    x86p_emit_ret(c->e);
  }
  for (i = 0; i < c->nx87_loads; i++) {
    x86p_emit_bind_to(c->e, c->x87_loads[i], entry[c->x87_load_depth[i]]);
  }
#else
  (void)c;
#endif
}

void x87_inline_begin_slow(BlockCtx *c, X87Inline *fast) {
  unsigned i;
  if (!fast->emitted) {
    return;
  }
  for (i = 0; i < fast->nslow; i++) {
    x86p_emit_bind(c->e, fast->slow[i]);
  }
  x87_cache_discard(c->e);
}

void x87_inline_end(BlockCtx *c, X87Inline *fast) {
  if (!fast->emitted) {
    return;
  }
#if X87_INLINE_HOST
  mirror_load(c, fast->depth);
#endif
  x86p_emit_bind(c->e, fast->done);
}

void x87_inline_load(BlockCtx *c, const X86pInsn *insn, X87Inline *fast) {
  begin(fast);
#if X87_INLINE_HOST
  {
    X86pEmit *e = c->e;
    const X86pOperand *o = &insn->operand[0];
    const unsigned src = (unsigned)o->reg;
    if (o->kind != kX86pOperandMem) {
      /* FLD ST(i): the source is read BEFORE the push renumbers the stack. */
      emit_phys(e, src);
      emit_tag_slot(e, kX64Rsi);
      guard_tag_is(e, fast, kX64Rsi, kTagEmpty);
      emit_reg_slot(e, kX64Rdi);
    }
    emit_phys(e, 7u); /* the slot a push fills: TOP - 1 */
    emit_tag_slot(e, kX64Rsi);
    guard_tag_is_not(e, fast, kX64Rsi, kTagEmpty); /* full: the helper reports the overflow */
    emit_reg_slot(e, kX64Rdx);
    x86p_emit_store8_reg(e, CPU_REG, top_off(), kX64Rax);
    mirror_make_room(c);
    if (o->kind == kX86pOperandMem) {
      fld_guest(e, o->size, insn->x87 == kX86pX87InsnLoadInt);
    } else if (src < c->x87_depth) {
      host_fld_st(e, src);
    } else {
      fld_ext80(e, kX64Rdi);
    }
    c->x87_depth++;
    write_through(e, 0u, kX64Rdx);
    emit_occupied(e, kX64Rsi);
    finish_fast(c, fast);
  }
#else
  (void)c;
  (void)insn;
#endif
}

void x87_inline_arith(BlockCtx *c, const X86pInsn *insn, X87Inline *fast) {
  begin(fast);
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
      const unsigned deepest = src > dst ? src : dst;
      /* Every encoded P form is `fop st(i), st(0)` with i > 0: the pop drops
         the source and the host's own P form does the same. Anything else, and
         an operand the mirror cannot reach, keeps the helper. */
      if ((pops && (src != 0u || dst == 0u || pops > 1u)) || deepest >= X87_MIRROR_MAX) {
        x87_cache_flush(c);
        return;
      }
      guard_host_control(c, fast);
      guard_census_disarmed(e, fast);
      emit_phys(e, src);
      emit_tag_slot(e, kX64Rsi);
      guard_tag_is(e, fast, kX64Rsi, kTagEmpty);
      emit_phys(e, dst);
      emit_tag_slot(e, kX64Rsi);
      guard_tag_is(e, fast, kX64Rsi, kTagEmpty);
      emit_reg_slot(e, kX64Rdx);
      mirror_ensure(c, deepest + 1u);
      if (divide) {
        guard_mirrored_nonzero(e, fast, reverse ? dst : src);
      }
      if (dst == 0u) {
        x86p_emit_x87_reg(e, 0xD8u, (uint8_t)(0xC0u | (st0_field(op, reverse) << 3) | src));
        write_through(e, 0u, kX64Rdx);
      } else if (pops) {
        /* fop st(dst), st(0) and pop: the result is host ST(dst - 1) now, and
           guest ST(dst - 1) once the guest pop below renumbers the stack. */
        x86p_emit_x87_reg(e, 0xDEu, (uint8_t)(0xC0u | (sti_field(op, reverse) << 3) | dst));
        c->x87_depth--;
        write_through(e, dst - 1u, kX64Rdx);
        emit_pop(e);
      } else {
        x86p_emit_x87_reg(e, 0xDCu, (uint8_t)(0xC0u | (sti_field(op, reverse) << 3) | dst));
        write_through(e, dst, kX64Rdx);
      }
      finish_fast(c, fast);
      return;
    }

    if (pops) {
      x87_cache_flush(c);
      return;
    }
    guard_host_control(c, fast);
    guard_census_disarmed(e, fast);
    emit_phys(e, 0u);
    emit_tag_slot(e, kX64Rsi);
    guard_tag_is(e, fast, kX64Rsi, kTagEmpty);
    emit_reg_slot(e, kX64Rdx);
    if (divide && !reverse) {
      guard_memory_nonzero(e, fast, o0->size, insn->x87_mem_int);
    }
    mirror_ensure(c, 1u);
    if (divide && reverse) {
      guard_mirrored_nonzero(e, fast, 0u);
    }
    /* fop st(0), m: D8 m32 and DC m64 floats, DA m32 and DE m16 integers. */
    x86p_emit_x87_m(e,
                    insn->x87_mem_int ? (o0->size == 2 ? 0xDEu : 0xDAu) : (o0->size == 4 ? 0xD8u : 0xDCu),
                    st0_field(op, reverse),
                    HOSTPTR_REG,
                    0);
    write_through(e, 0u, kX64Rdx);
    finish_fast(c, fast);
  }
#else
  (void)c;
  (void)insn;
#endif
}

void x87_inline_store_reg(BlockCtx *c, const X86pInsn *insn, X87Inline *fast) {
  begin(fast);
#if X87_INLINE_HOST
  {
    X86pEmit *e = c->e;
    const unsigned dst = (unsigned)insn->operand[0].reg;
    if (insn->x87_pops > 1u) {
      x87_cache_flush(c);
      return;
    }
    emit_phys(e, 0u);
    emit_tag_slot(e, kX64Rsi);
    guard_tag_is(e, fast, kX64Rsi, kTagEmpty);
    emit_phys(e, dst);
    emit_tag_slot(e, kX64Rsi);
    emit_reg_slot(e, kX64Rdx);
    mirror_ensure(c, 1u);
    if (dst != 0u && dst < c->x87_depth) {
      x86p_emit_x87_reg(e, 0xDDu, (uint8_t)(0xD0u + dst)); /* fst st(dst) */
    }
    write_through(e, 0u, kX64Rdx);
    emit_occupied(e, kX64Rsi);
    if (insn->x87_pops) {
      emit_pop(e);
      host_pop(e);
      c->x87_depth--;
    }
    finish_fast(c, fast);
  }
#else
  (void)c;
  (void)insn;
#endif
}

void x87_inline_store_mem(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip, X87Inline *fast) {
  begin(fast);
#if X87_INLINE_HOST
  {
    X86pEmit *e = c->e;
    const X86pOperand *o0 = &insn->operand[0];
    const int w = o0->size;
    /* The integer stores keep the helper: their overflow answer is the
       integer indefinite with IE raised, which is not one host instruction. */
    if (insn->x87 != kX86pX87InsnStore || (w != 4 && w != 8) || insn->x87_pops > 1u) {
      x87_cache_flush(c);
      return;
    }
    guard_host_control(c, fast);
    emit_phys(e, 0u);
    emit_tag_slot(e, kX64Rsi);
    guard_tag_is(e, fast, kX64Rsi, kTagEmpty);
    mirror_ensure(c, 1u);
    /* Only now may the access fault: ST(0) holds a value to store. The fault
       stub discards the mirror. */
    emit_mem_prepare_w(c, o0, insn_eip, w);
    /* fst / fstp m32 (D9 /2, /3) or m64 (DD /2, /3) */
    x86p_emit_x87_m(e, w == 4 ? 0xD9u : 0xDDu, insn->x87_pops ? 3u : 2u, HOSTPTR_REG, 0);
    if (insn->x87_pops) {
      c->x87_depth--;
      emit_pop(e);
    }
    finish_fast(c, fast);
  }
#else
  (void)c;
  (void)insn;
  (void)insn_eip;
#endif
}
