/*
 * jit_wasm_lower.c -- the block loop and the family dispatch table.
 * See jit_wasm_lower.h for why this is separable from the x86p_jit_* backend.
 */
#include "jit_wasm_lower.h"

#include "jit_wasm_cond.h"
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

void x86p_wasm_lower_flags_written(X86pWasmLower *l, int kind, int w) {
  l->last_kind = kind;
  /* The width exactly where a condition can be derived from it inline. */
  l->last_w = x86p_wasm_cond_is_inline(kind, kX86pCondZ) ? w : -1;
}

void x86p_wasm_call_import(X86pWasmLower *l, X86pWasmImport which) {
  x86p_wasm_call(l->e, (uint32_t)which);
}

/* CF for a predecessor of known `kind`, whose a/b/r are recorded at 32 bits or
   masked to their width: 0 or 1 on the stack. Returns 0, having emitted
   nothing, for a kind with no inline form. */
static int emit_known_cf(X86pWasmLower *l, int kind) {
  switch (kind) {
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
    return 0;
  }
  return 1;
}

/*
 * The kinds an unknown predecessor is answered for in the block, in the order
 * they are tested. Add and Sub only at 32 bits: state recorded outside a
 * translated block is not masked to its width, and at 32 bits there is nothing
 * to mask.
 */
typedef struct CarryArm {
  uint8_t kind;
  uint8_t dword_only;
} CarryArm;

static const CarryArm kCarryArms[] = {
    {kX86pFlagsSub, 1},
    {kX86pFlagsLogic, 0},
    {kX86pFlagsDec, 0},
    {kX86pFlagsInc, 0},
    {kX86pFlagsAdd, 1},
    {kX86pFlagsExplicit, 0},
    {kX86pFlagsNone, 0},
};

#define CARRY_ARMS (sizeof kCarryArms / sizeof kCarryArms[0])

_Static_assert(offsetof(X86pFlags, w) == offsetof(X86pFlags, kind) + 1u,
               "the unknown-predecessor dispatch reads kind and width as one halfword");

int x86p_wasm_carry_in_inline(int kind, int w) {
  size_t i;
  for (i = 0; i < CARRY_ARMS; ++i) {
    if (kCarryArms[i].kind == kind) {
      return !kCarryArms[i].dword_only || w == 4;
    }
  }
  return 0;
}

/*
 * An unknown predecessor: dispatch on the recorded kind and width, and ask the
 * one authority only for a pair no arm answers. The halfword waits in the
 * carry local, which the answer then replaces.
 */
static void emit_unknown_cf(X86pWasmLower *l) {
  size_t i;
  x86p_wasm_state_cpu(&l->state);
  x86p_wasm_i32_load16_u(l->e, 0u, (uint32_t)(offsetof(X86pCpu, flags) + offsetof(X86pFlags, kind)));
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalCarry);
  for (i = 0; i < CARRY_ARMS; ++i) {
    x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalCarry);
    if (kCarryArms[i].dword_only) {
      x86p_wasm_i32_const(l->e, (int32_t)(kCarryArms[i].kind | (4u << 8)));
    } else {
      x86p_wasm_i32_const(l->e, 0xFF);
      x86p_wasm_i32_op(l->e, kWasmI32And);
      x86p_wasm_i32_const(l->e, (int32_t)kCarryArms[i].kind);
    }
    x86p_wasm_i32_op(l->e, kWasmI32Eq);
    x86p_wasm_if(l->e, kWasmI32);
    (void)emit_known_cf(l, kCarryArms[i].kind);
    x86p_wasm_else(l->e);
  }
  x86p_wasm_state_flags_addr(&l->state);
  x86p_wasm_call_import(l, kX86pWasmImportFlagCf);
  for (i = 0; i < CARRY_ARMS; ++i) {
    x86p_wasm_end(l->e);
  }
}

void x86p_wasm_carry_in(X86pWasmLower *l) {
  if (!emit_known_cf(l, l->last_kind)) {
    emit_unknown_cf(l);
    l->flag_helper_calls++;
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

void (*x86p_wasm_continue_lower(uint8_t op))(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc) {
  switch (op) {
  case kX86pInsnJcc:
    return x86p_wasm_jcc_continue;
  case kX86pInsnJecxz:
    return x86p_wasm_jecxz_continue;
  default:
    return NULL;
  }
}

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

void x86p_wasm_plan_from_mem(const X86pMem *mem, X86pWasmPlan *plan) {
  plan->base = (uint32_t)(uintptr_t)mem->host;
  plan->lo = mem->lo;
  plan->size = mem->size;
  plan->memory_context = mem->sparse ? (uint32_t)(uintptr_t)mem : 0u;
  plan->perms = mem->sparse ? 0u : (uint32_t)(uintptr_t)mem->perms;
  plan->page_shift = mem->page_shift;
}

X86pJitStatus x86p_wasm_lower_block(X86pWasmModule *m,
                                    const X86pMem *fetch,
                                    const X86pWasmPlan *plan,
                                    uint32_t eip,
                                    X86pJitBoundaryFn boundary,
                                    void *boundary_user,
                                    const X86pWasmChainUse *chain,
                                    const X86pWasmLeafUse *leaf,
                                    X86pJitBlock *out,
                                    char *reason,
                                    unsigned reason_len) {
  X86pWasmLower l;
  uint32_t pc = eip;
  uint32_t count = 0;
  size_t body_start;
  X86pJitExit exit = kX86pJitExitBlockEnd;
  const char *stopper = NULL;
  /* The first instruction that lowered past its reservation, if any. */
  const char *oversized_insn = NULL;
  uint32_t oversized_at = 0;
  size_t oversized_bytes = 0;
  int terminated = 0;
  int keep_going;
  void (*continuation)(X86pWasmLower *l, const X86pInsn *insn, uint32_t pc);
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
  /* A leaf returns into the block through a chained exit. */
  l.leaf = chain && chain->chain && leaf && leaf->resolve ? leaf : NULL;
  x86p_wasm_lower_flags_written(&l, -1, -1);
  x86p_wasm_state_init(&l.state, l.e, plan, eip, chain);

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
    if (x86p_wasm_here(l.e) + X86P_WASM_WORST_CASE_INSN_BYTES + X86P_WASM_EXIT_BYTES +
            x86p_wasm_chain_reserve(&l.state.chain) >
        l.e->cap) {
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

    /*
     * A conditional hands the block the rest of its fall-through. Its taken
     * path already carries its own exit, so the run continues and whatever ends
     * it later -- the cap, a boundary, an unconditional exit -- closes the body.
     * `count < MAX` keeps the cap reachable: the loop top would otherwise be
     * skipped by this continue and the block could grow without bound.
     */
    /* Everything from the block's first byte up to and including this address
       has been lowered, so an exit naming an address in that span is a branch
       back into this block's own code. The exit census reads it. */
    l.state.pc = pc;

    continuation = entry->terminates ? x86p_wasm_continue_lower((uint8_t)insn.op) : NULL;
    keep_going = continuation != NULL && count < X86P_WASM_MAX_INSNS;
    const size_t insn_start = x86p_wasm_here(l.e);
    const size_t chain_start = l.state.chain.bytes;
    if (keep_going) {
      continuation(&l, &insn, pc);
    } else {
      entry->lower(&l, &insn, pc);
    }
    /* The room check above reserved the worst case; an instruction past it
       makes that check a guess, so the reservation is wrong, not this block.
       Its chained exits are not its own bytes: they draw on the chain reserve,
       which holds each to X86P_WASM_CHAIN_EXIT_BYTES. A CALL that calls a
       leaf has two. */
    const size_t insn_bytes = x86p_wasm_here(l.e) - insn_start - (l.state.chain.bytes - chain_start);
    if (!oversized_insn && insn_bytes > X86P_WASM_WORST_CASE_INSN_BYTES) {
      oversized_at = pc;
      oversized_bytes = insn_bytes;
      oversized_insn = insn.mnemonic;
    }
    pc += insn.length;
    count++;
    if (keep_going) {
      continue;
    }
    if (entry->terminates) {
      terminated = 1;
      break;
    }
  }

  if (!terminated) {
    x86p_wasm_state_exit_imm(&l.state, pc, exit);
  }
  x86p_wasm_module_body_end(m);

  if (oversized_insn) {
    say(reason,
        reason_len,
        "%s at %08X lowered to %zu bytes, past X86P_WASM_WORST_CASE_INSN_BYTES (%u)",
        oversized_insn,
        oversized_at,
        oversized_bytes,
        X86P_WASM_WORST_CASE_INSN_BYTES);
    return kX86pJitOutOfSpace;
  }
  if (l.state.chain.oversized != 0u) {
    say(reason,
        reason_len,
        "a chained exit of the block at %08X lowered to %zu bytes, past X86P_WASM_CHAIN_EXIT_BYTES (%u)",
        eip,
        l.state.chain.oversized,
        X86P_WASM_CHAIN_EXIT_BYTES);
    return kX86pJitOutOfSpace;
  }
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
  out->cond_helper_calls = l.conds - l.cond_inline;
  out->cond_inline = l.cond_inline;
  out->cond_unknown_kind = l.cond_unknown_kind;
  out->x87_loads = l.x87_loads;
  out->x87_loads_inline = l.x87_loads_inline;
  out->x87_stores = l.x87_stores;
  out->x87_stores_inline = l.x87_stores_inline;
  out->x87_compares = l.x87_compares;
  out->x87_compares_inline = l.x87_compares_inline;
  out->simd_ops = l.simd_ops;
  out->simd_inline = l.simd_inline;
  out->exits = l.state.exits.total;
  out->exits_static = l.state.exits.to_immediate;
  for (unsigned target = 0u; target < l.state.exits.target_count; target++) {
    out->static_targets[target] = l.state.exits.targets[target];
  }
  out->static_target_count = l.state.exits.target_count;
  out->static_targets_overflowed = l.state.exits.targets_overflowed;
  out->exits_backward = l.state.exits.backward;
  out->exits_loop = l.state.exits.within_block;
  out->exits_self = l.state.exits.to_entry;
  out->chain_exits = l.state.chain.slotted;
  out->chain_exits_unslotted = l.state.chain.unslotted;
  out->chain_exits_direct = l.state.chain.direct;
  out->chain_first_slot = l.state.chain.first;
  out->leaf_calls = l.leaf_calls;
  out->leaf_sites = l.leaf_sites;
  out->leaf_site = l.leaf_site;
  out->ends_in_branch = terminated;
  return kX86pJitOk;
}
