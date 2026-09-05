#include "string_ops.h"

#include "alu.h"
#include "flags.h"

#include <stddef.h>

static const char *kStatusNames[] = {"ok", "memory fault", "undefined repeat prefix"};
_Static_assert((int)(sizeof kStatusNames / sizeof kStatusNames[0]) == (int)kX86pStringStatusCount,
               "every X86pStringStatus needs a name");

static const char *kOpNames[] = {"movs", "stos", "lods", "scas", "cmps"};
_Static_assert((int)(sizeof kOpNames / sizeof kOpNames[0]) == (int)kX86pStringOpCount,
               "every X86pStringOp needs a name");

const char *x86p_string_status_name(X86pStringStatus s) {
  if (s < 0 || s >= kX86pStringStatusCount) {
    return "?";
  }
  return kStatusNames[s];
}

const char *x86p_string_op_name(X86pStringOp op) {
  if (op < 0 || op >= kX86pStringOpCount) {
    return "?";
  }
  return kOpNames[op];
}

/*
 * Does this operation define the prefix it was given?
 *
 * F3 on MOVS/STOS/LODS repeats a fixed number of times; F2 on them is not
 * architecturally defined, and CPUs differ. Refused by name rather than
 * treated as F3: a guest that emits it is doing something this framework
 * cannot claim to model, and quietly picking one of the two behaviours is how
 * a divergence gets buried.
 */
static int rep_is_defined(X86pStringOp op, X86pRepKind rep) {
  if (rep == kX86pRepNone) {
    return 1;
  }
  if (op == kX86pStringScas || op == kX86pStringCmps) {
    return 1; /* F3 is REPE, F2 is REPNE; both defined */
  }
  return rep == kX86pRepRep;
}

int x86p_string_is_supported(X86pStringOp op, X86pRepKind rep, int width) {
  return op >= 0 && op < kX86pStringOpCount && rep >= 0 && rep < kX86pRepKindCount &&
         (width == 1 || width == 2 || width == 4) && rep_is_defined(op, rep);
}

/* One iteration. Returns 0 and sets *fault on a bad access. */
static int step_once(X86pCpu *cpu, const X86pMem *mem, X86pStringOp op, int w, int32_t delta, uint32_t *fault) {
  uint32_t esi = cpu->reg[kX86pEsi];
  uint32_t edi = cpu->reg[kX86pEdi];
  uint32_t a = 0u;
  uint32_t b = 0u;

  switch (op) {
  case kX86pStringMovs:
    if (!x86p_mem_read(mem, esi, w, &a)) {
      *fault = esi;
      return 0;
    }
    if (!x86p_mem_write(mem, edi, w, a)) {
      *fault = edi;
      return 0;
    }
    break;
  case kX86pStringStos:
    if (!x86p_mem_write(mem, edi, w, x86p_reg_read(cpu, kX86pEax, w))) {
      *fault = edi;
      return 0;
    }
    break;
  case kX86pStringLods:
    if (!x86p_mem_read(mem, esi, w, &a)) {
      *fault = esi;
      return 0;
    }
    x86p_reg_write(cpu, kX86pEax, w, a);
    break;
  case kX86pStringScas:
    if (!x86p_mem_read(mem, edi, w, &b)) {
      *fault = edi;
      return 0;
    }
    /* Through x86p_alu, so the six flags are whatever a CMP writes -- there is
       one authority on that and this is not it. */
    (void)x86p_alu(kX86pAluCmp, x86p_reg_read(cpu, kX86pEax, w), b, w, &cpu->flags);
    break;
  case kX86pStringCmps:
    /* The SOURCE is at ESI and the destination at EDI, and the comparison is
       source MINUS destination. Getting that round the wrong way leaves every
       flag except ZF inverted, which a memcmp-shaped guest loop would not
       notice until it reported the wrong ordering. */
    if (!x86p_mem_read(mem, esi, w, &a)) {
      *fault = esi;
      return 0;
    }
    if (!x86p_mem_read(mem, edi, w, &b)) {
      *fault = edi;
      return 0;
    }
    (void)x86p_alu(kX86pAluCmp, a, b, w, &cpu->flags);
    break;
  case kX86pStringOpCount:
  default:
    return 0;
  }

  /* Which pointers advance is part of the operation, not a detail: STOS never
     touches ESI and LODS never touches EDI, and advancing both would corrupt a
     register the guest was using for something else entirely. */
  if (op == kX86pStringMovs || op == kX86pStringLods || op == kX86pStringCmps) {
    cpu->reg[kX86pEsi] = esi + (uint32_t)delta;
  }
  if (op != kX86pStringLods) {
    cpu->reg[kX86pEdi] = edi + (uint32_t)delta;
  }
  return 1;
}

X86pStringStatus x86p_string_execute(X86pCpu *cpu, const X86pMem *mem, const X86pInsn *insn, uint32_t *fault_addr) {
  X86pStringOp op;
  X86pRepKind rep;
  int w;
  int32_t delta;

  if (fault_addr) {
    *fault_addr = 0u;
  }
  if (!cpu || !insn) {
    return kX86pStringFault;
  }
  op = (X86pStringOp)insn->str;
  rep = (X86pRepKind)insn->rep;
  w = (int)insn->str_width;
  if (!x86p_string_is_supported(op, rep, w)) {
    return kX86pStringUnsupported;
  }

  /* DF chooses the SIGN of the stride, and the magnitude is the element width
     -- not one. A model that stepped by one byte would work for the byte forms
     and quietly overlap for every other. */
  delta = cpu->df ? -(int32_t)w : (int32_t)w;

  if (rep == kX86pRepNone) {
    uint32_t fault = 0u;
    if (!step_once(cpu, mem, op, w, delta, &fault)) {
      if (fault_addr) {
        *fault_addr = fault;
      }
      return kX86pStringFault;
    }
    return kX86pStringOk;
  }

  /* A forward, disjoint, fully mapped MOVS can copy in bulk. All other cases
     keep the iteration boundary, including the precise state at a fault. */
  if (op == kX86pStringMovs && !cpu->df) {
    uint64_t bytes = (uint64_t)cpu->reg[kX86pEcx] * (unsigned)w;
    if (bytes <= UINT32_MAX && x86p_mem_copy_disjoint(mem, cpu->reg[kX86pEdi], cpu->reg[kX86pEsi], (uint32_t)bytes)) {
      cpu->reg[kX86pEsi] += (uint32_t)bytes;
      cpu->reg[kX86pEdi] += (uint32_t)bytes;
      cpu->reg[kX86pEcx] = 0;
      return kX86pStringOk;
    }
  }

  /*
   * The repeat. ECX is tested BEFORE the first iteration, so REP with ECX == 0
   * does nothing at all -- it does not do one pass and then check.
   */
  while (cpu->reg[kX86pEcx] != 0u) {
    uint32_t fault = 0u;
    if (!step_once(cpu, mem, op, w, delta, &fault)) {
      if (fault_addr) {
        *fault_addr = fault;
      }
      return kX86pStringFault;
    }
    cpu->reg[kX86pEcx]--;
    /*
     * The ZF test comes AFTER the decrement and after the comparison, and only
     * the two operations that compare have one. Testing it before the
     * iteration would run one fewer pass and leave ECX one too high.
     */
    if (op == kX86pStringScas || op == kX86pStringCmps) {
      int zf = x86p_flag_zf(&cpu->flags);
      if (rep == kX86pRepRep && !zf) {
        break;
      }
      if (rep == kX86pRepRepne && zf) {
        break;
      }
    }
  }
  return kX86pStringOk;
}
