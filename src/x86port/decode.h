/*
 * decode.h -- one instruction, from guest bytes to something the engine can act
 * on.
 *
 * THE DECISION THIS SETTLES (jit-common I004). `shared/recomp-x86` has no
 * decoder: its front end is Ghidra and it lifts disassembled mnemonic TEXT,
 * which is why its failures read "mnemonic PFMUL" rather than an opcode byte.
 * A runtime engine cannot work that way -- Ghidra is a maintainer-only tool and
 * can never be a player prerequisite -- so x86port needs its own decode.
 *
 * It BORROWS rather than writes it, and the boundary is deliberate. Decode is
 * mechanical, exhaustively specified, and has no opinion about memory models,
 * threading or code caches, so embedding a proven implementation costs nothing
 * architecturally -- unlike embedding a whole CPU core, which brings all three.
 * Semantics stay ours, because S043's whole point is that we are the authority
 * on what correct means. Zydis (MIT, allocation-free, pre-generated tables, no
 * build-time code generation) is pinned at v4.1.1 in vendor/zydis.
 *
 * AND THE CHOICE IS MEASURED, NOT ASSERTED. pc/xmen2's Ghidra corpus carries
 * the raw bytes of every instruction beside Ghidra's own reading of them --
 * 2,168,629 real instructions across 20 modules. `x86p_decode_diff` decodes all
 * of them through THIS function and reports agreement with a denominator. See
 * tools/decode_diff.c.
 */
#ifndef X86PORT_DECODE_H
#define X86PORT_DECODE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The longest an x86 instruction may be. A decoder handed more than this has
   been handed data, not code. */
#define X86P_MAX_INSN_LEN 15

typedef struct X86pInsn {
  uint32_t length;      /* encoded bytes consumed; 0 only when decode failed */
  const char *mnemonic; /* upper case, static storage; never null on success */
  int operands;         /* explicit operand count, for diagnostics */
} X86pInsn;

/*
 * Decode one 32-bit-mode instruction from `bytes` (at most `len` readable).
 * Returns the instruction length, or 0 if these bytes are not a decodable
 * instruction -- and 0 is a REPORTABLE fact, not a signal to skip a byte and
 * try again. A decoder that resynchronises silently turns "this is data" into
 * "this is some other instruction", which is the failure mode static analysis
 * already has and the one a runtime engine exists to avoid.
 *
 * *out is untouched on failure.
 */
uint32_t x86p_decode(const uint8_t *bytes, size_t len, X86pInsn *out);

/* The decoder implementation and its pinned version, for run reports and for
   the "which decoder produced this evidence" question a divergence raises. */
const char *x86p_decoder_id(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_DECODE_H */
