#ifndef X86PORT_JIT_X64_COND_H
#define X86PORT_JIT_X64_COND_H

#include "decode.h"
#include "jit_x64_internal.h"

#include <stdint.h>

/* A conditional branch out of the block: evaluates `cond` and leaves through
   the epilogue at `target` or `fallthrough`. */
void x86p_x64_emit_jcc(BlockCtx *c, uint8_t cond, uint32_t target, uint32_t fallthrough);

/* SETcc: the condition as 0 or 1 into the instruction's byte destination,
   without touching guest flags. */
void x86p_x64_emit_setcc(BlockCtx *c, const X86pInsn *insn, uint32_t insn_eip);

#endif /* X86PORT_JIT_X64_COND_H */
