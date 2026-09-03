/*
 * jit_x64.h -- x86-32 guest basic block to x86-64 host code.
 *
 * The top half of the first backend. emit_x64 knows how to write host
 * instructions; this file decides which ones a guest instruction becomes.
 *
 * WHAT IS INLINED AND WHAT IS CALLED, AND WHY THAT SPLIT.
 * Data movement is emitted inline: a guest register is a slot in X86pCpu, so
 * reading one is a load and writing one is a store. Arithmetic is NOT emitted
 * inline; it calls x86p_alu, the same function the interpreter calls.
 *
 * That is a deliberate choice and not a placeholder for "real" code generation.
 * This framework's flags are lazy -- (kind, a, b, r, w) plus a carry_in derived
 * from the PREVIOUS flag state -- and the interpreter is the declared authority
 * on what an instruction means (S043). An emitter that reimplemented the flag
 * derivation inline would be a SECOND authority on it, free to disagree, and
 * the disagreement would surface as a branch taken differently thousands of
 * instructions later. Calling the one implementation makes the two engines
 * identical by construction rather than by agreement, which is exactly what
 * makes the differential test below meaningful instead of circular.
 *
 * The win over interpreting is still real and is the whole point: decode,
 * operand resolution, and the dispatch switch happen ONCE per block at
 * translation time instead of once per instruction per execution. What remains
 * per instruction is a handful of moves and one call. Inlining the hot
 * arithmetic with native host flags is a later, measurable optimisation --
 * and it is safe to attempt precisely because the differential exists first.
 *
 * A REFUSAL IS NAMED AND IS NOT A FAILURE. Most of x86-32 is not translatable
 * yet. An instruction this build cannot emit ends the block cleanly, with the
 * guest EIP left pointing AT it, so the caller can hand it to the interpreter
 * and translate again from the next address. That is the normal mixed-engine
 * arrangement, not an error path -- but it is COUNTED and the instruction is
 * NAMED, because "the JIT ran the block" and "the JIT emitted a prologue,
 * refused the first instruction, and returned" must never look alike.
 */
#ifndef X86PORT_JIT_X64_H
#define X86PORT_JIT_X64_H

#include "cpu.h"
#include "decode.h"

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
   * instruction to the interpreter. Distinct from Unsupported because "this
   * build cannot translate it" and "the guest did something invalid" want
   * opposite responses.
   */
  kX86pJitExitMemoryFault,
  /*
   * #DE, raised inside a helper-executed instruction. EIP is left AT it, as
   * for a memory fault.
   *
   * A separate exit rather than folding into MemoryFault: the guest must
   * RECEIVE a divide error, and a caller that could not tell the two apart
   * would deliver a page fault for a division by zero.
   */
  kX86pJitExitDivideError,
  /* Not gaps: outcomes. See exec.h -- an INT3 in a game binary usually means
     execution reached alignment padding, and saying so is more useful than
     saying the translator gave up. */
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
#define X86P_JIT_WORST_CASE_INSN_BYTES 224u
#define X86P_JIT_EPILOGUE_BYTES 64u
#define X86P_JIT_MIN_BLOCK_BYTES (X86P_JIT_WORST_CASE_INSN_BYTES + X86P_JIT_EPILOGUE_BYTES)

typedef struct X86pJitBlock {
  void *entry;        /* host address to call; see x86p_jit_enter */
  uint32_t guest_eip; /* the guest address this block starts at */
  uint32_t guest_len; /* guest bytes covered -- what range invalidation needs */
  uint32_t insns;     /* guest instructions translated. ZERO IS NEVER OK. */
  size_t host_bytes;  /* host bytes written */
  /* Whether the block ends in a translated branch. A caller can tell from this
     that the block has a known successor and needs no interpreter step, and a
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
   * How many instructions in the block were executed by calling the
   * interpreter rather than emitted as host code.
   *
   * Published because it is the difference between a JIT and a threaded
   * interpreter, and guest state cannot see it: routing EVERY instruction
   * through the helper would be entirely correct and pass every differential.
   * A test that did not watch this number could not tell the two apart.
   */
  unsigned helper_calls;
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
 */
X86pJitStatus x86p_jit_translate_bounded(const X86pMem *mem,
                                         uint32_t eip,
                                         void *code,
                                         size_t code_cap,
                                         X86pJitBoundaryFn boundary,
                                         void *boundary_user,
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

/*
 * Would this instruction become HOST CODE, as opposed to a call into the
 * interpreter?
 *
 * The distinction x86p_jit_can_translate deliberately hides, exported because
 * two callers need it and neither should reimplement it. The corpus tool wants
 * the ranked list of families still worth an emitter -- which is now a
 * performance queue rather than a coverage one. The differential wants to
 * bound the carry-in helper calls in a block, and a helper-executed
 * instruction writes flags this build did not choose, so it cannot derive the
 * next one's carry-in; taking that count from the block itself would be the
 * block grading its own work.
 */
int x86p_jit_emits_natively(const X86pInsn *insn);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_JIT_X64_H */
