/*
 * The bit-level binary128 helpers against the compiler's own conversions.
 *
 * The helpers exist to replace __extendsftf2, __extenddftf2, __cmptf2 and
 * __unordtf2 on a host that has no binary128 hardware, so the discriminator is
 * exactly those library results: this builds the same values with `__float128`
 * arithmetic and requires the bit patterns to match. A host without
 * `__float128` runs the fixed vectors only and says so.
 */
#include "x87_binary128.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void check(int ok, const char *what, uint64_t a, uint64_t b) {
  if (ok) {
    return;
  }
  printf("FAIL %s: %016llx vs %016llx\n", what, (unsigned long long)a, (unsigned long long)b);
  failures++;
}

static const uint32_t kF32[] = {
    0x00000000u, 0x80000000u, 0x3F800000u, 0xBF800000u, 0x00000001u, 0x007FFFFFu, 0x00400000u,
    0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7F800001u, 0x7F7FFFFFu, 0x00800000u, 0x40490FDBu,
};
static const uint64_t kF64[] = {
    0x0000000000000000ull, 0x8000000000000000ull, 0x3FF0000000000000ull, 0xBFF0000000000000ull,
    0x0000000000000001ull, 0x000FFFFFFFFFFFFFull, 0x0008000000000000ull, 0x7FF0000000000000ull,
    0xFFF0000000000000ull, 0x7FF8000000000000ull, 0x7FF0000000000001ull, 0x7FEFFFFFFFFFFFFFull,
    0x0010000000000000ull, 0x400921FB54442D18ull,
};

#if defined(__SIZEOF_FLOAT128__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define HAVE_REFERENCE 1
static X86pF128 reference_of(__float128 v) {
  X86pF128 out;
  memcpy(&out, &v, sizeof out);
  return out;
}
static __float128 reference_value(X86pF128 v) {
  __float128 out;
  memcpy(&out, &v, sizeof out);
  return out;
}
#endif

static void widening(void) {
  unsigned i;
  for (i = 0; i < sizeof kF32 / sizeof *kF32; i++) {
    const X86pF128 got = x86p_f128_from_f32(kF32[i]);
    float narrow;
    memcpy(&narrow, &kF32[i], sizeof narrow);
    check(x86p_f128_is_nan(got) == (isnan(narrow) != 0), "f32 nan", kF32[i], 0u);
    check(x86p_f128_is_inf(got) == (isinf(narrow) != 0), "f32 inf", kF32[i], 0u);
    check(x86p_f128_is_zero(got) == (narrow == 0.0f), "f32 zero", kF32[i], 0u);
#ifdef HAVE_REFERENCE
    {
      const X86pF128 want = reference_of((__float128)narrow);
      check(got.hi == want.hi && got.lo == want.lo, "f32 widen hi", got.hi, want.hi);
      check(got.lo == want.lo, "f32 widen lo", got.lo, want.lo);
    }
#endif
  }
  for (i = 0; i < sizeof kF64 / sizeof *kF64; i++) {
    const X86pF128 got = x86p_f128_from_f64(kF64[i]);
    double narrow;
    memcpy(&narrow, &kF64[i], sizeof narrow);
    check(x86p_f128_is_nan(got) == (isnan(narrow) != 0), "f64 nan", kF64[i], 0u);
    check(x86p_f128_is_inf(got) == (isinf(narrow) != 0), "f64 inf", kF64[i], 0u);
    check(x86p_f128_is_zero(got) == (narrow == 0.0), "f64 zero", kF64[i], 0u);
#ifdef HAVE_REFERENCE
    {
      const X86pF128 want = reference_of((__float128)narrow);
      check(got.hi == want.hi && got.lo == want.lo, "f64 widen hi", got.hi, want.hi);
      check(got.lo == want.lo, "f64 widen lo", got.lo, want.lo);
    }
#endif
  }
}

static void ordering(void) {
  unsigned i, j;
  for (i = 0; i < sizeof kF64 / sizeof *kF64; i++) {
    for (j = 0; j < sizeof kF64 / sizeof *kF64; j++) {
      const X86pF128 a = x86p_f128_from_f64(kF64[i]), b = x86p_f128_from_f64(kF64[j]);
      double da, db;
      int want;
      memcpy(&da, &kF64[i], sizeof da);
      memcpy(&db, &kF64[j], sizeof db);
      if (isnan(da) || isnan(db)) {
        continue; /* unordered; the caller tests for NaN before comparing */
      }
      want = da > db ? 1 : (da < db ? -1 : 0);
      check(x86p_f128_compare(a, b) == want, "compare", (uint64_t)i, (uint64_t)j);
#ifdef HAVE_REFERENCE
      {
        const __float128 qa = reference_value(a), qb = reference_value(b);
        const int reference = qa > qb ? 1 : (qa < qb ? -1 : 0);
        check(x86p_f128_compare(a, b) == reference, "compare reference", (uint64_t)i, (uint64_t)j);
      }
#endif
    }
  }
}

int main(void) {
#ifndef HAVE_REFERENCE
  printf("no __float128 on this host: fixed vectors only\n");
#endif
  widening();
  ordering();
  if (failures) {
    printf("test_x87_binary128: %d failure(s)\n", failures);
    return 1;
  }
  printf("test_x87_binary128: ok\n");
  return 0;
}
