/*
 * jit_chain.h -- block-to-block transfers that do not return to the dispatcher.
 *
 * Every translated block ends by storing the next guest EIP and returning to
 * x86p_jit_engine_run, which asks the block cache for that address and calls
 * the translation it finds. On the Dead Zone route that round trip -- the
 * return, the cache probe and an indirect call per block -- was about a sixth
 * of all cycles, most of it waiting on the cache's memory.
 *
 * A chained exit asks an inline cache instead. Each exit site owns one SLOT
 * naming the guest address it last left for and the translation that runs
 * there. When the next EIP is the slot's address, the exit jumps straight into
 * that translation, past its prologue; the frame is the same one. Otherwise it
 * returns as before, naming its slot, and the dispatcher fills the slot once it
 * has found the translation for that address.
 *
 * WHAT A LINK MAY SKIP. A dispatched entry can do three things a jump cannot:
 * ask the consumer's intercept, stop at the run's stop address, and end the
 * run when its step budget is spent. So a link is made only under the
 * intercept contract (x86p_jit_engine_set_run_stop), only to a block the cache
 * does not guard, and every chained exit checks the stop address and steps the
 * budget held in this module's run header. Nothing the dispatcher counts is
 * lost: the header records how many transfers were made and the last address
 * entered.
 *
 * WHAT RETIRES A LINK. A slot is plain data in this module's memory, not a
 * patched instruction, so unlinking never writes code: invalidating any guest
 * range unlinks every slot, and a flush of the code arena also returns every
 * slot to the allocator.
 */
#ifndef X86PORT_JIT_CHAIN_H
#define X86PORT_JIT_CHAIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* No guest address: a zero-extended 32-bit EIP never equals it, so an
   unlinked slot needs no second test. */
#define X86P_JIT_CHAIN_UNLINKED UINT64_MAX

/* One exit site's inline cache. */
typedef struct X86pJitChainSlot {
  uint64_t guest; /* the EIP this exit last left for, or X86P_JIT_CHAIN_UNLINKED */
  void *host;     /* that block's chain entry: its exec address past the prologue */
} X86pJitChainSlot;

/* What one call into translated code may do, and what it did. The dispatcher
   writes it before the call and reads it after. */
typedef struct X86pJitChainRun {
  /* Transfers this call may still make, plus one: a chained exit steps it and
     returns when it reaches zero. */
  uint64_t budget;
  /* The run's stop address, which only the dispatcher may enter. */
  uint32_t stop;
  /* The guest address of the last block entered, by the dispatcher or by a
     transfer. */
  uint32_t last;
  /* One more than the index of the slot whose exit returned unlinked, or 0. */
  uint32_t pending;
  uint32_t reserved;
} X86pJitChainRun;

typedef struct X86pJitChain X86pJitChain;

/* The block cache's front array (jitcommon block_cache.h). */
struct JcBlockFront;

/* NULL when the slot table cannot be allocated. */
X86pJitChain *x86p_jit_chain_create(size_t slots);
void x86p_jit_chain_destroy(X86pJitChain *chain);

X86pJitChainRun *x86p_jit_chain_run(X86pJitChain *chain);

/*
 * THE PROBE. A slot remembers one address, and an exit whose target varies --
 * a RET, an indirect JMP or CALL -- keeps missing it, returning to the
 * dispatcher, and having its slot relinked to the address it just left for.
 * On the Dead Zone route those round trips were the dispatcher's 4.5% of
 * samples. So an exit that misses its slot first asks the block cache's front
 * array, the dispatcher's own first question, and transfers when it holds the
 * address: under the same stop and budget rules as a linked slot, the block that
 * exited included. The front holds
 * only blocks the cache does not guard (jc_block_take_hit), exactly the ones a
 * link may reach, and every path that retires a translation clears its front
 * slot. The engine owns both structures, with the same lifetime.
 */
void x86p_jit_chain_set_front(X86pJitChain *chain, const struct JcBlockFront *front);
/* The front array to probe, or NULL when exits only use their slots. */
const struct JcBlockFront *x86p_jit_chain_front(const X86pJitChain *chain);

/* A fresh, unlinked slot for one exit site, or -1 when every slot is claimed.
   Claimed slots stay claimed until x86p_jit_chain_reset. */
int64_t x86p_jit_chain_claim(X86pJitChain *chain);
/* How many slots are claimed, and rewinding to that count: a translation that
   fails returns the slots it claimed. */
size_t x86p_jit_chain_claimed(const X86pJitChain *chain);
void x86p_jit_chain_rewind(X86pJitChain *chain, size_t claimed);
size_t x86p_jit_chain_capacity(const X86pJitChain *chain);

/* The run header's address, and a slot's field relative to it: what emitted
   code holds in a register and adds as a displacement. */
uintptr_t x86p_jit_chain_base(const X86pJitChain *chain);
int32_t x86p_jit_chain_slot_disp(const X86pJitChain *chain, int64_t slot);

/* Record that exit `slot` reaches `guest` through `host`. */
void x86p_jit_chain_link(X86pJitChain *chain, int64_t slot, uint32_t guest, void *host);
const X86pJitChainSlot *x86p_jit_chain_slot(const X86pJitChain *chain, int64_t slot);

/* Every slot unlinked, still claimed: guest code changed. */
void x86p_jit_chain_unlink_all(X86pJitChain *chain);
/* Every slot unlinked and returned: the code arena was flushed. */
void x86p_jit_chain_reset(X86pJitChain *chain);

#ifdef __cplusplus
}
#endif

#endif /* X86PORT_JIT_CHAIN_H */
