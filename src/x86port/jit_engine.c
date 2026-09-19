#include "jit_engine.h"

#include "block_cache.h"
#include "decode.h"
#include "jit_storage.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct X86pJitEngine {
  const X86pMem *mem;
  X86pJitStorage *storage;
  JcBlockCache *cache;
  X86pJitEngineStats stats;
  X86pJitInterceptFn intercept;
  void *intercept_user;
  X86pJitDispatchFn dispatch;
  void *dispatch_user;
  X86pJitBoundaryFn boundary;
  void *boundary_user;
  int cache_disabled;        /* diagnostic: retranslate every block, never reuse one */
  X86pJitProfile *profile;   /* diagnostic: block-entry histogram, or NULL */
  X86pJitChainCensus *chain; /* diagnostic: where dispatches actually went, or NULL */
  /* The previous block entry address, for stats.blocks_reentered. A two-entry
     history, not a successor graph; see where it is read. */
  uint32_t last_entry;
  /* diagnostic: report the first few entries to one address. See
     x86p_jit_engine_set_entry_watch. */
  X86pJitEntryWatchFn watch;
  void *watch_user;
  uint32_t watch_addr;
  uint64_t watch_left;
};

const char *x86p_jit_run_status_name(X86pJitRunStatus s) {
  switch (s) {
  case kX86pRunBudget:
    return "budget exhausted";
  case kX86pRunDecodeFailed:
    return "decode failed";
  case kX86pRunUnsupported:
    return "unsupported instruction";
  case kX86pRunFetchFault:
    return "fetch fault";
  case kX86pRunMemoryFault:
    return "memory fault";
  case kX86pRunDivideError:
    return "divide error";
  case kX86pRunInterrupt:
    return "software interrupt";
  case kX86pRunProtectionFault:
    return "general-protection fault (a ring-3 process may not do that)";
  case kX86pRunBoundRange:
    return "bound range exceeded";
  case kX86pRunTranslateFailed:
    return "translation failed";
  case kX86pRunOutOfCode:
    return "out of code memory";
  case kX86pRunIntercept:
    return "intercepted by consumer";
  case kX86pRunStatusCount:
    break;
  }
  return "unknown run status";
}

static void say(char *buf, unsigned len, const char *fmt, ...) {
  va_list ap;
  if (!buf || len == 0u) {
    return;
  }
  va_start(ap, fmt);
  (void)vsnprintf(buf, len, fmt, ap);
  va_end(ap);
}

/* Report the instruction the product refused, from the bytes currently mapped
   at its EIP. Both translation-time refusal and a mid-block unsupported exit
   use this owner so one path cannot regress to an address-only diagnostic. */
static void say_unsupported(const X86pMem *mem, uint32_t eip, char *reason, unsigned reason_len) {
  uint8_t encoded[X86P_MAX_INSN_LEN] = {0};
  char bytes[X86P_MAX_INSN_LEN * 3u];
  X86pInsn insn;
  uint32_t available = 0u;
  uint32_t length;
  uint32_t i;
  size_t used = 0u;

  for (i = 0u; i < X86P_MAX_INSN_LEN; i++) {
    uint32_t value;
    if (!x86p_mem_read(mem, eip + i, 1, &value)) {
      break;
    }
    encoded[available++] = (uint8_t)value;
  }
  length = x86p_decode(encoded, available, &insn);
  if (length == 0u || length > available) {
    say(reason, reason_len, "unsupported instruction at %08X: bytes could not be decoded", eip);
    return;
  }
  bytes[0] = '\0';
  for (i = 0u; i < length; i++) {
    int written = snprintf(bytes + used, sizeof bytes - used, "%s%02x", i == 0u ? "" : " ", encoded[i]);
    if (written < 0 || (size_t)written >= sizeof bytes - used) {
      break;
    }
    used += (size_t)written;
  }
  say(reason, reason_len, "unsupported instruction at %08X: %s bytes %s", eip, insn.mnemonic, bytes);
}

const char *x86p_jit_engine_mechanism(void) {
  return x86p_jit_storage_mechanism();
}

X86pJitEngine *
x86p_jit_engine_create(const X86pMem *mem, size_t code_bytes, size_t cache_blocks, char *reason, unsigned reason_len) {
  X86pJitEngine *e;
  char why[256];

  if (!mem) {
    say(reason, reason_len, "no guest memory");
    return NULL;
  }
#if !defined(__EMSCRIPTEN__)
  if (mem->sparse) {
    say(reason, reason_len, "sparse guest memory requires the WebAssembly backend");
    return NULL;
  }
#endif
  if (code_bytes < x86p_jit_storage_min_capacity()) {
    /* Refused rather than raised: a region too small for one block would fail
       on its first translation, and reporting that as "out of code" at run time
       blames the program for the caller's sizing. */
    say(reason,
        reason_len,
        "code region of %zu bytes is below the %u a single block may need",
        code_bytes,
        (unsigned)x86p_jit_storage_min_capacity());
    return NULL;
  }

  e = (X86pJitEngine *)calloc(1u, sizeof *e);
  if (!e) {
    say(reason, reason_len, "out of memory");
    return NULL;
  }
  e->mem = mem;

  why[0] = '\0';
  /* ONE number for both: the storage holds exactly as many translations as the
     cache can name. When they disagreed, the larger one was a lie -- a cache
     entry whose code had already been evicted is a miss with extra steps. */
  e->storage = x86p_jit_storage_create(code_bytes, cache_blocks, why, (unsigned)sizeof why);
  if (!e->storage) {
    say(reason, reason_len, "code memory (%s): %s", x86p_jit_storage_mechanism(), why);
    free(e);
    return NULL;
  }

  e->cache = jc_block_cache_create(cache_blocks);
  if (!e->cache) {
    say(reason, reason_len, "block cache of %zu entries could not be created", cache_blocks);
    x86p_jit_storage_destroy(e->storage);
    free(e);
    return NULL;
  }
  return e;
}

void x86p_jit_engine_destroy(X86pJitEngine *e) {
  if (!e) {
    return;
  }
  jc_block_cache_destroy(e->cache);
  x86p_jit_storage_destroy(e->storage);
  x86p_jit_profile_destroy(e->profile);
  x86p_jit_chain_census_destroy(e->chain);
  free(e);
}

static size_t drop_range(X86pJitEngine *e, uint32_t lo, uint32_t hi) {
  const size_t dropped = jc_block_invalidate_range(e->cache, lo, hi);
  x86p_jit_storage_invalidate(e->storage, lo, hi);
  return dropped;
}

void x86p_jit_engine_invalidate(X86pJitEngine *e, uint32_t lo, uint32_t hi) {
  if (e) {
    /*
     * Count the ASK and the EFFECT separately. An embedder that notifies on
     * every guest page operation and an embedder whose guest rewrites its own
     * code produce the same `blocks_translated` climb, and only these two
     * numbers tell them apart: many calls dropping nothing is a notification
     * the embedder did not need to send, where few calls dropping thousands is
     * the guest genuinely replacing code. Dropping only the effect would make
     * a pure-overhead notification storm invisible.
     *
     * The engine's OWN reclaim does not come through here -- see
     * `evict_for_room`. Counting both as one number reads as an embedder
     * notifying tens of thousands of times a second, which is a false
     * accusation: measured on a browser run, 500 embedder calls arrived beside
     * 106,000 evictions and the combined figure named the wrong owner.
     */
    e->stats.invalidations++;
    e->stats.invalidation_bytes += (uint64_t)(hi - lo);
    e->stats.invalidation_blocks_dropped += (uint64_t)drop_range(e, lo, hi);
  }
}

void x86p_jit_engine_stats(const X86pJitEngine *e, X86pJitEngineStats *out) {
  if (!out) {
    return;
  }
  if (!e) {
    memset(out, 0, sizeof *out);
    return;
  }
  *out = e->stats;
  out->code_bytes_used = x86p_jit_storage_used(e->storage);
  out->compactions = x86p_jit_storage_compactions(e->storage);
  out->compaction_refusals = x86p_jit_storage_compaction_refusals(e->storage);
  out->compaction_pending = x86p_jit_storage_compaction_pending(e->storage);
  out->compaction_stopped = (uint64_t)(x86p_jit_storage_compaction_stopped(e->storage) != 0);
  out->code_bytes_limit = (uint64_t)x86p_jit_storage_capacity(e->storage);
  out->block_records = (uint64_t)x86p_jit_storage_block_records(e->storage);
}

void x86p_jit_engine_stats_add(X86pJitEngineStats *sum, const X86pJitEngineStats *item) {
  size_t i;
  uint64_t *dst;
  const uint64_t *src;
  if (!sum || !item) {
    return;
  }
  dst = (uint64_t *)sum;
  src = (const uint64_t *)item;
  /*
   * Every field is a uint64_t total, so summing them as an array keeps a field
   * added later from being silently left out -- which is exactly what a
   * hand-written member list does, without failing to build.
   */
  _Static_assert(sizeof *sum % sizeof(uint64_t) == 0, "X86pJitEngineStats is counters only");
  for (i = 0; i < sizeof *sum / sizeof(uint64_t); i++) {
    dst[i] += src[i];
  }
}

void x86p_jit_engine_set_intercept(X86pJitEngine *e, X86pJitInterceptFn fn, void *user) {
  if (e) {
    e->intercept = fn;
    e->intercept_user = user;
  }
}

void x86p_jit_engine_set_dispatch(X86pJitEngine *e, X86pJitDispatchFn fn, void *user) {
  if (e) {
    e->dispatch = fn;
    e->dispatch_user = user;
  }
}

void x86p_jit_engine_set_boundary(X86pJitEngine *e, X86pJitBoundaryFn fn, void *user) {
  if (e) {
    e->boundary = fn;
    e->boundary_user = user;
  }
}

void x86p_jit_engine_set_cache(X86pJitEngine *e, int enabled) {
  if (e) {
    e->cache_disabled = enabled ? 0 : 1;
  }
}

int x86p_jit_engine_set_profile(X86pJitEngine *e, int enabled, uint32_t slot_hint, char *reason, unsigned reason_len) {
  if (!e) {
    say(reason, reason_len, "no JIT engine");
    return 0;
  }
  if (!enabled) {
    x86p_jit_profile_destroy(e->profile);
    e->profile = NULL;
    return 1;
  }
  if (!e->profile) {
    e->profile = x86p_jit_profile_create(slot_hint);
    if (!e->profile) {
      say(reason, reason_len, "block profile of %u slots could not be created", slot_hint);
      return 0;
    }
  }
  return 1;
}

void x86p_jit_engine_set_entry_watch(
    X86pJitEngine *e, uint32_t guest_addr, uint64_t reports, X86pJitEntryWatchFn fn, void *user) {
  if (!e) {
    return;
  }
  /* One store order: the count is what the hot path tests, so it goes last on
     arming and first on disarming. */
  if (!fn || reports == 0u) {
    e->watch_left = 0u;
    e->watch = NULL;
    e->watch_user = NULL;
    return;
  }
  e->watch = fn;
  e->watch_user = user;
  e->watch_addr = guest_addr;
  e->watch_left = reports;
}

const X86pJitProfile *x86p_jit_engine_profile(const X86pJitEngine *e) {
  return e ? e->profile : NULL;
}

int x86p_jit_engine_set_chain_census(
    X86pJitEngine *e, int enabled, uint32_t slot_hint, char *reason, unsigned reason_len) {
  if (!e) {
    say(reason, reason_len, "no JIT engine");
    return 0;
  }
  if (!enabled) {
    x86p_jit_chain_census_destroy(e->chain);
    e->chain = NULL;
    return 1;
  }
  if (!e->chain) {
    e->chain = x86p_jit_chain_census_create(slot_hint);
    if (!e->chain) {
      say(reason, reason_len, "chain census of %u slots could not be created", slot_hint);
      return 0;
    }
  }
  return 1;
}

const X86pJitChainCensus *x86p_jit_engine_chain_census(const X86pJitEngine *e) {
  return e ? e->chain : NULL;
}

/*
 * Drop every translation and rewind the code region.
 *
 * Both halves, together, or the engine is corrupt: a cache entry surviving a
 * rewind points at bytes the next translation is about to overwrite, and a
 * rewind without a flush leaks the region until nothing can be translated. The
 * two are one operation for that reason and there is no way to do half of it.
 */
int x86p_jit_engine_invalidate_all(X86pJitEngine *e, char *reason, unsigned reason_len) {
  if (!e) {
    say(reason, reason_len, "no JIT engine");
    return 0;
  }
  jc_block_flush(e->cache);
  /*
   * CHECKED, not assumed. An entry surviving the flush points into arena bytes
   * the next translation is about to overwrite, and entering it executes
   * whatever landed there -- intermittently, depending on what got translated
   * next. There is no outcome a test can watch for that: the stale entry is
   * only wrong once something has overwritten its bytes AND it is entered
   * again, which most runs never manage. So the invariant is enforced where it
   * is established, and a violation stops the process rather than running code
   * from a freed address.
   */
  if (jc_block_count(e->cache) != 0u) {
    say(reason,
        reason_len,
        "%zu block(s) survived a cache flush; refusing to reuse their code arena",
        jc_block_count(e->cache));
    return 0;
  }
  x86p_jit_storage_reset(e->storage);
  e->stats.cache_flushes++;
  return 1;
}

/*
 * Make room in the code region for one more translation.
 *
 * This is the engine reclaiming its OWN arena, not the embedder reporting that
 * guest memory changed, and the two are counted apart because they say
 * opposite things about what to do. An embedder notification dropping code
 * means the guest replaced it. An eviction dropping code means the arena is
 * too small for the working set, and every eviction is a block the run is
 * about to pay to translate again.
 *
 * Returns zero only when even a full flush cannot free space.
 */
/* Forget the engine's record of a block the storage is evicting, before its
   exec address can be handed to another block. The storage drops its own
   record; this side owns only the cache. */
static void forget_evicted(void *user, uint32_t lo, uint32_t hi) {
  X86pJitEngine *e = (X86pJitEngine *)user;
  e->stats.eviction_blocks_dropped += (uint64_t)jc_block_invalidate_range(e->cache, lo, hi);
}

static int evict_for_room(X86pJitEngine *e, char *reason, unsigned reason_len) {
  while (!x86p_jit_storage_has_room(e->storage)) {
    const size_t before = x86p_jit_storage_used(e->storage);
    if (x86p_jit_storage_evict(e->storage, forget_evicted, e) > 0u) {
      e->stats.evictions++;
      if (x86p_jit_storage_used(e->storage) < before) {
        continue;
      }
    }
    if (!x86p_jit_engine_invalidate_all(e, reason, reason_len)) {
      return 0;
    }
  }
  return 1;
}

/* Translate the block at `eip`, publish it, and record it. Returns the exec
   address, or NULL with `st` saying why. When it returns non-NULL and
   `out_blk` is non-NULL, `*out_blk` describes the block that was translated. */
static void *translate_at(
    X86pJitEngine *e, uint32_t eip, X86pJitStatus *st, X86pJitBlock *out_blk, char *reason, unsigned reason_len) {
  X86pJitBlock blk;
  void *exec;

  if (!evict_for_room(e, reason, reason_len)) {
    *st = kX86pJitOutOfSpace;
    return NULL;
  }
  *st = x86p_jit_storage_translate(e->storage, e->mem, eip, e->boundary, e->boundary_user, &blk, reason, reason_len);
  if (*st != kX86pJitOk) {
    return NULL;
  }
  exec = blk.entry;

  if (!jc_block_insert(e->cache, eip, exec, blk.guest_len)) {
    /* The table is full. Flushing invalidates the block just written, so the
       translation is redone rather than entered -- entering it would be a jump
       into memory the rewind has released. */
    if (!x86p_jit_engine_invalidate_all(e, reason, reason_len)) {
      *st = kX86pJitOutOfSpace;
      return NULL;
    }
    *st = kX86pJitOk;
    return NULL;
  }

  e->stats.blocks_translated++;
  e->stats.guest_insns_translated += blk.insns;
  e->stats.conds_translated += blk.conds;
  e->stats.conds_inline += blk.cond_inline;
  e->stats.conds_unknown_kind += blk.cond_unknown_kind;
  e->stats.x87_loads_translated += blk.x87_loads;
  e->stats.x87_loads_inline += blk.x87_loads_inline;
  e->stats.x87_stores_translated += blk.x87_stores;
  e->stats.x87_stores_inline += blk.x87_stores_inline;
  e->stats.simd_translated += blk.simd_ops;
  e->stats.simd_inline += blk.simd_inline;
  e->stats.exits += blk.exits;
  e->stats.exits_static += blk.exits_static;
  e->stats.exits_backward += blk.exits_backward;
  e->stats.exits_self += blk.exits_self;
  if (e->chain) {
    x86p_jit_chain_census_note_block(
        e->chain, eip, blk.static_targets, blk.static_target_count, blk.static_targets_overflowed);
  }
  if (out_blk) {
    *out_blk = blk;
  }
  return exec;
}

X86pJitRunStatus x86p_jit_engine_run(
    X86pJitEngine *e, X86pCpu *cpu, void *run_user, uint64_t max_steps, char *reason, unsigned reason_len) {
  uint64_t steps = 0u;
  unsigned consecutive_translate_retries = 0u;
  uint32_t previous_entry = 0u;
  int have_previous = 0;

  if (!e || !cpu) {
    say(reason, reason_len, "null argument");
    return kX86pRunTranslateFailed;
  }

  while (steps < max_steps) {
    if (e->intercept && e->intercept(cpu, e->intercept_user, run_user)) {
      if (e->dispatch && e->dispatch(cpu, e->dispatch_user, run_user) == kX86pDispatchContinue) {
        /* Handled in place; the run stays on this stack. Counts as a step so a
           handler that does not advance eip still ends the slice. */
        steps++;
        continue;
      }
      return kX86pRunIntercept;
    }
    void *host = e->cache_disabled ? NULL : jc_block_lookup(e->cache, cpu->eip);
    X86pJitExit exit;
    uint32_t before_eip = cpu->eip;

    if (!host) {
      X86pJitStatus st = kX86pJitOk;
      /* Wide enough for the refusal WITH its denominators. At 192 the arena's
         "%u live of %u slot(s); %u published and %u released" was cut off
         mid-sentence, which removed the one number the reader needed. */
      char why[512];
      why[0] = '\0';
      host = translate_at(e, cpu->eip, &st, NULL, why, (unsigned)sizeof why);
      if (!host) {
        if (st == kX86pJitOutOfSpace) {
          /* The storage may have JUST learned that this host holds fewer live
             translations than it was created for -- it can only learn that by
             being refused one. The attempt that discovers the ceiling must not
             be the one that ends the run: it now has room by a smaller
             measure, so evict and try again. Exactly once, because a second
             refusal after eviction is a host that cannot hold one block. */
          /* Not a has_room() test: right after the refusal the storage is
             full BY ITS NEW MEASURE, which is exactly the state the retry
             exists to resolve. translate_at() evicts before it translates, so
             going round once is what makes room. */
          if (++consecutive_translate_retries <= 1u) {
            continue;
          }
          say(reason, reason_len, "%s", why);
          return kX86pRunOutOfCode;
        }
        if (st == kX86pJitOk) {
          /* A full block cache, already flushed. Retrying is correct exactly
             once: a second failure in a row means the region cannot hold even
             one block after a flush, which is a sizing fault rather than
             pressure and must be reported instead of spun on. */
          if (++consecutive_translate_retries > 1u) {
            say(reason, reason_len, "block cache and code region cannot hold a single block");
            return kX86pRunOutOfCode;
          }
          continue;
        }
        if (st == kX86pJitUnsupportedAtEntry) {
          e->stats.translate_refusals++;
          say_unsupported(e->mem, cpu->eip, reason, reason_len);
          return kX86pRunUnsupported;
        }
        if (st == kX86pJitFetchFault) {
          say(reason, reason_len, "%s", why);
          return kX86pRunFetchFault;
        }
        if (st == kX86pJitDecodeFailed) {
          say(reason, reason_len, "%s", why);
          return kX86pRunDecodeFailed;
        }
        say(reason, reason_len, "%s", why);
        return kX86pRunTranslateFailed;
      }
    }
    consecutive_translate_retries = 0u;

    /* BEFORE the block runs, which is the whole point: taken after, the
       report shows a register file the block has already rewritten and a
       stack it has already pushed its own frames onto. */
    if (e->watch_left != 0u && before_eip == e->watch_addr) {
      e->watch_left--;
      e->watch(e->watch_user,
               before_eip,
               e->stats.blocks_entered != 0u ? e->last_entry : 0u,
               e->stats.blocks_entered != 0u,
               cpu);
    }
    uint32_t (*fn)(X86pCpu *);
    *(void **)&fn = host;
    exit = (X86pJitExit)fn(cpu);
    /*
     * The block just entered was the one just left: a guest loop going round
     * again, having paid a full dispatch -- the intercept callback, the cache
     * lookup and an indirect call out of the module -- to do it. This is the
     * exact population a backend that lowered a self-exit as a WebAssembly
     * `loop` would remove, counted against blocks_entered rather than against
     * translations, because a loop's cost is in its iterations.
     *
     * It is a two-entry history and not a successor graph on purpose: a
     * successor graph answers a bigger question and cannot be added to the hot
     * path for free, and this is the discriminator for the cheapest fix.
     */
    if (e->stats.blocks_entered != 0u && before_eip == e->last_entry) {
      e->stats.blocks_reentered++;
    }
    e->last_entry = before_eip;
    e->stats.blocks_entered++;
    if (e->profile) {
      x86p_jit_profile_hit(e->profile, before_eip);
    }
    if (e->chain) {
      /* Note AFTER the reentry counter above has used last_entry, and with
         the previous entry rather than this one: the question is whether the
         block just left already knew this address. */
      x86p_jit_chain_census_note_entry(e->chain, previous_entry, before_eip, have_previous);
      previous_entry = before_eip;
      have_previous = 1;
    }
    steps++;

    if (e->cache_disabled) {
      /* Drop the translation just run so the next entry to this address is
         made from whatever the guest bytes say NOW, not what they said when
         this block was built. */
      if (!x86p_jit_engine_invalidate_all(e, reason, reason_len)) {
        return kX86pRunOutOfCode;
      }
    }

    if (exit == kX86pJitExitBlockEnd) {
      continue;
    }
    if (exit == kX86pJitExitMemoryFault) {
      say(reason, reason_len, "guest memory fault at %08X", cpu->eip);
      return kX86pRunMemoryFault;
    }
    if (exit == kX86pJitExitUnsupported) {
      e->stats.translate_refusals++;
      say_unsupported(e->mem, cpu->eip, reason, reason_len);
      return kX86pRunUnsupported;
    }
    if (exit == kX86pJitExitDivideError) {
      say(reason, reason_len, "guest divide error at %08X", cpu->eip);
      return kX86pRunDivideError;
    }
    if (exit == kX86pJitExitInterrupt) {
      say(reason, reason_len, "guest software interrupt at %08X", cpu->eip);
      return kX86pRunInterrupt;
    }
    if (exit == kX86pJitExitProtectionFault) {
      say(reason, reason_len, "guest general-protection fault at %08X", cpu->eip);
      return kX86pRunProtectionFault;
    }
    if (exit == kX86pJitExitBoundRange) {
      say(reason, reason_len, "guest bound range exceeded at %08X", cpu->eip);
      return kX86pRunBoundRange;
    }
    say(reason, reason_len, "translated block returned invalid exit %u", (unsigned)exit);
    return kX86pRunTranslateFailed;
  }

  return kX86pRunBudget;
}
