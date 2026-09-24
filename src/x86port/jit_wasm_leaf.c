/* See jit_wasm_leaf.h. */
#include "jit_wasm_leaf.h"

#include "jit_leaf_sites.h"
#include "jit_wasm_internal.h"

#include <stddef.h>

/* Alignment hints are zero throughout this backend; jit_wasm_state.c explains
   why a promise the guest does not make must not be emitted. */
#define ALIGN_NONE 0u

/* `leaf`, or the leaf in kX86pWasmLocalR when it is NULL, is called with EIP at
   `target` (or at kX86pWasmLocalTarget when `indirect`), as the callee would be
   entered, and its answer is left on the stack. */
static void call_leaf(X86pWasmLower *l, X86pJitLeafFn leaf, uint32_t target, int indirect) {
  x86p_wasm_state_cpu(&l->state);
  if (indirect) {
    x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalTarget);
  } else {
    x86p_wasm_i32_const(l->e, (int32_t)target);
  }
  x86p_wasm_i32_store(l->e, ALIGN_NONE, (uint32_t)offsetof(X86pCpu, eip));
  x86p_wasm_state_cpu(&l->state);
  if (leaf) {
    x86p_wasm_i32_const(l->e, (int32_t)(uintptr_t)leaf);
  } else {
    x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalR);
  }
  x86p_wasm_call_import(l, kX86pWasmImportLeafCall);
}

/* The site's field at `offset`, from the site's address as a constant. */
static void site_field(X86pWasmLower *l, uint32_t site, size_t offset) {
  x86p_wasm_i32_const(l->e, (int32_t)site);
  x86p_wasm_i32_load(l->e, ALIGN_NONE, (uint32_t)offset);
}

/*
 * The leaf for the runtime target in kX86pWasmLocalTarget, into kX86pWasmLocalR
 * (0 for none), as the x86-64 backend's emit_leaf_site finds it:
 *
 *     target == site.target  ? site.leaf
 *   : site.refills < REFILLS ? fill(site, target)
 *   :                          0
 */
static void site_leaf(X86pWasmLower *l, uint32_t site) {
  site_field(l, site, offsetof(X86pJitLeafSite, target));
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalTarget);
  x86p_wasm_i32_op(l->e, kWasmI32Eq);
  x86p_wasm_if(l->e, kWasmI32);
  site_field(l, site, offsetof(X86pJitLeafSite, leaf));
  x86p_wasm_else(l->e);
  site_field(l, site, offsetof(X86pJitLeafSite, refills));
  x86p_wasm_i32_const(l->e, (int32_t)X86P_JIT_LEAF_SITE_REFILLS);
  x86p_wasm_i32_op(l->e, kWasmI32LtU);
  x86p_wasm_if(l->e, kWasmI32);
  x86p_wasm_i32_const(l->e, (int32_t)site);
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalTarget);
  x86p_wasm_call_import(l, kX86pWasmImportLeafSiteFill);
  x86p_wasm_else(l->e);
  x86p_wasm_i32_const(l->e, 0);
  x86p_wasm_end(l->e);
  x86p_wasm_end(l->e);
  x86p_wasm_local_set(l->e, (uint32_t)kX86pWasmLocalR);
}

static struct X86pJitLeafSite *take_site(X86pWasmLower *l) {
  if (l->leaf->relowering) {
    return l->leaf->site;
  }
  return l->leaf->sites ? x86p_jit_leaf_sites_claim(l->leaf->sites) : NULL;
}

/*
 *   block
 *     leaf = the site's answer; none: br 0
 *     [cpu.eip] = target; leaf(cpu); declined: br 0
 *     exit to the return address
 *   end
 *   exit to the target, as an ordinary CALL
 */
static int lower_site(X86pWasmLower *l, uint32_t next) {
  struct X86pJitLeafSite *const site = take_site(l);
  if (!site) {
    return 0;
  }
  l->leaf_site = site;
  l->leaf_sites++;
  x86p_wasm_block(l->e, kWasmVoid);
  site_leaf(l, (uint32_t)(uintptr_t)site);
  x86p_wasm_local_get(l->e, (uint32_t)kX86pWasmLocalR);
  x86p_wasm_i32_op(l->e, kWasmI32Eqz);
  x86p_wasm_br_if(l->e, 0u);
  call_leaf(l, NULL, 0u, 1);
  x86p_wasm_i32_op(l->e, kWasmI32Eqz);
  x86p_wasm_br_if(l->e, 0u);
  x86p_wasm_state_exit_imm(&l->state, next, kX86pJitExitBlockEnd);
  x86p_wasm_end(l->e);
  x86p_wasm_state_exit_local(&l->state, kX86pWasmLocalTarget, kX86pJitExitBlockEnd);
  return 1;
}

/*
 *   [cpu.eip] = target; leaf(cpu)
 *   if completed: exit to the return address
 *   exit to the target, as an ordinary CALL
 */
static int lower_direct(X86pWasmLower *l, uint32_t target, uint32_t next) {
  const X86pJitLeafFn leaf = l->leaf->resolve(target, l->leaf->user);
  if (!leaf) {
    return 0;
  }
  l->leaf_calls++;
  call_leaf(l, leaf, target, 0);
  x86p_wasm_if(l->e, kWasmVoid);
  x86p_wasm_state_exit_imm(&l->state, next, kX86pJitExitBlockEnd);
  x86p_wasm_end(l->e);
  x86p_wasm_state_exit_imm(&l->state, target, kX86pJitExitBlockEnd);
  return 1;
}

int x86p_wasm_leaf_call_lower(X86pWasmLower *l, uint32_t target, uint32_t next, int indirect) {
  if (!l->leaf) {
    return 0;
  }
  return indirect ? lower_site(l, next) : lower_direct(l, target, next);
}

int x86p_wasm_leaf_call(X86pCpu *cpu, X86pJitLeafFn leaf) {
  return leaf(cpu);
}
