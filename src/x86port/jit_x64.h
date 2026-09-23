/*
 * jit_x64.h -- x86-32 guest basic block to x86-64 host code.
 *
 * The top half of the first backend. emit_x64 knows how to write host
 * instructions; this file decides which ones a guest instruction becomes.
 *
 * WHAT IS INLINED AND WHAT IS CALLED, AND WHY THAT SPLIT.
 * Data movement is emitted inline: a guest register is a slot in X86pCpu, so
 * reading one is a load and writing one is a store. Some arithmetic calls the
 * narrow x86p_alu semantic owner rather than duplicating its flag rules.
 *
 * That is a deliberate choice and not a placeholder for "real" code generation.
 * This framework's flags are lazy -- (kind, a, b, r, w) plus a carry_in derived
 * from the PREVIOUS flag state. An emitter that reimplemented the flag
 * derivation inline would be a SECOND authority on it, free to disagree, and
 * the disagreement would surface as a branch taken differently thousands of
 * instructions later. Calling the one implementation makes the two engines
 * identical by construction rather than by agreement.
 *
 * The win over interpreting is still real and is the whole point: decode,
 * operand resolution, and the dispatch switch happen ONCE per block at
 * translation time instead of once per instruction per execution. What remains
 * per instruction is a handful of moves and one call. Inlining the hot
 * arithmetic with native host flags is a later, measurable optimisation --
 * and it is safe to attempt precisely because the differential exists first.
 *
 * A REFUSAL IS NAMED. Most of x86-32 is not translatable
 * yet. An instruction this build cannot emit ends the block cleanly, with the
 * guest EIP left pointing AT it, and the product dispatcher returns an
 * unsupported status. This backend never dispatches another engine itself.
 * The refusal is COUNTED and the instruction is NAMED, because "the JIT ran
 * the block" and "the JIT emitted a prologue, refused the first instruction,
 * and returned" must never look alike.
 */
#ifndef X86PORT_JIT_X64_H
#define X86PORT_JIT_X64_H

#include "cpu.h"
#include "decode.h"
#include "jit_chain.h"
#include "jit_chain_census.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Why a translated block returned. Written to EAX by the emitted epilogue. */
typedef enum X86pJitExit {
  kX86pJitExitBlockEnd = 0, /* every translated instruction ran; eip is the next address */
  kX86pJitExitUnsupported,  /* stopped AT an instruction this build cannot emit */
  /*
   * A guest memory access was outside the mapping. EIP is left AT the faulting
   * instruction, not past it, so the caller can deliver the fault or hand the
   * instruction. Distinct from Unsupported because "this build cannot
   * translate it" and "the guest did something invalid" are different
   * outcomes.
   */
  kX86pJitExitMemoryFault,
  /*
   * #DE raised by emitted code or a narrow semantic helper. EIP is left AT it,
   * as for a memory fault.
   *
   * A separate exit rather than folding into MemoryFault: the guest must
   * RECEIVE a divide error, and a caller that could not tell the two apart
   * would deliver a page fault for a division by zero.
   */
  kX86pJitExitDivideError,
  /* Architectural outcomes produced by emitted instructions. */
  kX86pJitExitInterrupt,
  kX86pJitExitProtectionFault,
  kX86pJitExitBoundRange,
  kX86pJitExitCount /* MUST stay last */
} X86pJitExit;

const char *x86p_jit_exit_name(X86pJitExit e);

typedef enum X86pJitStatus {
  kX86pJitOk = 0,
  kX86pJitFetchFault,   /* the guest EIP is not in mapped memory */
  kX86pJitDecodeFailed, /* the bytes there are not an instruction */
  /*
   * The FIRST instruction is one this build cannot emit, so there is no block.
   * Distinct from Ok-with-an-early-exit on purpose: a caller that treated them
   * alike would cache an empty block, enter it, immediately exit, and make no
   * progress -- forever, at full speed, with every counter looking healthy.
   */
  kX86pJitUnsupportedAtEntry,
  kX86pJitOutOfSpace, /* the code buffer could not hold the block */
  kX86pJitStatusCount /* MUST stay last */
} X86pJitStatus;

const char *x86p_jit_status_name(X86pJitStatus s);

/*
 * The smallest code buffer that can hold ANY block.
 *
 * Exported because the dispatch loop has to decide when its arena is too full
 * to translate into, and a number it picked itself would be a second opinion
 * about this file's worst case -- one that stays plausible while quietly
 * flushing after every block, or worse, while asking for a translation that
 * cannot fit. Below this, x86p_jit_translate refuses with kX86pJitOutOfSpace;
 * at or above it, a block of at least one instruction always comes back.
 */
#define X86P_JIT_WORST_CASE_INSN_BYTES 352u
/* The block tail, bounded by its parts rather than by what a corpus happened
   to reach: the x87 mirror's last stores and pops (at most 34 bytes), the
   normal exit (a chained exit is about 90 bytes on Win64), the two fault stubs
   (about 25 each), the one out-of-line x86p_cond path a Jcc's inline condition
   can need (a Jcc ends its block, so there is never more than one; about 30
   bytes, and then its two exits are the instruction's own), and the x87
   mirror's loader and write-back routines (153 bytes with all four), and the
   routine that reads the TOP and occupancy cache back from memory (about 75),
   and the chain probe that exits missing their slot jump to (about 125, with
   12 more per exit to reach it; jit_chain.h). That is about 575.

   Both numbers are enforced, not trusted: x86p_jit_translate refuses a block
   in which one instruction, or the tail, emitted more, naming the count. The
   instruction bound had drifted to 224 while forms emitted up to 303 bytes,
   which nothing noticed because a budget is only exceeded near the end of a
   buffer; the tail bound was once passed by the game at 337 bytes while no
   test corpus reached 200. The largest instruction measured over the test
   corpora is 321 bytes (an x87 form with its guards, a slow path that writes
   two values back, and its helper sequence), which the 352 above holds with
   room for an addressing form the corpora do not reach. */
#define X86P_JIT_EPILOGUE_BYTES 640u
/* The frame the block opens before its first instruction: at most eight
   pushes, the stack adjustment and the CPU pointer move -- 19 bytes on Win64.
   Enforced like the two bounds above. Leaving it out of the minimum let a
   region with less than this to spare above the other two translate nothing
   and be refused as unsupported instead of flushed. */
#define X86P_JIT_PROLOGUE_BYTES 24u
#define X86P_JIT_MIN_BLOCK_BYTES (X86P_JIT_PROLOGUE_BYTES + X86P_JIT_WORST_CASE_INSN_BYTES + X86P_JIT_EPILOGUE_BYTES)

typedef struct X86pJitBlock {
  void *entry;        /* host address to call; see x86p_jit_enter */
  uint32_t guest_eip; /* the guest address this block starts at */
  uint32_t guest_len; /* guest bytes covered -- what range invalidation needs */
  uint32_t insns;     /* guest instructions translated. ZERO IS NEVER OK. */
  size_t host_bytes;  /* host bytes written */
  /* Whether the block ends in a translated branch. A caller can tell from this
     that the block has a known successor and needs no dispatcher refusal, and a
     TEST can tell that its branch path ran at all -- without it, a suite whose
     generator stopped producing branches would keep reporting success. */
  int ends_in_branch;
  const char *stopper; /* mnemonic that ended the block, or NULL if it ran out
                          of room or hit the instruction limit. NAMED so the
                          unsupported set is a ranked work list, not a count. */
  /*
   * How many times the block had to CALL x86p_flag_cf to recover the incoming
   * carry, rather than deriving it inline from a predecessor whose flag kind
   * was known at translation time.
   *
   * Published because the difference is invisible in guest state: emitting the
   * call everywhere is entirely CORRECT and merely slow, so a change that lost
   * the inline derivation would pass every comparison. One per block is the
   * expected figure -- the first flag write faces a predecessor from outside
   * the block -- and more than that means something inside the block gave up
   * information it had.
   */
  unsigned flag_helper_calls;
  /*
   * Jcc and SETcc conditions evaluated by CALLING x86p_cond, and those lowered
   * to the host's own condition codes instead. Published for the same reason
   * as flag_helper_calls: calling the authority is entirely correct and merely
   * slow, so losing the inline form is invisible in guest state and in every
   * differential comparison. Both are needed -- the call count alone cannot
   * distinguish "nothing was inlined" from "there were no conditions".
   *
   * `conds` is counted one level up, at the Jcc/SETcc site itself, so the two
   * path counters can be checked against it. An inline lowering that returned
   * "handled" without emitting anything would otherwise be indistinguishable
   * from one that was never reached.
   */
  unsigned conds;
  unsigned cond_helper_calls;
  unsigned cond_inline;
  /* Of cond_inline, those whose recorded kind was proven at translation (THE
     PROOF in jit_x64_cond.c) and so emitted no runtime guard. */
  unsigned cond_proven;
  /*
   * Of the conditions that were NOT lowered inline, those whose predecessor
   * was not recorded at all -- the block's first flag reader, or an
   * instruction whose flag kind is only known at run time, such as a shift
   * whose count may be zero.
   *
   * This is the census that turns "nothing inlined" into a work item. Without
   * it a zero inline count has two entirely different causes that call for
   * opposite fixes: predecessors arriving unrecorded, which is a lowering that
   * throws information away, and predecessors of a kind no derivation exists
   * for yet, which is a derivation to write. Ranking that work needs the split,
   * not the total.
   */
  unsigned cond_unknown_kind;

  /*
   * Memory-operand x87 loads, and those whose widening the block performs
   * itself rather than calling out of its module for. Only the WebAssembly
   * backend emits the inline form; jit_wasm_x87_load.h says why that backend
   * has one at all.
   *
   * Both, for the reason the condition census gives: a build that inlined
   * nothing and a corpus with no float loads in it are the same zero, and they
   * call for opposite work.
   */
  unsigned x87_loads;
  unsigned x87_loads_inline;

  /*
   * Memory-operand x87 stores lowered, and those the block narrows itself.
   * jit_wasm_x87_store.h says what its inline arm takes; both counts, for the
   * same reason the pair above gives.
   */
  unsigned x87_stores;
  unsigned x87_stores_inline;

  /*
   * SIMD instructions lowered, and those the block performs with the host's own
   * 128-bit SIMD rather than calling out of its module for. Only the
   * WebAssembly backend emits the inline form; jit_wasm_simd_inline.h says what
   * it will and will not take. Both counts, for the same reason as above.
   */
  unsigned simd_ops;
  unsigned simd_inline;

  /*
   * How many places the block can leave to, and how many of those the
   * translator already knows the address of. This is what ranks block chaining:
   * an exit to a constant is one a backend could branch to directly, where an
   * exit to a register or a memory word has to go back through the dispatcher.
   * `exits_backward`, `exits_loop` and `exits_self` are three nested subsets of
   * `exits_static`: a guest loop backedge, one whose head is inside this block,
   * and one naming the block's own first address. They rank three fixes of
   * increasing cost and decreasing reach, and which one a given guest loop
   * falls into is decided by where the block was started rather than by the
   * guest. X86pWasmExitCensus states each.
   *
   * A BLOCK HAS MORE THAN ONE EXIT. A conditional branch does not end a block
   * here; its taken path is an exit inside the body and the run continues past
   * it. Counting a block's terminator alone answers a different question, and
   * answers it in a way that hides every loop.
   *
   * FILLED BY THE WEBASSEMBLY BACKEND ONLY. The machine-code backends leave
   * these zero, so a consumer that reports them must refuse rather than print a
   * zero that reads like a census; see tools/jit_coverage.c.
   */
  unsigned exits;
  unsigned exits_static;
  /* The distinct constant successor addresses of `exits_static`, capped at
     X86P_JIT_CHAIN_TARGETS, with the rest counted in `static_targets_overflowed`.
     A chaining backend branches to exactly these; the runtime chain census
     compares them against where the run actually went. Filled by the
     WebAssembly backend only, like the counts above. */
  uint32_t static_targets[X86P_JIT_CHAIN_TARGETS];
  unsigned static_target_count;
  unsigned static_targets_overflowed;
  unsigned exits_backward;
  unsigned exits_loop;
  unsigned exits_self;
  /* x86p_jit_host_state() when this block was translated. Its code assumes
     that state and x86p_jit_enter refuses the block under any other. */
  uint32_t host_state;
  /* Exits given a chain slot, and exits that asked for one when every slot
     was claimed (jit_chain.h). */
  unsigned chain_exits;
  unsigned chain_exits_unslotted;
} X86pJitBlock;

/*
 * Translate the basic block starting at `eip` into `code`.
 *
 * `code` is caller-owned WRITE memory. This function never makes it executable
 * and never flushes an instruction cache: on a dual-mapped host the write and
 * exec addresses differ, and only the caller knows both. Publishing is the
 * caller's job precisely so this file cannot get it half right.
 *
 * `out->entry` is set to `code`, so a caller whose exec address differs must
 * overwrite it with the corresponding exec pointer before entering.
 *
 * `reason` receives an explanation on any non-Ok status.
 */
X86pJitStatus x86p_jit_translate(const X86pMem *mem,
                                 uint32_t eip,
                                 void *code,
                                 size_t code_cap,
                                 X86pJitBlock *out,
                                 char *reason,
                                 unsigned reason_len);

/*
 * A guest address the CONSUMER intercepts -- a host thunk, a native override
 * entry, a return sentinel. Returns non-zero when the block must not translate
 * PAST `eip`: the byte at `eip` is not the guest code that runs there.
 *
 * The dispatch loop only checks its interception predicate between blocks, so
 * an interception point in the MIDDLE of a straight-line run would otherwise be
 * translated over and the original guest bytes executed where the consumer
 * meant to take control. Branch targets are block leaders already; this is for
 * the address reached by fall-through.
 */
typedef int (*X86pJitBoundaryFn)(uint32_t eip, void *user);

/*
 * As x86p_jit_translate, but ends the block before any address (other than
 * `eip` itself) for which `boundary` returns non-zero. `boundary` may be NULL,
 * which is exactly x86p_jit_translate.
 *
 * With `chain`, the block's exits to a next guest EIP claim slots in it and
 * may transfer straight to another translation (jit_chain.h); such a block
 * reads the chain's run header and is entered only by x86p_jit_engine_run.
 * NULL, and every exit returns. A backend that does not chain ignores it.
 */
X86pJitStatus x86p_jit_translate_bounded(const X86pMem *mem,
                                         uint32_t eip,
                                         void *code,
                                         size_t code_cap,
                                         X86pJitBoundaryFn boundary,
                                         void *boundary_user,
                                         X86pJitChain *chain,
                                         X86pJitBlock *out,
                                         char *reason,
                                         unsigned reason_len);

/*
 * Run a translated block. Returns an X86pJitExit; cpu->eip is left where the
 * guest should continue.
 *
 * The block must have been published (made executable, instruction cache
 * flushed) by the caller first. Entering unpublished code is undefined on every
 * host and silently fine on x86-64, which is what makes it worth saying here.
 */
X86pJitExit x86p_jit_enter(const X86pJitBlock *b, X86pCpu *cpu);

/*
 * Where a chained exit enters a translation: the offset of the code after its
 * prologue, whose frame the jumping block already has. Zero when this backend
 * does not chain.
 */
size_t x86p_jit_chain_entry_offset(void);

/*
 * The host execution state a translation may assume, as one value.
 *
 * On the x86-64 backend it is the host x87 control word: an inline x87
 * operation is exact only when the guest's control word equals the host's,
 * and the translation compares the guest's against this value as a constant
 * instead of storing and reloading the host word at every operation. That is
 * sound for a whole run because both x86-64 calling conventions make the x87
 * control word callee-saved -- no host call a block makes can return with it
 * changed -- so the state can only differ BETWEEN runs, where
 * x86p_jit_engine_run asks for it again. Zero on backends that assume nothing.
 */
uint32_t x86p_jit_host_state(void);

/* Whether this build has a backend for the host it was compiled for. Asked
   rather than assumed: on a host with no backend, translate() refuses instead
   of emitting bytes that are not instructions here. */
int x86p_jit_available(void);

/*
 * Would this instruction be translated, if a block reached it?
 *
 * Exported so a corpus tool can count what remains WITHOUT running the
 * translator. The ranked list of block ENDERS undercounts: everything after
 * the first refusal in a function is never looked at, so an instruction that
 * only ever appears late looks free. This gives the honest denominator.
 */
int x86p_jit_can_translate(const X86pInsn *insn);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_JIT_X64_H */
