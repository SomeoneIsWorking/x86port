/* jit_x64.c -- see jit_x64.h for why arithmetic is called rather than inlined. */
#include "jit_x64.h"

#include "alu.h"
#include "cond.h"
#include "decode.h"
#include "emit_x64.h"
#include "flags.h"
#include "jit_x64_internal.h"
#include "jit_x64_x87.h"
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

const char *x86p_jit_exit_name(X86pJitExit e) {
  switch (e) {
  case kX86pJitExitBlockEnd:
    return "block end";
  case kX86pJitExitUnsupported:
    return "unsupported instruction";
  case kX86pJitExitMemoryFault:
    return "guest memory fault";
  case kX86pJitExitDivideError:
    return "divide error";
  case kX86pJitExitInterrupt:
    return "software interrupt";
  case kX86pJitExitProtectionFault:
    return "general-protection fault";
  case kX86pJitExitBoundRange:
    return "bound range exceeded";
  case kX86pJitExitCount:
  default:
    return "?";
  }
}

const char *x86p_jit_status_name(X86pJitStatus s) {
  switch (s) {
  case kX86pJitOk:
    return "ok";
  case kX86pJitFetchFault:
    return "fetch fault";
  case kX86pJitDecodeFailed:
    return "decode failed";
  case kX86pJitUnsupportedAtEntry:
    return "unsupported at entry";
  case kX86pJitOutOfSpace:
    return "out of space";
  case kX86pJitStatusCount:
  default:
    return "?";
  }
}

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
static int32_t reg_off(int index) {
  return (int32_t)(offsetof(X86pCpu, reg) + (size_t)index * sizeof(uint32_t));
}

static int32_t eip_off(void) {
  return (int32_t)offsetof(X86pCpu, eip);
}

static int32_t flags_off(void) {
  return (int32_t)offsetof(X86pCpu, flags);
}

static int32_t flag_off(size_t field) {
  return (int32_t)(offsetof(X86pCpu, flags) + field);
}

#define FLAG_A flag_off(offsetof(X86pFlags, a))
#define FLAG_B flag_off(offsetof(X86pFlags, b))
#define FLAG_R flag_off(offsetof(X86pFlags, r))
#define FLAG_KIND flag_off(offsetof(X86pFlags, kind))
#define FLAG_W flag_off(offsetof(X86pFlags, w))
#define FLAG_CARRY_IN flag_off(offsetof(X86pFlags, carry_in))

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
/* ---- guest memory -------------------------------------------------------- */

/*
 * THE MAPPING IS BAKED IN AS CONSTANTS, and that is a contract, not an
 * oversight.
 *
 * A block embeds the host base, guest low address, and size of the X86pMem it
 * was translated against, so an access is a bounds check and an add rather than
 * a call. The cost is that a block is only valid for the
 * mapping it was translated against: if the guest memory is remapped, moved, or
 * resized, every block must be discarded. The block cache's flush is that
 * mechanism. A caller that remaps without flushing gets a block reading freed
 * host memory, so this is stated here and in the header rather than left to be
 * discovered.
 */
/* MemPlan lives in jit_x64_internal.h. */

/*
 * Emit: EA_REG = base + index*scale + disp, as the guest computes it.
 *
 * 32-bit throughout, so the wrap at 4 GB is the guest's wrap. Widening any part
 * of this to 64 bits would make an address that the guest wraps address
 * something real instead.
 */
/*
 * base + index*scale + disp, WITHOUT the segment base.
 *
 * Separate from emit_effective_address because LEA computes exactly this and
 * no more: it produces the OFFSET, not the linear address, so a LEA that added
 * the FS base would hand the guest a pointer it never asked for. Two named
 * functions rather than a flag, because a flag at a call site is a thing to
 * get the wrong way round.
 */
static void emit_address_parts(X86pEmit *e, const X86pOperand *o) {
  int have_base = (o->base >= 0);
  if (have_base) {
    x86p_emit_load32(e, EA_REG, CPU_REG, reg_off(o->base));
  } else {
    x86p_emit_mov_r32_imm32(e, EA_REG, 0u);
  }
  if (o->index >= 0) {
    unsigned shift = 0u;
    switch (o->scale) {
    case 2:
      shift = 1u;
      break;
    case 4:
      shift = 2u;
      break;
    case 8:
      shift = 3u;
      break;
    default:
      shift = 0u;
      break;
    }
    x86p_emit_load32(e, ADDR_TMP, CPU_REG, reg_off(o->index));
    if (shift) {
      x86p_emit_shl_r32_imm8(e, ADDR_TMP, (uint8_t)shift);
    }
    x86p_emit_alu_r32_r32(e, kX64Add, EA_REG, ADDR_TMP);
  }
  if (o->disp != 0) {
    x86p_emit_alu_r32_imm32(e, kX64Add, EA_REG, (uint32_t)o->disp);
  }
}

/*
 * The linear address an ACCESS uses: the offset plus the segment base.
 *
 * Only FS and GS have one -- see cpu.h on why the flat model is a contract --
 * and which segment an operand uses is resolved by the decoder, so this costs
 * nothing at all for the other four rather than a load and an add on every
 * memory access in the program.
 */
static void emit_effective_address(X86pEmit *e, const X86pOperand *o) {
  emit_address_parts(e, o);
  if (o->seg == (uint8_t)kX86pSegFs) {
    x86p_emit_alu_r32_mem(e, kX64Add, EA_REG, CPU_REG, (int32_t)offsetof(X86pCpu, fs_base));
  } else if (o->seg == (uint8_t)kX86pSegGs) {
    x86p_emit_alu_r32_mem(e, kX64Add, EA_REG, CPU_REG, (int32_t)offsetof(X86pCpu, gs_base));
  }
}

/*
 * Bounds-check EA_REG and leave the host address in HOSTPTR_REG.
 *
 * ONE unsigned compare covers both ends: (addr - lo) as unsigned is huge when
 * addr is below lo, so `ja` catches underflow and overflow together. Writing
 * two signed comparisons instead is the classic way to let a negative offset
 * through.
 *
 * The check is against size - w, so an access that STARTS inside the mapping
 * and runs off the end is refused rather than truncated -- the same rule
 * x86p_mem_read enforces.
 *
 * Returns the site to bind to the fault stub.
 */
static X86pEmitSite emit_bounds_check(X86pEmit *e, const MemPlan *plan, uint32_t insn_eip, int w) {
  x86p_emit_mov_r32_imm32(e, FAULTPC_REG, insn_eip);
  x86p_emit_mov_r32_r32(e, ADDR_TMP, EA_REG);
  if (plan->lo != 0u) {
    x86p_emit_alu_r32_imm32(e, kX64Sub, ADDR_TMP, plan->lo);
  }
  /*
   * A mapping narrower than the access has NO in-bounds address, so the check
   * becomes unconditional rather than arithmetic. Subtracting w from a smaller
   * size underflows to about four billion and would admit every address --
   * and w reaches 16 for an SSE access, so refusing to translate against a
   * mapping under one dword did not prevent it, it only moved it.
   */
  if (plan->size < (uint32_t)w) {
    return x86p_emit_jmp_rel32(e);
  }
  x86p_emit_alu_r32_imm32(e, kX64Cmp, ADDR_TMP, plan->size - (uint32_t)w);
  return x86p_emit_jcc_rel32(e, (unsigned)kX86pCondA);
}

/* HOSTPTR_REG = host + (EA - lo). ADDR_TMP already holds the offset, and
   writing a 32-bit register zero-extends, so the 64-bit add gets a clean
   offset. */
static void emit_host_pointer(X86pEmit *e, const MemPlan *plan) {
  x86p_emit_mov_r64_imm64(e, HOSTPTR_REG, plan->host);
  x86p_emit_alu_r64_r64(e, kX64Add, HOSTPTR_REG, ADDR_TMP);
}

/* ---- emitting ------------------------------------------------------------ */

/* BlockCtx (per-block emission state) lives in jit_x64_internal.h. */

static void note_fault(BlockCtx *c, X86pEmitSite site) {
  if (c->nfaults < sizeof c->faults / sizeof c->faults[0]) {
    c->faults[c->nfaults++] = site;
    return;
  }
  /* More fault sites than the block can hold. The site is real and now cannot
     be bound, so the buffer is poisoned and the block discarded -- never left
     with a jump to an arbitrary offset. */
  c->e->overflow = 1;
}

static void note_divide_fault(BlockCtx *c, X86pEmitSite site) {
  if (c->ndivide_faults < sizeof c->divide_faults / sizeof c->divide_faults[0]) {
    c->divide_faults[c->ndivide_faults++] = site;
    return;
  }
  c->e->overflow = 1;
}

/* Leave HOSTPTR_REG pointing at the guest operand, or jump to the fault stub. */
/* The width is the ACCESS width, not a constant: a two-byte access ending one
   byte past the mapping must be refused, and a check hard-coded to 4 would
   refuse a legal one-byte access at the last address instead. */
void emit_mem_prepare_w(BlockCtx *c, const X86pOperand *o, uint32_t insn_eip, int w) {
  emit_effective_address(c->e, o);
  note_fault(c, emit_bounds_check(c->e, &c->plan, insn_eip, w));
  emit_host_pointer(c->e, &c->plan);
}

static void emit_mem_prepare(BlockCtx *c, const X86pOperand *o, uint32_t insn_eip) {
  emit_mem_prepare_w(c, o, insn_eip, 4);
}

/*
 * Where a guest register operand of width `w` lives, as a byte offset.
 *
 * The host is little-endian and the guest slot is a dword, so the low byte of a
 * register is the slot's first byte and a HIGH byte register (AH, CH, DH, BH)
 * is its second. That means narrow writes need no read-modify-write at all: a
 * one-byte store to the right offset preserves the other 24 bits by
 * construction, which is exactly the rule x86p_reg_write states.
 *
 * x86p_byte_reg owns which register an index names -- indices 4..7 are the
 * SECOND byte of EAX..EBX, not four different registers -- so this does not
 * restate it.
 */
static int32_t reg_off_w(int index, int w) {
  if (w == 1) {
    int shift = 0;
    int r = x86p_byte_reg(index, &shift);
    return reg_off(r) + shift / 8;
  }
  return reg_off(index);
}

/* Load a guest value of width `w` into `dst`, zero-extended. The upper bits are
   cleared rather than left alone because the value is about to be stored back
   at that width, and stale high bits would be written into the neighbouring
   part of a register the guest still owns. */
static void emit_load_w(X86pEmit *e, X86pHostReg dst, X86pHostReg base, int32_t disp, int w) {
  if (w == 1) {
    x86p_emit_load8_zx(e, dst, base, disp);
  } else if (w == 2) {
    x86p_emit_load16_zx(e, dst, base, disp);
  } else {
    x86p_emit_load32(e, dst, base, disp);
  }
}

static void emit_store_w(X86pEmit *e, X86pHostReg base, int32_t disp, X86pHostReg src, int w) {
  if (w == 1) {
    x86p_emit_store8_reg(e, base, disp, src);
  } else if (w == 2) {
    x86p_emit_store16_reg(e, base, disp, src);
  } else {
    x86p_emit_store32(e, base, disp, src);
  }
}

static void emit_store_imm_w(X86pEmit *e, X86pHostReg base, int32_t disp, uint32_t imm, int w) {
  if (w == 1) {
    x86p_emit_store8_imm(e, base, disp, (uint8_t)(imm & 0xFFu));
  } else if (w == 2) {
    x86p_emit_store16_imm(e, base, disp, (uint16_t)(imm & 0xFFFFu));
  } else {
    x86p_emit_store32_imm(e, base, disp, imm);
  }
}

/*
 * Which host ALU opcode computes a guest ALU op, and which flag kind it
 * records. Returns 0 for the ops that are not inlined.
 *
 * ADC and SBB are deliberately absent. They do not use the lazy tuple at all --
 * x86p_alu computes a real EFLAGS word for them and stores it as Explicit,
 * because the triple cannot carry a carry-in. Reproducing that inline would be
 * a second implementation of the eager derivation, which is exactly what this
 * backend does not do; they keep calling x86p_alu.
 */
static int inline_alu_shape(uint8_t alu, X86pHostAlu *host, X86pFlagKind *kind, int *writes_dest) {
  *writes_dest = 1;
  switch (alu) {
  case kX86pAluAdd:
    *host = kX64Add;
    *kind = kX86pFlagsAdd;
    return 1;
  case kX86pAluSub:
    *host = kX64Sub;
    *kind = kX86pFlagsSub;
    return 1;
  case kX86pAluCmp:
    *host = kX64Sub;
    *kind = kX86pFlagsSub;
    *writes_dest = 0;
    return 1;
  case kX86pAluOr:
    *host = kX64Or;
    *kind = kX86pFlagsLogic;
    return 1;
  case kX86pAluAnd:
    *host = kX64And;
    *kind = kX86pFlagsLogic;
    return 1;
  case kX86pAluTest:
    *host = kX64And;
    *kind = kX86pFlagsLogic;
    *writes_dest = 0;
    return 1;
  case kX86pAluXor:
    *host = kX64Xor;
    *kind = kX86pFlagsLogic;
    return 1;
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
      return 0; /* shift / ADC / SBB / memory ALU: reads CF or can fault */
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
 * Store `carry_in`: the CF the flag state held BEFORE this operation overwrites
 * it. x86p_flags_set records it because INC and DEC preserve CF, and guest code
 * really does put an INC between an ADD and an ADC.
 *
 * WHY THIS CAN BE INLINED AT ALL. CF's derivation depends on the PREVIOUS
 * operation's kind, which is runtime data in general -- but inside a block it
 * is known at translation time, because this file emitted the previous
 * operation and knows what kind it recorded. Only the first flag write in a
 * block faces an unknown predecessor, and that one calls x86p_flag_cf. So the
 * call happens once per block instead of once per instruction.
 *
 * The derivations mirror x86p_flag_cf exactly, at w == 4 where the width mask
 * is the identity. Shl/Shr/Sar and Explicit never appear as a known
 * predecessor because this backend does not emit them; they can only arrive
 * through the unknown-predecessor path, which asks the real function.
 */
static int emit_compute_carry_in(X86pEmit *e, int last_kind) {
  switch (last_kind) {
  case kX86pFlagsNone:
  case kX86pFlagsLogic:
    /* Both give CF == 0 with no computation at all. */
    x86p_emit_mov_r32_imm32(e, CARRY_REG, 0u);
    return 0;
  case kX86pFlagsAdd:
    /* CF = r < a, unsigned */
    x86p_emit_load32(e, CARRY_REG, CPU_REG, FLAG_R);
    x86p_emit_alu_r32_mem(e, kX64Cmp, CARRY_REG, CPU_REG, FLAG_A);
    x86p_emit_setcc_r8(e, (unsigned)kX86pCondB, CARRY_REG);
    return 0;
  case kX86pFlagsExplicit:
    /* A real EFLAGS word, which ADC and SBB leave behind: CF is bit 0 of `a`,
       so masking it IS the 0-or-1 the field wants. Known at translation time
       like any other kind -- x86p_alu always records Explicit for those two --
       so it needs no more of a helper call than an ADD does. */
    x86p_emit_load32(e, CARRY_REG, CPU_REG, FLAG_A);
    x86p_emit_alu_r32_imm32(e, kX64And, CARRY_REG, X86P_CF);
    return 0;
  case kX86pFlagsInc:
  case kX86pFlagsDec:
    /* PRESERVED. INC and DEC do not write CF, so the carry the state already
       holds IS the carry, and x86p_flag_cf returns exactly this byte. */
    x86p_emit_load8_zx(e, CARRY_REG, CPU_REG, FLAG_CARRY_IN);
    return 0;
  case kX86pFlagsSub:
    /* CF = a < b, unsigned */
    x86p_emit_load32(e, CARRY_REG, CPU_REG, FLAG_A);
    x86p_emit_alu_r32_mem(e, kX64Cmp, CARRY_REG, CPU_REG, FLAG_B);
    x86p_emit_setcc_r8(e, (unsigned)kX86pCondB, CARRY_REG);
    return 0;
  default:
    /* Unknown predecessor: ask the one authority. Once per block. */
    x86p_emit_lea64(e, X86P_JIT_HOST_ARG0, CPU_REG, flags_off());
    x86p_emit_mov_r64_imm64(e, kX64Rax, (uint64_t)(uintptr_t)&x86p_flag_cf);
    x86p_emit_call_r64(e, kX64Rax);
    x86p_emit_mov_r32_r32(e, CARRY_REG, kX64Rax);
    return 1;
  }
}

/* x86p_alu(op, a, b, w, &cpu->flags) -> result in EAX. Argument placement is
 * owned by jit_x64_abi.h. The operand loads run first because they read through
 * CPU_REG; the later argument setup writes registers they would otherwise have
 * to avoid. */
/*
 * The inlined form: native host arithmetic plus the lazy tuple written
 * directly, with no call at all.
 *
 * This is the same computation x86p_alu performs, not a second opinion about
 * it: the host ALU op is chosen to compute exactly what the guest op computes
 * at 32 bits, and the tuple stored is field for field what x86p_flags_set
 * stores. What it does NOT duplicate is any policy -- widths other than 4,
 * shifts, ADC/SBB and the flag DERIVATIONS all still live in one place and
 * still go through it. The differential is what keeps that claim honest.
 */
static uint32_t width_mask(int w) {
  return (w == 1) ? 0xFFu : ((w == 2) ? 0xFFFFu : 0xFFFFFFFFu);
}

/* The non-memory side of an ALU operand, at width `w`. An immediate is masked
   HERE, at translation time, because x86p_alu masks its `b` and the tuple has
   to match: `83 /r` sign-extends an imm8 to a dword that the operation then
   narrows again. */
static void emit_read_alu_src(BlockCtx *c, X86pHostReg dst, const X86pOperand *o, int w) {
  if (o->kind == kX86pOperandImm) {
    x86p_emit_mov_r32_imm32(c->e, dst, o->imm & width_mask(w));
    return;
  }
  emit_load_w(c->e, dst, CPU_REG, reg_off_w(o->reg, w), w);
}

static void emit_alu_inline(BlockCtx *c,
                            const X86pInsn *insn,
                            X86pHostAlu host,
                            X86pFlagKind kind,
                            int writes_dest,
                            int last_kind,
                            int flags_dead,
                            uint32_t insn_eip) {
  const X86pOperand *dst = &insn->operand[0];
  const X86pOperand *src = &insn->operand[1];

  /*
   * COMPUTED first, while the old flag state is still intact -- and STORED only
   * after the memory operand's bounds check, because a refused access must
   * leave every flag field exactly as it was. Writing carry_in before the check
   * left the flags half-updated on a fault: a divergence that only appears
   * three instructions later, when something finally reads CF.
   *
   * `flags_dead` (from flag_write_is_dead) means a later instruction rewrites
   * every EFLAGS bit before anything reads them: the whole tuple, carry_in
   * included, is skipped. The native arithmetic and its write-back stay.
   */
  if (!flags_dead) {
    c->flag_helper_calls += (unsigned)emit_compute_carry_in(c->e, last_kind);
  }

  /*
   * The memory operand, whichever side it is on, is prepared ONCE and its
   * pointer reused for both the read and the write-back. Recomputing the
   * address for the store would double the cost and, worse, would recompute it
   * from registers the operation may have just modified -- `ADD [EAX+4], EAX`
   * must store where it loaded.
   */
  const int w = dst->size;

  if (dst->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, dst, insn_eip, w);
    if (!flags_dead) {
      x86p_emit_store8_reg(c->e, CPU_REG, FLAG_CARRY_IN, CARRY_REG);
    }
    emit_load_w(c->e, kX64Rsi, HOSTPTR_REG, 0, w);
    emit_read_alu_src(c, kX64Rdx, src, w);
  } else if (src->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, src, insn_eip, w);
    if (!flags_dead) {
      x86p_emit_store8_reg(c->e, CPU_REG, FLAG_CARRY_IN, CARRY_REG);
    }
    emit_load_w(c->e, kX64Rdx, HOSTPTR_REG, 0, w);
    emit_load_w(c->e, kX64Rsi, CPU_REG, reg_off_w(dst->reg, w), w);
  } else {
    if (!flags_dead) {
      x86p_emit_store8_reg(c->e, CPU_REG, FLAG_CARRY_IN, CARRY_REG);
    }
    emit_read_alu_src(c, kX64Rdx, src, w);
    emit_load_w(c->e, kX64Rsi, CPU_REG, reg_off_w(dst->reg, w), w);
  }

  x86p_emit_mov_r32_r32(c->e, kX64Rax, kX64Rsi);
  x86p_emit_alu_r32_r32(c->e, host, kX64Rax, kX64Rdx); /* r */
  if (w != 4) {
    /*
     * The tuple must hold the values x86p_alu would have stored, and it masks
     * a, b and r to the operand width. `a` and `b` arrive masked because they
     * were loaded zero-extended; `r` is the 32-bit result of a 32-bit host
     * operation and is not. Every DERIVED flag masks by w and would agree
     * either way -- it is the raw tuple that would differ, which is exactly
     * the field a caller inspecting flag state reads.
     */
    x86p_emit_alu_r32_imm32(c->e, kX64And, kX64Rax, width_mask(w));
  }

  if (!flags_dead) {
    x86p_emit_store32(c->e, CPU_REG, FLAG_A, kX64Rsi);
    x86p_emit_store32(c->e, CPU_REG, FLAG_B, kX64Rdx);
    x86p_emit_store32(c->e, CPU_REG, FLAG_R, kX64Rax);
    x86p_emit_store8_imm(c->e, CPU_REG, FLAG_KIND, (uint8_t)kind);
    x86p_emit_store8_imm(c->e, CPU_REG, FLAG_W, (uint8_t)w);
  }

  if (writes_dest) {
    if (dst->kind == kX86pOperandMem) {
      emit_store_w(c->e, HOSTPTR_REG, 0, kX64Rax, w);
    } else {
      emit_store_w(c->e, CPU_REG, reg_off_w(dst->reg, w), kX64Rax, w);
    }
  }
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
  emit_address_parts(c->e, &insn->operand[1]);
  x86p_emit_store32(c->e, CPU_REG, reg_off(insn->operand[0].reg), EA_REG);
}

/* LEAVE is ordered state transition, not a MOV followed by an ordinary POP:
   ESP becomes EBP before the stack read, and remains there if that read faults.
   Only a successful read advances ESP and replaces EBP. */
static void emit_leave(BlockCtx *c, uint32_t insn_eip) {
  x86p_emit_load32(c->e, EA_REG, CPU_REG, reg_off(kX86pEbp));
  x86p_emit_store32(c->e, CPU_REG, reg_off(kX86pEsp), EA_REG);
  note_fault(c, emit_bounds_check(c->e, &c->plan, insn_eip, 4));
  emit_host_pointer(c->e, &c->plan);
  x86p_emit_load32(c->e, kX64Rsi, HOSTPTR_REG, 0);
  x86p_emit_alu_r32_imm32(c->e, kX64Add, EA_REG, 4u);
  x86p_emit_store32(c->e, CPU_REG, reg_off(kX86pEsp), EA_REG);
  x86p_emit_store32(c->e, CPU_REG, reg_off(kX86pEbp), kX64Rsi);
}

static void emit_cdq(X86pEmit *e) {
  x86p_emit_load32(e, kX64Rax, CPU_REG, reg_off(kX86pEax));
  x86p_emit_sar_r32_imm8(e, kX64Rax, 31u);
  x86p_emit_store32(e, CPU_REG, reg_off(kX86pEdx), kX64Rax);
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
    x86p_emit_load32(c->e, X86P_JIT_HOST_ARG1, CPU_REG, reg_off(divisor->reg));
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
      x86p_emit_load32(c->e, X86P_JIT_HOST_ARG3, CPU_REG, reg_off(source->reg));
    }
    x86p_emit_load32(c->e, X86P_JIT_HOST_ARG2, CPU_REG, reg_off(destination->reg));
  } else {
    if (source->kind == kX86pOperandMem) {
      emit_mem_prepare_w(c, source, insn_eip, 4);
      x86p_emit_load32(c->e, X86P_JIT_HOST_ARG2, HOSTPTR_REG, 0);
    } else {
      x86p_emit_load32(c->e, X86P_JIT_HOST_ARG2, CPU_REG, reg_off(source->reg));
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
  x86p_emit_load32(c->e, EA_REG, CPU_REG, reg_off(kX86pEsp));
  x86p_emit_alu_r32_imm32(c->e, kX64Sub, EA_REG, 4u);
  note_fault(c, emit_bounds_check(c->e, &c->plan, insn_eip, 4));
  emit_host_pointer(c->e, &c->plan);
  x86p_emit_store32(c->e, HOSTPTR_REG, 0, kX64Rsi);
  /* The bounds check preserves EA_REG -- it copies into ADDR_TMP -- so the new
     ESP is still here and needs no second computation. */
  x86p_emit_store32(c->e, CPU_REG, reg_off(kX86pEsp), EA_REG);
}

static void emit_push(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  const X86pOperand *o = &insn->operand[0];

  if (o->kind == kX86pOperandImm) {
    x86p_emit_mov_r32_imm32(c->e, kX64Rsi, x86p_sign_extend(o->imm, o->size));
  } else if (o->kind == kX86pOperandMem) {
    emit_mem_prepare(c, o, insn_eip);
    x86p_emit_load32(c->e, kX64Rsi, HOSTPTR_REG, 0);
  } else {
    x86p_emit_load32(c->e, kX64Rsi, CPU_REG, reg_off(o->reg));
  }

  emit_push_rsi(c, insn_eip);
}

/* Pop whatever is at [ESP] into RSI and advance ESP past it, the one
   implementation of the stack load shared by POP and POPFD (mirrors
   emit_push_rsi below the corresponding PUSH). The caller decides where the
   popped value in RSI ends up. */
static void emit_pop_rsi(BlockCtx *c, uint32_t insn_eip) {
  x86p_emit_load32(c->e, EA_REG, CPU_REG, reg_off(kX86pEsp));
  note_fault(c, emit_bounds_check(c->e, &c->plan, insn_eip, 4));
  emit_host_pointer(c->e, &c->plan);
  x86p_emit_load32(c->e, kX64Rsi, HOSTPTR_REG, 0);

  x86p_emit_mov_r32_r32(c->e, kX64Rdx, EA_REG);
  x86p_emit_alu_r32_imm32(c->e, kX64Add, kX64Rdx, 4u);
  x86p_emit_store32(c->e, CPU_REG, reg_off(kX86pEsp), kX64Rdx);
}

static void emit_pop(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  const X86pOperand *o = &insn->operand[0];

  emit_pop_rsi(c, insn_eip);

  if (o->kind == kX86pOperandMem) {
    emit_mem_prepare(c, o, insn_eip);
    x86p_emit_store32(c->e, HOSTPTR_REG, 0, kX64Rsi);
    return;
  }
  x86p_emit_store32(c->e, CPU_REG, reg_off(o->reg), kX64Rsi);
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

/*
 * INC, DEC, NEG and NOT.
 *
 * NOT writes NO FLAGS AT ALL, which is why it is a separate case rather than
 * `XOR a, -1`: the XOR would clear CF and OF. It therefore leaves last_kind
 * alone as well -- an instruction that writes no flags does not become the
 * predecessor of the next one.
 *
 * INC and DEC PRESERVE CF, which is the entire reason `carry_in` exists: guest
 * code really does put an INC between an ADD and an ADC.
 *
 * Returns the flag kind recorded, or -1 for NOT, which records none.
 */
static int emit_alu_unary_inline(BlockCtx *c, const X86pInsn *insn, int last_kind, int flags_dead, uint32_t insn_eip) {
  const X86pOperand *o = &insn->operand[0];
  const int w = o->size;
  const int is_mem = (o->kind == kX86pOperandMem);
  X86pFlagKind kind;

  if (insn->alu == (uint8_t)kX86pAluNot) {
    if (is_mem) {
      emit_mem_prepare_w(c, o, insn_eip, w);
      emit_load_w(c->e, kX64Rax, HOSTPTR_REG, 0, w);
    } else {
      emit_load_w(c->e, kX64Rax, CPU_REG, reg_off_w(o->reg, w), w);
    }
    /* XOR with all ones is the host's NOT; the guest's flag rule is honoured by
       storing nothing, not by choosing a different host opcode. */
    x86p_emit_alu_r32_imm32(c->e, kX64Xor, kX64Rax, 0xFFFFFFFFu);
    if (w != 4) {
      x86p_emit_alu_r32_imm32(c->e, kX64And, kX64Rax, width_mask(w));
    }
    if (is_mem) {
      emit_store_w(c->e, HOSTPTR_REG, 0, kX64Rax, w);
    } else {
      emit_store_w(c->e, CPU_REG, reg_off_w(o->reg, w), kX64Rax, w);
    }
    return -1;
  }

  if (!flags_dead) {
    c->flag_helper_calls += (unsigned)emit_compute_carry_in(c->e, last_kind);
  }

  if (is_mem) {
    emit_mem_prepare_w(c, o, insn_eip, w);
    if (!flags_dead) {
      x86p_emit_store8_reg(c->e, CPU_REG, FLAG_CARRY_IN, CARRY_REG);
    }
    emit_load_w(c->e, kX64Rsi, HOSTPTR_REG, 0, w);
  } else {
    if (!flags_dead) {
      x86p_emit_store8_reg(c->e, CPU_REG, FLAG_CARRY_IN, CARRY_REG);
    }
    emit_load_w(c->e, kX64Rsi, CPU_REG, reg_off_w(o->reg, w), w);
  }

  if (insn->alu == (uint8_t)kX86pAluNeg) {
    /* 0 - a, recorded as the SUB it is, so CF falls out of the borrow rather
       than being special-cased as "a was nonzero". */
    x86p_emit_mov_r32_imm32(c->e, kX64Rax, 0u);
    x86p_emit_alu_r32_r32(c->e, kX64Sub, kX64Rax, kX64Rsi);
    kind = kX86pFlagsSub;
  } else {
    x86p_emit_mov_r32_r32(c->e, kX64Rax, kX64Rsi);
    if (insn->alu == (uint8_t)kX86pAluInc) {
      x86p_emit_alu_r32_imm32(c->e, kX64Add, kX64Rax, 1u);
      kind = kX86pFlagsInc;
    } else {
      x86p_emit_alu_r32_imm32(c->e, kX64Sub, kX64Rax, 1u);
      kind = kX86pFlagsDec;
    }
  }
  if (w != 4) {
    x86p_emit_alu_r32_imm32(c->e, kX64And, kX64Rax, width_mask(w));
  }

  /* NEG's operands are (0, a); INC and DEC's are (a, 1). The tuple must be
     what x86p_alu_unary would have stored, because every derived flag reads it
     and so does the next instruction's carry-in. */
  if (!flags_dead) {
    if (kind == kX86pFlagsSub) {
      x86p_emit_store32_imm(c->e, CPU_REG, FLAG_A, 0u);
      x86p_emit_store32(c->e, CPU_REG, FLAG_B, kX64Rsi);
    } else {
      x86p_emit_store32(c->e, CPU_REG, FLAG_A, kX64Rsi);
      x86p_emit_store32_imm(c->e, CPU_REG, FLAG_B, 1u);
    }
    x86p_emit_store32(c->e, CPU_REG, FLAG_R, kX64Rax);
    x86p_emit_store8_imm(c->e, CPU_REG, FLAG_KIND, (uint8_t)kind);
    x86p_emit_store8_imm(c->e, CPU_REG, FLAG_W, (uint8_t)w);
  }

  if (is_mem) {
    emit_store_w(c->e, HOSTPTR_REG, 0, kX64Rax, w);
  } else {
    emit_store_w(c->e, CPU_REG, reg_off_w(o->reg, w), kX64Rax, w);
  }
  return (int)kind;
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
    emit_load_w(c->e, kX64Rax, CPU_REG, reg_off_w(src->reg, w), w);
    emit_store_w(c->e, HOSTPTR_REG, 0, kX64Rax, w);
    return;
  }

  if (src->kind == kX86pOperandImm) {
    emit_store_imm_w(c->e, CPU_REG, reg_off_w(dst->reg, w), src->imm, w);
    return;
  }
  if (src->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, src, insn_eip, w);
    emit_load_w(c->e, kX64Rax, HOSTPTR_REG, 0, w);
  } else {
    emit_load_w(c->e, kX64Rax, CPU_REG, reg_off_w(src->reg, w), w);
  }
  emit_store_w(c->e, CPU_REG, reg_off_w(dst->reg, w), kX64Rax, w);
}

static void emit_xchg32(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  const X86pOperand *first = &insn->operand[0];
  const X86pOperand *second = &insn->operand[1];
  const X86pOperand *memory = first->kind == kX86pOperandMem ? first : second;
  const X86pOperand *reg = first->kind == kX86pOperandReg ? first : second;

  if (memory->kind == kX86pOperandMem) {
    emit_mem_prepare_w(c, memory, insn_eip, 4);
    x86p_emit_load32(c->e, kX64Rax, HOSTPTR_REG, 0);
    x86p_emit_load32(c->e, kX64Rsi, CPU_REG, reg_off(reg->reg));
    x86p_emit_store32(c->e, HOSTPTR_REG, 0, kX64Rsi);
    x86p_emit_store32(c->e, CPU_REG, reg_off(reg->reg), kX64Rax);
    return;
  }

  x86p_emit_load32(c->e, kX64Rax, CPU_REG, reg_off(first->reg));
  x86p_emit_load32(c->e, kX64Rsi, CPU_REG, reg_off(second->reg));
  x86p_emit_store32(c->e, CPU_REG, reg_off(first->reg), kX64Rsi);
  x86p_emit_store32(c->e, CPU_REG, reg_off(second->reg), kX64Rax);
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
    emit_load_w(c->e, kX64Rax, CPU_REG, reg_off_w(src->reg, sw), sw);
  }

  if (is_signed) {
    const uint8_t fill = (uint8_t)(32 - 8 * sw);
    x86p_emit_shl_r32_imm8(c->e, kX64Rax, fill);
    x86p_emit_sar_r32_imm8(c->e, kX64Rax, fill);
  }

  emit_store_w(c->e, CPU_REG, reg_off_w(dst->reg, dw), kX64Rax, dw);
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

static void emit_condition_value(X86pEmit *e, uint8_t cond) {
  x86p_emit_mov_r32_imm32(e, X86P_JIT_HOST_ARG0, (uint32_t)cond);
  x86p_emit_lea64(e, X86P_JIT_HOST_ARG1, CPU_REG, flags_off());
  x86p_emit_mov_r64_imm64(e, kX64Rax, (uint64_t)(uintptr_t)&x86p_cond);
  x86p_emit_call_r64(e, kX64Rax);
}

static void emit_jcc(X86pEmit *e, uint8_t cond, uint32_t target, uint32_t fallthrough) {
  emit_condition_value(e, cond);
  x86p_emit_test_r32_r32(e, kX64Rax, kX64Rax);
  x86p_emit_mov_r32_imm32(e, kX64Rax, fallthrough);
  x86p_emit_mov_r32_imm32(e, kX64Rcx, target);
  x86p_emit_cmovcc_r32_r32(e, (unsigned)kX86pCondNZ, kX64Rax, kX64Rcx);
  emit_epilogue_from(e, kX64Rax, kX86pJitExitBlockEnd);
}

/* SETcc materialises the canonical condition evaluator's 0/1 result without
   touching guest flags. A memory destination computes the condition first,
   then preserves it in RCX while the shared address/bounds path uses RAX. */
static void emit_setcc(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  const X86pOperand *dst = &insn->operand[0];

  emit_condition_value(c->e, insn->cond);
  if (dst->kind == kX86pOperandMem) {
    x86p_emit_mov_r32_r32(c->e, CARRY_REG, kX64Rax);
    emit_mem_prepare_w(c, dst, insn_eip, 1);
    x86p_emit_store8_reg(c->e, HOSTPTR_REG, 0, CARRY_REG);
    return;
  }
  x86p_emit_store8_reg(c->e, CPU_REG, reg_off_w(dst->reg, 1), kX64Rax);
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
  emit_epilogue(c->e, target, kX86pJitExitBlockEnd);
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
  x86p_emit_load32(c->e, TARGET_REG, CPU_REG, reg_off(o->reg));
}

static void emit_jmp_indirect(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  emit_read_branch_target(c, &insn->operand[0], insn_eip);
  emit_epilogue_from(c->e, TARGET_REG, kX86pJitExitBlockEnd);
}

static void emit_call_indirect(BlockCtx *c, const X86pInsn *insn, uint32_t return_eip, uint32_t insn_eip) {
  emit_read_branch_target(c, &insn->operand[0], insn_eip);
  x86p_emit_mov_r32_imm32(c->e, kX64Rsi, return_eip);
  emit_push_rsi(c, insn_eip);
  emit_epilogue_from(c->e, TARGET_REG, kX86pJitExitBlockEnd);
}

/* `release` is RET imm16's argument count, applied AFTER the pop because the
   immediate counts bytes ABOVE the return address. */
static void emit_ret(BlockCtx *c, uint32_t release, uint32_t insn_eip) {
  x86p_emit_load32(c->e, EA_REG, CPU_REG, reg_off(kX86pEsp));
  note_fault(c, emit_bounds_check(c->e, &c->plan, insn_eip, 4));
  emit_host_pointer(c->e, &c->plan);
  x86p_emit_load32(c->e, kX64Rsi, HOSTPTR_REG, 0);

  x86p_emit_mov_r32_r32(c->e, kX64Rdx, EA_REG);
  x86p_emit_alu_r32_imm32(c->e, kX64Add, kX64Rdx, 4u + release);
  x86p_emit_store32(c->e, CPU_REG, reg_off(kX86pEsp), kX64Rdx);

  emit_epilogue_from(c->e, kX64Rsi, kX86pJitExitBlockEnd);
}

/* ---- translation --------------------------------------------------------- */

X86pJitStatus x86p_jit_translate(const X86pMem *mem,
                                 uint32_t eip,
                                 void *code,
                                 size_t code_cap,
                                 X86pJitBlock *out,
                                 char *reason,
                                 unsigned reason_len) {
  return x86p_jit_translate_bounded(mem, eip, code, code_cap, NULL, NULL, out, reason, reason_len);
}

X86pJitStatus x86p_jit_translate_bounded(const X86pMem *mem,
                                         uint32_t eip,
                                         void *code,
                                         size_t code_cap,
                                         X86pJitBoundaryFn boundary,
                                         void *boundary_user,
                                         X86pJitBlock *out,
                                         char *reason,
                                         unsigned reason_len) {
  X86pEmit e;
  BlockCtx ctx;
  uint32_t pc = eip;
  uint32_t count = 0;
  X86pJitExit exit = kX86pJitExitBlockEnd;
  const char *stopper = NULL;
  int terminated = 0; /* a branch already emitted the exit */
  /* The flag kind the last emitted instruction recorded, or -1 when the
     predecessor is whatever ran before this block. */
  int last_kind = -1;

  if (!mem || !out || !code) {
    say(reason, reason_len, "null argument");
    return kX86pJitOutOfSpace;
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
  ctx.plan.host = (uint64_t)(uintptr_t)mem->host;
  ctx.plan.lo = mem->lo;
  ctx.plan.size = mem->size;
  emit_prologue(&e);

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
        emit_epilogue(&e, target, kX86pJitExitBlockEnd);
      } else if (insn.op != kX86pInsnJcc) {
        emit_loop(&ctx, &insn, target, next);
      } else {
        emit_jcc(&e, insn.cond, target, next);
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
      emit_setcc(&ctx, &insn, pc);
      break;
    case kX86pInsnAluUnary: {
      int dead = flag_write_is_dead(mem, pc + insn.length, eip, boundary, boundary_user, count, e.len, code_cap);
      int k = emit_alu_unary_inline(&ctx, &insn, last_kind, dead, pc);
      /* NOT records no flags, so the PREVIOUS instruction is still the
         predecessor for the next one's carry-in -- and so is an INC/DEC/NEG
         whose tuple was elided: the last stored kind is what memory holds. */
      if (k >= 0 && !dead) {
        last_kind = k;
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
      emit_cdq(&e);
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
      break;
    case kX86pInsnImul:
      if (insn.operands == 1) {
        emit_mul32(&ctx, &insn, pc);
      } else {
        emit_imul32(&ctx, &insn, pc);
      }
      /* The semantic owner materialises CF/OF into explicit flags. */
      last_kind = -1;
      break;
    case kX86pInsnString:
      emit_string(&ctx, &insn, pc);
      if (insn.str == (uint8_t)kX86pStringScas || insn.str == (uint8_t)kX86pStringCmps) {
        last_kind = -1;
      }
      break;
    case kX86pInsnX87:
      if (insn.x87 == kX86pX87InsnWait) {
        /* The synchronous, masked-exception model has no pending work. */
      } else if (x87_register_is_emittable(&insn)) {
        emit_x87_register(&ctx, &insn);
      } else if (x87_fn_is_emittable(&insn)) {
        emit_x87_fn(&ctx, &insn);
      } else if (x87_control_is_emittable(&insn)) {
        emit_x87_control(&ctx, &insn, pc);
      } else if (x87_arith_is_emittable(&insn)) {
        emit_x87_arith(&ctx, &insn, pc);
      } else if (x87_compare_mem_is_emittable(&insn)) {
        emit_x87_compare_mem(&ctx, &insn, pc);
      } else if (x87_store_reg_is_emittable(&insn)) {
        emit_x87_store_reg(&ctx, &insn);
      } else if (x87_store_mem_is_emittable(&insn)) {
        emit_x87_store_mem(&ctx, &insn, pc);
      } else if (x87_constant_is_emittable(&insn)) {
        emit_x87_constant(&ctx, &insn);
      } else if (x87_status_ax_is_emittable(&insn)) {
        emit_x87_status_ax(&ctx);
      } else if (x87_clear_exceptions_is_emittable(&insn)) {
        emit_x87_clear_exceptions(&ctx);
      } else {
        emit_x87_load(&ctx, &insn, pc);
      }
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
        }
      } else {
        emit_alu_helper(&ctx, &insn, pc);
        if (insn.alu >= (uint8_t)kX86pAluShl && insn.alu <= (uint8_t)kX86pAluRcr) {
          /* A shift's recorded kind depends on its COUNT, which is not known
             until the block runs: a zero count writes no flags, leaving
             whatever was there. Genuinely unknown, so the next carry-in asks
             the real function. */
          last_kind = -1;
        } else {
          /* x86p_alu records Explicit for ADC and SBB, unconditionally -- so
             the next instruction's predecessor IS statically known, and
             treating it as unknown cost a helper call per ADC in every
             block. */
          last_kind = (int)kX86pFlagsExplicit;
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

  if (!terminated) {
    emit_epilogue(&e, pc, exit);
  }

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
  out->ends_in_branch = terminated;
  return kX86pJitOk;
}

X86pJitExit x86p_jit_enter(const X86pJitBlock *b, X86pCpu *cpu) {
  uint32_t (*fn)(X86pCpu *);
  if (!b || !b->entry || !cpu) {
    return kX86pJitExitUnsupported;
  }
  /* The cast goes through a function-pointer-sized integer because ISO C does
     not define object-to-function pointer conversion; every host this targets
     does, and saying so here keeps the compiler from warning at each call. */
  *(void **)&fn = b->entry;
  return (X86pJitExit)fn(cpu);
}

int x86p_jit_emits_natively(const X86pInsn *insn) {
  return can_emit(insn);
}

int x86p_jit_can_translate(const X86pInsn *insn) {
  return can_emit(insn);
}
