/*
 * jit_wasm_arena.c -- module lifetime accounting. See jit_wasm_arena.h for why
 * a cap and a named refusal are the answer rather than eviction.
 */
#include "jit_wasm_arena.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
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

int x86p_wasm_arena_init(X86pWasmArena *a, const X86pWasmHost *host, unsigned capacity) {
  unsigned i;
  if (!a) {
    return 0;
  }
  memset(a, 0, sizeof *a);
  if (host_is_usable(host)) {
    a->host = *host;
  }
  if (capacity == 0u) {
    return 0;
  }
  a->slot = calloc(capacity, sizeof *a->slot);
  if (!a->slot) {
    return 0;
  }
  a->capacity = capacity;
  /* Thread the free list through the slots, lowest index first, so a fresh
     arena hands out slot 0 before slot 1 and the tests can say which. */
  for (i = 0; i < capacity; i++) {
    a->slot[i].next_free = (i + 1u < capacity) ? (i + 2u) : 0u;
  }
  a->free_head = 1u;
  return 1;
}

void x86p_wasm_arena_dispose(X86pWasmArena *a) {
  if (!a) {
    return;
  }
  x86p_wasm_arena_release_all(a);
  free(a->slot);
  a->slot = NULL;
  a->capacity = 0u;
  a->free_head = 0u;
}

unsigned x86p_wasm_arena_capacity(const X86pWasmArena *a) {
  return a ? a->capacity : 0u;
}

int x86p_wasm_arena_publish(X86pWasmArena *a, const void *bytes, size_t len, char *reason, unsigned reason_len) {
  unsigned i;
  int module;
  char detail[192];
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
  if (a->free_head == 0u) {
    a->refusals++;
    say(reason,
        reason_len,
        "all %u module slots are live; the caller has published %u and released %u",
        a->capacity,
        a->published,
        a->released,
        a->ceiling);
    return -1;
  }
  i = a->free_head - 1u;
  if (i >= a->capacity || a->slot[i].live) {
    /* The free list disagreed with the slots. That is a defect in this file,
       and it must not be papered over by overwriting a live slot. */
    a->refusals++;
    say(reason, reason_len, "internal: free list names slot %u of %u, which is not free", i, a->capacity);
    return -1;
  }
  detail[0] = '\0';
  module = a->host.instantiate(a->host.user, bytes, len, detail, sizeof detail);
  if (module < 0) {
    a->failures++;
    /* A ceiling is learned only from a refusal that was about the ENGINE. A
       module it would not compile says nothing about how many modules it will
       hold, and treating the two alike capped one run at 2,363 modules -- the
       live count when a single malformed module was refused -- after which it
       evicted live code for four minutes to stay under a limit that was never
       there. */
    if (module == kX86pWasmRefusedByEngine && (a->ceiling == 0u || a->live < a->ceiling)) {
      a->ceiling = a->live;
      a->headroom = a->ceiling / kX86pWasmCeilingHeadroomDivisor;
    }
    /* With the denominators: an engine that refuses the fortieth module and
       one that refuses the eight-thousandth are different problems, and the
       refusal is the only place that number is ever seen. */
    if (a->ceiling == 0u) {
      say(reason,
          reason_len,
          "the engine rejected a %zu-byte MODULE: %s (%u live of %u slot(s); %u published and %u released so far; "
          "this says nothing about how many modules the engine will hold, so no ceiling is learned from it)",
          len,
          detail[0] ? detail : "no reason given",
          a->live,
          a->capacity,
          a->published,
          a->released);
    } else {
      say(reason,
          reason_len,
          "the ENGINE rejected a %zu-byte module: %s (%u live of %u slot(s); %u published and %u released so far; "
          "the arena will work at no more than %u of a %u ceiling from here)",
          len,
          detail[0] ? detail : "no reason given",
          a->live,
          a->capacity,
          a->published,
          a->released,
          a->ceiling - a->headroom,
          a->ceiling);
    }
    return -1;
  }
  a->free_head = a->slot[i].next_free;
  a->slot[i].live = 1;
  a->slot[i].module = module;
  a->slot[i].next_free = 0u;
  a->live++;
  a->published++;
  return (int)i;
}

void *x86p_wasm_arena_entry(X86pWasmArena *a, int token, const char *field) {
  int callable;
  if (!a || token < 0 || (unsigned)token >= a->capacity || !field) {
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

int x86p_wasm_arena_can_adopt(const X86pWasmArena *a) {
  return a && host_is_usable(&a->host) && a->host.adopt != NULL;
}

int x86p_wasm_arena_adopt(
    X86pWasmArena *a, int to, const char *to_field, int from, const char *from_field, void *entry) {
  unsigned index;
  if (!x86p_wasm_arena_can_adopt(a) || !to_field || !from_field || !entry) {
    return 0;
  }
  if (to < 0 || (unsigned)to >= a->capacity || from < 0 || (unsigned)from >= a->capacity) {
    return 0;
  }
  if (!a->slot[to].live || !a->slot[from].live) {
    return 0;
  }
  index = (unsigned)(uintptr_t)entry;
  if (index == 0u) {
    /* Table entry 0 is the null entry, so it is not an address a block was
       ever entered at -- adopting it would point the null entry at real code. */
    return 0;
  }
  if (!a->host.adopt(a->host.user, a->slot[to].module, to_field, a->slot[from].module, from_field, (int)index)) {
    return 0;
  }
  a->adoptions++;
  return 1;
}

void x86p_wasm_arena_release(X86pWasmArena *a, int token) {
  if (!a || token < 0 || (unsigned)token >= a->capacity) {
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
  a->slot[token].next_free = a->free_head;
  a->free_head = (unsigned)token + 1u;
  a->live--;
  a->released++;
}

void x86p_wasm_arena_release_all(X86pWasmArena *a) {
  unsigned i;
  if (!a) {
    return;
  }
  for (i = 0; i < a->capacity; i++) {
    x86p_wasm_arena_release(a, (int)i);
  }
}

unsigned x86p_wasm_arena_live(const X86pWasmArena *a) {
  return a ? a->live : 0u;
}

unsigned x86p_wasm_arena_ceiling(const X86pWasmArena *a) {
  return a ? a->ceiling : 0u;
}

int x86p_wasm_arena_has_room(const X86pWasmArena *a) {
  if (!a || a->live >= a->capacity) {
    return 0;
  }
  return a->ceiling == 0u || a->live + a->headroom < a->ceiling;
}

unsigned x86p_wasm_arena_headroom(const X86pWasmArena *a) {
  return a ? a->headroom : 0u;
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

unsigned x86p_wasm_arena_adoptions(const X86pWasmArena *a) {
  return a ? a->adoptions : 0u;
}

unsigned x86p_wasm_arena_failures(const X86pWasmArena *a) {
  return a ? a->failures : 0u;
}
