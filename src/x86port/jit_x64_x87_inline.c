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
 * at most X87_MIRROR_MAX deep: one host register always stays free, because
 * storing a register the mirror does not pop duplicates it first and the
 * divisor guard pushes a zero. Every guard is emitted before the sequence changes the host stack or
 * any guest state, so a slow path starts from the guest state the helper
 * expects.
 */
#include "jit_x64_x87_inline.h"

#include "cond.h"
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

static int32_t status_off(void) {
  return x87_field(offsetof(X86pX87, status));
}

static int32_t control_off(void) {
  return x87_field(offsetof(X86pX87, control));
}

static int32_t census_off(void) {
  return x87_field(offsetof(X86pX87, op_census));
}

/* The mirror as the write-back routine takes it: bit k is guest ST(k)'s dirty
   bit, and a stop bit sits at the depth. Zero for an empty mirror. */
static unsigned mirror_state(const BlockCtx *c) {
  return c->x87_depth ? c->x87_dirty | 1u << c->x87_depth : 0u;
}

static void note_slow(BlockCtx *c, X87Inline *fast, unsigned cc) {
  X86pEmit *e = c->e;
  const unsigned state = mirror_state(c);
  if (fast->nslow == 0u) {
    fast->spill = state;
  } else if (fast->spill != state) {
    fast->spill = X87_SPILL_UNKNOWN;
  }
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
  x86p_emit_shift_r32_imm8(e, kX64Shl, slot, 4u);
  x86p_emit_alu_r64_r64(e, kX64Add, slot, CPU_REG);
}

/* Jump to the slow path when the tag at [tag] is `tag_value`. */
static void guard_tag_is(BlockCtx *c, X87Inline *fast, X86pHostReg tag, unsigned tag_value) {
  X86pEmit *e = c->e;
  x86p_emit_load8_zx(e, kX64Rcx, tag, tag0_off());
  x86p_emit_alu_r32_imm32(e, kX64Cmp, kX64Rcx, tag_value);
  note_slow(c, fast, kCcE);
}

/* Jump to the slow path when the tag at [tag] is anything BUT `tag_value`. */
static void guard_tag_is_not(BlockCtx *c, X87Inline *fast, X86pHostReg tag, unsigned tag_value) {
  X86pEmit *e = c->e;
  x86p_emit_load8_zx(e, kX64Rcx, tag, tag0_off());
  x86p_emit_alu_r32_imm32(e, kX64Cmp, kX64Rcx, tag_value);
  note_slow(c, fast, kCcNe);
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
  note_slow(c, fast, kCcNe);
}

/* The op census counts inside x86p_x87_arith_raw; while one is armed every
   operation goes there, or the instrument would silently under-report. */
static void guard_census_disarmed(BlockCtx *c, X87Inline *fast) {
  X86pEmit *e = c->e;
  x86p_emit_load32(e, kX64Rax, CPU_REG, census_off());
  x86p_emit_alu_r32_mem(e, kX64Or, kX64Rax, CPU_REG, census_off() + 4);
  note_slow(c, fast, kCcNe);
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
   as it was. x87 has no non-popping ten-byte store. For a register the mirror
   does not hold, whose value only memory has. */
static void write_through(X86pEmit *e, unsigned host_index, X86pHostReg slot) {
  host_fld_st(e, host_index);
  fstp_ext80(e, slot);
}

/* Call the block's store_pop routine: host ST(0) into guest ST(ECX)'s slot,
   popped. It addresses the slot from TOP in memory. */
static void call_store_pop(BlockCtx *c, unsigned guest_index) {
  if (c->nx87_store_pops >= sizeof c->x87_store_pops / sizeof c->x87_store_pops[0]) {
    c->e->overflow = 1; /* an unbound call would jump anywhere; refuse the block */
    return;
  }
  x86p_emit_mov_r32_imm32(c->e, kX64Rcx, guest_index);
  c->x87_store_pops[c->nx87_store_pops++] = x86p_emit_call_rel32(c->e);
}

/* Call the block's spill routine: every value on the host stack stored to its
   guest register, and the host stack left empty. */
static void call_spill(BlockCtx *c) {
  if (c->nx87_spills >= sizeof c->x87_spills / sizeof c->x87_spills[0]) {
    c->e->overflow = 1;
    return;
  }
  c->x87_spills[c->nx87_spills++] = x86p_emit_call_rel32(c->e);
}

/* Call the block's write_back routine for a mirror whose state is `state`
   (mirror_state). */
static void call_write_back(BlockCtx *c, unsigned state) {
  if (c->nx87_write_backs >= sizeof c->x87_write_backs / sizeof c->x87_write_backs[0]) {
    c->e->overflow = 1;
    return;
  }
  x86p_emit_mov_r32_imm32(c->e, kX64Rcx, state);
  c->x87_write_backs[c->nx87_write_backs++] = x86p_emit_call_rel32(c->e);
}

/* Host ST(i) now holds guest ST(i)'s value and the register file does not. */
static void mirror_dirty(BlockCtx *c, unsigned host_index) {
  c->x87_dirty |= 1u << host_index;
}

/* The host push this sequence just made mirrors guest ST(0), a new value. */
static void mirror_pushed(BlockCtx *c) {
  c->x87_depth++;
  c->x87_dirty = (c->x87_dirty << 1) | 1u;
  c->x87_used = 1;
}

/* Host ST(0) popped, as guest ST(`guest_index`) relative to TOP in memory:
   stored on the way when the register file does not have it, because a popped
   register keeps its value (FSAVE writes all eight). */
static void mirror_pop(BlockCtx *c, unsigned guest_index) {
  if (c->x87_dirty & 1u) {
    call_store_pop(c, guest_index);
  } else {
    host_pop(c->e);
  }
  c->x87_dirty >>= 1;
  c->x87_depth--;
}

/*
 * Host ST(0..depth-1) = guest ST(0..depth-1), read from the register file into
 * an empty host stack. The loads are the block's shared loader, called with the
 * depth in ECX: a reload inline was about 25 bytes per register at every
 * rebuild, which put a single x87 instruction past twice the per-instruction
 * code budget, and a loader unrolled per depth was most of the block tail.
 */
static void mirror_load(BlockCtx *c, unsigned depth) {
  c->x87_depth = depth;
  c->x87_dirty = 0u;
  if (depth == 0u) {
    return;
  }
  c->x87_used = 1;
  if (c->nx87_loads >= sizeof c->x87_loads / sizeof c->x87_loads[0]) {
    c->e->overflow = 1; /* an unbound call would jump anywhere; refuse the block */
    return;
  }
  x86p_emit_mov_r32_imm32(c->e, kX64Rcx, depth);
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

/* Room for one push: a full mirror lets go of its deepest value, storing it
   first when only the host has it. FFREE leaves that host register empty,
   which is where the host's own push lands. Called after the push has moved
   TOP in memory, so host ST(6) is guest ST(7) there. */
static void mirror_make_room(BlockCtx *c) {
  const unsigned deepest = X87_MIRROR_MAX - 1u;
  if (c->x87_depth == X87_MIRROR_MAX) {
    if (c->x87_dirty & (1u << deepest)) {
      host_fld_st(c->e, deepest);
      call_store_pop(c, deepest + 1u);
    }
    x86p_emit_x87_reg(c->e, 0xDDu, (uint8_t)(0xC0u + deepest)); /* ffree st(6) */
    c->x87_dirty &= ~(1u << deepest);
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
static void guard_mirrored_nonzero(BlockCtx *c, X87Inline *fast, unsigned i) {
  X86pEmit *e = c->e;
  X86pEmitSite unordered;
  x86p_emit_x87_reg(e, 0xD9u, 0xEEu);                     /* fldz */
  x86p_emit_x87_reg(e, 0xDFu, (uint8_t)(0xE8u + i + 1u)); /* fucomip st(0), st(i+1) */
  unordered = x86p_emit_jcc_rel32(e, kCcP);
  note_slow(c, fast, kCcE);
  x86p_emit_bind(e, unordered);
}

/*
 * Jump to the slow path when a prepared memory divisor is a zero of either
 * sign, asked of its bits: integer zero, or a float whose exponent and
 * significand are both zero. That is exactly the condition the helper's
 * `y == 0.0L` names after the exact widening.
 */
static void guard_memory_nonzero(BlockCtx *c, X87Inline *fast, int w, int integer) {
  X86pEmit *e = c->e;
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
  note_slow(c, fast, kCcE);
}

/*
 * The condition codes of an ordered comparison, from the EFLAGS an FUCOMI just
 * wrote: C0 for below and C3 for equal, C2 clear, exactly what
 * x86p_x87_compare stores. An unordered result goes to the slow path, which
 * owns IE and the all-three answer. EAX and ECX were zeroed before the
 * comparison, since MOV does not touch EFLAGS and XOR would.
 */
static void finish_compare(BlockCtx *c, X87Inline *fast, unsigned below_cc) {
  X86pEmit *e = c->e;
  note_slow(c, fast, (unsigned)kX86pCondP);
  x86p_emit_setcc_r8(e, below_cc, kX64Rax);
  x86p_emit_setcc_r8(e, (unsigned)kX86pCondZ, kX64Rcx);
  x86p_emit_shift_r32_imm8(e, kX64Shl, kX64Rax, 8u);  /* C0 */
  x86p_emit_shift_r32_imm8(e, kX64Shl, kX64Rcx, 14u); /* C3 */
  x86p_emit_alu_r32_r32(e, kX64Or, kX64Rax, kX64Rcx);
  x86p_emit_load16_zx(e, kX64Rdx, CPU_REG, status_off());
  x86p_emit_alu_r32_imm32(e, kX64And, kX64Rdx, ~(uint32_t)(X86P_X87_C0 | X86P_X87_C2 | X86P_X87_C3));
  x86p_emit_alu_r32_r32(e, kX64Or, kX64Rdx, kX64Rax);
  x86p_emit_store16_reg(e, CPU_REG, status_off(), kX64Rdx);
}

static void zero_compare_scratch(X86pEmit *e) {
  x86p_emit_mov_r32_imm32(e, kX64Rax, 0u);
  x86p_emit_mov_r32_imm32(e, kX64Rcx, 0u);
}

/* `pops` guest pops of values the mirror holds, and their host pops. The host
   pop stores from TOP in memory, so it goes first. */
static void pop_mirrored(BlockCtx *c, unsigned pops) {
  while (pops--) {
    mirror_pop(c, 0u);
    emit_pop(c->e);
  }
}

static void finish_fast(BlockCtx *c, X87Inline *fast) {
  fast->done = x86p_emit_jmp_rel32(c->e);
  fast->depth = c->x87_depth;
  fast->dirty = c->x87_dirty;
  fast->emitted = 1;
}

/* More dirty values than this go through the write_back routine, which
   bounds the sequence to one call. */
#define X87_FLUSH_INLINE_STORES 2u

/* Store the dirty values of a mirror in `state` (mirror_state) and pop it,
   inline or through the routine. The host stack is empty after this. */
static void write_back(BlockCtx *c, unsigned state) {
  unsigned guest_index;
  if (__builtin_popcount(state) - 1 > (int)X87_FLUSH_INLINE_STORES) {
    call_write_back(c, state);
    return;
  }
  for (guest_index = 0u; state > 1u; guest_index++, state >>= 1) {
    if (state & 1u) {
      call_store_pop(c, guest_index);
    } else {
      host_pop(c->e);
    }
  }
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
  fast->dirty = 0;
  fast->spill = 0;
}

void x87_cache_flush(BlockCtx *c) {
#if X87_INLINE_HOST
  write_back(c, mirror_state(c));
  c->x87_depth = 0u;
  c->x87_dirty = 0u;
#else
  (void)c;
#endif
}

void x87_cache_spill(BlockCtx *c) {
#if X87_INLINE_HOST
  if (c->x87_used) {
    call_spill(c);
  }
#else
  (void)c;
#endif
}

/*
 * store_pop: host ST(0) into the slot of guest ST(ECX), relative to TOP in
 * memory, and popped. Clobbers RAX; ECX survives.
 *
 * spill: store_pop for guest ST(0), ST(1), ... while the host stack holds
 * anything. The host stack holds the mirror and nothing else, so what it holds
 * IS the mirror, however deep: a fault or a slow path needs no record of the
 * depth at its site. Clobbers RAX and RCX.
 */
#if X87_INLINE_HOST
/*
 * write_back: ECX holds a mirror_state. Guest ST(0) upward, each host ST(0) is
 * stored through store_pop when its bit is set and popped when it is not,
 * until only the stop bit is left. The state moves to EDX, which is saved
 * because a flush inside an inline sequence can find it holding a slot, and
 * ECX counts the guest index store_pop takes.
 */
static void emit_write_back(BlockCtx *c, size_t *entry, size_t store_pop) {
  X86pEmit *e = c->e;
  X86pEmitSite done;
  X86pEmitSite clean;
  X86pEmitSite next;
  size_t loop;
  *entry = x86p_emit_here(e);
  x86p_emit_push_r64(e, kX64Rdx);
  x86p_emit_mov_r32_r32(e, kX64Rdx, kX64Rcx);
  x86p_emit_alu_r32_r32(e, kX64Xor, kX64Rcx, kX64Rcx);
  loop = x86p_emit_here(e);
  x86p_emit_alu_r32_imm32(e, kX64Cmp, kX64Rdx, 1u);
  done = x86p_emit_jcc_rel32(e, kCcE);
  x86p_emit_mov_r32_r32(e, kX64Rax, kX64Rdx);
  x86p_emit_alu_r32_imm32(e, kX64And, kX64Rax, 1u);
  clean = x86p_emit_jcc_rel32(e, kCcE);
  x86p_emit_bind_to(e, x86p_emit_call_rel32(e), store_pop);
  next = x86p_emit_jmp_rel32(e);
  x86p_emit_bind(e, clean);
  host_pop(e);
  x86p_emit_bind(e, next);
  x86p_emit_alu_r32_imm32(e, kX64Add, kX64Rcx, 1u);
  x86p_emit_shift_r32_imm8(e, kX64Shr, kX64Rdx, 1u);
  x86p_emit_bind_to(e, x86p_emit_jmp_rel32(e), loop);
  x86p_emit_bind(e, done);
  x86p_emit_pop_r64(e, kX64Rdx);
  x86p_emit_ret(e);
}

static void emit_write_back_routines(BlockCtx *c) {
  X86pEmit *e = c->e;
  size_t store_pop = 0u;
  size_t spill = 0u;
  size_t write_back_entry = 0u;
  unsigned i;
  if (c->nx87_store_pops || c->nx87_spills || c->nx87_write_backs) {
    X86pEmitSite empty;
    size_t loop;
    store_pop = x86p_emit_here(e);
    x86p_emit_load8_zx(e, kX64Rax, CPU_REG, top_off());
    x86p_emit_alu_r32_r32(e, kX64Add, kX64Rax, kX64Rcx);
    x86p_emit_alu_r32_imm32(e, kX64And, kX64Rax, 7u);
    x86p_emit_shift_r32_imm8(e, kX64Shl, kX64Rax, 4u);
    x86p_emit_alu_r64_r64(e, kX64Add, kX64Rax, CPU_REG);
    fstp_ext80(e, kX64Rax);
    x86p_emit_ret(e);
    if (c->nx87_write_backs) {
      emit_write_back(c, &write_back_entry, store_pop);
    }
    if (c->nx87_spills) {
      spill = x86p_emit_here(e);
      x86p_emit_mov_r32_imm32(e, kX64Rcx, 0u);
      loop = x86p_emit_here(e);
      x86p_emit_x87_reg(e, 0xD9u, 0xE5u); /* fxam */
      x86p_emit_x87_reg(e, 0xDFu, 0xE0u); /* fnstsw ax */
      /* Empty is C3 and C0 with C2 clear. */
      x86p_emit_alu_r32_imm32(e, kX64And, kX64Rax, X86P_X87_C3 | X86P_X87_C2 | X86P_X87_C0);
      x86p_emit_alu_r32_imm32(e, kX64Cmp, kX64Rax, X86P_X87_C3 | X86P_X87_C0);
      empty = x86p_emit_jcc_rel32(e, kCcE);
      x86p_emit_bind_to(e, x86p_emit_call_rel32(e), store_pop);
      x86p_emit_alu_r32_imm32(e, kX64Add, kX64Rcx, 1u);
      x86p_emit_bind_to(e, x86p_emit_jmp_rel32(e), loop);
      x86p_emit_bind(e, empty);
      x86p_emit_ret(e);
    }
  }
  for (i = 0; i < c->nx87_store_pops; i++) {
    x86p_emit_bind_to(e, c->x87_store_pops[i], store_pop);
  }
  for (i = 0; i < c->nx87_spills; i++) {
    x86p_emit_bind_to(e, c->x87_spills[i], spill);
  }
  for (i = 0; i < c->nx87_write_backs; i++) {
    x86p_emit_bind_to(e, c->x87_write_backs[i], write_back_entry);
  }
}

/*
 * The loader: guest ST(ECX-1) down to ST(0) pushed in that order, so host ST(k)
 * ends as guest ST(k). EAX walks the physical index down from TOP + depth - 1,
 * wrapping at eight; it is scaled to the sixteen-byte slot for one load and
 * back. Clobbers EAX, ECX and the flags, as every call here may.
 */
static void emit_loader(BlockCtx *c) {
  X86pEmit *e = c->e;
  size_t loop;
  size_t entry;
  unsigned i;
  if (!c->nx87_loads) {
    return;
  }
  entry = x86p_emit_here(e);
  x86p_emit_load8_zx(e, kX64Rax, CPU_REG, top_off());
  x86p_emit_alu_r32_r32(e, kX64Add, kX64Rax, kX64Rcx);
  loop = x86p_emit_here(e);
  x86p_emit_alu_r32_imm32(e, kX64Sub, kX64Rax, 1u);
  x86p_emit_alu_r32_imm32(e, kX64And, kX64Rax, 7u);
  x86p_emit_shift_r32_imm8(e, kX64Shl, kX64Rax, 4u);
  x86p_emit_alu_r64_r64(e, kX64Add, kX64Rax, CPU_REG);
  fld_ext80(e, kX64Rax);
  x86p_emit_alu_r64_r64(e, kX64Sub, kX64Rax, CPU_REG);
  x86p_emit_shift_r32_imm8(e, kX64Shr, kX64Rax, 4u);
  x86p_emit_alu_r32_imm32(e, kX64Sub, kX64Rcx, 1u);
  x86p_emit_bind_to(e, x86p_emit_jcc_rel32(e, kCcNe), loop);
  x86p_emit_ret(e);
  for (i = 0; i < c->nx87_loads; i++) {
    x86p_emit_bind_to(e, c->x87_loads[i], entry);
  }
}
#endif

void x87_cache_emit_routines(BlockCtx *c) {
#if X87_INLINE_HOST
  emit_loader(c);
  emit_write_back_routines(c);
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
#if X87_INLINE_HOST
  if (fast->spill == X87_SPILL_UNKNOWN) {
    call_spill(c);
  } else if (fast->spill) {
    write_back(c, fast->spill);
  }
  /* The helper sequence runs with the host stack empty and the register file
     complete. */
  c->x87_depth = 0u;
  c->x87_dirty = 0u;
#endif
}

void x87_inline_end(BlockCtx *c, X87Inline *fast) {
  if (!fast->emitted) {
    return;
  }
#if X87_INLINE_HOST
  mirror_load(c, fast->depth);
  /* The paths meet with the fast path's dirty set: after the slow path those
     registers are clean, and storing a clean value again is exact. */
  c->x87_dirty = fast->dirty;
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
      guard_tag_is(c, fast, kX64Rsi, kTagEmpty);
      emit_reg_slot(e, kX64Rdi);
    }
    emit_phys(e, 7u); /* the slot a push fills: TOP - 1 */
    emit_tag_slot(e, kX64Rsi);
    guard_tag_is_not(c, fast, kX64Rsi, kTagEmpty); /* full: the helper reports the overflow */
    x86p_emit_store8_reg(e, CPU_REG, top_off(), kX64Rax);
    mirror_make_room(c);
    if (o->kind == kX86pOperandMem) {
      fld_guest(e, o->size, insn->x87 == kX86pX87InsnLoadInt);
    } else if (src < c->x87_depth) {
      host_fld_st(e, src);
    } else {
      fld_ext80(e, kX64Rdi);
    }
    mirror_pushed(c);
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
      guard_census_disarmed(c, fast);
      emit_phys(e, src);
      emit_tag_slot(e, kX64Rsi);
      guard_tag_is(c, fast, kX64Rsi, kTagEmpty);
      emit_phys(e, dst);
      emit_tag_slot(e, kX64Rsi);
      guard_tag_is(c, fast, kX64Rsi, kTagEmpty);
      mirror_ensure(c, deepest + 1u);
      if (divide) {
        guard_mirrored_nonzero(c, fast, reverse ? dst : src);
      }
      if (dst == 0u) {
        x86p_emit_x87_reg(e, 0xD8u, (uint8_t)(0xC0u | (st0_field(op, reverse) << 3) | src));
        mirror_dirty(c, 0u);
      } else if (pops) {
        /* fop st(dst), st(0) and pop: the result is host ST(dst - 1) now, and
           guest ST(dst - 1) once the guest pop below renumbers the stack. The
           popped ST(0) keeps its value, so a copy only the host has is
           stored first. */
        if (c->x87_dirty & 1u) {
          host_fld_st(e, 0u);
          call_store_pop(c, 0u);
        }
        x86p_emit_x87_reg(e, 0xDEu, (uint8_t)(0xC0u | (sti_field(op, reverse) << 3) | dst));
        c->x87_depth--;
        c->x87_dirty >>= 1;
        mirror_dirty(c, dst - 1u);
        emit_pop(e);
      } else {
        x86p_emit_x87_reg(e, 0xDCu, (uint8_t)(0xC0u | (sti_field(op, reverse) << 3) | dst));
        mirror_dirty(c, dst);
      }
      finish_fast(c, fast);
      return;
    }

    if (pops) {
      x87_cache_flush(c);
      return;
    }
    guard_host_control(c, fast);
    guard_census_disarmed(c, fast);
    emit_phys(e, 0u);
    emit_tag_slot(e, kX64Rsi);
    guard_tag_is(c, fast, kX64Rsi, kTagEmpty);
    if (divide && !reverse) {
      guard_memory_nonzero(c, fast, o0->size, insn->x87_mem_int);
    }
    mirror_ensure(c, 1u);
    if (divide && reverse) {
      guard_mirrored_nonzero(c, fast, 0u);
    }
    /* fop st(0), m: D8 m32 and DC m64 floats, DA m32 and DE m16 integers. */
    x86p_emit_x87_m(e,
                    insn->x87_mem_int ? (o0->size == 2 ? 0xDEu : 0xDAu) : (o0->size == 4 ? 0xD8u : 0xDCu),
                    st0_field(op, reverse),
                    HOSTPTR_REG,
                    0);
    mirror_dirty(c, 0u);
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
    guard_tag_is(c, fast, kX64Rsi, kTagEmpty);
    emit_phys(e, dst);
    emit_tag_slot(e, kX64Rsi);
    emit_reg_slot(e, kX64Rdx);
    mirror_ensure(c, 1u);
    if (dst != 0u && dst < c->x87_depth) {
      x86p_emit_x87_reg(e, 0xDDu, (uint8_t)(0xD0u + dst)); /* fst st(dst) */
      mirror_dirty(c, dst);
    } else if (dst != 0u) {
      write_through(e, 0u, kX64Rdx); /* a register only memory holds */
    }
    emit_occupied(e, kX64Rsi);
    if (insn->x87_pops) {
      pop_mirrored(c, 1u);
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
    guard_tag_is(c, fast, kX64Rsi, kTagEmpty);
    mirror_ensure(c, 1u);
    /* Only now may the access fault: ST(0) holds a value to store. The fault
       stub spills the mirror. */
    emit_mem_prepare_w(c, o0, insn_eip, w);
    /* fst / fstp m32 (D9 /2, /3) or m64 (DD /2, /3). A popped value only the
       host has goes to its register too, through the ordinary pop. */
    if (insn->x87_pops && !(c->x87_dirty & 1u)) {
      x86p_emit_x87_m(e, w == 4 ? 0xD9u : 0xDDu, 3u, HOSTPTR_REG, 0);
      c->x87_depth--;
      c->x87_dirty >>= 1;
      emit_pop(e);
    } else {
      x86p_emit_x87_m(e, w == 4 ? 0xD9u : 0xDDu, 2u, HOSTPTR_REG, 0);
      pop_mirrored(c, insn->x87_pops);
    }
    finish_fast(c, fast);
  }
#else
  (void)c;
  (void)insn;
  (void)insn_eip;
#endif
}

void x87_inline_register(BlockCtx *c, const X86pInsn *insn, X87Inline *fast) {
  begin(fast);
#if X87_INLINE_HOST
  {
    X86pEmit *e = c->e;
    const unsigned index = insn->operands ? (unsigned)insn->operand[0].reg : 1u;
    const unsigned pops = insn->x87_pops;
    const int unary = insn->x87 == kX86pX87InsnChangeSign || insn->x87 == kX86pX87InsnAbs;
    const int exchange = insn->x87 == kX86pX87InsnExchange;
    if (!(unary || exchange || insn->x87 == kX86pX87InsnCompare) || index >= X87_MIRROR_MAX ||
        pops > (exchange || unary ? 0u : 2u) || (pops == 2u && index != 1u)) {
      x87_cache_flush(c);
      return;
    }
    emit_phys(e, 0u);
    emit_tag_slot(e, kX64Rsi);
    guard_tag_is(c, fast, kX64Rsi, kTagEmpty);
    if (unary) {
      mirror_ensure(c, 1u);
      x86p_emit_x87_reg(e, 0xD9u, insn->x87 == kX86pX87InsnAbs ? 0xE1u : 0xE0u); /* fabs / fchs */
      mirror_dirty(c, 0u);
      finish_fast(c, fast);
      return;
    }
    emit_phys(e, index);
    emit_tag_slot(e, kX64Rsi);
    guard_tag_is(c, fast, kX64Rsi, kTagEmpty);
    mirror_ensure(c, index + 1u);
    if (exchange) {
      x86p_emit_x87_reg(e, 0xD9u, (uint8_t)(0xC8u + index)); /* fxch st(i) */
      mirror_dirty(c, 0u);
      mirror_dirty(c, index);
      finish_fast(c, fast);
      return;
    }
    zero_compare_scratch(e);
    x86p_emit_x87_reg(e, 0xDBu, (uint8_t)(0xE8u + index)); /* fucomi st(0), st(i) */
    finish_compare(c, fast, (unsigned)kX86pCondB);
    pop_mirrored(c, pops);
    finish_fast(c, fast);
  }
#else
  (void)c;
  (void)insn;
#endif
}

void x87_inline_compare_mem(BlockCtx *c, const X86pInsn *insn, X87Inline *fast) {
  begin(fast);
#if X87_INLINE_HOST
  {
    X86pEmit *e = c->e;
    const X86pOperand *o0 = &insn->operand[0];
    if (insn->x87_pops > 1u) {
      x87_cache_flush(c);
      return;
    }
    emit_phys(e, 0u);
    emit_tag_slot(e, kX64Rsi);
    guard_tag_is(c, fast, kX64Rsi, kTagEmpty);
    mirror_ensure(c, 1u);
    zero_compare_scratch(e);
    /* The operand goes on top and FUCOMIP drops it again, so the flags compare
       the operand with ST(0): ST(0) is below it when the operand is ABOVE. */
    fld_guest(e, o0->size, insn->x87_mem_int);
    x86p_emit_x87_reg(e, 0xDFu, 0xE9u); /* fucomip st(0), st(1) */
    finish_compare(c, fast, (unsigned)kX86pCondA);
    pop_mirrored(c, insn->x87_pops);
    finish_fast(c, fast);
  }
#else
  (void)c;
  (void)insn;
#endif
}

void x87_inline_constant(BlockCtx *c, const X86pInsn *insn, X87Inline *fast) {
  begin(fast);
#if X87_INLINE_HOST
  {
    X86pEmit *e = c->e;
    if (insn->x87 != kX86pX87InsnConstZero && insn->x87 != kX86pX87InsnConstOne) {
      x87_cache_flush(c);
      return;
    }
    emit_phys(e, 7u); /* the slot a push fills: TOP - 1 */
    emit_tag_slot(e, kX64Rsi);
    guard_tag_is_not(c, fast, kX64Rsi, kTagEmpty);
    x86p_emit_store8_reg(e, CPU_REG, top_off(), kX64Rax);
    mirror_make_room(c);
    x86p_emit_x87_reg(e, 0xD9u, insn->x87 == kX86pX87InsnConstZero ? 0xEEu : 0xE8u); /* fldz / fld1 */
    mirror_pushed(c);
    emit_occupied(e, kX64Rsi);
    finish_fast(c, fast);
  }
#else
  (void)c;
  (void)insn;
#endif
}

/* x86p_x87_status: the stored word with TOP merged into bits 11..13, written
   to AX with EAX's upper half kept. */
void x87_inline_status_ax(BlockCtx *c) {
  X86pEmit *e = c->e;
  const int32_t status = (int32_t)(offsetof(X86pCpu, x87) + offsetof(X86pX87, status));
  const int32_t top = (int32_t)(offsetof(X86pCpu, x87) + offsetof(X86pX87, top));
  x86p_emit_load16_zx(e, kX64Rcx, CPU_REG, status);
  x86p_emit_alu_r32_imm32(e, kX64And, kX64Rcx, ~(7u << X86P_X87_TOP_SHIFT) & 0xFFFFu);
  x86p_emit_load8_zx(e, kX64Rax, CPU_REG, top);
  x86p_emit_alu_r32_imm32(e, kX64And, kX64Rax, 7u);
  x86p_emit_shift_r32_imm8(e, kX64Shl, kX64Rax, (uint8_t)X86P_X87_TOP_SHIFT);
  x86p_emit_alu_r32_r32(e, kX64Or, kX64Rcx, kX64Rax);
  x86p_emit_store16_reg(e, CPU_REG, (int32_t)offsetof(X86pCpu, reg[kX86pEax]), kX64Rcx);
}
