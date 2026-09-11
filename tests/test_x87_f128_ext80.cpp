/*
 * The exact binary128<->ext80 reassembly must agree with the general softfloat
 * conversion wherever it claims a value, and must refuse every form it cannot
 * represent exactly. Both directions are checked against softfloat itself, so a
 * silent divergence in either one fails here rather than inside guest FP state.
 */
#include "x87_f128_ext80.h"

#include "softfloat.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

softfloat_status_t environment() {
  softfloat_status_t status{};
  status.softfloat_exceptionMasks = 0x3F;
  status.softfloat_roundingMode = 0;
  status.extF80_roundingPrecision = 80;
  return status;
}

float128_t f128_from_words(uint64_t low, uint64_t high) {
  float128_t value{};
  const uint64_t words[2] = {low, high};
  std::memcpy(&value, words, sizeof words);
  return value;
}

int same_ext80(floatx80 a, floatx80 b) { return a.signExp == b.signExp && a.signif == b.signif; }

unsigned failures;
unsigned checks;
unsigned exact_widen;
unsigned exact_narrow;

void check_widen(uint64_t low, uint64_t high, int expect_exact) {
  const float128_t value = f128_from_words(low, high);
  floatx80 fast{};
  uint64_t storage[2] = {low, high};
  const int taken = x86p_x87_f128_to_ext80_exact(storage, &fast);
  checks++;
  if (taken != expect_exact) {
    failures++;
    printf("widen %016llx:%016llx claimed %d, expected %d\n", (unsigned long long)high,
           (unsigned long long)low, taken, expect_exact);
    return;
  }
  if (!taken) {
    return;
  }
  exact_widen++;
  auto status = environment();
  const floatx80 general = f128_to_extF80(value, &status);
  if (!same_ext80(fast, general)) {
    failures++;
    printf("widen %016llx:%016llx gave %04x:%016llx, softfloat gave %04x:%016llx\n",
           (unsigned long long)high, (unsigned long long)low, fast.signExp,
           (unsigned long long)fast.signif, general.signExp, (unsigned long long)general.signif);
  }
}

void check_narrow(uint16_t sign_exponent, uint64_t significand, int expect_exact) {
  floatx80 value{};
  value.signExp = sign_exponent;
  value.signif = significand;
  uint64_t fast[2] = {0, 0};
  const int taken = x86p_x87_ext80_to_f128_exact(value, fast);
  checks++;
  if (taken != expect_exact) {
    failures++;
    printf("narrow %04x:%016llx claimed %d, expected %d\n", sign_exponent,
           (unsigned long long)significand, taken, expect_exact);
    return;
  }
  if (!taken) {
    return;
  }
  exact_narrow++;
  auto status = environment();
  const float128_t general = extF80_to_f128(value, &status);
  uint64_t expected[2];
  std::memcpy(expected, &general, sizeof expected);
  if (fast[0] != expected[0] || fast[1] != expected[1]) {
    failures++;
    printf("narrow %04x:%016llx gave %016llx:%016llx, softfloat gave %016llx:%016llx\n",
           sign_exponent, (unsigned long long)significand, (unsigned long long)fast[1],
           (unsigned long long)fast[0], (unsigned long long)expected[1],
           (unsigned long long)expected[0]);
  }
}

/* Every ext80 normal narrowed to binary128 must widen back to itself: this is
   the round trip the guest register file performs between two arithmetic ops. */
void check_round_trip(uint16_t sign_exponent, uint64_t significand) {
  floatx80 value{};
  value.signExp = sign_exponent;
  value.signif = significand;
  uint64_t bits[2];
  floatx80 back{};
  checks++;
  if (!x86p_x87_ext80_to_f128_exact(value, bits) || !x86p_x87_f128_to_ext80_exact(bits, &back) ||
      !same_ext80(value, back)) {
    failures++;
    printf("round trip lost %04x:%016llx\n", sign_exponent, (unsigned long long)significand);
  }
}

} // namespace

int main(void) {
  /* Exactly representable: the low 49 fraction bits are clear. */
  check_widen(0ULL, 0x3FFF000000000000ULL, 1);                  /* 1.0 */
  check_widen(0ULL, 0xBFFF000000000000ULL, 1);                  /* -1.0 */
  check_widen(0ULL, 0x4000800000000000ULL, 1);                  /* 3.0 */
  check_widen(0xFFFE000000000000ULL, 0x3FFFFFFFFFFFFFFFULL, 1); /* full 64-bit significand */
  /* Not exactly representable, or not a finite normal. */
  check_widen(1ULL, 0x3FFF000000000000ULL, 0);                  /* a bit below ext80 */
  check_widen(0x0001000000000000ULL, 0x3FFF000000000000ULL, 0); /* bit 48 set */
  check_widen(0ULL, 0x0000000000000000ULL, 0);                  /* +0 */
  check_widen(0ULL, 0x8000000000000000ULL, 0);                  /* -0 */
  check_widen(1ULL, 0x0000000000000000ULL, 0);                  /* subnormal */
  check_widen(0ULL, 0x7FFF000000000000ULL, 0);                  /* infinity */
  check_widen(1ULL, 0x7FFF000000000000ULL, 0);                  /* NaN */

  check_narrow(0x3FFFu, 0x8000000000000000ULL, 1);  /* 1.0 */
  check_narrow(0xBFFFu, 0x8000000000000000ULL, 1);  /* -1.0 */
  check_narrow(0x4000u, 0xC000000000000000ULL, 1);  /* 3.0 */
  check_narrow(0x0001u, 0x8000000000000000ULL, 1);  /* smallest normal exponent */
  check_narrow(0x7FFEu, 0xFFFFFFFFFFFFFFFFULL, 1);  /* largest finite */
  check_narrow(0x0000u, 0x8000000000000000ULL, 0);  /* zero/subnormal exponent */
  check_narrow(0x7FFFu, 0x8000000000000000ULL, 0);  /* infinity */
  check_narrow(0x7FFFu, 0xC000000000000000ULL, 0);  /* NaN */
  check_narrow(0x3FFFu, 0x7FFFFFFFFFFFFFFFULL, 0);  /* unnormal: integer bit clear */

  /* A sweep of normals across the exponent range and significand shapes. */
  for (unsigned exponent = 1u; exponent <= 0x7FFEu; exponent += 37u) {
    uint64_t significand = 0x8000000000000000ULL;
    for (unsigned step = 0; step < 8u; step++) {
      check_round_trip(static_cast<uint16_t>(exponent), significand);
      check_round_trip(static_cast<uint16_t>(exponent | 0x8000u), significand);
      uint64_t bits[2];
      floatx80 value{};
      value.signExp = static_cast<uint16_t>(exponent);
      value.signif = significand;
      if (x86p_x87_ext80_to_f128_exact(value, bits)) {
        check_widen(bits[0], bits[1], 1);
        check_narrow(static_cast<uint16_t>(exponent), significand, 1);
      }
      significand = significand | (significand >> (step + 1u)) | (1ULL << step);
    }
  }

  printf("x87 binary128<->ext80 exact conversion: %u check(s), %u exact widen, %u exact narrow, "
         "%u failure(s)\n",
         checks, exact_widen, exact_narrow, failures);
  return failures != 0u;
}
