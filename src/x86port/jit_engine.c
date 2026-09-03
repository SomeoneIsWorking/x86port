#include "jit_engine.h"

#include "block_cache.h"
#include "code_memory.h"
#include "cpu_compare.h"
#include "decode.h"
#include "exec.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One shadow-interpreter store, kept so verify mode can put memory back before
   the JIT runs the block for real. 16 bytes covers the widest guest store (an
   XMM register). */
typedef struct {
  uint32_t addr;
  uint32_t len;
  uint8_t old[16]; /* bytes before the shadow wrote -- used to restore memory */
  uint8_t neu[16]; /* bytes the shadow left there -- what the JIT must match */
} X86pVerifyWrite;

#define X86P_VERIFY_LOG_CAP 8192u

/*
 * Just enough of a translated block to shadow-run and compare it again on a
 * cache HIT -- guest_eip, guest_len, insns. Kept in verify mode only, so a
 * block whose emitter is wrong for register values it did not see on its first
 * translation is still caught on the entry where those values occur. Open
 * addressing on guest_eip; eip 0 is the empty slot (this port never translates
 * a block at guest address 0). Sized to the block cache, so it cannot fill
 * before the cache does.
 */
typedef struct {
  uint32_t guest_eip;
  uint32_t guest_len;
  uint32_t insns;
} X86pVerifyMeta;

struct X86pJitEngine {
  const X86pMem *mem;
  JcCodeRegion code;
  size_t used;
  JcBlockCache *cache;
  X86pJitEngineStats stats;
  X86pJitInterceptFn intercept;
  void *intercept_user;
  X86pJitDispatchFn dispatch;
  void *dispatch_user;
  X86pJitBoundaryFn boundary;
  void *boundary_user;
  int cache_disabled; /* diagnostic: retranslate every block, never reuse one */
  int verify;         /* diagnostic: shadow-interpret every block and compare */
  X86pVerifyWrite *verify_log;
  uint32_t verify_log_n;
  int verify_log_overflow;
  X86pVerifyMeta *verify_meta;
  uint32_t verify_meta_cap; /* power of two */
  X86pJitProfile *profile;  /* diagnostic: block-entry histogram, or NULL */
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
  case kX86pRunVerifyDivergence:
    return "verify: JIT block disagreed with the interpreter";
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

const char *x86p_jit_engine_mechanism(void) {
  return jc_code_mechanism();
}

X86pJitEngine *
x86p_jit_engine_create(const X86pMem *mem, size_t code_bytes, size_t cache_blocks, char *reason, unsigned reason_len) {
  X86pJitEngine *e;
  char why[256];

  if (!mem) {
    say(reason, reason_len, "no guest memory");
    return NULL;
  }
  if (code_bytes < X86P_JIT_MIN_BLOCK_BYTES) {
    /* Refused rather than raised: a region too small for one block would fail
       on its first translation, and reporting that as "out of code" at run time
       blames the program for the caller's sizing. */
    say(reason,
        reason_len,
        "code region of %zu bytes is below the %u a single block may need",
        code_bytes,
        (unsigned)X86P_JIT_MIN_BLOCK_BYTES);
    return NULL;
  }

  e = (X86pJitEngine *)calloc(1u, sizeof *e);
  if (!e) {
    say(reason, reason_len, "out of memory");
    return NULL;
  }
  e->mem = mem;

  why[0] = '\0';
  if (jc_code_region_create(code_bytes, &e->code, why, (unsigned)sizeof why) != kJcCodeOk) {
    say(reason, reason_len, "code memory (%s): %s", jc_code_mechanism(), why);
    free(e);
    return NULL;
  }

  e->cache = jc_block_cache_create(cache_blocks);
  if (!e->cache) {
    say(reason, reason_len, "block cache of %zu entries could not be created", cache_blocks);
    jc_code_region_destroy(&e->code);
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
  jc_code_region_destroy(&e->code);
  free(e->verify_log);
  free(e->verify_meta);
  x86p_jit_profile_destroy(e->profile);
  free(e);
}

void x86p_jit_engine_invalidate(X86pJitEngine *e, uint32_t lo, uint32_t hi) {
  if (e) {
    (void)jc_block_invalidate_range(e->cache, lo, hi);
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
  out->code_bytes_used = e->used;
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

void x86p_jit_engine_set_verify(X86pJitEngine *e, int enabled) {
  if (!e) {
    return;
  }
  if (enabled && !e->verify_log) {
    e->verify_meta_cap = 1u << 18; /* 4x a full block cache; never fills first */
    e->verify_log = (X86pVerifyWrite *)calloc(X86P_VERIFY_LOG_CAP, sizeof *e->verify_log);
    e->verify_meta = (X86pVerifyMeta *)calloc(e->verify_meta_cap, sizeof *e->verify_meta);
    if (!e->verify_log || !e->verify_meta) {
      fprintf(stderr, "x86p_jit_engine: verify mode needs memory it could not get\n");
      abort();
    }
  }
  e->verify = (enabled && e->verify_log) ? 1 : 0;
}

void x86p_jit_engine_set_profile(X86pJitEngine *e, int enabled, uint32_t slot_hint) {
  if (!e) {
    return;
  }
  if (!enabled) {
    x86p_jit_profile_destroy(e->profile);
    e->profile = NULL;
    return;
  }
  if (!e->profile) {
    e->profile = x86p_jit_profile_create(slot_hint);
    if (!e->profile) {
      fprintf(stderr, "x86p_jit_engine: block profile needs memory it could not get\n");
      abort();
    }
  }
}

const X86pJitProfile *x86p_jit_engine_profile(const X86pJitEngine *e) {
  return e ? e->profile : NULL;
}

/* ── verify: shadow-interpret a block and undo its memory writes ──────────── */

/* Installed only for the duration of a shadow run. `user` is the engine. */
static void verify_observe_write(uint32_t addr, uint32_t len, void *user) {
  X86pJitEngine *e = (X86pJitEngine *)user;
  X86pVerifyWrite *w;
  if (len > sizeof w->old || e->verify_log_n >= X86P_VERIFY_LOG_CAP) {
    e->verify_log_overflow = 1;
    return;
  }
  w = &e->verify_log[e->verify_log_n++];
  w->addr = addr;
  w->len = len;
  /* The bytes about to be overwritten. The write is already known mapped. */
  (void)x86p_mem_read_bytes(e->mem, addr, w->old, len);
}

/* Snapshot what the shadow left at each written region (its post-block memory
   image for that range), then put the original bytes back. Two passes because a
   later write may overlap an earlier one: the snapshot must see the final
   state. */
static void verify_snapshot_and_undo(X86pJitEngine *e) {
  uint32_t i;
  for (i = 0u; i < e->verify_log_n; i++) {
    (void)x86p_mem_read_bytes(e->mem, e->verify_log[i].addr, e->verify_log[i].neu, e->verify_log[i].len);
  }
  i = e->verify_log_n;
  while (i-- > 0u) {
    (void)x86p_mem_write_bytes(e->mem, e->verify_log[i].addr, e->verify_log[i].old, e->verify_log[i].len);
  }
}

/* After the JIT block has run: does memory match what the shadow produced at
   every region the shadow touched? Catches a store the JIT emits to the wrong
   address, with the wrong value, or at the wrong width even when the registers
   came out right. Returns 1 and fills `first` on the first mismatch. */
static int verify_memory_diverged(X86pJitEngine *e, char *first, unsigned first_len) {
  uint32_t i;
  for (i = 0u; i < e->verify_log_n; i++) {
    uint8_t now[16];
    const X86pVerifyWrite *w = &e->verify_log[i];
    if (!x86p_mem_read_bytes(e->mem, w->addr, now, w->len) || memcmp(now, w->neu, w->len) != 0) {
      char it[48];
      char jt[48];
      unsigned k;
      unsigned p = 0u;
      for (k = 0u; k < w->len && p + 2u < sizeof it; k++) {
        p += (unsigned)snprintf(it + p, sizeof it - p, "%02X", w->neu[k]);
      }
      p = 0u;
      for (k = 0u; k < w->len && p + 2u < sizeof jt; k++) {
        p += (unsigned)snprintf(jt + p, sizeof jt - p, "%02X", now[k]);
      }
      (void)snprintf(first, first_len, "memory[%08X..+%u] interp=%s jit=%s", w->addr, w->len, it, jt);
      return 1;
    }
  }
  return 0;
}

/* A block the shadow run cannot undo soundly: a REP string op can write more
   than the log holds, and re-running the JIT over half-undone memory would be
   worse than not checking. Decoded from the guest bytes rather than guessed. */
static int block_has_rep_string(const X86pMem *mem, uint32_t eip, uint32_t guest_len) {
  uint8_t buf[16 * 64]; /* MAX_INSNS * X86P_MAX_INSN_LEN, the widest a block can be */
  uint32_t span = x86p_mem_readable_span(mem, eip, (uint32_t)sizeof buf);
  uint32_t lim = (guest_len < span) ? guest_len : span;
  uint32_t p = 0u;
  if (!x86p_mem_read_bytes(mem, eip, buf, lim)) {
    return 1; /* cannot read the block to classify it -- skip the check */
  }
  while (p < lim) {
    X86pInsn insn;
    uint32_t len = x86p_decode(buf + p, lim - p, &insn);
    if (len == 0u) {
      return 1; /* undecodable here -- let the shadow decide it, skip verify */
    }
    if (insn.op == kX86pInsnString && insn.rep != kX86pRepNone) {
      return 1;
    }
    p += len;
  }
  return 0;
}

/*
 * Shadow-run the interpreter over the just-translated block from `c0`.
 *
 * Returns 1 when `*out` holds a trustworthy post-block machine to compare the
 * JIT against and memory has been restored to its pre-block bytes; 0 when the
 * block was skipped (memory is still restored, nothing to compare).
 */
static int verify_shadow_run(X86pJitEngine *e, const X86pCpu *c0, const X86pJitBlock *blk, X86pCpu *out) {
  uint32_t k;
  int ok = 1;
  if (block_has_rep_string(e->mem, blk->guest_eip, blk->guest_len)) {
    return 0;
  }
  *out = *c0;
  e->verify_log_n = 0u;
  e->verify_log_overflow = 0;
  x86p_mem_set_write_observer(verify_observe_write, e);
  for (k = 0; k < blk->insns; k++) {
    if (x86p_step(out, e->mem, NULL) != kX86pStepOk) {
      ok = 0;
      break;
    }
  }
  x86p_mem_set_write_observer(NULL, NULL);
  verify_snapshot_and_undo(e);
  return (ok && !e->verify_log_overflow) ? 1 : 0;
}

/* Fowler-Noll-Vo over the four address bytes, masked to the table. Cheap and
   spreads the low-entropy code addresses of one module well enough. */
static uint32_t verify_meta_slot(const X86pJitEngine *e, uint32_t eip) {
  uint32_t h = 2166136261u;
  int i;
  for (i = 0; i < 4; i++) {
    h ^= (eip >> (i * 8)) & 0xFFu;
    h *= 16777619u;
  }
  return h & (e->verify_meta_cap - 1u);
}

static void verify_meta_put(X86pJitEngine *e, const X86pJitBlock *blk) {
  uint32_t s = verify_meta_slot(e, blk->guest_eip);
  uint32_t tries = 0u;
  while (e->verify_meta[s].guest_eip != 0u && e->verify_meta[s].guest_eip != blk->guest_eip) {
    s = (s + 1u) & (e->verify_meta_cap - 1u);
    if (++tries >= e->verify_meta_cap) {
      return; /* full -- a cache hit for a missing entry simply is not re-verified */
    }
  }
  e->verify_meta[s].guest_eip = blk->guest_eip;
  e->verify_meta[s].guest_len = blk->guest_len;
  e->verify_meta[s].insns = blk->insns;
}

/* Fill `out` (guest_eip/guest_len/insns only) from the table; 1 if found. */
static int verify_meta_get(const X86pJitEngine *e, uint32_t eip, X86pJitBlock *out) {
  uint32_t s = verify_meta_slot(e, eip);
  uint32_t tries = 0u;
  while (e->verify_meta[s].guest_eip != eip) {
    if (e->verify_meta[s].guest_eip == 0u || ++tries >= e->verify_meta_cap) {
      return 0;
    }
    s = (s + 1u) & (e->verify_meta_cap - 1u);
  }
  out->guest_eip = e->verify_meta[s].guest_eip;
  out->guest_len = e->verify_meta[s].guest_len;
  out->insns = e->verify_meta[s].insns;
  return 1;
}

static void verify_capture_first(const char *field, const char *a_text, const char *b_text, void *user) {
  char *buf = (char *)user;
  if (buf[0] == '\0') {
    (void)snprintf(buf, 160u, "%s interp=%s jit=%s", field, a_text, b_text);
  }
}

/*
 * Drop every translation and rewind the code region.
 *
 * Both halves, together, or the engine is corrupt: a cache entry surviving a
 * rewind points at bytes the next translation is about to overwrite, and a
 * rewind without a flush leaks the region until nothing can be translated. The
 * two are one operation for that reason and there is no way to do half of it.
 */
static void reset_code(X86pJitEngine *e) {
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
    fprintf(stderr,
            "x86p_jit_engine: %zu block(s) survived a cache flush; the arena is about to be\n"
            "reused underneath them and entering one would execute an unrelated block\n",
            jc_block_count(e->cache));
    abort();
  }
  e->used = 0u;
  e->stats.cache_flushes++;
}

/* Translate the block at `eip`, publish it, and record it. Returns the exec
   address, or NULL with `st` saying why. When it returns non-NULL and
   `out_blk` is non-NULL, `*out_blk` describes the block that was translated. */
static void *translate_at(
    X86pJitEngine *e, uint32_t eip, X86pJitStatus *st, X86pJitBlock *out_blk, char *reason, unsigned reason_len) {
  X86pJitBlock blk;
  void *exec;

  /*
   * Translate into whatever remains rather than reserving a worst case per
   * block. Reserving one meant a region below the worst case held exactly ONE
   * block and flushed after every translation -- correct output, and a cache
   * that never got a chance to work. The translator shortens a block to fit and
   * refuses only below X86P_JIT_MIN_BLOCK_BYTES, so that refusal is the signal
   * to flush.
   */
  if (e->code.size - e->used < X86P_JIT_MIN_BLOCK_BYTES) {
    reset_code(e);
    if (e->code.size - e->used < X86P_JIT_MIN_BLOCK_BYTES) {
      *st = kX86pJitOutOfSpace;
      say(reason, reason_len, "code region of %zu bytes cannot hold one block", e->code.size);
      return NULL;
    }
  }

  if (jc_code_begin_write(&e->code) != kJcCodeOk) {
    *st = kX86pJitOutOfSpace;
    say(reason, reason_len, "code memory (%s) refused a write window", jc_code_mechanism());
    return NULL;
  }

  *st = x86p_jit_translate_bounded(e->mem,
                                   eip,
                                   e->code.write + e->used,
                                   e->code.size - e->used,
                                   e->boundary,
                                   e->boundary_user,
                                   &blk,
                                   reason,
                                   reason_len);
  if (*st != kX86pJitOk) {
    /* Published anyway: the region must not be left writable, whether or not
       anything was written into it. */
    (void)jc_code_publish(&e->code, e->used);
    return NULL;
  }

  if (jc_code_publish(&e->code, e->used + blk.host_bytes) != kJcCodeOk) {
    *st = kX86pJitOutOfSpace;
    say(reason, reason_len, "code memory (%s) refused to publish %zu bytes", jc_code_mechanism(), blk.host_bytes);
    return NULL;
  }

  /* The EXEC address, never the write address: under dual mapping they differ,
     and a cache full of write addresses runs correctly on Linux and crashes on
     Android. */
  exec = e->code.exec + e->used;
  e->used += blk.host_bytes;

  if (!jc_block_insert(e->cache, eip, exec, blk.guest_len)) {
    /* The table is full. Flushing invalidates the block just written, so the
       translation is redone rather than entered -- entering it would be a jump
       into memory the rewind has released. */
    reset_code(e);
    *st = kX86pJitOk;
    return NULL;
  }

  e->stats.blocks_translated++;
  e->stats.guest_insns_translated += blk.insns;
  e->stats.guest_insns_via_helper += blk.helper_calls;
  if (e->verify) {
    verify_meta_put(e, &blk);
  }
  if (out_blk) {
    *out_blk = blk;
  }
  return exec;
}

static X86pJitRunStatus from_step(X86pStepStatus s) {
  switch (s) {
  case kX86pStepOk:
    return kX86pRunBudget;
  case kX86pStepDecodeFailed:
    return kX86pRunDecodeFailed;
  case kX86pStepUnsupported:
    return kX86pRunUnsupported;
  case kX86pStepFetchFault:
    return kX86pRunFetchFault;
  case kX86pStepMemoryFault:
    return kX86pRunMemoryFault;
  case kX86pStepDivideError:
    return kX86pRunDivideError;
  case kX86pStepInterrupt:
    return kX86pRunInterrupt;
  case kX86pStepProtectionFault:
    return kX86pRunProtectionFault;
  case kX86pStepBoundRange:
    return kX86pRunBoundRange;
  case kX86pStepStatusCount:
    break;
  }
  return kX86pRunUnsupported;
}

X86pJitRunStatus
x86p_jit_engine_run(X86pJitEngine *e, X86pCpu *cpu, uint64_t max_steps, char *reason, unsigned reason_len) {
  uint64_t steps = 0u;
  unsigned consecutive_translate_retries = 0u;

  if (!e || !cpu) {
    say(reason, reason_len, "null argument");
    return kX86pRunTranslateFailed;
  }

  while (steps < max_steps) {
    if (e->intercept && e->intercept(cpu, e->intercept_user)) {
      if (e->dispatch && e->dispatch(cpu, e->dispatch_user) == kX86pDispatchContinue) {
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
    X86pJitBlock jit_blk;
    int have_blk = 0;
    X86pCpu verify_c0;
    if (e->verify) {
      verify_c0 = *cpu;
    }

    if (!host) {
      X86pJitStatus st = kX86pJitOk;
      char why[192];
      why[0] = '\0';
      host = translate_at(e, cpu->eip, &st, &jit_blk, why, (unsigned)sizeof why);
      have_blk = host != NULL;
      if (!host) {
        if (st == kX86pJitOutOfSpace) {
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
          /* The very first instruction has no emitter. The interpreter owns it;
             this is the fallback, and it is why coverage is a performance
             property rather than a correctness one. */
          X86pStepStatus ss = x86p_step(cpu, e->mem, NULL);
          e->stats.translate_refusals++;
          e->stats.fallback_steps++;
          steps++;
          if (ss != kX86pStepOk) {
            say(reason, reason_len, "interpreter: %s at %08X", x86p_step_status_name(ss), before_eip);
            return from_step(ss);
          }
          consecutive_translate_retries = 0u;
          continue;
        }
        say(reason, reason_len, "%s", why);
        return kX86pRunTranslateFailed;
      }
    }
    consecutive_translate_retries = 0u;

    /* On a cache HIT the block metadata is not in hand -- recover it from the
       verify-only side table so this entry is re-checked too. A wrong emitter
       can be right for the register values a block saw when it was first
       translated and wrong for the ones a later entry brings. */
    if (host && !have_blk && e->verify) {
      have_blk = verify_meta_get(e, before_eip, &jit_blk);
    }

    /* Verify: shadow-interpret the block from the pre-block state, with its
       memory writes recorded and undone, so the JIT below still runs over the
       original bytes. */
    X86pCpu verify_shadow;
    int verify_compare = 0;
    if (e->verify && have_blk) {
      verify_compare = verify_shadow_run(e, &verify_c0, &jit_blk, &verify_shadow);
      if (!verify_compare) {
        e->stats.verify_blocks_skipped++;
      }
    }

    uint32_t (*fn)(X86pCpu *);
    *(void **)&fn = host;
    exit = (X86pJitExit)fn(cpu);
    e->stats.blocks_entered++;
    if (e->profile) {
      x86p_jit_profile_hit(e->profile, before_eip);
    }
    steps++;

    if (verify_compare && exit != kX86pJitExitBlockEnd) {
      /* The JIT stopped early -- an unmodelled instruction, a trap. The shadow
         ran a fixed instruction count and the two are no longer at the same
         point; comparing them here would cry divergence on a boundary
         mismatch. */
      verify_compare = 0;
      e->stats.verify_blocks_skipped++;
    }
    if (verify_compare) {
      char first[192];
      first[0] = '\0';
      if (x86p_cpu_diff(&verify_shadow, cpu, verify_capture_first, first) != 0u ||
          verify_memory_diverged(e, first, (unsigned)sizeof first)) {
        say(reason, reason_len, "verify: JIT block at %08X (%u insn) disagreed: %s", before_eip, jit_blk.insns, first);
        e->verify_log_n = 0u;
        return kX86pRunVerifyDivergence;
      }
      e->stats.verify_blocks_checked++;
    }
    e->verify_log_n = 0u;

    if (e->cache_disabled) {
      /* Drop the translation just run so the next entry to this address is
         made from whatever the guest bytes say NOW, not what they said when
         this block was built. */
      reset_code(e);
    }

    if (exit == kX86pJitExitMemoryFault) {
      say(reason, reason_len, "guest memory fault at %08X", cpu->eip);
      return kX86pRunMemoryFault;
    }

    if (exit == kX86pJitExitUnsupported) {
      X86pStepStatus ss = x86p_step(cpu, e->mem, NULL);
      e->stats.fallback_after_block++;
      e->stats.fallback_steps++;
      steps++;
      if (ss != kX86pStepOk) {
        say(reason, reason_len, "interpreter: %s at %08X", x86p_step_status_name(ss), cpu->eip);
        return from_step(ss);
      }
      continue;
    }
  }

  return kX86pRunBudget;
}
