/*
 * bit_ops.h -- the bit-test family (BT/BTS/BTR/BTC) and the double-precision
 * shifts (SHLD/SHRD).
 *
 * Both groups are here for the same reason: they are the instructions whose
 * flag behaviour does not fit the lazy triple in flags.h. A double shift's CF
 * is a bit of the ORIGINAL destination selected by the count, and a bit test
 * writes CF from a bit of a value it may not otherwise touch -- neither is
 * derivable from `a`, `b`, `r` and a width. So they compute an explicit EFLAGS
 * word, which is the same escape hatch ADC and SBB already use.
 *
 * WHAT IS NOT HERE. The bit-string ADDRESSING of `BT [mem], reg` -- where the
 * offset is signed, may run to any distance, and moves the effective address
 * by whole operands -- needs the effective address, so it belongs with the
 * other addressing in exec.c. This file is the value and flag arithmetic only,
 * which is the part worth testing against silicon in isolation.
 */
#ifndef X86PORT_BIT_OPS_H
#define X86PORT_BIT_OPS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum X86pBitOp {
  kX86pBitTest = 0, /* BT:  read the bit, change nothing */
  kX86pBitSet,      /* BTS: read it, then set it */
  kX86pBitReset,    /* BTR: ... then clear it */
  kX86pBitComp,     /* BTC: ... then complement it */
  kX86pBitOpCount   /* MUST stay last */
} X86pBitOp;

const char *x86p_bit_op_name(X86pBitOp op);

/*
 * Apply one bit operation to `value`, whose width is `w` bytes, at `bit`
 * (already reduced modulo the operand width by the caller -- for a memory
 * operand that reduction is inseparable from the address adjustment, so it
 * cannot honestly be done here).
 *
 * CF becomes the bit as it was BEFORE the operation. The SDM leaves OF, SF, AF
 * and PF undefined; this preserves them, which tests/test_bit_ops.c checks
 * against the real instruction rather than assuming.
 */
uint32_t x86p_bit_apply(X86pBitOp op, uint32_t value, unsigned bit, uint32_t *eflags);

/*
 * SHLD and SHRD: shift `dst` by `count`, feeding in the bits of `src` from the
 * other end. `w` is 2 or 4 bytes.
 *
 * Returns 0 when the masked count is zero, which is not a failure but the
 * instruction's own rule: it writes NOTHING, not even flags, and a caller that
 * stored a result anyway would corrupt the destination's upper bits at 16-bit
 * width. Same rule, and same reason, as a shift of zero in flags.h.
 *
 * A masked count greater than the operand width is undefined in the SDM. It is
 * NOT emulated as anything in particular here: the function reports it through
 * `*defined` so the caller can see it happened, because a value invented for
 * an undefined case is indistinguishable from a correct one at the call site.
 */
int x86p_double_shift(
    int left, uint32_t dst, uint32_t src, unsigned count, int w, uint32_t *result, uint32_t *eflags, int *defined);

#ifdef __cplusplus
}
#endif

#endif
