/* jit_x64.c -- see jit_x64.h for why arithmetic is called rather than inlined. */
#include "jit_x64.h"

#include "alu.h"
#include "cond.h"
#include "decode.h"
#include "emit_x64.h"
#include "flags.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/*
 * The host register holding the X86pCpu pointer for the life of a block.
 *
 * RBX is callee-saved, so the helper calls this file emits cannot clobber it.
 * Choosing a caller-saved register instead would work perfectly until the first
 * call to x86p_alu, after which every guest register access would read from
 * whatever the helper left behind.
 */
#define CPU_REG kX64Rbx

/*
 * Scratch roles in the emitted memory sequence. Named rather than spelled at
 * each site, because a collision between the host pointer and an operand is
 * silent: the access simply reads the wrong address.
 */
#define EA_REG kX64Rax      /* the guest effective address, 32-bit */
#define HOSTPTR_REG kX64R11 /* the host address it maps to */
#define FAULTPC_REG kX64R10 /* guest EIP to report if the access faults */
/*
 * ADDR_TMP and CARRY_REG must be DIFFERENT registers, and that is a
 * correctness constraint rather than a preference. The carry-in bit is computed
 * before the address -- it reads the OLD flag state, which the operation is
 * about to overwrite -- but it must not be STORED until the bounds check has
 * passed, because an access that faults must leave the flag state untouched.
 * So the bit stays live in CARRY_REG across the whole address computation, and
 * the address machinery cannot use that register.
 */
#define ADDR_TMP kX64Rdi  /* index/offset scratch during address forming */
#define CARRY_REG kX64Rcx /* the pending carry-in bit, until it is safe to store */

/* Guest instructions per block. A cap so a straight-line run of translatable
   code cannot consume the whole arena in one block, and so a caller always gets
   a chance to invalidate between blocks. */
#define MAX_INSNS 64

/* Emitting one guest instruction never exceeds this, so the buffer is checked
   once per instruction rather than after every emit. The margin is generous
   and the emitter's own overflow flag is still the authority -- this only
   decides when to stop trying. */
#define WORST_CASE_INSN_BYTES 224
#define EPILOGUE_BYTES 64 /* normal exit plus the shared memory-fault stub */

const char *x86p_jit_exit_name(X86pJitExit e) {
  switch (e) {
  case kX86pJitExitBlockEnd:
    return "block end";
  case kX86pJitExitUnsupported:
    return "unsupported instruction";
  case kX86pJitExitMemoryFault:
    return "guest memory fault";
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
#if defined(__x86_64__)
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
 * emitted code reads a different register than the interpreter does -- while
 * both still run.
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

static int operand_readable(const X86pOperand *o) {
  return operand_is_reg32(o) || operand_is_imm(o) || operand_is_mem32(o);
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
  if (insn->op != (uint8_t)kX86pInsnJmp && insn->op != (uint8_t)kX86pInsnJcc) {
    return 0;
  }
  return insn->operands >= 1 && insn->operand[0].kind == kX86pOperandImm && insn->operand[0].relative;
}

static int can_emit(const X86pInsn *insn) {
  if (is_relative_branch(insn)) {
    return 1;
  }
  switch (insn->op) {
  case kX86pInsnNop:
    return 1;
  case kX86pInsnMov:
    /* At most ONE memory operand: x86 has no memory-to-memory MOV, and
       accepting one would emit two host pointers into the same register. */
    if (operand_is_mem32(&insn->operand[0]) && operand_is_mem32(&insn->operand[1])) {
      return 0;
    }
    return insn->operands == 2 && operand_writable(&insn->operand[0]) && operand_readable(&insn->operand[1]);
  case kX86pInsnAlu:
    /* Shifts and rotates take their count from CL or an immediate and have
       their own flag rules; excluded until they are tested on their own. */
    if (insn->alu > (uint8_t)kX86pAluTest) {
      return 0;
    }
    /* ADC and SBB still call x86p_alu, which takes VALUES -- a memory operand
       would need the result written back through a pointer the call has
       clobbered, so they stay register-only for now. */
    if (insn->alu == (uint8_t)kX86pAluAdc || insn->alu == (uint8_t)kX86pAluSbb) {
      return insn->operands == 2 && operand_is_reg32(&insn->operand[0]) &&
             (operand_is_reg32(&insn->operand[1]) || operand_is_imm(&insn->operand[1]));
    }
    if (operand_is_mem32(&insn->operand[0]) && operand_is_mem32(&insn->operand[1])) {
      return 0;
    }
    return insn->operands == 2 && operand_writable(&insn->operand[0]) && operand_readable(&insn->operand[1]);
  case kX86pInsnPush:
    /* An immediate narrower than 32 bits is sign-extended by a rule that lives
       in the interpreter, and folding a copy of it in here would be a second
       authority on it. PUSH imm8 is refused BY NAME until the value can come
       from the one place that owns the rule. */
    return insn->operands == 1 && (operand_is_reg32(&insn->operand[0]) || operand_is_mem32(&insn->operand[0]) ||
                                   (operand_is_imm(&insn->operand[0]) && insn->operand[0].size == 4));
  case kX86pInsnPop:
    return insn->operands == 1 && operand_writable(&insn->operand[0]);
  case kX86pInsnCall:
    /* Direct only. An indirect CALL's target is not known until the block runs,
       for exactly the reason an indirect JMP's is not. */
    return insn->operands == 1 && insn->operand[0].kind == kX86pOperandImm && insn->operand[0].relative;
  case kX86pInsnRet:
    /* RET, or RET imm16 which also releases the caller's arguments. */
    return insn->operands == 0 || (insn->operands == 1 && operand_is_imm(&insn->operand[0]));
  case kX86pInsnLea:
    /* The size of an LEA's memory operand describes an access that never
       happens, so operand_is_mem32's width rule does not apply -- only that
       the operand really is a memory reference to compute. */
    return insn->operands == 2 && operand_is_reg32(&insn->operand[0]) && insn->operand[1].kind == kX86pOperandMem;
  default:
    return 0;
  }
}

/* CMP and TEST compute a result only to derive flags from it. Emitting the
   store anyway would clobber a register the guest still expects to hold its
   original value -- and every flag assertion would still pass. */
static int alu_writes_dest(uint8_t op) {
  return op != (uint8_t)kX86pAluCmp && op != (uint8_t)kX86pAluTest;
}

/* ---- guest memory -------------------------------------------------------- */

/*
 * THE MAPPING IS BAKED IN AS CONSTANTS, and that is a contract, not an
 * oversight.
 *
 * A block embeds the host base, guest low address, and size of the X86pMem it
 * was translated against, so an access is a bounds check and an add rather than
 * a call. This is what static recompilation does too, and it is why memory
 * access can be fast at all. The cost is that a block is only valid for the
 * mapping it was translated against: if the guest memory is remapped, moved, or
 * resized, every block must be discarded. The block cache's flush is that
 * mechanism. A caller that remaps without flushing gets a block reading freed
 * host memory, so this is stated here and in the header rather than left to be
 * discovered.
 */
typedef struct MemPlan {
  uint64_t host;
  uint32_t lo;
  uint32_t size;
} MemPlan;

/*
 * Emit: EA_REG = base + index*scale + disp, as the guest computes it.
 *
 * 32-bit throughout, so the wrap at 4 GB is the guest's wrap. Widening any part
 * of this to 64 bits would make an address that the guest wraps address
 * something real instead.
 */
static void emit_effective_address(X86pEmit *e, const X86pOperand *o) {
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

/*
 * Per-block emission state.
 *
 * The fault sites are collected rather than bound as they are made, because
 * they all jump to ONE stub emitted after the normal epilogue. Binding each to
 * its own copy of the stub would be correct and would also add a dozen bytes
 * per memory access to every block.
 */
typedef struct BlockCtx {
  X86pEmit *e;
  MemPlan plan;
  X86pEmitSite faults[MAX_INSNS * 2];
  unsigned nfaults;
} BlockCtx;

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

/* Leave HOSTPTR_REG pointing at the guest operand, or jump to the fault stub. */
static void emit_mem_prepare(BlockCtx *c, const X86pOperand *o, uint32_t insn_eip) {
  emit_effective_address(c->e, o);
  note_fault(c, emit_bounds_check(c->e, &c->plan, insn_eip, 4));
  emit_host_pointer(c->e, &c->plan);
}

/*
 * Load an operand's VALUE into a host register.
 *
 * A memory operand prepares its pointer here, so at most one operand per
 * instruction may be memory -- which x86 guarantees, and can_emit enforces.
 * A second would overwrite HOSTPTR_REG and the first access would silently
 * read the second's address.
 */
static void emit_read_operand(BlockCtx *c, X86pHostReg dst, const X86pOperand *o, uint32_t insn_eip) {
  switch (o->kind) {
  case kX86pOperandImm:
    x86p_emit_mov_r32_imm32(c->e, dst, o->imm);
    return;
  case kX86pOperandMem:
    emit_mem_prepare(c, o, insn_eip);
    x86p_emit_load32(c->e, dst, HOSTPTR_REG, 0);
    return;
  default:
    x86p_emit_load32(c->e, dst, CPU_REG, reg_off(o->reg));
    return;
  }
}

/* Write a host register into an operand's destination. Memory destinations
   must already have been prepared -- HOSTPTR_REG still holds the pointer. */
static void emit_write_operand(BlockCtx *c, const X86pOperand *o, X86pHostReg src) {
  if (o->kind == kX86pOperandMem) {
    x86p_emit_store32(c->e, HOSTPTR_REG, 0, src);
    return;
  }
  x86p_emit_store32(c->e, CPU_REG, reg_off(o->reg), src);
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
static void emit_compute_carry_in(X86pEmit *e, int last_kind) {
  switch (last_kind) {
  case kX86pFlagsNone:
  case kX86pFlagsLogic:
    /* Both give CF == 0 with no computation at all. */
    x86p_emit_mov_r32_imm32(e, CARRY_REG, 0u);
    return;
  case kX86pFlagsAdd:
    /* CF = r < a, unsigned */
    x86p_emit_load32(e, CARRY_REG, CPU_REG, FLAG_R);
    x86p_emit_alu_r32_mem(e, kX64Cmp, CARRY_REG, CPU_REG, FLAG_A);
    x86p_emit_setcc_r8(e, (unsigned)kX86pCondB, CARRY_REG);
    return;
  case kX86pFlagsSub:
    /* CF = a < b, unsigned */
    x86p_emit_load32(e, CARRY_REG, CPU_REG, FLAG_A);
    x86p_emit_alu_r32_mem(e, kX64Cmp, CARRY_REG, CPU_REG, FLAG_B);
    x86p_emit_setcc_r8(e, (unsigned)kX86pCondB, CARRY_REG);
    return;
  default:
    /* Unknown predecessor: ask the one authority. Once per block. */
    x86p_emit_lea64(e, kX64Rdi, CPU_REG, flags_off());
    x86p_emit_mov_r64_imm64(e, kX64Rax, (uint64_t)(uintptr_t)&x86p_flag_cf);
    x86p_emit_call_r64(e, kX64Rax);
    x86p_emit_mov_r32_r32(e, CARRY_REG, kX64Rax);
    return;
  }
}

/*
 * x86p_alu(op, a, b, w, &cpu->flags) -> result in EAX.
 *
 * System V argument order is RDI, RSI, RDX, RCX, R8. The two operand loads run
 * FIRST, because they read through CPU_REG; the later argument setup writes
 * registers they would otherwise have to avoid.
 */
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
static void emit_alu_inline(BlockCtx *c,
                            const X86pInsn *insn,
                            X86pHostAlu host,
                            X86pFlagKind kind,
                            int writes_dest,
                            int last_kind,
                            uint32_t insn_eip) {
  const X86pOperand *dst = &insn->operand[0];
  const X86pOperand *src = &insn->operand[1];

  /*
   * COMPUTED first, while the old flag state is still intact -- and STORED only
   * after the memory operand's bounds check, because a refused access must
   * leave every flag field exactly as it was. Writing carry_in before the check
   * left the flags half-updated on a fault: a divergence that only appears
   * three instructions later, when something finally reads CF.
   */
  emit_compute_carry_in(c->e, last_kind);

  /*
   * The memory operand, whichever side it is on, is prepared ONCE and its
   * pointer reused for both the read and the write-back. Recomputing the
   * address for the store would double the cost and, worse, would recompute it
   * from registers the operation may have just modified -- `ADD [EAX+4], EAX`
   * must store where it loaded.
   */
  if (dst->kind == kX86pOperandMem) {
    emit_mem_prepare(c, dst, insn_eip);
    x86p_emit_store8_reg(c->e, CPU_REG, FLAG_CARRY_IN, CARRY_REG);
    x86p_emit_load32(c->e, kX64Rsi, HOSTPTR_REG, 0);
    emit_read_operand(c, kX64Rdx, src, insn_eip);
  } else if (src->kind == kX86pOperandMem) {
    emit_mem_prepare(c, src, insn_eip);
    x86p_emit_store8_reg(c->e, CPU_REG, FLAG_CARRY_IN, CARRY_REG);
    x86p_emit_load32(c->e, kX64Rdx, HOSTPTR_REG, 0);
    x86p_emit_load32(c->e, kX64Rsi, CPU_REG, reg_off(dst->reg));
  } else {
    x86p_emit_store8_reg(c->e, CPU_REG, FLAG_CARRY_IN, CARRY_REG);
    emit_read_operand(c, kX64Rdx, src, insn_eip);
    x86p_emit_load32(c->e, kX64Rsi, CPU_REG, reg_off(dst->reg));
  }

  x86p_emit_mov_r32_r32(c->e, kX64Rax, kX64Rsi);
  x86p_emit_alu_r32_r32(c->e, host, kX64Rax, kX64Rdx); /* r */

  x86p_emit_store32(c->e, CPU_REG, FLAG_A, kX64Rsi);
  x86p_emit_store32(c->e, CPU_REG, FLAG_B, kX64Rdx);
  x86p_emit_store32(c->e, CPU_REG, FLAG_R, kX64Rax);
  x86p_emit_store8_imm(c->e, CPU_REG, FLAG_KIND, (uint8_t)kind);
  x86p_emit_store8_imm(c->e, CPU_REG, FLAG_W, 4u);

  if (writes_dest) {
    emit_write_operand(c, dst, kX64Rax);
  }
}

/*
 * PUSH and POP.
 *
 * The stack is ordinary guest memory, so both go through the same bounds check
 * and the same fault stub as any other access. What is specific to them is the
 * ORDER, and in both cases it is the order the interpreter uses because it is
 * the order the architecture specifies:
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
  emit_effective_address(c->e, &insn->operand[1]);
  x86p_emit_store32(c->e, CPU_REG, reg_off(insn->operand[0].reg), EA_REG);
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
    x86p_emit_mov_r32_imm32(c->e, kX64Rsi, o->imm);
  } else if (o->kind == kX86pOperandMem) {
    emit_mem_prepare(c, o, insn_eip);
    x86p_emit_load32(c->e, kX64Rsi, HOSTPTR_REG, 0);
  } else {
    x86p_emit_load32(c->e, kX64Rsi, CPU_REG, reg_off(o->reg));
  }

  emit_push_rsi(c, insn_eip);
}

static void emit_pop(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  const X86pOperand *o = &insn->operand[0];

  x86p_emit_load32(c->e, EA_REG, CPU_REG, reg_off(kX86pEsp));
  note_fault(c, emit_bounds_check(c->e, &c->plan, insn_eip, 4));
  emit_host_pointer(c->e, &c->plan);
  x86p_emit_load32(c->e, kX64Rsi, HOSTPTR_REG, 0);

  x86p_emit_mov_r32_r32(c->e, kX64Rdx, EA_REG);
  x86p_emit_alu_r32_imm32(c->e, kX64Add, kX64Rdx, 4u);
  x86p_emit_store32(c->e, CPU_REG, reg_off(kX86pEsp), kX64Rdx);

  if (o->kind == kX86pOperandMem) {
    emit_mem_prepare(c, o, insn_eip);
    x86p_emit_store32(c->e, HOSTPTR_REG, 0, kX64Rsi);
    return;
  }
  x86p_emit_store32(c->e, CPU_REG, reg_off(o->reg), kX64Rsi);
}

static void emit_alu(X86pEmit *e, const X86pInsn *insn) {
  const X86pOperand *dst = &insn->operand[0];
  const X86pOperand *src = &insn->operand[1];

  /* Register-only: can_emit keeps memory operands away from ADC and SBB. */
  x86p_emit_load32(e, kX64Rsi, CPU_REG, reg_off(dst->reg)); /* a */
  if (src->kind == kX86pOperandImm) {
    x86p_emit_mov_r32_imm32(e, kX64Rdx, src->imm); /* b */
  } else {
    x86p_emit_load32(e, kX64Rdx, CPU_REG, reg_off(src->reg));
  }
  x86p_emit_mov_r32_imm32(e, kX64Rdi, (uint32_t)insn->alu);
  x86p_emit_mov_r32_imm32(e, kX64Rcx, 4u);
  x86p_emit_lea64(e, kX64R8, CPU_REG, flags_off());
  x86p_emit_mov_r64_imm64(e, kX64Rax, (uint64_t)(uintptr_t)&x86p_alu);
  x86p_emit_call_r64(e, kX64Rax);
  if (alu_writes_dest(insn->alu)) {
    x86p_emit_store32(e, CPU_REG, reg_off(dst->reg), kX64Rax);
  }
}

static void emit_mov(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip) {
  const X86pOperand *dst = &insn->operand[0];
  const X86pOperand *src = &insn->operand[1];

  if (dst->kind == kX86pOperandMem) {
    /* Prepare the destination FIRST, then materialise the value: reading the
       source cannot disturb HOSTPTR_REG, but computing an address can disturb
       a value already sitting in a scratch register. */
    emit_mem_prepare(c, dst, insn_eip);
    if (src->kind == kX86pOperandImm) {
      x86p_emit_store32_imm(c->e, HOSTPTR_REG, 0, src->imm);
      return;
    }
    x86p_emit_load32(c->e, kX64Rax, CPU_REG, reg_off(src->reg));
    x86p_emit_store32(c->e, HOSTPTR_REG, 0, kX64Rax);
    return;
  }

  if (src->kind == kX86pOperandImm) {
    x86p_emit_store32_imm(c->e, CPU_REG, reg_off(dst->reg), src->imm);
    return;
  }
  emit_read_operand(c, kX64Rax, src, insn_eip);
  x86p_emit_store32(c->e, CPU_REG, reg_off(dst->reg), kX64Rax);
}

/*
 * Prologue: save RBX, park the X86pCpu pointer in it.
 *
 * STACK ALIGNMENT. System V requires RSP to be 16-byte aligned immediately
 * before a CALL. On entry to this block -- itself reached by a CALL -- RSP is
 * 8 mod 16. One 8-byte push brings it to 0, which is exactly what the helper
 * calls below need. Pushing an even number of registers instead would leave it
 * misaligned, and misalignment does not fault: it corrupts whichever helper
 * first touches aligned SSE state, far from here.
 */
static void emit_prologue(X86pEmit *e) {
  x86p_emit_push_r64(e, CPU_REG);
  x86p_emit_mov_r64_r64(e, CPU_REG, kX64Rdi);
}

static void emit_epilogue(X86pEmit *e, uint32_t next_eip, X86pJitExit exit) {
  x86p_emit_store32_imm(e, CPU_REG, eip_off(), next_eip);
  x86p_emit_mov_r32_imm32(e, kX64Rax, (uint32_t)exit);
  x86p_emit_pop_r64(e, CPU_REG);
  x86p_emit_ret(e);
}

/* The same exit, but with the guest EIP already computed into a register --
   which is what a conditional branch produces. */
static void emit_epilogue_from(X86pEmit *e, X86pHostReg eip_reg, X86pJitExit exit) {
  x86p_emit_store32(e, CPU_REG, eip_off(), eip_reg);
  x86p_emit_mov_r32_imm32(e, kX64Rax, (uint32_t)exit);
  x86p_emit_pop_r64(e, CPU_REG);
  x86p_emit_ret(e);
}

/*
 * A conditional branch, emitted WITHOUT a forward jump.
 *
 * x86p_cond(cc, &cpu->flags) is the interpreter's own condition evaluator --
 * the same reason arithmetic calls x86p_alu. Both candidate addresses are then
 * materialised and CMOVcc selects between them, so there is no branch to patch
 * and no fixup list to forget to apply. `mov` does not disturb flags, so the
 * ZF that TEST set is still live at the CMOV.
 */
static void emit_jcc(X86pEmit *e, uint8_t cond, uint32_t target, uint32_t fallthrough) {
  x86p_emit_mov_r32_imm32(e, kX64Rdi, (uint32_t)cond);
  x86p_emit_lea64(e, kX64Rsi, CPU_REG, flags_off());
  x86p_emit_mov_r64_imm64(e, kX64Rax, (uint64_t)(uintptr_t)&x86p_cond);
  x86p_emit_call_r64(e, kX64Rax);
  x86p_emit_test_r32_r32(e, kX64Rax, kX64Rax);
  x86p_emit_mov_r32_imm32(e, kX64Rax, fallthrough);
  x86p_emit_mov_r32_imm32(e, kX64Rcx, target);
  x86p_emit_cmovcc_r32_r32(e, (unsigned)kX86pCondNZ, kX64Rax, kX64Rcx);
  emit_epilogue_from(e, kX64Rax, kX86pJitExitBlockEnd);
}

/*
 * CALL and RET: control transfers the block can COMPLETE rather than refuse.
 *
 * Both end the block -- the target is another block -- but ending it with
 * kX86pJitExitBlockEnd and the right EIP is a different thing from ending it
 * with kX86pJitExitUnsupported. The second hands the instruction back to the
 * interpreter; the first leaves the dispatcher a plain address to look up. On
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
  ctx.plan.host = (uint64_t)(uintptr_t)mem->host;
  ctx.plan.lo = mem->lo;
  ctx.plan.size = mem->size;
  if (mem->size < 4u) {
    /* The bounds check computes size - 4; a mapping smaller than one dword
       would underflow it into an enormous limit that admits everything. */
    say(reason, reason_len, "guest mapping of %u byte(s) is too small to address", mem->size);
    return kX86pJitOutOfSpace;
  }
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

    if (!can_emit(&insn)) {
      if (count == 0) {
        say(reason, reason_len, "%s at %08X has no x86-64 emitter in this build", insn.mnemonic, pc);
        return kX86pJitUnsupportedAtEntry;
      }
      exit = kX86pJitExitUnsupported;
      stopper = insn.mnemonic;
      break;
    }

    if (insn.op == (uint8_t)kX86pInsnCall || insn.op == (uint8_t)kX86pInsnRet) {
      uint32_t next = pc + insn.length;
      if (insn.op == (uint8_t)kX86pInsnCall) {
        emit_call_rel(&ctx, next, next + insn.operand[0].imm, pc);
      } else {
        emit_ret(&ctx, insn.operands == 1 ? insn.operand[0].imm : 0u, pc);
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
    case kX86pInsnMov:
      emit_mov(&ctx, &insn, pc);
      break;
    case kX86pInsnLea:
      emit_lea(&ctx, &insn);
      break;
    case kX86pInsnPush:
      emit_push(&ctx, &insn, pc);
      break;
    case kX86pInsnPop:
      emit_pop(&ctx, &insn, pc);
      break;
    case kX86pInsnAlu: {
      X86pHostAlu host;
      X86pFlagKind kind;
      int writes_dest;
      if (inline_alu_shape(insn.alu, &host, &kind, &writes_dest)) {
        emit_alu_inline(&ctx, &insn, host, kind, writes_dest, last_kind, pc);
        last_kind = (int)kind;
      } else {
        emit_alu(&e, &insn);
        /* x86p_alu decides the kind for ADC/SBB (Explicit), so the next
           instruction's predecessor is no longer statically known. */
        last_kind = -1;
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
   * before each bounds check -- so EIP lands ON the faulting instruction rather
   * than past it, and a caller can deliver the fault or hand that one
   * instruction to the interpreter.
   */
  if (ctx.nfaults) {
    unsigned f;
    for (f = 0; f < ctx.nfaults; f++) {
      x86p_emit_bind(&e, ctx.faults[f]);
    }
    x86p_emit_store32(&e, CPU_REG, eip_off(), FAULTPC_REG);
    x86p_emit_mov_r32_imm32(&e, kX64Rax, (uint32_t)kX86pJitExitMemoryFault);
    x86p_emit_pop_r64(&e, CPU_REG);
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
