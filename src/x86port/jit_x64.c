/* jit_x64.c -- see jit_x64.h for why arithmetic is called rather than inlined. */
#include "jit_x64.h"

#include "alu.h"
#include "cond.h"
#include "decode.h"
#include "emit_x64.h"
#include "flags.h"
#include "jit_x64_cond.h"
#include "jit_x64_gpr.h"
#include "jit_x64_internal.h"
#include "jit_x64_x87.h"
#include "jit_x64_x87_inline.h"
#include "simd.h"
#include "string_ops.h"
#include "three_dnow.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* The host register roles (CPU_REG, HOSTPTR_REG, ...) and MAX_INSNS live in
   jit_x64_internal.h -- the x87 emission unit needs the same definitions. */

/* Emitting one guest instruction never exceeds this, so the buffer is checked
   once per instruction rather than after every emit. The margin is generous
   and the emitter's own overflow flag is still the authority -- this only
   decides when to stop trying. The values live in the header because the
   dispatch loop needs the same worst case to size its arena. */
#define WORST_CASE_INSN_BYTES X86P_JIT_WORST_CASE_INSN_BYTES
#define EPILOGUE_BYTES X86P_JIT_EPILOGUE_BYTES /* normal exit plus the fault stub */

int x86p_jit_available(void) {
#if defined(__x86_64__) || defined(_M_X64)
  return 1;
#else
  return 0;
#endif
}

static void say(char *buf, unsigned len, const char *fmt, ...) {
  va_list ap;
  if (!buf || len == 0) {
    return;
  }
  va_start(ap, fmt);
  vsnprintf(buf, len, fmt, ap);
  va_end(ap);
}

/* ---- where things live in X86pCpu --------------------------------------- */

/*
 * Offsets are taken from the real struct, never written down as constants. A
 * hardcoded offset keeps working until a field is added above it, and then the
 * emitted code silently reads a different register than the canonical CPU
 * layout requires.
 */
static int32_t eip_off(void) {
  return (int32_t)offsetof(X86pCpu, eip);
}

/* ---- can this instruction be emitted? ----------------------------------- */

/*
 * The translatable set, stated in ONE place.
 *
 * Deliberately narrow for a first backend: 32-bit register-to-register and
 * register-to-immediate MOV and ALU, and NOP. Everything else -- memory
 * operands, 8- and 16-bit widths with their partial-write rules, branches,
 * the stack, x87 -- ends the block and is named.
 *
 * Byte and word widths are excluded rather than approximated because a partial
 * write preserves the bits outside it (cpu.h), and a backend that got that
 * subtly wrong would corrupt a value the guest is still using.
 */
static int operand_is_reg32(const X86pOperand *o) {
  return o->kind == kX86pOperandReg && o->size == 4;
}

static int operand_is_imm(const X86pOperand *o) {
  return o->kind == kX86pOperandImm;
}

/* A 32-bit memory operand this backend can address. Widths 1 and 2 stay out
   until their partial-write rules are emitted; a byte load that wrote a whole
   register would corrupt the three bytes above it. */
static int operand_is_mem32(const X86pOperand *o) {
  return o->kind == kX86pOperandMem && o->size == 4;
}

static int operand_writable(const X86pOperand *o) {
  return operand_is_reg32(o) || operand_is_mem32(o);
}

/*
 * A branch this backend can emit: PC-relative with an immediate displacement.
 *
 * An indirect jump (through a register or memory) is excluded, and that is not
 * a temporary gap -- its target is not known until the block runs, so it needs
 * the block cache to resolve at run time rather than a constant folded in here.
 * Treating one as translatable would fold in whatever the operand decoded to
 * and jump somewhere fixed and wrong.
 */
static int is_relative_branch(const X86pInsn *insn) {
  if (insn->op != (uint8_t)kX86pInsnJmp && insn->op != (uint8_t)kX86pInsnJcc && insn->op != kX86pInsnLoop &&
      insn->op != kX86pInsnLoope && insn->op != kX86pInsnLoopne) {
    return 0;
  }
  return insn->operands >= 1 && insn->operand[0].kind == kX86pOperandImm && insn->operand[0].relative;
}

/* An indirect JMP or CALL: the target is a register or a memory location, so it
   is not known until the block runs. The block ends with the computed address
   in EIP and the dispatcher looks it up -- which is exactly what the block
   cache is for, and why these could not be emitted before there was one. */
static int is_indirect_branch(const X86pInsn *insn) {
  if (insn->op != (uint8_t)kX86pInsnJmp) {
    return 0;
  }
  return insn->operands == 1 && insn->operand[0].kind != kX86pOperandImm;
}

/*
 * MOV at 8, 16 or 32 bits.
 *
 * A byte-register INDEX above 7 is refused: reproducing an "ignore it" behavior
 * in emitted code would bake
 * a guess about an encoding this decoder should never produce. Refusing by name
 * makes it visible if one ever appears.
 */
static int mov_operand_ok(const X86pOperand *o, int w, int for_write) {
  if (o->kind == kX86pOperandImm) {
    return !for_write;
  }
  if (o->kind == kX86pOperandMem) {
    return o->size == w;
  }
  if (o->kind != kX86pOperandReg) {
    return 0;
  }
  if (o->size != w) {
    return 0;
  }
  return w != 1 || (o->reg >= 0 && o->reg < 8);
}

static int mov_is_emittable(const X86pInsn *insn) {
  int w;
  if (insn->operands != 2) {
    return 0;
  }
  w = insn->operand[0].size;
  if (w != 1 && w != 2 && w != 4) {
    return 0;
  }
  /* At most ONE memory operand: x86 has no memory-to-memory MOV, and accepting
     one would emit two host pointers into the same register. */
  if (insn->operand[0].kind == kX86pOperandMem && insn->operand[1].kind == kX86pOperandMem) {
    return 0;
  }
  return mov_operand_ok(&insn->operand[0], w, 1) && mov_operand_ok(&insn->operand[1], w, 0);
}

/*
 * MOVZX / MOVSX: destination a 16- or 32-bit register, source a narrower
 * register or memory. A byte source register index above 7 is refused for the
 * same reason MOV refuses it. The widen is real -- src strictly narrower than
 * dst -- so a decoder that ever produced a same-width form is refused rather
 * than becoming a no-op extend.
 */
static int movx_is_emittable(const X86pInsn *insn) {
  const X86pOperand *dst;
  const X86pOperand *src;
  if (insn->operands != 2) {
    return 0;
  }
  dst = &insn->operand[0];
  src = &insn->operand[1];
  if (dst->kind != kX86pOperandReg || (dst->size != 2 && dst->size != 4)) {
    return 0;
  }
  if (src->size != 1 && src->size != 2) {
    return 0;
  }
  if (src->size >= dst->size) {
    return 0;
  }
  if (src->kind == kX86pOperandReg) {
    return src->size != 1 || (src->reg >= 0 && src->reg < 8);
  }
  return src->kind == kX86pOperandMem;
}

/* The x87 predicates (x87_load_is_emittable, ...) live in jit_x64_x87.c. */

static int can_emit(const X86pInsn *insn) {
  int i;
  /*
   * A 16-BIT ADDRESS is refused until its wrapping address rule has an emitter.
   *
   * Every emitter here computes the effective address the 32-bit way, and the
   * two agree until a sum crosses 0xFFFF -- so a wrong translation would be
   * right on nearly every input and wrong on the wrap, which is the case the
   * encoding exists for. One check at the gate rather than one in each of the
   * dozen operand predicates, because the predicate that gets forgotten is
   * the one that silently produces a valid-looking block.
   */
  for (i = 0; i < insn->operands && i < X86P_MAX_OPERANDS; i++) {
    if (insn->operand[i].kind == kX86pOperandMem && insn->operand[i].addr16) {
      return 0;
    }
  }
  if (is_relative_branch(insn)) {
    return 1;
  }
  if (is_indirect_branch(insn)) {
    return operand_is_reg32(&insn->operand[0]) || operand_is_mem32(&insn->operand[0]);
  }
  switch (insn->op) {
  case kX86pInsnNop:
    return 1;
  case kX86pInsnShld:
  case kX86pInsnShrd:
    return double_shift_is_emittable(insn);
  case kX86pInsnSimd:
    return simd_bits_is_emittable(insn);
  case kX86pInsnMov:
    return mov_is_emittable(insn);
  case kX86pInsnMovzx:
  case kX86pInsnMovsx:
    return movx_is_emittable(insn);
  case kX86pInsnXchg:
    return insn->operands == 2 && insn->operand[0].size == 4 && insn->operand[1].size == 4 &&
           ((operand_is_reg32(&insn->operand[0]) &&
             (operand_is_reg32(&insn->operand[1]) || operand_is_mem32(&insn->operand[1]))) ||
            (operand_is_mem32(&insn->operand[0]) && operand_is_reg32(&insn->operand[1])));
  case kX86pInsnSetcc:
    return insn->cond < (uint8_t)kX86pCondCount && insn->operands == 1 && mov_operand_ok(&insn->operand[0], 1, 1);
  case kX86pInsnAlu:
    if (insn->alu >= (uint8_t)kX86pAluShl && insn->alu <= (uint8_t)kX86pAluRcr) {
      /*
       * Shifts and rotates go through x86p_alu rather than being emitted inline,
       * for the same reason ADC and SBB do: their rules are not a host shift.
       * The count is masked to five bits, a count at or past the operand width
       * has three different answers depending on direction, and a count of ZERO
       * writes no flags AT ALL -- not preserved, not written -- which the lazy
       * model has no kind for and flags.c refuses to record. Reproducing that
       * here would be a second authority on it. A memory destination's host
       * pointer survives the call in R12 (see emit_alu), so it is allowed here
       * too.
       */
      return insn->operands == 2 && mov_operand_ok(&insn->operand[0], insn->operand[0].size, 1) &&
             (insn->operand[1].kind == kX86pOperandImm ||
              (insn->operand[1].kind == kX86pOperandReg && insn->operand[1].reg == kX86pEcx &&
               insn->operand[1].size == 1));
    }
    if (insn->alu > (uint8_t)kX86pAluTest) {
      return 0;
    }
    /* ADC and SBB still call x86p_alu, which takes VALUES rather than a
       pointer to the destination -- emit_alu stashes a memory destination's
       host pointer in R12 across the call, exactly as the shifts above do. */
    if (insn->alu == (uint8_t)kX86pAluAdc || insn->alu == (uint8_t)kX86pAluSbb) {
      return insn->operands == 2 && mov_operand_ok(&insn->operand[0], insn->operand[0].size, 1) &&
             (insn->operand[0].kind != kX86pOperandMem || insn->operand[1].kind != kX86pOperandMem) &&
             mov_operand_ok(&insn->operand[1], insn->operand[0].size, 0);
    }
    return mov_is_emittable(insn);
  case kX86pInsnPush:
    /* A narrow immediate is sign-extended by the canonical x86p_sign_extend
       rule at translation time so the constant
       folded into the code is the one the semantics owner computes. */
    return insn->operands == 1 && (operand_is_reg32(&insn->operand[0]) || operand_is_mem32(&insn->operand[0]) ||
                                   operand_is_imm(&insn->operand[0]));
  case kX86pInsnPop:
    return insn->operands == 1 && operand_writable(&insn->operand[0]);
  case kX86pInsnRdtsc:
  case kX86pInsnCpuid:
  case kX86pInsnCld:
  case kX86pInsnStd:
  case kX86pInsnSahf:
  case kX86pInsnLahf:
  case kX86pInsnPushfd:
  case kX86pInsnPopfd:
    return insn->operands == 0;
  case kX86pInsnCall:
    return insn->operands == 1 && (operand_is_imm(&insn->operand[0]) || operand_is_reg32(&insn->operand[0]) ||
                                   operand_is_mem32(&insn->operand[0]));
  case kX86pInsnRet:
    /* RET, or RET imm16 which also releases the caller's arguments. */
    return insn->operands == 0 || (insn->operands == 1 && operand_is_imm(&insn->operand[0]));
  case kX86pInsnLeave:
    return insn->operands == 0;
  case kX86pInsnCdq:
    return insn->operands == 0;
  case kX86pInsnDiv:
  case kX86pInsnIdiv:
    return insn->operands == 1 && (operand_is_reg32(&insn->operand[0]) || operand_is_mem32(&insn->operand[0]));
  case kX86pInsnMul:
    return insn->operands == 1 && mov_operand_ok(&insn->operand[0], insn->operand[0].size, 1);
  case kX86pInsnImul:
    if (insn->operands == 1) {
      return mov_operand_ok(&insn->operand[0], insn->operand[0].size, 1);
    }
    return (insn->operands == 2 || (insn->operands == 3 && operand_is_imm(&insn->operand[2]))) &&
           operand_is_reg32(&insn->operand[0]) &&
           (operand_is_reg32(&insn->operand[1]) || operand_is_mem32(&insn->operand[1]));
  case kX86pInsnString:
    return x86p_string_is_supported((X86pStringOp)insn->str, (X86pRepKind)insn->rep, insn->str_width);
  case kX86pInsnAluUnary:
    if (insn->alu > (uint8_t)kX86pAluDec) {
      return 0;
    }
    return insn->operands == 1 && insn->operand[0].kind != kX86pOperandImm &&
           mov_operand_ok(&insn->operand[0], insn->operand[0].size, 1);
  case kX86pInsnLea:
    /* The size of an LEA's memory operand describes an access that never
       happens, so operand_is_mem32's width rule does not apply -- only that
       the operand really is a memory reference to compute. */
    return insn->operands == 2 && operand_is_reg32(&insn->operand[0]) && insn->operand[1].kind == kX86pOperandMem;
  case kX86pInsnX87:
    return x87_register_is_emittable(insn) || x87_fn_is_emittable(insn) || x87_control_is_emittable(insn) ||
           (insn->x87 == kX86pX87InsnWait && insn->operands == 0) || x87_load_is_emittable(insn) ||
           x87_arith_is_emittable(insn) || x87_store_reg_is_emittable(insn) || x87_store_mem_is_emittable(insn) ||
           x87_compare_mem_is_emittable(insn) || x87_constant_is_emittable(insn) || x87_status_ax_is_emittable(insn) ||
           x87_clear_exceptions_is_emittable(insn);
  default:
    return 0;
  }
}

/* CMP and TEST compute a result only to derive flags from it. Emitting the
   store anyway would clobber a register the guest still expects to hold its
   original value -- and every flag assertion would still pass. */
/* ---- emitting ------------------------------------------------------------ */

/* The block budget reserves WORST_CASE_INSN_BYTES per instruction, so one that
   emits more could overrun a buffer the budget said had room. Measured, not
   assumed: the instruction that ended before `pc` is refused as an internal
   defect, and the next one starts at the current length. */
static int insn_fit(const X86pEmit *e, size_t *insn_start, uint32_t pc, char *reason, unsigned reason_len) {
  if (e->len - *insn_start > WORST_CASE_INSN_BYTES) {
    say(reason,
        reason_len,
        "internal: the instruction before %08X emitted %zu bytes, past the %u-byte worst case",
        pc,
        e->len - *insn_start,
        (unsigned)WORST_CASE_INSN_BYTES);
    return 0;
  }
  *insn_start = e->len;
  return 1;
}

static void emit_mem_prepare(BlockCtx *c, const X86pOperand *o, uint32_t insn_eip) {
  emit_mem_prepare_w(c, o, insn_eip, 4);
}

/*
 * Whether an instruction leaves the host-stack x87 mirror in place.
 *
 * The mirror must be empty wherever the block calls out or exits, since a call
 * finds the host x87 stack empty under both ABIs and the next block starts
 * with none. It need not be empty anywhere else: nothing but an x87 form reads
 * the register file between two instructions, and a memory fault stores and
 * empties the host stack in the shared stub. So the instructions whose
 * emitters never call keep it, and the integer work between two x87 forms no
 * longer costs the second one a reload. INC and DEC are among them because
 * their one possible call, the carry-in after an unknown predecessor, empties
 * the mirror itself before it calls.
 */
static int keeps_x87_mirror(const X86pInsn *insn) {
  X86pHostAlu host;
  X86pFlagKind kind;
  int writes_dest;
  switch (insn->op) {
  case kX86pInsnX87:
  case kX86pInsnMov:
  case kX86pInsnMovzx:
  case kX86pInsnMovsx:
  case kX86pInsnLea:
  case kX86pInsnNop:
  case kX86pInsnPush:
  case kX86pInsnPop:
  case kX86pInsnXchg:
  case kX86pInsnCdq:
  case kX86pInsnLeave:
  case kX86pInsnAluUnary:
    return 1;
  case kX86pInsnAlu:
    return inline_alu_shape(insn->alu, &host, &kind, &writes_dest) || is_inline_shift(insn->alu);
  default:
    return 0;
  }
}

/*
 * DEAD FLAG STORE ELIMINATION.
 *
 * Most guest arithmetic never has its flags read: `add / add / cmp / jl` writes
 * three flag tuples and only the last one matters. This scans forward from the
 * instruction AFTER `pc` and returns 1 when the tuple that instruction wrote is
 * provably overwritten before anything can observe it -- so its six stores (and
 * its carry-in computation) can be skipped entirely.
 *
 * It only says "dead" for a later full-width inlined ALU flag write (or NEG)
 * with register/immediate operands: that overwrites every EFLAGS bit and cannot
 * fault. It stops -- conservatively "not dead" -- at the first thing that could
 * read flags (Jcc, INC/DEC which preserve CF, ADC/SBB, a helper), could fault
 * mid-block and expose a stale tuple (any memory operand), or
 * ends the block (branch, ret, unsupported, interception point, unmapped).
 * Register moves and NOPs are transparent and scanned through.
 *
 * Safety of the carry-in: the block's LAST flag writer is never dead (the scan
 * from it hits the block boundary), so it always stores. If ITS predecessor was
 * elided, its carry_in is computed from an older kind -- but a wrong carry_in is
 * only observable for kind Inc/Dec, and the predecessor of an Inc/Dec is never
 * elided (this scan returns 0 at Inc/Dec). cpu_compare.c states the matching
 * rule for the differential.
 *
 * The killer must be an instruction this block will ACTUALLY EMIT: `count` and
 * the remaining code budget cut the scan short exactly where the emit loop
 * would stop, so a store is never dropped on the strength of a successor that
 * ends up in the next block instead.
 */
static int flag_write_is_dead(const X86pMem *mem,
                              uint32_t pc,
                              uint32_t eip,
                              X86pJitBoundaryFn boundary,
                              void *boundary_user,
                              uint32_t count,
                              size_t code_len,
                              size_t code_cap) {
  int step;
  for (step = 0; step < 8; step++) {
    uint8_t bytes[X86P_MAX_INSN_LEN];
    uint32_t avail = 0;
    uint32_t i;
    X86pInsn insn;

    /* Would the emit loop still be running when it reached this instruction? */
    if (count + 1u + (uint32_t)step >= MAX_INSNS) {
      return 0;
    }
    if (code_len + (size_t)(step + 2) * WORST_CASE_INSN_BYTES + EPILOGUE_BYTES > code_cap) {
      return 0;
    }

    for (i = 0; i < (uint32_t)X86P_MAX_INSN_LEN; i++) {
      uint32_t byte;
      if (!x86p_mem_read(mem, pc + i, 1, &byte)) {
        break;
      }
      bytes[i] = (uint8_t)byte;
      avail++;
    }
    if (avail == 0 || !x86p_decode(bytes, avail, &insn)) {
      return 0;
    }
    if (pc != eip && boundary && boundary(pc, boundary_user)) {
      return 0;
    }
    /* A later flag write is a valid killer only if the emit loop will reach
       and translate it. Shape-only checks below are intentionally narrower
       than the complete translation gate, so consulting them first could
       discard flags on the strength of an instruction the block then refuses. */
    if (!can_emit(&insn)) {
      return 0;
    }

    if (insn.op == (uint8_t)kX86pInsnAlu) {
      X86pHostAlu host;
      X86pFlagKind kind;
      int writes_dest;
      if (inline_alu_shape(insn.alu, &host, &kind, &writes_dest) && insn.operand[0].kind != kX86pOperandMem &&
          insn.operand[1].kind != kX86pOperandMem) {
        return 1;
      }
      if (is_inline_shift(insn.alu) && insn.operand[0].kind != kX86pOperandMem &&
          insn.operand[1].kind == kX86pOperandImm && (insn.operand[1].imm & 0x1Fu) != 0u) {
        return 1; /* a nonzero constant count rewrites the whole tuple */
      }
      return 0; /* CL shift / rotate / ADC / SBB / memory ALU: may keep, read CF, or fault */
    }
    if (insn.op == (uint8_t)kX86pInsnAluUnary) {
      if (insn.alu == (uint8_t)kX86pAluNeg && insn.operand[0].kind != kX86pOperandMem) {
        return 1; /* NEG rewrites every flag */
      }
      if (insn.alu == (uint8_t)kX86pAluNot && insn.operand[0].kind != kX86pOperandMem) {
        pc += insn.length; /* NOT writes no flags -- transparent */
        continue;
      }
      return 0; /* INC / DEC preserve (read) CF; memory forms can fault */
    }
    if (insn.op == (uint8_t)kX86pInsnNop) {
      pc += insn.length;
      continue;
    }
    if (insn.op == (uint8_t)kX86pInsnMov && insn.operand[0].kind == kX86pOperandReg &&
        insn.operand[1].kind != kX86pOperandMem) {
      pc += insn.length; /* reg <- reg/imm : no flags, no fault */
      continue;
    }
    return 0;
  }
  return 0;
}

/*
 * PUSH and POP.
 *
 * The stack is ordinary guest memory, so both go through the same bounds check
 * and the same fault stub as any other access. What is specific to them is the
 * ORDER, and in both cases it is the order the architecture specifies:
 *
 *   - PUSH reads its operand BEFORE moving ESP, so `PUSH ESP` stores the old
 *     value, and it moves ESP only AFTER the store has been accepted, so a
 *     faulting push leaves the stack pointer where it was rather than past a
 *     value it never wrote. The second rule is invisible until something
 *     faults, and then it corrupts every frame above it.
 *   - POP advances ESP BEFORE writing the destination, so `POP ESP` ends
 *     holding the popped value rather than the adjusted pointer, and a memory
 *     destination is addressed from the ALREADY advanced ESP.
 */
/*
 * LEA: the address, never the contents.
 *
 * NO BOUNDS CHECK, and that is the whole point of the instruction rather than
 * an omission. LEA is the one memory-operand form that does not access memory,
 * so checking it would fault on an address the guest deliberately never
 * touched -- and guest code really does use it as a three-input adder on values
 * that are not addresses at all.
 */
static void emit_lea(BlockCtx *c, const X86pInsn *insn) {
  emit_address_parts(c, &insn->operand[1]);
  gpr_store(c, insn->operand[0].reg, EA_REG, 4);
}

/* LEAVE is ordered state transition, not a MOV followed by an ordinary POP:
   ESP becomes EBP before the stack read, and remains there if that read faults.
   Only a successful read advances ESP and replaces EBP. */
static void emit_leave(BlockCtx *c, uint32_t insn_eip) {
  gpr_load(c, EA_REG, kX86pEbp, 4);
  gpr_store(c, kX86pEsp, EA_REG, 4);
  note_fault(c, emit_bounds_check(c->e, &c->plan, insn_eip, 4));
  emit_host_pointer(c->e, &c->plan);
  x86p_emit_load32(c->e, kX64Rsi, HOSTPTR_REG, 0);
  x86p_emit_alu_r32_imm32(c->e, kX64Add, EA_REG, 4u);
  gpr_store(c, kX86pEsp, EA_REG, 4);
  gpr_store(c, kX86pEbp, kX64Rsi, 4);
}

static void emit_cdq(BlockCtx *c) {
  gpr_load(c, kX64Rax, kX86pEax, 4);
  x86p_emit_shift_r32_imm8(c->e, kX64Sar, kX64Rax, 31u);
  gpr_store(c, kX86pEdx, kX64Rax, 4);
}

/* This helper owns only one already-decoded operation's value semantics. It
   cannot fetch, decode, dispatch, or fall back to the test-only interpreter. */
static int jit_div32(X86pCpu *cpu, uint32_t divisor, uint32_t signed_divide) {
  uint32_t quotient = 0u;
  uint32_t remainder = 0u;

  int ok = signed_divide
               ? x86p_alu_idiv(cpu->reg[kX86pEdx], cpu->reg[kX86pEax], divisor, 4, &quotient, &remainder, &cpu->flags)
               : x86p_alu_div(cpu->reg[kX86pEdx], cpu->reg[kX86pEax], divisor, 4, &quotient, &remainder, &cpu->flags);
  if (!ok) {
    return 0;
  }
  cpu->reg[kX86pEax] = quotient;
  cpu->reg[kX86pEdx] = remainder;
  return 1;
}

static void emit_div32(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip, int signed_divide) {
  const X86pOperand *divisor = &insn->operand[0];
  X86pEmitSite failed;

  if (divisor->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, divisor, insn_eip, 4);
    x86p_emit_load32(c->e, X86P_JIT_HOST_ARG1, HOSTPTR_REG, 0);
  } else {
    gpr_load(c, X86P_JIT_HOST_ARG1, divisor->reg, 4);
  }
  x86p_emit_mov_r64_r64(c->e, X86P_JIT_HOST_ARG0, CPU_REG);
  x86p_emit_mov_r32_imm32(c->e, X86P_JIT_HOST_ARG2, (uint32_t)signed_divide);
  x86p_emit_mov_r64_imm64(c->e, kX64Rax, (uint64_t)(uintptr_t)&jit_div32);
  x86p_emit_call_r64(c->e, kX64Rax);
  x86p_emit_test_r32_r32(c->e, kX64Rax, kX64Rax);
  x86p_emit_mov_r32_imm32(c->e, FAULTPC_REG, insn_eip);
  failed = x86p_emit_jcc_rel32(c->e, (unsigned)kX86pCondZ);
  note_divide_fault(c, failed);
}

/* The shipping emitter calls the canonical widening-multiply semantics after
   capturing the explicit operand. Capturing first is essential for MUL EAX
   and MUL EDX: both implicit destination registers are overwritten. */
static void jit_imul32(X86pCpu *cpu, uint32_t destination, uint32_t left, uint32_t right) {
  uint32_t low = 0u;
  uint32_t high = 0u;

  x86p_alu_imul(left, right, 4, &low, &high, &cpu->flags);
  cpu->reg[destination] = low;
}

static void emit_imul32(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  const X86pOperand *destination = &insn->operand[0];
  const X86pOperand *source = &insn->operand[1];

  if (insn->operands == 2) {
    if (source->kind == kX86pOperandMem) {
      emit_mem_prepare_w(c, source, insn_eip, 4);
      x86p_emit_load32(c->e, X86P_JIT_HOST_ARG3, HOSTPTR_REG, 0);
    } else {
      gpr_load(c, X86P_JIT_HOST_ARG3, source->reg, 4);
    }
    gpr_load(c, X86P_JIT_HOST_ARG2, destination->reg, 4);
  } else {
    if (source->kind == kX86pOperandMem) {
      emit_mem_prepare_w(c, source, insn_eip, 4);
      x86p_emit_load32(c->e, X86P_JIT_HOST_ARG2, HOSTPTR_REG, 0);
    } else {
      gpr_load(c, X86P_JIT_HOST_ARG2, source->reg, 4);
    }
    x86p_emit_mov_r32_imm32(c->e, X86P_JIT_HOST_ARG3, insn->operand[2].imm);
  }
  x86p_emit_mov_r64_r64(c->e, X86P_JIT_HOST_ARG0, CPU_REG);
  x86p_emit_mov_r32_imm32(c->e, X86P_JIT_HOST_ARG1, destination->reg);
  x86p_emit_mov_r64_imm64(c->e, kX64Rax, (uint64_t)(uintptr_t)&jit_imul32);
  x86p_emit_call_r64(c->e, kX64Rax);
}

static int jit_string(X86pCpu *cpu, const X86pMem *mem, uint32_t operation, uint32_t repeat, uint32_t width) {
  X86pInsn insn;

  memset(&insn, 0, sizeof insn);
  insn.op = (uint8_t)kX86pInsnString;
  insn.str = (uint8_t)operation;
  insn.rep = (uint8_t)repeat;
  insn.str_width = (uint8_t)width;
  return x86p_string_execute(cpu, mem, &insn, NULL) == kX86pStringOk;
}

static void emit_string(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  X86pEmitSite failed;

  x86p_emit_mov_r64_r64(c->e, X86P_JIT_HOST_ARG0, CPU_REG);
  x86p_emit_mov_r64_imm64(c->e, X86P_JIT_HOST_ARG1, (uint64_t)(uintptr_t)c->mem);
  x86p_emit_mov_r32_imm32(c->e, X86P_JIT_HOST_ARG2, insn->str);
  x86p_emit_mov_r32_imm32(c->e, X86P_JIT_HOST_ARG3, insn->rep);
  x86p_jit_abi_emit_arg32_imm(c->e, X86P_JIT_HOST_ABI, 4u, insn->str_width);
  x86p_emit_mov_r64_imm64(c->e, kX64Rax, (uint64_t)(uintptr_t)&jit_string);
  x86p_emit_call_r64(c->e, kX64Rax);
  x86p_emit_test_r32_r32(c->e, kX64Rax, kX64Rax);
  x86p_emit_mov_r32_imm32(c->e, FAULTPC_REG, insn_eip);
  failed = x86p_emit_jcc_rel32(c->e, (unsigned)kX86pCondZ);
  note_fault(c, failed);
}

/* Push whatever is in RSI. The one implementation of the stack store, shared by
   PUSH and by CALL's return address, so the fault ordering above is stated once
   rather than reproduced next to each caller. */
static void emit_push_rsi(BlockCtx *c, uint32_t insn_eip) {
  gpr_load(c, EA_REG, kX86pEsp, 4);
  x86p_emit_alu_r32_imm32(c->e, kX64Sub, EA_REG, 4u);
  note_fault(c, emit_bounds_check(c->e, &c->plan, insn_eip, 4));
  emit_host_pointer(c->e, &c->plan);
  x86p_emit_store32(c->e, HOSTPTR_REG, 0, kX64Rsi);
  /* The bounds check preserves EA_REG -- it copies into ADDR_TMP -- so the new
     ESP is still here and needs no second computation. */
  gpr_store(c, kX86pEsp, EA_REG, 4);
}

static void emit_push(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  const X86pOperand *o = &insn->operand[0];

  if (o->kind == kX86pOperandImm) {
    x86p_emit_mov_r32_imm32(c->e, kX64Rsi, x86p_sign_extend(o->imm, o->size));
  } else if (o->kind == kX86pOperandMem) {
    emit_mem_prepare(c, o, insn_eip);
    x86p_emit_load32(c->e, kX64Rsi, HOSTPTR_REG, 0);
  } else {
    gpr_load(c, kX64Rsi, o->reg, 4);
  }

  emit_push_rsi(c, insn_eip);
}

/* Pop whatever is at [ESP] into RSI and advance ESP past it, the one
   implementation of the stack load shared by POP and POPFD (mirrors
   emit_push_rsi below the corresponding PUSH). The caller decides where the
   popped value in RSI ends up. */
static void emit_pop_rsi(BlockCtx *c, uint32_t insn_eip) {
  gpr_load(c, EA_REG, kX86pEsp, 4);
  note_fault(c, emit_bounds_check(c->e, &c->plan, insn_eip, 4));
  emit_host_pointer(c->e, &c->plan);
  x86p_emit_load32(c->e, kX64Rsi, HOSTPTR_REG, 0);

  x86p_emit_mov_r32_r32(c->e, kX64Rdx, EA_REG);
  x86p_emit_alu_r32_imm32(c->e, kX64Add, kX64Rdx, 4u);
  gpr_store(c, kX86pEsp, kX64Rdx, 4);
}

static void emit_pop(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  const X86pOperand *o = &insn->operand[0];

  emit_pop_rsi(c, insn_eip);

  if (o->kind == kX86pOperandMem) {
    emit_mem_prepare(c, o, insn_eip);
    x86p_emit_store32(c->e, HOSTPTR_REG, 0, kX64Rsi);
    return;
  }
  gpr_store(c, o->reg, kX64Rsi, 4);
}

/*
 * PUSHFD and POPFD are where the two representations of EFLAGS meet: the six
 * arithmetic flags are derived from the lazy (kind, a, b, r) record, and DF is
 * held apart because nothing computes it (see X86pCpu::df). Both halves cross
 * through one call each, matching exec.c's interpreter path exactly rather
 * than reproducing that merge as a second authority here.
 */
static uint32_t jit_pushfd_value(X86pCpu *cpu) {
  return x86p_eflags(&cpu->flags) | (cpu->df ? X86P_DF : 0u);
}

static void jit_popfd_apply(X86pCpu *cpu, uint32_t v) {
  x86p_flags_set_explicit(&cpu->flags, v);
  cpu->df = (v & X86P_DF) ? 1u : 0u;
}

static void emit_pushfd(BlockCtx *c, uint32_t insn_eip) {
  x86p_emit_mov_r64_r64(c->e, X86P_JIT_HOST_ARG0, CPU_REG);
  x86p_emit_mov_r64_imm64(c->e, kX64Rax, (uint64_t)(uintptr_t)&jit_pushfd_value);
  x86p_emit_call_r64(c->e, kX64Rax);
  x86p_emit_mov_r32_r32(c->e, kX64Rsi, kX64Rax);
  emit_push_rsi(c, insn_eip);
}

static void emit_popfd(BlockCtx *c, uint32_t insn_eip) {
  emit_pop_rsi(c, insn_eip);
  x86p_emit_mov_r32_r32(c->e, X86P_JIT_HOST_ARG1, kX64Rsi);
  x86p_emit_mov_r64_r64(c->e, X86P_JIT_HOST_ARG0, CPU_REG);
  x86p_emit_mov_r64_imm64(c->e, kX64Rax, (uint64_t)(uintptr_t)&jit_popfd_apply);
  x86p_emit_call_r64(c->e, kX64Rax);
}

static void emit_mov(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  const X86pOperand *dst = &insn->operand[0];
  const X86pOperand *src = &insn->operand[1];

  const int w = dst->size;

  if (dst->kind == kX86pOperandMem) {
    /* Prepare the destination FIRST, then materialise the value: reading the
       source cannot disturb HOSTPTR_REG, but computing an address can disturb
       a value already sitting in a scratch register. */
    emit_mem_prepare_w(c, dst, insn_eip, w);
    if (src->kind == kX86pOperandImm) {
      emit_store_imm_w(c->e, HOSTPTR_REG, 0, src->imm, w);
      return;
    }
    gpr_load(c, kX64Rax, src->reg, w);
    emit_store_w(c->e, HOSTPTR_REG, 0, kX64Rax, w);
    return;
  }

  if (src->kind == kX86pOperandImm) {
    gpr_store_imm(c, dst->reg, src->imm, w);
    return;
  }
  if (src->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, src, insn_eip, w);
    emit_load_w(c->e, kX64Rax, HOSTPTR_REG, 0, w);
  } else {
    gpr_load(c, kX64Rax, src->reg, w);
  }
  gpr_store(c, dst->reg, kX64Rax, w);
}

static void emit_xchg32(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  const X86pOperand *first = &insn->operand[0];
  const X86pOperand *second = &insn->operand[1];
  const X86pOperand *memory = first->kind == kX86pOperandMem ? first : second;
  const X86pOperand *reg = first->kind == kX86pOperandReg ? first : second;

  if (memory->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, memory, insn_eip, 4);
    x86p_emit_load32(c->e, kX64Rax, HOSTPTR_REG, 0);
    gpr_load(c, kX64Rsi, reg->reg, 4);
    x86p_emit_store32(c->e, HOSTPTR_REG, 0, kX64Rsi);
    gpr_store(c, reg->reg, kX64Rax, 4);
    return;
  }

  gpr_load(c, kX64Rax, first->reg, 4);
  gpr_load(c, kX64Rsi, second->reg, 4);
  gpr_store(c, first->reg, kX64Rsi, 4);
  gpr_store(c, second->reg, kX64Rax, 4);
}

/*
 * MOVZX / MOVSX: a narrow source widened into a wider register.
 *
 * The zero-extended load already exists (emit_load_w, w == 1 or 2), so MOVZX is
 * that load then a store at the destination width. MOVSX is the same load with
 * the sign bit propagated by a shl/sar pair -- host MOVSX would be one
 * instruction, but the pair needs no new emitter and the result is identical at
 * 32 bits. The destination is always a register here (can_emit gate).
 */
static void emit_movx(BlockCtx *c, const X86pInsn *insn, int is_signed, uint32_t insn_eip) {
  const X86pOperand *dst = &insn->operand[0];
  const X86pOperand *src = &insn->operand[1];
  const int sw = src->size; /* 1 or 2 */
  const int dw = dst->size; /* 2 or 4 */

  if (src->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, src, insn_eip, sw);
    emit_load_w(c->e, kX64Rax, HOSTPTR_REG, 0, sw);
  } else {
    gpr_load(c, kX64Rax, src->reg, sw);
  }

  if (is_signed) {
    const uint8_t fill = (uint8_t)(32 - 8 * sw);
    x86p_emit_shift_r32_imm8(c->e, kX64Shl, kX64Rax, fill);
    x86p_emit_shift_r32_imm8(c->e, kX64Sar, kX64Rax, fill);
  }

  gpr_store(c, dst->reg, kX64Rax, dw);
}

/*
 * Prologue: preserve every nonvolatile register this emitter uses, park the
 * X86pCpu pointer in RBX, align calls, and reserve Win64 shadow/stack-argument
 * space when that is the host ABI. jit_x64_abi.h owns the exact sequence. No
 * helper may decode or dispatch a guest instruction: an instruction without
 * an emitter is a named refusal.
 */
static void emit_prologue(X86pEmit *e) {
  x86p_jit_abi_emit_enter(e, X86P_JIT_HOST_ABI, CPU_REG);
}

static void emit_restore_host_frame(X86pEmit *e) {
  x86p_jit_abi_emit_leave(e, X86P_JIT_HOST_ABI, CPU_REG);
}

/*
 * CALL and RET: control transfers the block can COMPLETE rather than refuse.
 *
 * Both end the block -- the target is another block -- but ending it with
 * kX86pJitExitBlockEnd and the right EIP is a different thing from ending it
 * with kX86pJitExitUnsupported. The second refuses the run; the first leaves
 * the dispatcher a plain address to look up. On
 * this corpus that is the difference between 17,640 blocks that must fall back
 * and 17,640 that do not.
 *
 * Indirect forms stay out: a CALL through a register or memory has no target
 * until the block runs, so it belongs to the block cache, not to a constant
 * folded in here.
 */
static void emit_call_rel(BlockCtx *c, uint32_t return_eip, uint32_t target, uint32_t insn_eip) {
  x86p_emit_mov_r32_imm32(c->e, kX64Rsi, return_eip);
  emit_push_rsi(c, insn_eip);
  emit_exit(c, target);
}

/*
 * The indirect forms. TARGET_REG is read before anything else touches memory,
 * because CALL [ESP+4] must take its target from the stack as it stands and not
 * from the stack after the return address has been pushed onto it.
 */
#define TARGET_REG kX64Rdx

static void emit_read_branch_target(BlockCtx *c, const X86pOperand *o, uint32_t insn_eip) {
  if (o->kind == kX86pOperandMem) {
    emit_mem_prepare(c, o, insn_eip);
    x86p_emit_load32(c->e, TARGET_REG, HOSTPTR_REG, 0);
    return;
  }
  gpr_load(c, TARGET_REG, o->reg, 4);
}

static void emit_jmp_indirect(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  emit_read_branch_target(c, &insn->operand[0], insn_eip);
  emit_exit_from(c, TARGET_REG);
}

static void emit_call_indirect(BlockCtx *c, const X86pInsn *insn, uint32_t return_eip, uint32_t insn_eip) {
  emit_read_branch_target(c, &insn->operand[0], insn_eip);
  x86p_emit_mov_r32_imm32(c->e, kX64Rsi, return_eip);
  emit_push_rsi(c, insn_eip);
  emit_exit_from(c, TARGET_REG);
}

/* `release` is RET imm16's argument count, applied AFTER the pop because the
   immediate counts bytes ABOVE the return address. */
static void emit_ret(BlockCtx *c, uint32_t release, uint32_t insn_eip) {
  gpr_load(c, EA_REG, kX86pEsp, 4);
  note_fault(c, emit_bounds_check(c->e, &c->plan, insn_eip, 4));
  emit_host_pointer(c->e, &c->plan);
  x86p_emit_load32(c->e, kX64Rsi, HOSTPTR_REG, 0);

  x86p_emit_mov_r32_r32(c->e, kX64Rdx, EA_REG);
  x86p_emit_alu_r32_imm32(c->e, kX64Add, kX64Rdx, 4u + release);
  gpr_store(c, kX86pEsp, kX64Rdx, 4);

  emit_exit_from(c, kX64Rsi);
}

/* ---- translation --------------------------------------------------------- */

X86pJitStatus x86p_jit_translate(const X86pMem *mem,
                                 uint32_t eip,
                                 void *code,
                                 size_t code_cap,
                                 X86pJitBlock *out,
                                 char *reason,
                                 unsigned reason_len) {
  return x86p_jit_translate_bounded(mem, eip, code, code_cap, NULL, NULL, NULL, out, reason, reason_len);
}

X86pJitStatus x86p_jit_translate_bounded(const X86pMem *mem,
                                         uint32_t eip,
                                         void *code,
                                         size_t code_cap,
                                         X86pJitBoundaryFn boundary,
                                         void *boundary_user,
                                         X86pJitChain *chain,
                                         X86pJitBlock *out,
                                         char *reason,
                                         unsigned reason_len) {
  X86pEmit e;
  BlockCtx ctx;
  uint32_t pc = eip;
  uint32_t count = 0;
  X86pJitExit exit = kX86pJitExitBlockEnd;
  size_t insn_start;
  size_t tail_start;
  const char *stopper = NULL;
  int terminated = 0; /* a branch already emitted the exit */
  /* The flag kind the last emitted instruction recorded, or -1 when the
     predecessor is whatever ran before this block. */
  int last_kind = -1;
  /* The operand width, in bytes, of the operation that recorded last_kind, or
     -1 when unknown. Together they choose an inline condition lowering. */
  int last_w = -1;

  if (!mem || !out || !code || mem->sparse) {
    say(reason, reason_len, mem && mem->sparse ? "sparse memory requires the WASM backend" : "null argument");
    return mem && mem->sparse ? kX86pJitUnsupportedAtEntry : kX86pJitOutOfSpace;
  }
  memset(out, 0, sizeof *out);

  if (!x86p_jit_available()) {
    say(reason, reason_len, "no x86-64 backend in this build; host is not x86-64");
    return kX86pJitOutOfSpace;
  }

  x86p_emit_init(&e, code, code_cap);
  memset(&ctx, 0, sizeof ctx);
  ctx.e = &e;
  ctx.mem = mem;
  ctx.host_state = x86p_jit_host_state();
  ctx.chain = x86p_jit_chain_entry_offset() != 0u ? chain : NULL;
  ctx.entry_eip = eip;
  out->host_state = ctx.host_state;
  ctx.plan.host = (uint64_t)(uintptr_t)mem->host;
  ctx.plan.lo = mem->lo;
  ctx.plan.size = mem->size;
  emit_prologue(&e);
  if (e.len > X86P_JIT_PROLOGUE_BYTES) {
    say(reason, reason_len, "internal: the prologue emitted %zu bytes, past %u", e.len, X86P_JIT_PROLOGUE_BYTES);
    return kX86pJitOutOfSpace;
  }
  insn_start = e.len;

  for (;;) {
    uint8_t bytes[X86P_MAX_INSN_LEN];
    X86pInsn insn;
    uint32_t avail;
    uint32_t i;

    if (count >= MAX_INSNS) {
      break;
    }
    /* Stop while there is still certainly room for this instruction AND the
       epilogue. Discovering the overflow afterwards would mean discarding a
       block that was nearly finished, and worse, a caller that ignored the
       flag would run a block with no RET. */
    if (!insn_fit(&e, &insn_start, pc, reason, reason_len)) {
      return kX86pJitOutOfSpace;
    }
    gpr_check(&ctx); /* the checked build's, charged to no instruction */
    insn_start = e.len;
    if (e.len + WORST_CASE_INSN_BYTES + EPILOGUE_BYTES > code_cap) {
      break;
    }

    /* Fetch. A partial fetch at the end of the mapping is not a decode
       failure -- it is a fetch fault, and only the first instruction can make
       the whole translation fail. */
    avail = 0;
    for (i = 0; i < (uint32_t)X86P_MAX_INSN_LEN; i++) {
      uint32_t byte;
      if (!x86p_mem_read(mem, pc + i, 1, &byte)) {
        break;
      }
      bytes[i] = (uint8_t)byte;
      avail++;
    }
    if (avail == 0) {
      if (count == 0) {
        say(reason, reason_len, "guest EIP %08X is not mapped", pc);
        return kX86pJitFetchFault;
      }
      break;
    }

    if (!x86p_decode(bytes, avail, &insn)) {
      if (count == 0) {
        say(reason, reason_len, "the bytes at %08X are not an instruction", pc);
        return kX86pJitDecodeFailed;
      }
      break;
    }

    /* Stop before an address the consumer intercepts: the dispatch loop checks
       its predicate only between blocks, so translating past one would run the
       original guest bytes where the consumer meant to take control. `pc != eip`
       because the block leader was already cleared by that same predicate. */
    if (pc != eip && boundary && boundary(pc, boundary_user)) {
      stopper = "consumer interception point";
      break;
    }

    if (!can_emit(&insn)) {
      if (count == 0) {
        say(reason, reason_len, "%s at %08X has no JIT emitter in this build", insn.mnemonic, pc);
        return kX86pJitUnsupportedAtEntry;
      }
      exit = kX86pJitExitUnsupported;
      stopper = insn.mnemonic;
      break;
    }

    x87_cache_before(&ctx, &insn, keeps_x87_mirror(&insn));

    if (insn.op == (uint8_t)kX86pInsnCall || insn.op == (uint8_t)kX86pInsnRet || is_indirect_branch(&insn)) {
      uint32_t next = pc + insn.length;
      if (is_indirect_branch(&insn)) {
        emit_jmp_indirect(&ctx, &insn, pc);
      } else if (insn.op != (uint8_t)kX86pInsnCall) {
        emit_ret(&ctx, insn.operands == 1 ? insn.operand[0].imm : 0u, pc);
      } else if (insn.operand[0].kind == kX86pOperandImm) {
        emit_call_rel(&ctx, next, next + insn.operand[0].imm, pc);
      } else {
        emit_call_indirect(&ctx, &insn, next, pc);
      }
      pc = next;
      count++;
      terminated = 1;
      break;
    }

    if (is_relative_branch(&insn)) {
      /* A branch ENDS the block -- that is what makes it a basic block. The
         target is relative to the NEXT instruction, and the addition wraps at
         32 bits exactly as the guest's does. */
      uint32_t next = pc + insn.length;
      uint32_t target = next + insn.operand[0].imm;
      if (insn.op == (uint8_t)kX86pInsnJmp) {
        emit_exit(&ctx, target);
      } else if (insn.op != kX86pInsnJcc) {
        emit_loop(&ctx, &insn, target, next);
      } else {
        x86p_x64_emit_jcc(&ctx, insn.cond, target, next, last_kind, last_w);
      }
      pc = next;
      count++;
      terminated = 1;
      break;
    }

    switch (insn.op) {
    case kX86pInsnNop:
      break;
    case kX86pInsnShld:
    case kX86pInsnShrd:
      emit_double_shift(&ctx, &insn, pc);
      last_kind = -1;
      last_w = -1;
      break;
    case kX86pInsnSimd:
      emit_simd_bits(&ctx, &insn, pc);
      break;
    case kX86pInsnMov:
      emit_mov(&ctx, &insn, pc);
      break;
    case kX86pInsnMovzx:
      emit_movx(&ctx, &insn, 0, pc);
      break;
    case kX86pInsnMovsx:
      emit_movx(&ctx, &insn, 1, pc);
      break;
    case kX86pInsnXchg:
      emit_xchg32(&ctx, &insn, pc);
      break;
    case kX86pInsnSetcc:
      x86p_x64_emit_setcc(&ctx, &insn, pc, last_kind, last_w);
      break;
    case kX86pInsnAluUnary: {
      int dead = flag_write_is_dead(mem, pc + insn.length, eip, boundary, boundary_user, count, e.len, code_cap);
      int k = emit_alu_unary_inline(&ctx, &insn, last_kind, dead, pc);
      /* NOT records no flags, so the PREVIOUS instruction is still the
         predecessor for the next one's carry-in -- and so is an INC/DEC/NEG
         whose tuple was elided: the last stored kind is what memory holds. */
      if (k >= 0 && !dead) {
        last_kind = k;
        last_w = insn.operand[0].size;
      }
      break;
    }
    case kX86pInsnLea:
      emit_lea(&ctx, &insn);
      break;
    case kX86pInsnLeave:
      emit_leave(&ctx, pc);
      break;
    case kX86pInsnCdq:
      emit_cdq(&ctx);
      break;
    case kX86pInsnDiv:
      emit_div32(&ctx, &insn, pc, 0);
      break;
    case kX86pInsnIdiv:
      emit_div32(&ctx, &insn, pc, 1);
      break;
    case kX86pInsnMul:
      emit_mul32(&ctx, &insn, pc);
      /* The semantic owner materialises CF/OF into explicit flags. */
      last_kind = -1;
      last_w = -1;
      break;
    case kX86pInsnImul:
      if (insn.operands == 1) {
        emit_mul32(&ctx, &insn, pc);
      } else {
        emit_imul32(&ctx, &insn, pc);
      }
      /* The semantic owner materialises CF/OF into explicit flags. */
      last_kind = -1;
      last_w = -1;
      break;
    case kX86pInsnString:
      emit_string(&ctx, &insn, pc);
      if (insn.str == (uint8_t)kX86pStringScas || insn.str == (uint8_t)kX86pStringCmps) {
        last_kind = -1;
        last_w = -1;
      }
      break;
    case kX86pInsnX87:
      emit_x87(&ctx, &insn, pc);
      break;
    case kX86pInsnPush:
      emit_push(&ctx, &insn, pc);
      break;
    case kX86pInsnPop:
      emit_pop(&ctx, &insn, pc);
      break;
    case kX86pInsnRdtsc:
    case kX86pInsnCpuid:
    case kX86pInsnCld:
    case kX86pInsnStd:
    case kX86pInsnSahf:
    case kX86pInsnLahf:
      emit_cpu_transfer(&ctx, insn.op);
      if (insn.op == kX86pInsnSahf) {
        last_kind = (int)kX86pFlagsExplicit;
        last_w = -1;
      }
      break;
    case kX86pInsnPushfd:
      emit_pushfd(&ctx, pc);
      break;
    case kX86pInsnPopfd:
      emit_popfd(&ctx, pc);
      /* x86p_flags_set_explicit unconditionally records Explicit, exactly
         like ADC/SBB below -- the next carry-in is statically known rather
         than worth a helper call to ask. */
      last_kind = (int)kX86pFlagsExplicit;
      last_w = -1;
      break;
    case kX86pInsnAlu: {
      X86pHostAlu host;
      X86pFlagKind kind;
      int writes_dest;
      if (inline_alu_shape(insn.alu, &host, &kind, &writes_dest)) {
        int dead = flag_write_is_dead(mem, pc + insn.length, eip, boundary, boundary_user, count, e.len, code_cap);
        emit_alu_inline(&ctx, &insn, host, kind, writes_dest, last_kind, dead, pc);
        /* A dead tuple was not stored, so the predecessor for the next
           carry-in is still the last kind actually written to memory. */
        if (!dead) {
          last_kind = (int)kind;
          last_w = insn.operand[0].size;
        }
      } else if (is_inline_shift(insn.alu)) {
        int dead = flag_write_is_dead(mem, pc + insn.length, eip, boundary, boundary_user, count, e.len, code_cap);
        int k = emit_shift_inline(&ctx, &insn, dead, pc);
        if (k == SHIFT_FLAGS_UNKNOWN) {
          last_kind = -1;
          last_w = -1;
        } else if (k != SHIFT_FLAGS_UNCHANGED && !dead) {
          last_kind = k;
          last_w = insn.operand[0].size;
        }
      } else {
        emit_alu_helper(&ctx, &insn, pc);
        if (insn.alu >= (uint8_t)kX86pAluShl && insn.alu <= (uint8_t)kX86pAluRcr) {
          /* A rotate's recorded state depends on its COUNT, which is not
             known until the block runs: a zero count writes no flags, leaving
             whatever was there. Genuinely unknown, so the next carry-in asks
             the real function. */
          last_kind = -1;
          last_w = -1;
        } else {
          /* x86p_alu records Explicit for ADC and SBB, unconditionally -- so
             the next instruction's predecessor IS statically known, and
             treating it as unknown cost a helper call per ADC in every
             block. */
          last_kind = (int)kX86pFlagsExplicit;
          last_w = -1;
        }
      }
      break;
    }
    default:
      /* can_emit() said yes and this switch has no arm: that is a defect in
         this file, not in the guest program, and it must not silently emit
         nothing. */
      say(reason, reason_len, "internal: %s passed can_emit but has no emitter", insn.mnemonic);
      return kX86pJitOutOfSpace;
    }

    pc += insn.length;
    count++;
  }

  if (!insn_fit(&e, &insn_start, pc, reason, reason_len)) {
    return kX86pJitOutOfSpace;
  }
  tail_start = e.len;
  if (!terminated) {
    x87_cache_flush(&ctx);
    emit_block_end(&ctx, pc, exit);
  }

  x86p_x64_emit_cond_slow_path(&ctx);

  /*
   * The shared fault stub, AFTER the normal return so it is never fallen into.
   * FAULTPC_REG holds the guest EIP of whichever access failed, set immediately
   * before each bounds check -- so EIP lands ON the faulting instruction and a
   * caller can deliver the correct guest fault.
   */
  if (ctx.nfaults) {
    unsigned f;
    for (f = 0; f < ctx.nfaults; f++) {
      x86p_emit_bind(&e, ctx.faults[f]);
    }
    x87_cache_spill(&ctx);
    x86p_emit_store32(&e, CPU_REG, eip_off(), FAULTPC_REG);
    x86p_emit_mov_r32_imm32(&e, kX64Rax, (uint32_t)kX86pJitExitMemoryFault);
    emit_restore_host_frame(&e);
    x86p_emit_ret(&e);
  }

  if (ctx.ndivide_faults) {
    unsigned f;
    for (f = 0; f < ctx.ndivide_faults; f++) {
      x86p_emit_bind(&e, ctx.divide_faults[f]);
    }
    x86p_emit_store32(&e, CPU_REG, eip_off(), FAULTPC_REG);
    x86p_emit_mov_r32_imm32(&e, kX64Rax, (uint32_t)kX86pJitExitDivideError);
    emit_restore_host_frame(&e);
    x86p_emit_ret(&e);
  }
  emit_tail_routines(&ctx);

  if (e.len - tail_start > EPILOGUE_BYTES) {
    say(reason,
        reason_len,
        "internal: the tail of the block at %08X emitted %zu bytes, past the %u-byte budget",
        eip,
        e.len - tail_start,
        (unsigned)EPILOGUE_BYTES);
    return kX86pJitOutOfSpace;
  }

  if (!x86p_emit_sites_bound(&e)) {
    /* An unbound forward jump carries whatever displacement the buffer held --
       a branch into the middle of an unrelated instruction. Refuse. */
    say(reason, reason_len, "internal: %u jump site(s) left unbound at %08X", e.sites_made - e.sites_bound, eip);
    return kX86pJitOutOfSpace;
  }

  if (!x86p_emit_ok(&e)) {
    say(reason, reason_len, "code buffer of %zu byte(s) too small for the block at %08X", code_cap, eip);
    return kX86pJitOutOfSpace;
  }
  if (count == 0) {
    /* Nothing was translated and no earlier branch claimed it. A block that
       runs zero guest instructions makes no progress, and a caller that cached
       it would spin at full speed forever. */
    say(reason, reason_len, "translated 0 instructions at %08X", eip);
    return kX86pJitUnsupportedAtEntry;
  }

  out->entry = code;
  out->guest_eip = eip;
  out->guest_len = pc - eip;
  out->insns = count;
  out->host_bytes = e.len;
  out->stopper = stopper;
  out->flag_helper_calls = ctx.flag_helper_calls;
  out->conds = ctx.conds;
  out->cond_helper_calls = ctx.cond_helper_calls;
  out->cond_inline = ctx.cond_inline;
  out->cond_proven = ctx.cond_proven;
  out->cond_unknown_kind = ctx.cond_unknown_kind;
  out->ends_in_branch = terminated;
  out->chain_exits = ctx.chain_exits;
  out->chain_exits_unslotted = ctx.chain_exits_unslotted;
  return kX86pJitOk;
}

int x86p_jit_can_translate(const X86pInsn *insn) {
  return can_emit(insn);
}
