/* privilege.c -- see privilege.h. */
#include "privilege.h"

int x86p_insn_is_privileged(const X86pInsn *insn) {
  switch ((X86pInsnOp)insn->op) {
  case kX86pInsnHlt:
  case kX86pInsnWbinvd:
  case kX86pInsnCli:
  case kX86pInsnSti:
  case kX86pInsnPortIo:
    return 1;
  default:
    return 0;
  }
}
