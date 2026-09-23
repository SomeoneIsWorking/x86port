/*
 * jit_leaf_sites.h -- leaves for CALLs whose target is known only at run time.
 *
 * A direct CALL names its target, so the translation asks the consumer's
 * resolver for its leaf once (jit_x64.h, X86pJitLeafFn). A CALL through a
 * register or memory -- a COM vtable, an import slot -- does not, but most such
 * sites call one target every time. Each one gets a SITE here: the target it
 * last called and that target's leaf, or NULL. The translated CALL compares
 * its runtime target with the site's and, when they match, calls the leaf in
 * place as a direct CALL would; otherwise it asks x86p_jit_leaf_site_fill,
 * which records the new target and the resolver's answer for it.
 *
 * A site that keeps changing target stops asking after
 * X86P_JIT_LEAF_SITE_REFILLS answers: from then on only its last target takes
 * the leaf path, and every other call leaves the block as an ordinary CALL
 * without calling out. So a polymorphic guest call site costs a compare and
 * two branches, not a resolver call per CALL.
 *
 * A site is plain data owned by the engine, never patched code. The sites are
 * returned together when the engine drops every translation (a flush);
 * evicting single blocks leaves their sites claimed, and a full pool only
 * means later sites are translated without one.
 */
#ifndef X86PORT_JIT_LEAF_SITES_H
#define X86PORT_JIT_LEAF_SITES_H

#include "jit_x64.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define X86P_JIT_LEAF_SITE_REFILLS 4u

/* Read by translated code at these offsets; see the static asserts in
   jit_leaf_sites.c. */
typedef struct X86pJitLeafSite {
  uint32_t target;        /* the target last resolved; 0 before the first call */
  uint32_t refills;       /* resolver answers taken, up to X86P_JIT_LEAF_SITE_REFILLS */
  X86pJitLeafFn leaf;     /* `target`'s leaf, or NULL */
  X86pJitLeafSites *pool; /* the pool whose resolver answers */
} X86pJitLeafSite;

/* A pool of `capacity` sites answering from `resolve`. NULL on allocation
   failure. */
X86pJitLeafSites *x86p_jit_leaf_sites_create(size_t capacity, X86pJitLeafResolveFn resolve, void *user);
void x86p_jit_leaf_sites_destroy(X86pJitLeafSites *s);

/* A fresh site for one translated CALL, or NULL when the pool is spent. */
X86pJitLeafSite *x86p_jit_leaf_sites_claim(X86pJitLeafSites *s);

/* The claim position, and a return to it: for a translation that failed after
   claiming. */
size_t x86p_jit_leaf_sites_mark(const X86pJitLeafSites *s);
void x86p_jit_leaf_sites_rewind(X86pJitLeafSites *s, size_t mark);

/* Return every site; only when no translation that holds one survives. */
void x86p_jit_leaf_sites_reset(X86pJitLeafSites *s);

/* Called by translated code when `site` has not seen `target` and can still
   refill: records `target` and its leaf, and returns that leaf. */
X86pJitLeafFn x86p_jit_leaf_site_fill(X86pJitLeafSite *site, uint32_t target);

typedef struct X86pJitLeafSitesStats {
  uint64_t claimed;    /* sites handed to translations since the last reset */
  uint64_t refused;    /* claims made while the pool was spent */
  uint64_t fills;      /* resolver answers recorded at run time */
  uint64_t leaf_fills; /* ... of which named a leaf */
  uint64_t exhausted;  /* sites that used their last refill */
} X86pJitLeafSitesStats;

void x86p_jit_leaf_sites_stats(const X86pJitLeafSites *s, X86pJitLeafSitesStats *out);

#ifdef __cplusplus
}
#endif

#endif
