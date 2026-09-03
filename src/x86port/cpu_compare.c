#include "cpu_compare.h"

#include "flags.h"
#include "x87.h"

#include <stdio.h>
#include <string.h>

typedef struct {
  const X86pCpu *a;
  const X86pCpu *b;
  X86pCpuDiffFn on_diff;
  void *user;
  unsigned count;
} Diff;

static void report(Diff *d, const char *field, const char *a_text, const char *b_text) {
  d->count++;
  if (d->on_diff) {
    d->on_diff(field, a_text, b_text, d->user);
  }
}

static void report_u32(Diff *d, const char *field, uint32_t a, uint32_t b) {
  char at[16];
  char bt[16];
  if (a == b) {
    return;
  }
  (void)snprintf(at, sizeof at, "%08X", a);
  (void)snprintf(bt, sizeof bt, "%08X", b);
  report(d, field, at, bt);
}

static void report_int(Diff *d, const char *field, int a, int b) {
  char at[16];
  char bt[16];
  if (a == b) {
    return;
  }
  (void)snprintf(at, sizeof at, "%d", a);
  (void)snprintf(bt, sizeof bt, "%d", b);
  report(d, field, at, bt);
}

static void diff_flags(Diff *d) {
  const X86pFlags *fa = &d->a->flags;
  const X86pFlags *fb = &d->b->flags;
  /*
   * `carry_in` is a cache, not architectural state: x86p_flag_cf reads it ONLY
   * when kind is Inc or Dec (flags.c), because those are the operations that
   * preserve CF without recomputing it. For every other kind CF falls out of
   * a/b/r and carry_in is dead. The JIT's dead-flag-store elimination can leave
   * a stale carry_in behind exactly one such write (the block's last flag
   * writer, whose own predecessor's store was elided) -- semantically
   * invisible, so comparing it there would be a false divergence. When the kind
   * IS Inc/Dec the field is live and still compared.
   */
  const int carry_live = (fa->kind == (uint8_t)kX86pFlagsInc || fa->kind == (uint8_t)kX86pFlagsDec);
  const int carry_diff = carry_live && fa->carry_in != fb->carry_in;
  if (fa->kind != fb->kind || fa->a != fb->a || fa->b != fb->b || fa->r != fb->r || fa->w != fb->w || carry_diff) {
    char at[80];
    char bt[80];
    (void)snprintf(at, sizeof at, "k%u %08X %08X %08X w%u c%u", fa->kind, fa->a, fa->b, fa->r, fa->w, fa->carry_in);
    (void)snprintf(bt, sizeof bt, "k%u %08X %08X %08X w%u c%u", fb->kind, fb->a, fb->b, fb->r, fb->w, fb->carry_in);
    report(d, "lazy flags", at, bt);
  }
  report_int(d, "CF", x86p_flag_cf(fa), x86p_flag_cf(fb));
  report_int(d, "PF", x86p_flag_pf(fa), x86p_flag_pf(fb));
  report_int(d, "AF", x86p_flag_af(fa), x86p_flag_af(fb));
  report_int(d, "ZF", x86p_flag_zf(fa), x86p_flag_zf(fb));
  report_int(d, "SF", x86p_flag_sf(fa), x86p_flag_sf(fb));
  report_int(d, "OF", x86p_flag_of(fa), x86p_flag_of(fb));
}

static void diff_x87(Diff *d) {
  const X86pX87 *xa = &d->a->x87;
  const X86pX87 *xb = &d->b->x87;
  int k;
  report_u32(d, "x87 TOP", xa->top, xb->top);
  report_u32(d, "x87 CW", xa->control, xb->control);
  report_u32(d, "x87 SW", xa->status, xb->status);
  for (k = 0; k < X86P_X87_REGS; k++) {
    long double va = 0.0L;
    long double vb = 0.0L;
    const int ha = x86p_x87_get(xa, k, &va);
    const int hb = x86p_x87_get(xb, k, &vb);
    /* The architectural bytes only: an 80-bit value sits in a wider host object
       whose padding no instruction defines. x87.c is the authority. */
    const size_t significant = x86p_x87_precision_is_exact() ? 10u : sizeof va;
    if (ha != hb || (ha && memcmp(&va, &vb, significant) != 0)) {
      char field[16];
      char at[48];
      char bt[48];
      (void)snprintf(field, sizeof field, "ST(%d)", k);
      (void)snprintf(at, sizeof at, "%s%.20Lg", ha ? "" : "(empty)", va);
      (void)snprintf(bt, sizeof bt, "%s%.20Lg", hb ? "" : "(empty)", vb);
      report(d, field, at, bt);
    }
  }
}

unsigned x86p_cpu_diff(const X86pCpu *a, const X86pCpu *b, X86pCpuDiffFn on_diff, void *user) {
  Diff d;
  int i;
  d.a = a;
  d.b = b;
  d.on_diff = on_diff;
  d.user = user;
  d.count = 0u;

  for (i = 0; i < kX86pRegCount; i++) {
    report_u32(&d, x86p_reg_name(i, 4), a->reg[i], b->reg[i]);
  }
  report_u32(&d, "EIP", a->eip, b->eip);
  for (i = 0; i < kX86pSegRegCount; i++) {
    char field[12];
    (void)snprintf(field, sizeof field, "seg[%d]", i);
    report_u32(&d, field, a->seg[i], b->seg[i]);
  }
  report_u32(&d, "FS base", a->fs_base, b->fs_base);
  report_u32(&d, "GS base", a->gs_base, b->gs_base);
  report_int(&d, "DF", a->df, b->df);
  report_u32(&d, "MXCSR", a->mxcsr, b->mxcsr);
  for (i = 0; i < 8; i++) {
    if (memcmp(a->xmm[i], b->xmm[i], 16) != 0) {
      char field[12];
      char at[40];
      char bt[40];
      (void)snprintf(field, sizeof field, "XMM%d", i);
      (void)snprintf(at,
                     sizeof at,
                     "%08X%08X%08X%08X",
                     ((const uint32_t *)a->xmm[i])[3],
                     ((const uint32_t *)a->xmm[i])[2],
                     ((const uint32_t *)a->xmm[i])[1],
                     ((const uint32_t *)a->xmm[i])[0]);
      (void)snprintf(bt,
                     sizeof bt,
                     "%08X%08X%08X%08X",
                     ((const uint32_t *)b->xmm[i])[3],
                     ((const uint32_t *)b->xmm[i])[2],
                     ((const uint32_t *)b->xmm[i])[1],
                     ((const uint32_t *)b->xmm[i])[0]);
      report(&d, field, at, bt);
    }
  }
  diff_flags(&d);
  diff_x87(&d);
  return d.count;
}
