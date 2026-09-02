/*
 * privilege.h -- what a 32-bit user-mode process is allowed to execute, and
 * what happens when it executes something else.
 *
 * THIS IS SEMANTICS, NOT A REFUSAL. HLT, CLI, IN and OUT are not instructions
 * this framework has failed to get to: they are instructions whose defined
 * behaviour at ring 3 with IOPL 0 -- which is every Win32 process -- is to
 * raise a general-protection fault. A model that reported them as unsupported
 * would be saying "I do not know what this does" about an instruction whose
 * outcome is completely specified, and would hide the difference between an
 * engine gap and a guest doing something illegal.
 *
 * That distinction matters in practice, because most of these appear in game
 * binaries only as alignment padding decoded as code. An engine that stops on
 * "unsupported" invites someone to go and implement port I/O; one that stops on
 * "#GP at this address" says the truth, which is that execution should never
 * have arrived there.
 *
 * The flat, ring-3, IOPL-0 model is the same CONTRACT cpu.h states for the
 * segment registers, and it is stated once, here.
 */
#ifndef X86PORT_PRIVILEGE_H
#define X86PORT_PRIVILEGE_H

#include "decode.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Is this instruction one a ring-3, IOPL-0 process may not execute?
 *
 * Covers the privileged instructions (HLT, WBINVD, CLI/STI, IRETD's privileged
 * forms are NOT here -- see exec.c) and the I/O family (IN, OUT, INS, OUTS),
 * which is guarded by IOPL rather than by CPL and is therefore illegal for the
 * same reason at a different place in the manual.
 */
int x86p_insn_is_privileged(const X86pInsn *insn);

#ifdef __cplusplus
}
#endif

#endif
