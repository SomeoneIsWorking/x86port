/* jit_leaf_sites.c -- see jit_leaf_sites.h. */
#include "jit_leaf_sites.h"

#include <stdlib.h>
#include <string.h>

_Static_assert(offsetof(X86pJitLeafSite, target) == 0u, "translated code compares the target at +0");
_Static_assert(offsetof(X86pJitLeafSite, refills) == 4u, "translated code reads the refills at +4");
_Static_assert(offsetof(X86pJitLeafSite, leaf) == 8u, "translated code loads the leaf at +8");

struct X86pJitLeafSites {
  X86pJitLeafSite *site;
  size_t capacity;
  size_t used;
  X86pJitLeafResolveFn resolve;
  void *user;
  X86pJitLeafSitesStats stats;
};

X86pJitLeafSites *x86p_jit_leaf_sites_create(size_t capacity, X86pJitLeafResolveFn resolve, void *user) {
  if (!resolve || capacity == 0u) {
    return NULL;
  }
  X86pJitLeafSites *s = calloc(1u, sizeof *s);
  if (!s) {
    return NULL;
  }
  s->site = calloc(capacity, sizeof *s->site);
  if (!s->site) {
    free(s);
    return NULL;
  }
  s->capacity = capacity;
  s->resolve = resolve;
  s->user = user;
  return s;
}

void x86p_jit_leaf_sites_destroy(X86pJitLeafSites *s) {
  if (s) {
    free(s->site);
    free(s);
  }
}

X86pJitLeafSite *x86p_jit_leaf_sites_claim(X86pJitLeafSites *s) {
  if (s->used == s->capacity) {
    s->stats.refused++;
    return NULL;
  }
  X86pJitLeafSite *site = &s->site[s->used++];
  site->target = 0u;
  site->refills = 0u;
  site->leaf = NULL;
  site->pool = s;
  s->stats.claimed++;
  return site;
}

size_t x86p_jit_leaf_sites_mark(const X86pJitLeafSites *s) {
  return s ? s->used : 0u;
}

void x86p_jit_leaf_sites_rewind(X86pJitLeafSites *s, size_t mark) {
  if (s && mark <= s->used) {
    s->stats.claimed -= s->used - mark;
    s->used = mark;
  }
}

void x86p_jit_leaf_sites_reset(X86pJitLeafSites *s) {
  if (s) {
    s->used = 0u;
    s->stats.claimed = 0u;
  }
}

X86pJitLeafFn x86p_jit_leaf_site_fill(X86pJitLeafSite *site, uint32_t target) {
  X86pJitLeafSites *const s = site->pool;
  const X86pJitLeafFn leaf = s->resolve(target, s->user);
  site->target = target;
  site->leaf = leaf;
  if (++site->refills == X86P_JIT_LEAF_SITE_REFILLS) {
    s->stats.exhausted++;
  }
  s->stats.fills++;
  s->stats.leaf_fills += leaf != NULL;
  return leaf;
}

void x86p_jit_leaf_sites_stats(const X86pJitLeafSites *s, X86pJitLeafSitesStats *out) {
  if (!out) {
    return;
  }
  if (!s) {
    memset(out, 0, sizeof *out);
    return;
  }
  *out = s->stats;
}
