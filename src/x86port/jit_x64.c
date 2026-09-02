/* jit_x64.c -- see jit_x64.h for why arithmetic is called rather than inlined. */
#include "jit_x64.h"

#include "alu.h"
#include "cond.h"
#include "decode.h"
#include "emit_x64.h"

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

/* Guest instructions per block. A cap so a straight-line run of translatable
   code cannot consume the whole arena in one block, and so a caller always gets
   a chance to invalidate between blocks. */
#define MAX_INSNS 64

/* Emitting one guest instruction never exceeds this, so the buffer is checked
   once per instruction rather than after every emit. The margin is generous
   and the emitter's own overflow flag is still the authority -- this only
   decides when to stop trying. */
#define WORST_CASE_INSN_BYTES 96
#define EPILOGUE_BYTES 32

const char *x86p_jit_exit_name(X86pJitExit e) {
  switch (e) {
  case kX86pJitExitBlockEnd:
    return "block end";
  case kX86pJitExitUnsupported:
    return "unsupported instruction";
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
    return insn->operands == 2 && operand_is_reg32(&insn->operand[0]) &&
           (operand_is_reg32(&insn->operand[1]) || operand_is_imm(&insn->operand[1]));
  case kX86pInsnAlu:
    /* Shifts and rotates take their count from CL or an immediate and have
       their own flag rules; excluded until they are tested on their own. */
    if (insn->alu > (uint8_t)kX86pAluTest) {
      return 0;
    }
    return insn->operands == 2 && operand_is_reg32(&insn->operand[0]) &&
           (operand_is_reg32(&insn->operand[1]) || operand_is_imm(&insn->operand[1]));
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

/* ---- emitting ------------------------------------------------------------ */

/* Load a reg-or-immediate operand into a host register. */
static void emit_operand_value(X86pEmit *e, X86pHostReg dst, const X86pOperand *o) {
  if (o->kind == kX86pOperandImm) {
    x86p_emit_mov_r32_imm32(e, dst, o->imm);
  } else {
    x86p_emit_load32(e, dst, CPU_REG, reg_off(o->reg));
  }
}

/*
 * x86p_alu(op, a, b, w, &cpu->flags) -> result in EAX.
 *
 * System V argument order is RDI, RSI, RDX, RCX, R8. The two operand loads run
 * FIRST, because they read through CPU_REG; the later argument setup writes
 * registers they would otherwise have to avoid.
 */
static void emit_alu(X86pEmit *e, const X86pInsn *insn) {
  const X86pOperand *dst = &insn->operand[0];
  const X86pOperand *src = &insn->operand[1];

  emit_operand_value(e, kX64Rsi, dst); /* a */
  emit_operand_value(e, kX64Rdx, src); /* b */
  x86p_emit_mov_r32_imm32(e, kX64Rdi, (uint32_t)insn->alu);
  x86p_emit_mov_r32_imm32(e, kX64Rcx, 4u);
  x86p_emit_lea64(e, kX64R8, CPU_REG, flags_off());
  x86p_emit_mov_r64_imm64(e, kX64Rax, (uint64_t)(uintptr_t)&x86p_alu);
  x86p_emit_call_r64(e, kX64Rax);
  if (alu_writes_dest(insn->alu)) {
    x86p_emit_store32(e, CPU_REG, reg_off(dst->reg), kX64Rax);
  }
}

static void emit_mov(X86pEmit *e, const X86pInsn *insn) {
  const X86pOperand *dst = &insn->operand[0];
  const X86pOperand *src = &insn->operand[1];
  if (src->kind == kX86pOperandImm) {
    x86p_emit_store32_imm(e, CPU_REG, reg_off(dst->reg), src->imm);
    return;
  }
  x86p_emit_load32(e, kX64Rax, CPU_REG, reg_off(src->reg));
  x86p_emit_store32(e, CPU_REG, reg_off(dst->reg), kX64Rax);
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

/* ---- translation --------------------------------------------------------- */

X86pJitStatus x86p_jit_translate(const X86pMem *mem,
                                 uint32_t eip,
                                 void *code,
                                 size_t code_cap,
                                 X86pJitBlock *out,
                                 char *reason,
                                 unsigned reason_len) {
  X86pEmit e;
  uint32_t pc = eip;
  uint32_t count = 0;
  X86pJitExit exit = kX86pJitExitBlockEnd;
  const char *stopper = NULL;
  int terminated = 0; /* a branch already emitted the exit */

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
      emit_mov(&e, &insn);
      break;
    case kX86pInsnAlu:
      emit_alu(&e, &insn);
      break;
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
