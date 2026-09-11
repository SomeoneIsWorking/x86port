/*
 * jit_wasm_lower.c -- the block loop and the family dispatch table.
 * See jit_wasm_lower.h for why this is separable from the x86p_jit_* backend.
 */
#include "jit_wasm_lower.h"

#include "jit_wasm_internal.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static void say(char *buf, unsigned len, const char *fmt, ...) {
  va_list ap;
  if (!buf || len == 0) {
    return;
  }
  va_start(ap, fmt);
  vsnprintf(buf, len, fmt, ap);
  va_end(ap);
}

/* ---- shared operand helpers --------------------------------------------- */

uint32_t x86p_wasm_memory_context(const X86pWasmLower *l) {
  return l->state.plan.memory_context ? l->state.plan.memory_context : (uint32_t)(uintptr_t)l->fetch;
}

int x86p_wasm_width_ok(int w) {
  return w == 1 || w == 2 || w == 4;
}

uint32_t x86p_wasm_width_mask(int w) {
  return (w == 1) ? 0xFFu : ((w == 2) ? 0xFFFFu : 0xFFFFFFFFu);
}

int x86p_wasm_operand_ok(const X86pOperand *o, int w, int for_write) {
  if (!x86p_wasm_width_ok(w)) {
    return 0;
  }
  if (o->kind == kX86pOperandImm) {
    /*
     * An immediate has no width of its own that matters here. The decoder has
     * already sign-extended it to 32 bits and the operation masks it to the
     * destination's width, so requiring size == w would refuse every `83 /r`
     * -- the ubiquitous imm8 form of the dword ALU ops.
     */
    return !for_write;
  }
  if (o->kind == kX86pOperandMem) {
    return o->size == (uint8_t)w && !o->addr16;
  }
  if (o->kind != kX86pOperandReg || o->size != (uint8_t)w) {
    return 0;
  }
  /* A byte-register INDEX above 7 is refused rather than interpreted:
     x86p_byte_reg owns which register an index names, and an index outside its
     range is an encoding this decoder should never produce. Refusing by name
     makes it visible if one ever appears. */
  return w != 1 || (o->reg >= 0 && o->reg < 8);
}

void x86p_wasm_push_operand(X86pWasmLower *l, const X86pOperand *o, int w) {
  if (o->kind == kX86pOperandImm) {
    x86p_wasm_i32_const(l->e, (int32_t)(o->imm & x86p_wasm_width_mask(w)));
    return;
  }
  x86p_wasm_state_load_reg(&l->state, o->reg, w);
}

void x86p_wasm_call_import(X86pWasmLower *l, X86pWasmImport which) {
  /* This backend lowers no condition to a host comparison -- wasm has no flag
     register to read one off -- so every condition it evaluates is a call to
     the shared authority, counted here rather than at four call sites. */
  if (which == kX86pWasmImportCond) {
    l->conds++;
  }
  x86p_wasm_call(l->e, (uint32_t)which);
}

void x86p_wasm_carry_in(X86pWasmLower *l) {
  switch (l->last_kind) {
  case kX86pFlagsNone:
  case kX86pFlagsLogic:
    /* Both give CF == 0 with no computation at all. */
    x86p_wasm_i32_const(l->e, 0);
    break;
  case kX86pFlagsAdd:
    /* CF = r < a, unsigned. */
    x86p_wasm_state_cpu(&l->state);
    x86p_wasm_i32_load(l->e, 0u, (uint32_t)(offsetof(X86pCpu, flags) + offsetof(X86pFlags, r)));
    x86p_wasm_state_cpu(&l->state);
    x86p_wasm_i32_load(l->e, 0u, (uint32_t)(offsetof(X86pCpu, flags) + offsetof(X86pFlags, a)));
    x86p_wasm_i32_op(l->e, kWasmI32LtU);
    break;
  case kX86pFlagsSub:
    /* CF = a < b, unsigned. */
    x86p_wasm_state_cpu(&l->state);
    x86p_wasm_i32_load(l->e, 0u, (uint32_t)(offsetof(X86pCpu, flags) + offsetof(X86pFlags, a)));
    x86p_wasm_state_cpu(&l->state);
    x86p_wasm_i32_load(l->e, 0u, (uint32_t)(offsetof(X86pCpu, flags) + offsetof(X86pFlags, b)));
    x86p_wasm_i32_op(l->e, kWasmI32LtU);
    break;
  case kX86pFlagsExplicit:
    /* A real EFLAGS word, which ADC, SBB and POPFD leave behind: CF is bit 0
       of `a`, so masking it IS the 0-or-1 the field wants. */
    x86p_wasm_state_cpu(&l->state);
    x86p_wasm_i32_load(l->e, 0u, (uint32_t)(offsetof(X86pCpu, flags) + offsetof(X86pFlags, a)));
    x86p_wasm_i32_const(l->e, (int32_t)X86P_CF);
    x86p_wasm_i32_op(l->e, kWasmI32And);
    break;
  case kX86pFlagsInc:
  case kX86pFlagsDec:
    /* PRESERVED. INC and DEC do not write CF, so the carry the state already
       holds IS the carry, and x86p_flag_cf returns exactly this byte. */
    x86p_wasm_state_cpu(&l->state);
    x86p_wasm_i32_load8_u(l->e, 0u, (uint32_t)(offsetof(X86pCpu, flags) + offsetof(X86pFlags, carry_in)));
    break;
  default:
    /* Unknown predecessor: ask the one authority. Once per block. */
    x86p_wasm_state_flags_addr(&l->state);
    x86p_wasm_call_import(l, kX86pWasmImportFlagCf);
    l->flag_helper_calls++;
    break;
  }
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalCarry);
}

/* ---- the family dispatch table ------------------------------------------ */

/*
 * One row per instruction family this backend lowers. A family absent from the
 * table is refused BY NAME at the instruction that has it, which is the whole
 * unsupported-set report: the stopper mnemonic is what ranks the remaining
 * work.
 */
static const X86pWasmOpEntry kTable[kX86pInsnOpCount] = {
    [kX86pInsnShld] = {x86p_wasm_shift_accepts, x86p_wasm_shift_lower, 0},
    [kX86pInsnShrd] = {x86p_wasm_shift_accepts, x86p_wasm_shift_lower, 0},
    [kX86pInsnBit] = {x86p_wasm_bit_accepts, x86p_wasm_bit_lower, 0},
    [kX86pInsnBcd] = {x86p_wasm_bcd_accepts, x86p_wasm_bcd_lower, 0},
    [kX86pInsnCmovcc] = {x86p_wasm_cmov_accepts, x86p_wasm_cmov_lower, 0},
    [kX86pInsnSahf] = {NULL, x86p_wasm_flags_lower, 0},
    [kX86pInsnLahf] = {NULL, x86p_wasm_flags_lower, 0},
    [kX86pInsnStc] = {NULL, x86p_wasm_flags_lower, 0},
    [kX86pInsnClc] = {NULL, x86p_wasm_flags_lower, 0},
    [kX86pInsnCmc] = {NULL, x86p_wasm_flags_lower, 0},
    [kX86pInsnSalc] = {NULL, x86p_wasm_flags_lower, 0},
    [kX86pInsnPushad] = {NULL, x86p_wasm_stack_lower, 0},
    [kX86pInsnPopad] = {NULL, x86p_wasm_stack_lower, 0},
    [kX86pInsnEnter] = {x86p_wasm_enter_accepts, x86p_wasm_stack_lower, 0},
    [kX86pInsnInt3] = {x86p_wasm_trap_accepts, x86p_wasm_trap_lower, 1},
    [kX86pInsnInt1] = {x86p_wasm_trap_accepts, x86p_wasm_trap_lower, 1},
    [kX86pInsnInto] = {x86p_wasm_trap_accepts, x86p_wasm_trap_lower, 1},
    [kX86pInsnInt] = {x86p_wasm_trap_accepts, x86p_wasm_trap_lower, 1},
    [kX86pInsnHlt] = {NULL, x86p_wasm_privilege_lower, 1},
    [kX86pInsnWbinvd] = {NULL, x86p_wasm_privilege_lower, 1},
    [kX86pInsnCli] = {NULL, x86p_wasm_privilege_lower, 1},
    [kX86pInsnSti] = {NULL, x86p_wasm_privilege_lower, 1},
    [kX86pInsnPortIo] = {NULL, x86p_wasm_privilege_lower, 1},
    [kX86pInsnCpuid] = {NULL, x86p_wasm_cpu_lower, 0},
    [kX86pInsnRdtsc] = {NULL, x86p_wasm_cpu_lower, 0},
    [kX86pInsnX87] = {x86p_wasm_x87_accepts, x86p_wasm_x87_lower, 0},
    [kX86pInsnSimd] = {x86p_wasm_simd_accepts, x86p_wasm_simd_lower, 0},
    [kX86pInsnMul] = {x86p_wasm_multiply_accepts, x86p_wasm_multiply_lower, 0},
    [kX86pInsnImul] = {x86p_wasm_multiply_accepts, x86p_wasm_multiply_lower, 0},
    [kX86pInsnDiv] = {x86p_wasm_divide_accepts, x86p_wasm_divide_lower, 0},
    [kX86pInsnIdiv] = {x86p_wasm_divide_accepts, x86p_wasm_divide_lower, 0},
    [kX86pInsnString] = {x86p_wasm_string_accepts, x86p_wasm_string_lower, 0},
    [kX86pInsnLoop] = {x86p_wasm_loop_accepts, x86p_wasm_loop_lower, 1},
    [kX86pInsnLoope] = {x86p_wasm_loop_accepts, x86p_wasm_loop_lower, 1},
    [kX86pInsnLoopne] = {x86p_wasm_loop_accepts, x86p_wasm_loop_lower, 1},
    [kX86pInsnPushfd] = {NULL, x86p_wasm_pushfd_lower, 0},
    [kX86pInsnPopfd] = {NULL, x86p_wasm_popfd_lower, 0},
    [kX86pInsnNop] = {NULL, x86p_wasm_nop_lower, 0},
    [kX86pInsnMov] = {x86p_wasm_mov_accepts, x86p_wasm_mov_lower, 0},
    [kX86pInsnMovzx] = {x86p_wasm_movx_accepts, x86p_wasm_movzx_lower, 0},
    [kX86pInsnMovsx] = {x86p_wasm_movx_accepts, x86p_wasm_movsx_lower, 0},
    [kX86pInsnAlu] = {x86p_wasm_alu_accepts, x86p_wasm_alu_lower, 0},
    [kX86pInsnAluUnary] = {x86p_wasm_alu_unary_accepts, x86p_wasm_alu_unary_lower, 0},
    [kX86pInsnLea] = {x86p_wasm_lea_accepts, x86p_wasm_lea_lower, 0},
    [kX86pInsnXchg] = {x86p_wasm_xchg_accepts, x86p_wasm_xchg_lower, 0},
    [kX86pInsnSetcc] = {x86p_wasm_setcc_accepts, x86p_wasm_setcc_lower, 0},
    [kX86pInsnPush] = {x86p_wasm_push_accepts, x86p_wasm_push_lower, 0},
    [kX86pInsnPop] = {x86p_wasm_pop_accepts, x86p_wasm_pop_lower, 0},
    [kX86pInsnLeave] = {NULL, x86p_wasm_leave_lower, 0},
    [kX86pInsnCdq] = {NULL, x86p_wasm_cdq_lower, 0},
    [kX86pInsnCwde] = {NULL, x86p_wasm_cwde_lower, 0},
    [kX86pInsnCld] = {NULL, x86p_wasm_cld_lower, 0},
    [kX86pInsnStd] = {NULL, x86p_wasm_std_lower, 0},
    [kX86pInsnJmp] = {x86p_wasm_jmp_accepts, x86p_wasm_jmp_lower, 1},
    [kX86pInsnJcc] = {x86p_wasm_jcc_accepts, x86p_wasm_jcc_lower, 1},
    [kX86pInsnJecxz] = {x86p_wasm_jecxz_accepts, x86p_wasm_jecxz_lower, 1},
    [kX86pInsnCall] = {x86p_wasm_call_accepts, x86p_wasm_call_lower, 1},
    [kX86pInsnRet] = {x86p_wasm_ret_accepts, x86p_wasm_ret_lower, 1},
};

const X86pWasmOpEntry *x86p_wasm_op_entry(uint8_t op) {
  const X86pWasmOpEntry *entry;
  if (op >= (uint8_t)kX86pInsnOpCount) {
    return NULL;
  }
  entry = &kTable[op];
  /* An all-zero row is a family with no emitter. `lower` is what says so --
     `accepts` is legitimately NULL for the operandless families. */
  return entry->lower ? entry : NULL;
}

int x86p_wasm_can_lower(const X86pInsn *insn) {
  const X86pWasmOpEntry *entry;
  if (!insn) {
    return 0;
  }
  entry = x86p_wasm_op_entry(insn->op);
  if (!entry) {
    return 0;
  }
  return entry->accepts ? entry->accepts(insn) : 1;
}

/* ---- the block loop ------------------------------------------------------ */

X86pJitStatus x86p_wasm_lower_block(X86pWasmModule *m,
                                    const X86pMem *fetch,
                                    const X86pWasmPlan *plan,
                                    uint32_t eip,
                                    X86pJitBoundaryFn boundary,
                                    void *boundary_user,
                                    X86pJitBlock *out,
                                    char *reason,
                                    unsigned reason_len) {
  X86pWasmLower l;
  uint32_t pc = eip;
  uint32_t count = 0;
  size_t body_start;
  X86pJitExit exit = kX86pJitExitBlockEnd;
  const char *stopper = NULL;
  int terminated = 0;
  int body;

  if (!m || !fetch || !plan || !out) {
    say(reason, reason_len, "null argument");
    return kX86pJitOutOfSpace;
  }
  memset(out, 0, sizeof *out);

  memset(&l, 0, sizeof l);
  l.module = m;
  l.e = x86p_wasm_module_emitter(m);
  l.fetch = fetch;
  l.last_kind = -1;
  x86p_wasm_state_init(&l.state, l.e, plan);

  body = x86p_wasm_module_body_begin(m);
  if (body < 0) {
    say(reason, reason_len, "the module has no room for another block body");
    return kX86pJitOutOfSpace;
  }
  body_start = x86p_wasm_here(l.e);

  for (;;) {
    uint8_t bytes[X86P_MAX_INSN_LEN];
    X86pInsn insn;
    const X86pWasmOpEntry *entry;
    uint32_t avail;
    uint32_t i;

    if (count >= X86P_WASM_MAX_INSNS) {
      break;
    }
    /* Stop while there is certainly room for this instruction AND for closing
       the module around it. Discovering the overflow afterwards would mean
       discarding a nearly finished block, and worse, would leave a body whose
       last instruction is half written. */
    if (x86p_wasm_here(l.e) + X86P_WASM_WORST_CASE_INSN_BYTES + X86P_WASM_EXIT_BYTES > l.e->cap) {
      break;
    }

    /* Fetch. A partial fetch at the end of the mapping is a fetch fault, not a
       decode failure, and only the FIRST instruction can fail the whole
       lowering. */
    avail = 0;
    for (i = 0; i < (uint32_t)X86P_MAX_INSN_LEN; i++) {
      uint32_t byte;
      if (!x86p_mem_read(fetch, pc + i, 1, &byte)) {
        break;
      }
      bytes[i] = (uint8_t)byte;
      avail++;
    }
    if (avail == 0) {
      if (count == 0) {
        say(reason, reason_len, "guest EIP %08X is not mapped", pc);
        x86p_wasm_state_exit_imm(&l.state, eip, kX86pJitExitUnsupported);
        x86p_wasm_module_body_end(m);
        return kX86pJitFetchFault;
      }
      break;
    }

    if (!x86p_decode(bytes, avail, &insn)) {
      if (count == 0) {
        say(reason, reason_len, "the bytes at %08X are not an instruction", pc);
        x86p_wasm_state_exit_imm(&l.state, eip, kX86pJitExitUnsupported);
        x86p_wasm_module_body_end(m);
        return kX86pJitDecodeFailed;
      }
      break;
    }

    /* Stop before an address the consumer intercepts. `pc != eip` because the
       block leader was already cleared by that same predicate. */
    if (pc != eip && boundary && boundary(pc, boundary_user)) {
      stopper = "consumer interception point";
      break;
    }

    entry = x86p_wasm_op_entry(insn.op);
    if (!entry || (entry->accepts && !entry->accepts(&insn))) {
      if (count == 0) {
        say(reason, reason_len, "%s at %08X has no WebAssembly lowering in this build", insn.mnemonic, pc);
        /*
         * The body is closed with an immediate refusal rather than left open.
         * An open body would make the whole module unbalanced and cost the
         * caller every OTHER block batched into it; a body that refuses is
         * structurally valid, and a caller holding a non-Ok status has no
         * reason to enter it in the first place.
         */
        x86p_wasm_state_exit_imm(&l.state, eip, kX86pJitExitUnsupported);
        x86p_wasm_module_body_end(m);
        return kX86pJitUnsupportedAtEntry;
      }
      exit = kX86pJitExitUnsupported;
      stopper = insn.mnemonic;
      break;
    }

    entry->lower(&l, &insn, pc);
    pc += insn.length;
    count++;
    if (entry->terminates) {
      terminated = 1;
      break;
    }
  }

  if (!terminated) {
    x86p_wasm_state_exit_imm(&l.state, pc, exit);
  }
  x86p_wasm_module_body_end(m);

  if (!x86p_wasm_intact(l.e)) {
    /* `intact` and not `ok`: the module's code section is still open here, and
       ok() would also be false for a healthy one. */
    say(reason, reason_len, "module buffer of %zu byte(s) too small for the block at %08X", l.e->cap, eip);
    return kX86pJitOutOfSpace;
  }
  if (count == 0) {
    /* Nothing was lowered and no branch claimed it. A block that runs zero
       guest instructions makes no progress, and a caller that cached it would
       spin at full speed forever. */
    say(reason, reason_len, "lowered 0 instructions at %08X", eip);
    return kX86pJitUnsupportedAtEntry;
  }

  out->entry = NULL; /* a module is not an address; see jit_wasm_arena.h */
  out->guest_eip = eip;
  out->guest_len = pc - eip;
  out->insns = count;
  out->host_bytes = x86p_wasm_here(l.e) - body_start;
  out->stopper = stopper;
  out->flag_helper_calls = l.flag_helper_calls;
  out->conds = l.conds;
  out->cond_helper_calls = l.conds;
  out->cond_inline = 0u;
  out->ends_in_branch = terminated;
  return kX86pJitOk;
}
