#include "jit_engine.h"

#include "block_cache.h"
#include "code_memory.h"
#include "decode.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  int cache_disabled;      /* diagnostic: retranslate every block, never reuse one */
  X86pJitProfile *profile; /* diagnostic: block-entry histogram, or NULL */
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
  uint8_t encoded[X86P_MAX_INSN_LEN];
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
  if (length == 0u) {
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

const X86pJitProfile *x86p_jit_engine_profile(const X86pJitEngine *e) {
  return e ? e->profile : NULL;
}

/*
 * Drop every translation and rewind the code region.
 *
 * Both halves, together, or the engine is corrupt: a cache entry surviving a
 * rewind points at bytes the next translation is about to overwrite, and a
 * rewind without a flush leaks the region until nothing can be translated. The
 * two are one operation for that reason and there is no way to do half of it.
 */
static int reset_code(X86pJitEngine *e, char *reason, unsigned reason_len) {
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
  e->used = 0u;
  e->stats.cache_flushes++;
  return 1;
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
    if (!reset_code(e, reason, reason_len)) {
      *st = kX86pJitOutOfSpace;
      return NULL;
    }
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
    if (!reset_code(e, reason, reason_len)) {
      *st = kX86pJitOutOfSpace;
      return NULL;
    }
    *st = kX86pJitOk;
    return NULL;
  }

  e->stats.blocks_translated++;
  e->stats.guest_insns_translated += blk.insns;
  if (out_blk) {
    *out_blk = blk;
  }
  return exec;
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

    if (!host) {
      X86pJitStatus st = kX86pJitOk;
      char why[192];
      why[0] = '\0';
      host = translate_at(e, cpu->eip, &st, NULL, why, (unsigned)sizeof why);
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

    uint32_t (*fn)(X86pCpu *);
    *(void **)&fn = host;
    exit = (X86pJitExit)fn(cpu);
    e->stats.blocks_entered++;
    if (e->profile) {
      x86p_jit_profile_hit(e->profile, before_eip);
    }
    steps++;

    if (e->cache_disabled) {
      /* Drop the translation just run so the next entry to this address is
         made from whatever the guest bytes say NOW, not what they said when
         this block was built. */
      if (!reset_code(e, reason, reason_len)) {
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
