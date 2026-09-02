/*
 * x87_state.h -- FNSAVE and FRSTOR: the whole floating-point unit, to and from
 * 108 bytes of guest memory.
 *
 * A SEPARATE MODULE BECAUSE IT IS A SEPARATE JOB. x87_exec.c executes
 * instructions against the register file; this serialises the file into an
 * architectural memory layout. They share nothing but the struct, and the
 * layout is the kind of thing that is right or wrong in one place: field
 * offsets, the 2-bit-per-register tag word (which is NOT the byte-per-register
 * form the model keeps), and the fact that the ten-byte registers are stored in
 * PHYSICAL order rather than stack order.
 *
 * THE 32-BIT PROTECTED-MODE LAYOUT is the one every Win32 process gets, and it
 * is the only one implemented. A 16-bit or real-mode FNSAVE writes a different,
 * shorter structure; this refuses rather than writing the wrong one, because a
 * state image that is subtly misaligned restores as garbage registers and the
 * failure appears in arithmetic much later.
 */
#ifndef X86PORT_X87_STATE_H
#define X86PORT_X87_STATE_H

#include "cpu.h"
#include "x87.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The protected-mode 32-bit image: 7 dwords of environment, then 8 registers
   of 10 bytes each. */
#define X86P_X87_STATE_BYTES 108u

/*
 * Write the unit's state to `addr`, then reinitialise it -- FNSAVE does both,
 * and a version that only saved would leave a guest's next FLD landing on a
 * full stack.
 *
 * Returns 0 if any part of the image could not be written; the guest state is
 * NOT reinitialised in that case, so a faulting save leaves a machine that can
 * be retried rather than one that has silently lost its registers.
 */
int x86p_x87_save_state(X86pX87 *fpu, const X86pMem *mem, uint32_t addr);

/* Read it back. Returns 0 without modifying the unit if the image could not be
   read in full -- a half-restored FPU is worse than an unrestored one. */
int x86p_x87_restore_state(X86pX87 *fpu, const X86pMem *mem, uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif
