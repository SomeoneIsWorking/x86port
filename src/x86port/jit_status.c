/*
 * jit_status.c -- the names of the backend contract's outcomes.
 *
 * These belong to the CONTRACT in jit_x64.h, not to any one backend, and they
 * used to be copied into each of them. Two identical copies were already
 * there; a third arrived with the WebAssembly backend, which is the point at
 * which "each backend spells its own" stops being duplication and becomes a
 * place for the spellings to disagree -- a diagnostic that says "unsupported
 * instruction" on one host and something else on another for the same outcome
 * is worse than no diagnostic, because the two reports cannot be compared.
 *
 * Compiled unconditionally, unlike the backends themselves: naming an outcome
 * needs no host.
 */
#include "jit_x64.h"

const char *x86p_jit_exit_name(X86pJitExit e) {
  switch (e) {
  case kX86pJitExitBlockEnd:
    return "block end";
  case kX86pJitExitUnsupported:
    return "unsupported instruction";
  case kX86pJitExitMemoryFault:
    return "guest memory fault";
  case kX86pJitExitDivideError:
    return "divide error";
  case kX86pJitExitInterrupt:
    return "software interrupt";
  case kX86pJitExitProtectionFault:
    return "general-protection fault";
  case kX86pJitExitBoundRange:
    return "bound range exceeded";
  case kX86pJitExitCount:
  default:
    return "?";
  }
}

const char *x86p_jit_status_name(X86pJitStatus s) {
  switch (s) {
  case kX86pJitOk:
    return "ok";
  case kX86pJitFetchFault:
    return "fetch fault";
  case kX86pJitDecodeFailed:
    return "decode failed";
  case kX86pJitUnsupportedAtEntry:
    return "unsupported at entry";
  case kX86pJitOutOfSpace:
    return "out of space";
  case kX86pJitStatusCount:
  default:
    return "?";
  }
}
