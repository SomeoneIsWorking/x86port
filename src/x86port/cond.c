/* cond.c -- see cond.h for why the signed/unsigned pairs are the trap. */
#include "cond.h"

static const char *kNames[] = {"O", "NO", "B", "NB", "Z", "NZ", "BE", "A", "S", "NS", "P", "NP", "L", "GE", "LE", "G"};
_Static_assert((int)(sizeof kNames / sizeof kNames[0]) == (int)kX86pCondCount, "every X86pCond needs a name");

const char *x86p_cond_name(X86pCond cc) {
  if ((unsigned)cc >= (unsigned)kX86pCondCount) {
    return "??";
  }
  return kNames[(int)cc];
}

int x86p_cond_eflags(X86pCond cc, uint32_t eflags) {
  const int cf = (eflags & X86P_CF) != 0;
  const int pf = (eflags & X86P_PF) != 0;
  const int zf = (eflags & X86P_ZF) != 0;
  const int sf = (eflags & X86P_SF) != 0;
  const int of = (eflags & X86P_OF) != 0;

  switch (cc) {
  case kX86pCondO:
    return of;
  case kX86pCondNO:
    return !of;
  case kX86pCondB:
    return cf;
  case kX86pCondNB:
    return !cf;
  case kX86pCondZ:
    return zf;
  case kX86pCondNZ:
    return !zf;
  case kX86pCondBE:
    return cf || zf;
  case kX86pCondA:
    return !cf && !zf;
  case kX86pCondS:
    return sf;
  case kX86pCondNS:
    return !sf;
  case kX86pCondP:
    return pf;
  case kX86pCondNP:
    return !pf;
  /* The signed four. SF != OF, not CF -- JB and JL both read "less than" in
     English and test entirely different flags, and choosing wrong works for
     small positive values and fails across the sign boundary. */
  case kX86pCondL:
    return sf != of;
  case kX86pCondGE:
    return sf == of;
  case kX86pCondLE:
    return zf || (sf != of);
  case kX86pCondG:
    return !zf && (sf == of);
  case kX86pCondCount:
    break;
  }
  /* Total: an unknown condition is false and nameable, never an index off the
     end of a table. */
  return 0;
}

int x86p_cond(X86pCond cc, const X86pFlags *f) {
  if (!f || f->kind == kX86pFlagsNone) {
    return x86p_cond_eflags(cc, X86P_EFLAGS_FIXED);
  }
  if (f->kind == kX86pFlagsExplicit) {
    return x86p_cond_eflags(cc, f->a);
  }
  switch (cc) {
  case kX86pCondO:
    return x86p_flag_of(f);
  case kX86pCondNO:
    return !x86p_flag_of(f);
  case kX86pCondB:
    return x86p_flag_cf(f);
  case kX86pCondNB:
    return !x86p_flag_cf(f);
  case kX86pCondZ:
    return x86p_flag_zf(f);
  case kX86pCondNZ:
    return !x86p_flag_zf(f);
  case kX86pCondBE:
    return x86p_flag_zf(f) || x86p_flag_cf(f);
  case kX86pCondA:
    return !x86p_flag_zf(f) && !x86p_flag_cf(f);
  case kX86pCondS:
    return x86p_flag_sf(f);
  case kX86pCondNS:
    return !x86p_flag_sf(f);
  case kX86pCondP:
    return x86p_flag_pf(f);
  case kX86pCondNP:
    return !x86p_flag_pf(f);
  case kX86pCondL:
    return x86p_flag_sf(f) != x86p_flag_of(f);
  case kX86pCondGE:
    return x86p_flag_sf(f) == x86p_flag_of(f);
  case kX86pCondLE:
    return x86p_flag_zf(f) || (x86p_flag_sf(f) != x86p_flag_of(f));
  case kX86pCondG:
    return !x86p_flag_zf(f) && (x86p_flag_sf(f) == x86p_flag_of(f));
  case kX86pCondCount:
    break;
  }
  return 0;
}
