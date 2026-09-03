#include "jit_engine.h"

#include "block_cache.h"
#include "code_memory.h"
#include "exec.h"

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
   address, or NULL with `st` saying why. */
static void *translate_at(X86pJitEngine *e, uint32_t eip, X86pJitStatus *st, char *reason, unsigned reason_len) {
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

  *st = x86p_jit_translate(e->mem, eip, e->code.write + e->used, e->code.size - e->used, &blk, reason, reason_len);
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
      return kX86pRunIntercept;
    }
    void *host = jc_block_lookup(e->cache, cpu->eip);
    X86pJitExit exit;
    uint32_t before_eip = cpu->eip;

    if (!host) {
      X86pJitStatus st = kX86pJitOk;
      char why[192];
      why[0] = '\0';
      host = translate_at(e, cpu->eip, &st, why, (unsigned)sizeof why);
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

    uint32_t (*fn)(X86pCpu *);
    *(void **)&fn = host;
    exit = (X86pJitExit)fn(cpu);
    e->stats.blocks_entered++;
    steps++;

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
