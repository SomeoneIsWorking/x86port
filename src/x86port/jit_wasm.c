/*
 * jit_wasm.c -- the x86p_jit_* contract on a WebAssembly host.
 *
 * A thin adapter, and deliberately so. Everything about turning guest
 * instructions into WebAssembly lives in jit_wasm_lower.c and its per-family
 * units, which build anywhere; this file is the only part of the backend that
 * requires the wasm host, because it is the only part that assumes a host
 * pointer IS a linear-memory offset.
 *
 * That is why it is small: what cannot be built on a developer machine cannot
 * be tested there either, so as little as possible is in here.
 */
#include "jit_wasm.h"

#include "jit_wasm_lower.h"
#include "jit_wasm_module.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*
 * The whole backend rests on this: a host pointer and a guest linear address
 * are the same 32 bits, so the mapping's host base can be baked into emitted
 * code as an i32 constant. On a 64-bit host it could not be, and the truncation
 * would silently address the wrong memory.
 */
_Static_assert(sizeof(void *) == 4, "the WebAssembly backend assumes wasm32: a host pointer must be 32 bits");

static void say(char *buf, unsigned len, const char *fmt, ...) {
  va_list ap;
  if (!buf || len == 0) {
    return;
  }
  va_start(ap, fmt);
  vsnprintf(buf, len, fmt, ap);
  va_end(ap);
}

int x86p_jit_available(void) {
  /* This file is only compiled for the wasm host -- the backend selection in
     CMakeLists.txt is what decides that -- so reaching it means there is one. */
  return 1;
}

size_t x86p_jit_chain_entry_offset(void) {
  /* A transfer enters the block's function itself, by its table index. */
  return 0u;
}

unsigned x86p_jit_chain_slots_per_block(void) {
  return X86P_WASM_CHAIN_SLOTS;
}

/* A transfer is a tail call, which holds no frame (jit_wasm_chain.h). */
uint64_t x86p_jit_chain_transfer_limit(void) {
  return UINT64_MAX;
}

uint32_t x86p_jit_host_state(void) {
  return 0u;
}

X86pJitStatus x86p_jit_translate_bounded(const X86pMem *mem,
                                         uint32_t eip,
                                         void *code,
                                         size_t code_cap,
                                         const X86pJitTranslateEnv *env,
                                         X86pJitBlock *out,
                                         char *reason,
                                         unsigned reason_len) {
  const X86pJitBoundaryFn boundary = env ? env->boundary : NULL;
  void *const boundary_user = env ? env->boundary_user : NULL;
  const X86pWasmChainUse chain = {env ? env->chain : NULL, -1, 0u, NULL, 0u};
  const X86pWasmLeafUse leaf = {
      env ? env->leaf : NULL, env ? env->leaf_user : NULL, env ? env->leaf_sites : NULL, 0, NULL};
  X86pWasmModule module;
  X86pWasmPlan plan;
  X86pJitStatus status;
  size_t length;

  if (!mem || !out || !code) {
    say(reason, reason_len, "null argument");
    return kX86pJitOutOfSpace;
  }
  if (code_cap < X86P_WASM_MIN_MODULE_BYTES) {
    say(reason,
        reason_len,
        "a module buffer of %zu byte(s) cannot hold any block; the minimum is %u",
        code_cap,
        (unsigned)X86P_WASM_MIN_MODULE_BYTES);
    memset(out, 0, sizeof *out);
    return kX86pJitOutOfSpace;
  }

  x86p_wasm_plan_from_mem(mem, &plan);
  /* One block per module here. Batching several is what the builder is for and
     what the block cache will want; a caller that has only one block to
     translate is not made to pretend otherwise. */
  x86p_wasm_module_init(&module, code, code_cap, 1u);
  status =
      x86p_wasm_lower_block(&module, mem, &plan, eip, boundary, boundary_user, &chain, &leaf, out, reason, reason_len);
  if (status != kX86pJitOk) {
    return status;
  }
  length = x86p_wasm_module_finish(&module);
  if (length == 0) {
    say(reason, reason_len, "the module for the block at %08X could not be closed", eip);
    return kX86pJitOutOfSpace;
  }
  /*
   * host_bytes becomes the MODULE's length rather than the body's. What the
   * caller has to keep, hand to the engine and account for is the module, and
   * a figure that named only the body would under-report every block by the
   * size of the sections around it.
   */
  out->host_bytes = length;
  return kX86pJitOk;
}

X86pJitStatus x86p_jit_translate(const X86pMem *mem,
                                 uint32_t eip,
                                 void *code,
                                 size_t code_cap,
                                 X86pJitBlock *out,
                                 char *reason,
                                 unsigned reason_len) {
  return x86p_jit_translate_bounded(mem, eip, code, code_cap, NULL, out, reason, reason_len);
}

int x86p_jit_wasm_publish(
    X86pWasmArena *arena, X86pJitBlock *block, const void *module, size_t len, char *reason, unsigned reason_len) {
  const char *field = x86p_wasm_body_name(0);
  int token;
  void *entry;

  if (!block) {
    say(reason, reason_len, "no block to publish");
    return -1;
  }
  token = x86p_wasm_arena_publish(arena, module, len, reason, reason_len);
  if (token < 0) {
    return -1;
  }
  entry = x86p_wasm_arena_entry(arena, token, field);
  if (!entry) {
    /* Released rather than left live: a module whose export cannot be reached
       is a permanent engine object nothing will ever call. */
    x86p_wasm_arena_release(arena, token);
    say(reason, reason_len, "the instantiated module has no callable export named %s", field);
    return -1;
  }
  block->entry = entry;
  return token;
}

X86pJitExit x86p_jit_enter(const X86pJitBlock *b, X86pCpu *cpu) {
  if (!b || !b->entry || !cpu) {
    /* A block that was translated but never published has a NULL entry, which
       is exactly the case this refuses -- on this host that is the difference
       between "the engine has it" and "it is still bytes in a buffer". */
    return kX86pJitExitUnsupported;
  }
  /*
   * The entry converts to a function pointer through a pointer-sized integer
   * because ISO C does not define object-to-function pointer conversion. Here
   * the value really IS an index into the indirect function table, which is
   * what a function pointer is on this host, so the conversion is the intended
   * mechanism rather than a portability compromise.
   */
  return (X86pJitExit)x86p_jit_call_entry(b->entry, cpu);
}

int x86p_jit_can_translate(const X86pInsn *insn) {
  return x86p_wasm_can_lower(insn);
}
