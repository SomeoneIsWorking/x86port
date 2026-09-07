/*
 * jit_wasm_arena.c -- module lifetime accounting. See jit_wasm_arena.h for why
 * a cap and a named refusal are the answer rather than eviction.
 */
#include "jit_wasm_arena.h"

#include <stdarg.h>
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

static int host_is_usable(const X86pWasmHost *h) {
  return h && h->instantiate && h->resolve && h->release;
}

void x86p_wasm_arena_init(X86pWasmArena *a, const X86pWasmHost *host) {
  if (!a) {
    return;
  }
  memset(a, 0, sizeof *a);
  if (host_is_usable(host)) {
    a->host = *host;
  }
}

int x86p_wasm_arena_publish(X86pWasmArena *a, const void *bytes, size_t len, char *reason, unsigned reason_len) {
  unsigned i;
  int module;
  if (!a) {
    say(reason, reason_len, "no arena");
    return -1;
  }
  if (!host_is_usable(&a->host)) {
    /* Counted as a refusal rather than a failure: nothing was asked of an
       engine, because this build has none bound. */
    a->refusals++;
    say(reason, reason_len, "no WebAssembly engine is bound to this arena");
    return -1;
  }
  if (!bytes || len == 0) {
    a->refusals++;
    say(reason, reason_len, "empty module");
    return -1;
  }
  if (a->live >= X86P_WASM_MAX_LIVE_MODULES) {
    a->refusals++;
    say(reason,
        reason_len,
        "all %u module slots are live; the caller has published %u and released %u",
        (unsigned)X86P_WASM_MAX_LIVE_MODULES,
        a->published,
        a->released);
    return -1;
  }
  for (i = 0; i < (unsigned)X86P_WASM_MAX_LIVE_MODULES; i++) {
    if (!a->slot[i].live) {
      break;
    }
  }
  if (i == (unsigned)X86P_WASM_MAX_LIVE_MODULES) {
    /* `live` disagreed with the slots. That is a defect in this file, and it
       must not be papered over by overwriting a live slot. */
    a->refusals++;
    say(reason, reason_len, "internal: %u live modules but no free slot", a->live);
    return -1;
  }
  module = a->host.instantiate(a->host.user, bytes, len);
  if (module < 0) {
    a->failures++;
    say(reason, reason_len, "the engine rejected a %zu-byte module", len);
    return -1;
  }
  a->slot[i].live = 1;
  a->slot[i].module = module;
  a->live++;
  a->published++;
  return (int)i;
}

void *x86p_wasm_arena_entry(X86pWasmArena *a, int token, const char *field) {
  int callable;
  if (!a || token < 0 || (unsigned)token >= X86P_WASM_MAX_LIVE_MODULES || !field) {
    return NULL;
  }
  if (!a->slot[token].live || !host_is_usable(&a->host)) {
    return NULL;
  }
  callable = a->host.resolve(a->host.user, a->slot[token].module, field);
  if (callable <= 0) {
    /* Zero is the null table entry, so it cannot be a block. Returning NULL
       rather than a zero pointer keeps a caller from entering it. */
    return NULL;
  }
  return (void *)(uintptr_t)(unsigned)callable;
}

void x86p_wasm_arena_release(X86pWasmArena *a, int token) {
  if (!a || token < 0 || (unsigned)token >= X86P_WASM_MAX_LIVE_MODULES) {
    return;
  }
  if (!a->slot[token].live) {
    return;
  }
  if (host_is_usable(&a->host)) {
    a->host.release(a->host.user, a->slot[token].module);
  }
  a->slot[token].live = 0;
  a->slot[token].module = 0;
  a->live--;
  a->released++;
}

void x86p_wasm_arena_release_all(X86pWasmArena *a) {
  unsigned i;
  if (!a) {
    return;
  }
  for (i = 0; i < (unsigned)X86P_WASM_MAX_LIVE_MODULES; i++) {
    x86p_wasm_arena_release(a, (int)i);
  }
}

unsigned x86p_wasm_arena_live(const X86pWasmArena *a) {
  return a ? a->live : 0u;
}

unsigned x86p_wasm_arena_published(const X86pWasmArena *a) {
  return a ? a->published : 0u;
}

unsigned x86p_wasm_arena_released(const X86pWasmArena *a) {
  return a ? a->released : 0u;
}

unsigned x86p_wasm_arena_refusals(const X86pWasmArena *a) {
  return a ? a->refusals : 0u;
}

unsigned x86p_wasm_arena_failures(const X86pWasmArena *a) {
  return a ? a->failures : 0u;
}
