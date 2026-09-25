/*
 * The ordinary case of narrowing an x87 ext80 value to an IEEE binary32 or
 * binary64 operand, as integer bit manipulation.
 *
 * FST m32 and FST m64 are the direction that ROUNDS, which is why this is a
 * separate module from x87_ext80_widen.h rather than its mirror image: a load
 * can be a shift and a rebias for every value there is, and a store cannot.
 * The general conversion therefore stays exactly where it was -- this decides
 * only whether a value is the case it can answer, and refuses every other one
 * to it.
 *
 * WHY IT EXISTS. The store side is the larger half of the browser's x87
 * operand plumbing: x86p_x87_operand_bytes_from_reg at 4.28% of the guest
 * worker and x86p_jit_x87_store_at at 3.60%, against the load side's 2.52%
 * after it was emitted inline. Every FST crosses out of its translated module
 * to reach a softfloat conversion whose rounding decisions, for the values a
 * game stores, are eleven or forty dropped bits and a tie test.
 *
 * WHAT "ORDINARY" MEANS, and each exclusion is a case that needs a different
 * answer rather than a case that is merely rare:
 *   - round-to-nearest-even only. The other three modes are the guest's RC
 *     field, which #162 measured moving on 1.65% of operations, so they are
 *     real and they are not this.
 *   - a normal ext80 source: a stored exponent that is neither zero nor all
 *     ones, AND the explicit integer bit set. An unnormal is invalid and an
 *     infinity, a NaN and a subnormal each have their own encoding.
 *   - a result that is a normal in the target: no overflow to infinity and no
 *     subnormal, including after a rounding carry.
 *   - OR a zero of either sign, which is the target's zero of that sign in
 *     every rounding mode. It is not rare: FST of 0.0 was the larger part of
 *     the stores that still reached the helper, 24.6 million in 90 seconds of
 *     the Dead Zone.
 * Everything else returns 0 and reaches the same conversion it always did.
 *
 * THERE ARE TWO IMPLEMENTATIONS OF THIS and that is deliberate: the C one
 * below, and the WebAssembly the JIT emits in jit_wasm_x87_store.c. They are
 * held together by x86p_ext80_source, which publishes the target format's
 * three numbers to both, and by the differential tests, which run the emitted
 * form against the interpreter over the values that separate the arms.
 */
#ifndef X86PORT_X87_EXT80_NARROW_H
#define X86PORT_X87_EXT80_NARROW_H

#include "x87_ext80_widen.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Round `value` to a `width`-byte IEEE value, writing its bits to `out`.
 *
 * Returns 1 when this was the ordinary case and `out` holds the same bits the
 * general conversion would have produced, and 0 -- having written nothing --
 * when it was not. `width` is 4 or 8; any other width is refused like any
 * other case this cannot answer.
 *
 * It takes no control word because it implements one rounding mode: a caller
 * holding a control word tests the RC field itself, against x87.h's
 * X86P_X87_RC_MASK and X86P_X87_RC_NEAREST, and that test is one instruction
 * in both implementations.
 */
int x86p_ext80_narrow_nearest(X86pExt80 value, unsigned width, uint64_t *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_X87_EXT80_NARROW_H */
