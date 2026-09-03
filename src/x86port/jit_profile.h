/*
 * jit_profile.h -- an execution-weighted histogram of guest basic-block
 * entries.
 *
 * x86p_jit_engine_stats says how MUCH was translated and how often the
 * interpreter fell back; it cannot say WHERE a running program spends its
 * time, because a block translated once and entered ten million times and a
 * block entered once weigh the same there. This is the missing number: one
 * saturating counter per distinct guest block address, bumped on every entry
 * when a profile is attached, read back sorted at the end.
 *
 * It is a fixed-capacity open-addressed table on purpose. The hot path is a
 * single masked index and a compare in the common (already-present) case, no
 * allocation; a table that fills past its load factor drops new keys and
 * counts the drops, so "the top is trustworthy, the tail past N distinct
 * blocks is not" is a reported fact rather than a silent lie. Off by default:
 * a consumer opts in (x86p_jit_engine_set_profile) and the bump is behind a
 * NULL check.
 */
#ifndef X86PORT_JIT_PROFILE_H
#define X86PORT_JIT_PROFILE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct X86pJitProfile X86pJitProfile;

typedef struct X86pJitProfileEntry {
  uint32_t guest_eip;
  uint64_t entries;
} X86pJitProfileEntry;

/*
 * `slot_hint` distinct block addresses are held without a drop; the table is
 * rounded up to a power of two at twice that so the load factor stays at 0.5.
 * NULL on allocation failure.
 */
X86pJitProfile *x86p_jit_profile_create(uint32_t slot_hint);
void x86p_jit_profile_destroy(X86pJitProfile *p);

/* One entry into the block at `guest_eip`. Saturates at UINT64_MAX rather than
   wrapping. A key that arrives when the table is full is dropped and counted;
   an existing key is always still counted. */
void x86p_jit_profile_hit(X86pJitProfile *p, uint32_t guest_eip);

/* Fill `out[0..min(n,distinct)-1]` with the hottest blocks, most entries
   first, and return how many were written. */
uint32_t x86p_jit_profile_top(const X86pJitProfile *p, X86pJitProfileEntry *out, uint32_t n);

uint64_t x86p_jit_profile_total_hits(const X86pJitProfile *p);
uint32_t x86p_jit_profile_distinct(const X86pJitProfile *p);
uint64_t x86p_jit_profile_dropped_keys(const X86pJitProfile *p);

#ifdef __cplusplus
}
#endif

#endif /* X86PORT_JIT_PROFILE_H */
