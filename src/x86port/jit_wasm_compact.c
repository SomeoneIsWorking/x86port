#include "jit_wasm_compact.h"

#include "jit_wasm_lower.h"
#include "jit_wasm_module.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void say(char *reason, unsigned reason_len, const char *fmt, ...) {
  va_list args;
  if (!reason || reason_len == 0u) {
    return;
  }
  va_start(args, fmt);
  vsnprintf(reason, reason_len, fmt, args);
  va_end(args);
}

size_t x86p_jit_translate_batch(const X86pMem *mem,
                                const uint32_t *eips,
                                unsigned count,
                                void *code,
                                size_t code_cap,
                                X86pJitBoundaryFn boundary,
                                void *boundary_user,
                                const X86pWasmChainUse *chains,
                                const X86pWasmLeafUse *leaves,
                                X86pJitBlock *out,
                                char *reason,
                                unsigned reason_len) {
  X86pWasmModule module;
  X86pWasmPlan plan;
  unsigned i;

  if (!mem || !eips || !code || !out || count == 0u) {
    say(reason, reason_len, "null argument");
    return 0;
  }
  if (count > X86P_WASM_MAX_BODIES) {
    say(reason, reason_len, "%u blocks is more than the %u a module may hold", count, X86P_WASM_MAX_BODIES);
    return 0;
  }
  x86p_wasm_plan_from_mem(mem, &plan);
  x86p_wasm_module_init(&module, code, code_cap, count);
  for (i = 0; i < count; ++i) {
    X86pJitStatus status = x86p_wasm_lower_block(&module,
                                                 mem,
                                                 &plan,
                                                 eips[i],
                                                 boundary,
                                                 boundary_user,
                                                 chains ? &chains[i] : NULL,
                                                 leaves ? &leaves[i] : NULL,
                                                 &out[i],
                                                 reason,
                                                 reason_len);
    if (status != kX86pJitOk) {
      /*
       * All or nothing. The module's sections already promised `count` bodies,
       * so a short module is not a smaller module -- it is a broken one, and
       * x86p_wasm_module_finish refuses it. Saying so here names the block
       * that stopped it, which a caller retrying with a smaller set needs.
       */
      char said[256];
      snprintf(said, sizeof said, "%s", (reason && reason_len && reason[0]) ? reason : "no reason given");
      say(reason,
          reason_len,
          "block %u of %u (guest %08X) could not be lowered into a shared module: %s",
          i + 1u,
          count,
          eips[i],
          said);
      return 0;
    }
    /* Each body is a separate export, and the caller enters block i through
       the export named for body i. */
    out[i].host_bytes = 0u;
  }
  return x86p_wasm_module_finish(&module);
}

/*
 * Did the block lower to the same thing it was published as?
 *
 * The batch is lowered from guest memory a second time, so this is the check
 * that the guest has not rewritten the code in between. If it has, the new
 * body is NOT the code the block's callers were entered into, and adopting the
 * entry would silently replace a running block with different instructions.
 * The block needs invalidating instead, which is the caller's job, so all this
 * does is refuse.
 */
static int lowered_the_same(const X86pWasmCompactBlock *was, const X86pJitBlock *now) {
  return now->guest_eip == was->guest && now->guest_len == was->guest_len && now->chain_exits == was->chain_exits &&
         now->leaf_site == was->leaf_site;
}

int x86p_wasm_compact(X86pWasmArena *arena,
                      const X86pMem *mem,
                      void *buffer,
                      size_t buffer_bytes,
                      const X86pJitTranslateEnv *env,
                      const X86pWasmCompactBlock *blocks,
                      unsigned count,
                      X86pWasmCompactResult *out,
                      char *reason,
                      unsigned reason_len) {
  uint32_t eips[X86P_WASM_MAX_BODIES];
  X86pWasmChainUse chains[X86P_WASM_MAX_BODIES];
  X86pWasmLeafUse leaves[X86P_WASM_MAX_BODIES];
  X86pJitChain *const chain = env ? env->chain : NULL;
  X86pJitBlock lowered[X86P_WASM_MAX_BODIES];
  size_t bytes;
  int token;
  unsigned i;

  if (!arena || !mem || !buffer || !blocks || !out || count == 0u) {
    say(reason, reason_len, "null argument");
    return 0;
  }
  memset(out, 0, sizeof *out);
  out->token = -1;
  if (count > X86P_WASM_MAX_BODIES) {
    say(reason, reason_len, "%u blocks is more than the %u a module may hold", count, X86P_WASM_MAX_BODIES);
    return 0;
  }
  if (!x86p_wasm_arena_can_adopt(arena)) {
    /* Not a failure of this batch: this engine cannot move an entry at all, so
       no batch will ever compact and the caller should stop asking. */
    say(reason, reason_len, "this engine cannot move a block's entry between modules");
    return 0;
  }
  for (i = 0; i < count; ++i) {
    eips[i] = blocks[i].guest;
    /* A block published with no slot keeps none: reuse of zero slots. */
    chains[i] = (X86pWasmChainUse){chain, blocks[i].chain_exits ? blocks[i].chain_first : 0, blocks[i].chain_exits};
    /* The same resolver, and the site the block was published with: a
       relowering claims none. */
    leaves[i] = (X86pWasmLeafUse){env ? env->leaf : NULL, env ? env->leaf_user : NULL, NULL, 1, blocks[i].leaf_site};
  }
  bytes = x86p_jit_translate_batch(mem,
                                   eips,
                                   count,
                                   buffer,
                                   buffer_bytes,
                                   env ? env->boundary : NULL,
                                   env ? env->boundary_user : NULL,
                                   chains,
                                   leaves,
                                   lowered,
                                   reason,
                                   reason_len);
  if (bytes == 0u) {
    return 0;
  }
  for (i = 0; i < count; ++i) {
    if (!lowered_the_same(&blocks[i], &lowered[i])) {
      say(reason,
          reason_len,
          "the block at %08X now covers %u guest byte(s) with %u chained exit(s) where it covered %u with %u "
          "when it was published; the guest changed it and it needs invalidating, not rebuilding",
          blocks[i].guest,
          lowered[i].guest_len,
          lowered[i].chain_exits,
          blocks[i].guest_len,
          blocks[i].chain_exits);
      return 0;
    }
  }
  token = x86p_wasm_arena_publish(arena, buffer, bytes, reason, reason_len);
  if (token < 0) {
    return 0;
  }
  for (i = 0; i < count; ++i) {
    const char *to_field = x86p_wasm_body_name(i);
    const char *from_field = x86p_wasm_body_name(0);
    if (to_field && from_field &&
        x86p_wasm_arena_adopt(arena, token, to_field, blocks[i].token, from_field, blocks[i].entry)) {
      out->moved++;
      continue;
    }
    /*
     * Half a batch is not a state to leave anything in: the blocks already
     * moved are entered through the new module, the rest through their old
     * ones, and releasing either side now frees an entry something still uses.
     * So move the moved ones BACK, and release the module that was just
     * published. Every block ends where it started.
     */
    while (out->moved > 0u) {
      unsigned back = --out->moved;
      const char *was = x86p_wasm_body_name(back);
      x86p_wasm_arena_adopt(arena, blocks[back].token, from_field, token, was, blocks[back].entry);
    }
    x86p_wasm_arena_release(arena, token);
    say(reason,
        reason_len,
        "the block at %08X could not be moved onto the shared module's body %u; "
        "every block was put back where it was",
        blocks[i].guest,
        i);
    return 0;
  }
  /* Only now, with every entry pointing at the new module, are the old ones
     free of anything that can be entered. */
  for (i = 0; i < count; ++i) {
    x86p_wasm_arena_release(arena, blocks[i].token);
  }
  out->token = token;
  out->bytes = bytes;
  return 1;
}
