/*
 * The exact widening, checked against the hardware the format is named after.
 *
 * `x86p_ext80_from_f32_bits` and its binary64 twin replace a softfloat
 * conversion on the guest's hottest load path, so "close enough" is not a
 * result: the answer has to be the same 80 bits, for every class of input,
 * including the ones a frame of a game never produces.
 *
 * Two independent authorities, because each covers what the other cannot:
 *
 *   - A written-out table of every class -- zero, the subnormal boundaries,
 *     the normal boundaries, infinity, quiet and signalling NaN -- with the
 *     bits spelled out. It runs on EVERY host, including the WASM one where
 *     there is no x87 to ask, and it is the only check that knows what the
 *     answer should be rather than merely what something else also says.
 *   - On a host whose `long double` really is ext80, the x87 unit itself,
 *     over millions of values: every subnormal binary32 exhaustively, a stride
 *     sweep of the whole binary32 space, and a structured plus pseudo-random
 *     sweep of binary64. A conversion that matched the table by construction
 *     and drifted anywhere else is caught here.
 *
 * The comparator is itself checked against a deliberately wrong value, because
 * a differential test that cannot fail reports agreement it never established.
 */
#include "x87_ext80_widen.h"

#include "x87_binary128.h"
#include "x87_softfloat.h"

#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int g_failures;
long long g_checks;

bool same(X86pExt80 got, uint64_t signif, uint16_t sign_exp) {
  return got.signif == signif && got.sign_exp == sign_exp;
}

void expect(X86pExt80 got, uint64_t signif, uint16_t sign_exp, const char *what) {
  g_checks++;
  if (!same(got, signif, sign_exp)) {
    std::printf("FAIL %s: got %04x:%016llx want %04x:%016llx\n",
                what,
                got.sign_exp,
                (unsigned long long)got.signif,
                sign_exp,
                (unsigned long long)signif);
    g_failures++;
  }
}

#if X86P_X87_BINARY128
#define HAVE_ORACLE 1
/*
 * The softfloat conversion this module replaces, on the host where it was the
 * shipping one and where there is no x87 to ask instead. Comparing the
 * replacement against the replaced is the exact statement the change needs,
 * and it runs in the WASM gate, which is the build that actually changed.
 */
X86pExt80 oracle_from_f32(uint32_t bits) {
  const X86pX87Reg reg = x86p_x87_software_widen_f32(bits);
  X86pExt80 out;
  out.signif = reg.signif;
  out.sign_exp = reg.sign_exp;
  return out;
}

X86pExt80 oracle_from_f64(uint64_t bits) {
  const X86pX87Reg reg = x86p_x87_software_widen_f64(bits);
  X86pExt80 out;
  out.signif = reg.signif;
  out.sign_exp = reg.sign_exp;
  return out;
}

const char *oracle_name = "softfloat";
#elif LDBL_MANT_DIG == 64 && LDBL_MAX_EXP == 16384
#define HAVE_ORACLE 1
#define ORACLE_IS_HOST_X87 1

/* The host's own conversion, read back as the two architectural fields. */
X86pExt80 host_of(long double v) {
  unsigned char bytes[sizeof(long double)];
  X86pExt80 out{};
  std::memcpy(bytes, &v, sizeof bytes);
  std::memcpy(&out.signif, bytes, 8);
  std::memcpy(&out.sign_exp, bytes + 8, 2);
  return out;
}

X86pExt80 oracle_from_f32(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, sizeof f);
  return host_of((long double)f);
}

X86pExt80 oracle_from_f64(uint64_t bits) {
  double d;
  std::memcpy(&d, &bits, sizeof d);
  return host_of((long double)d);
}

const char *oracle_name = "host x87";
#endif

#if HAVE_ORACLE
/*
 * A NaN is compared against softfloat, which is the behaviour that shipped,
 * and skipped against the host x87. Every host quiets a signalling NaN and
 * preserves its payload, but WHICH instruction a compiler chooses to widen a
 * float with is its own business, and for a signalling NaN the answer can
 * follow that choice rather than the format. The table pins the NaN encoding
 * on every host regardless, so nothing here goes unchecked.
 */
bool skip_nan(bool is_nan) {
#if ORACLE_IS_HOST_X87
  return is_nan;
#else
  (void)is_nan;
  return false;
#endif
}

bool is_nan_f32(uint32_t bits) {
  return ((bits >> 23) & 0xFFu) == 0xFFu && (bits & 0x7FFFFFu) != 0u;
}

bool is_nan_f64(uint64_t bits) {
  return ((bits >> 52) & 0x7FFu) == 0x7FFu && (bits & 0xFFFFFFFFFFFFFull) != 0u;
}

void differ_f32(uint32_t bits, const char *what) {
  if (skip_nan(is_nan_f32(bits))) {
    return;
  }
  const X86pExt80 want = oracle_from_f32(bits), got = x86p_ext80_from_f32_bits(bits);
  g_checks++;
  if (!same(got, want.signif, want.sign_exp)) {
    std::printf("FAIL %s 0x%08x: got %04x:%016llx host %04x:%016llx\n",
                what,
                bits,
                got.sign_exp,
                (unsigned long long)got.signif,
                want.sign_exp,
                (unsigned long long)want.signif);
    g_failures++;
  }
}

void differ_f64(uint64_t bits, const char *what) {
  if (skip_nan(is_nan_f64(bits))) {
    return;
  }
  const X86pExt80 want = oracle_from_f64(bits), got = x86p_ext80_from_f64_bits(bits);
  g_checks++;
  if (!same(got, want.signif, want.sign_exp)) {
    std::printf("FAIL %s 0x%016llx: got %04x:%016llx host %04x:%016llx\n",
                what,
                (unsigned long long)bits,
                got.sign_exp,
                (unsigned long long)got.signif,
                want.sign_exp,
                (unsigned long long)want.signif);
    g_failures++;
  }
}
#endif

void table() {
  /* binary32. 16383 is ext80's bias, so 0x3FFF is an exponent of zero. */
  expect(x86p_ext80_from_f32_bits(0x00000000u), 0u, 0x0000u, "f32 +0");
  expect(x86p_ext80_from_f32_bits(0x80000000u), 0u, 0x8000u, "f32 -0");
  expect(x86p_ext80_from_f32_bits(0x3F800000u), 0x8000000000000000ull, 0x3FFFu, "f32 1.0");
  expect(x86p_ext80_from_f32_bits(0xBF800000u), 0x8000000000000000ull, 0xBFFFu, "f32 -1.0");
  expect(x86p_ext80_from_f32_bits(0x40000000u), 0x8000000000000000ull, 0x4000u, "f32 2.0");
  expect(x86p_ext80_from_f32_bits(0x3FC00000u), 0xC000000000000000ull, 0x3FFFu, "f32 1.5");
  /* The smallest subnormal, 2^-149: 23 normalising shifts below 2^-126. */
  expect(x86p_ext80_from_f32_bits(0x00000001u), 0x8000000000000000ull, 0x3F6Au, "f32 min subnormal");
  /* The largest subnormal, one shift below the smallest normal. */
  expect(x86p_ext80_from_f32_bits(0x007FFFFFu), 0xFFFFFE0000000000ull, 0x3F80u, "f32 max subnormal");
  expect(x86p_ext80_from_f32_bits(0x00800000u), 0x8000000000000000ull, 0x3F81u, "f32 min normal");
  expect(x86p_ext80_from_f32_bits(0x7F7FFFFFu), 0xFFFFFF0000000000ull, 0x407Eu, "f32 max normal");
  expect(x86p_ext80_from_f32_bits(0x7F800000u), 0x8000000000000000ull, 0x7FFFu, "f32 +inf");
  expect(x86p_ext80_from_f32_bits(0xFF800000u), 0x8000000000000000ull, 0xFFFFu, "f32 -inf");
  expect(x86p_ext80_from_f32_bits(0x7FC00000u), 0xC000000000000000ull, 0x7FFFu, "f32 quiet NaN");
  /* A signalling NaN is quieted and keeps its payload, bit for bit. */
  expect(x86p_ext80_from_f32_bits(0x7F800001u), 0xC000010000000000ull, 0x7FFFu, "f32 signalling NaN");
  expect(x86p_ext80_from_f32_bits(0xFFC00001u), 0xC000010000000000ull, 0xFFFFu, "f32 -NaN payload");

  /* binary64. */
  expect(x86p_ext80_from_f64_bits(0x0000000000000000ull), 0u, 0x0000u, "f64 +0");
  expect(x86p_ext80_from_f64_bits(0x8000000000000000ull), 0u, 0x8000u, "f64 -0");
  expect(x86p_ext80_from_f64_bits(0x3FF0000000000000ull), 0x8000000000000000ull, 0x3FFFu, "f64 1.0");
  expect(x86p_ext80_from_f64_bits(0xBFF0000000000000ull), 0x8000000000000000ull, 0xBFFFu, "f64 -1.0");
  expect(x86p_ext80_from_f64_bits(0x3FF8000000000000ull), 0xC000000000000000ull, 0x3FFFu, "f64 1.5");
  expect(x86p_ext80_from_f64_bits(0x0000000000000001ull), 0x8000000000000000ull, 0x3BCDu, "f64 min subnormal");
  expect(x86p_ext80_from_f64_bits(0x000FFFFFFFFFFFFFull), 0xFFFFFFFFFFFFF000ull, 0x3C00u, "f64 max subnormal");
  expect(x86p_ext80_from_f64_bits(0x0010000000000000ull), 0x8000000000000000ull, 0x3C01u, "f64 min normal");
  expect(x86p_ext80_from_f64_bits(0x7FEFFFFFFFFFFFFFull), 0xFFFFFFFFFFFFF800ull, 0x43FEu, "f64 max normal");
  expect(x86p_ext80_from_f64_bits(0x7FF0000000000000ull), 0x8000000000000000ull, 0x7FFFu, "f64 +inf");
  expect(x86p_ext80_from_f64_bits(0x7FF8000000000000ull), 0xC000000000000000ull, 0x7FFFu, "f64 quiet NaN");
  expect(x86p_ext80_from_f64_bits(0x7FF0000000000001ull), 0xC000000000000800ull, 0x7FFFu, "f64 signalling NaN");
}

/* The comparator must reject a wrong answer, or every agreement above is
   vacuous. This checks the check, not the conversion. */
void comparator_rejects_a_wrong_value() {
  const X86pExt80 one = x86p_ext80_from_f32_bits(0x3F800000u);
  g_checks++;
  if (same(one, one.signif + 1u, one.sign_exp) || same(one, one.signif, (uint16_t)(one.sign_exp + 1u))) {
    std::printf("FAIL the comparator accepts a value it should reject\n");
    g_failures++;
  }
}

} // namespace

int main() {
  table();
  comparator_rejects_a_wrong_value();

#if HAVE_ORACLE
  /* Every binary32 subnormal, exhaustively: the normalising loop is the only
     branch here with a variable trip count, and this is its whole domain. */
  for (uint32_t mant = 0u; mant < 0x800000u; mant++) {
    differ_f32(mant, "f32 subnormal");
    differ_f32(mant | 0x80000000u, "f32 -subnormal");
  }
  /* A stride sweep of the entire binary32 space. The stride is odd and coprime
     with every power of two, so it does not settle into one exponent. */
  for (uint64_t bits = 0u; bits < 0x100000000ull; bits += 1009u) {
    differ_f32((uint32_t)bits, "f32 sweep");
  }
  /* Both boundaries of every binary32 exponent, where a rebias goes wrong. */
  for (uint32_t exp = 0u; exp < 0x100u; exp++) {
    differ_f32(exp << 23, "f32 exponent");
    differ_f32((exp << 23) | 0x7FFFFFu, "f32 exponent top");
    differ_f32((exp << 23) | 0x80000000u, "f32 -exponent");
  }

  /* binary64: every exponent against a set of fraction shapes, then a walking
     one through all 52 fraction bits, which is what the subnormal loop sees. */
  static const uint64_t shapes[] = {0ull,
                                    1ull,
                                    0xFFFFFFFFFFFFFull,
                                    0x8000000000000ull,
                                    0xAAAAAAAAAAAAAull,
                                    0x5555555555555ull,
                                    0xFFFFFull,
                                    0xFFFFFFFFFFF00ull};
  for (uint32_t exp = 0u; exp < 0x800u; exp++) {
    for (uint64_t shape : shapes) {
      differ_f64(((uint64_t)exp << 52) | shape, "f64 exponent");
      differ_f64(((uint64_t)exp << 52) | shape | 0x8000000000000000ull, "f64 -exponent");
    }
  }
  for (unsigned bit = 0u; bit < 52u; bit++) {
    differ_f64(1ull << bit, "f64 subnormal walk");
    differ_f64((1ull << bit) | 1ull, "f64 subnormal walk pair");
  }
  /* A deterministic pseudo-random sweep, so the shapes above are not the only
     fractions this has ever seen. */
  {
    uint64_t state = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 2000000; i++) {
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
      differ_f64(state, "f64 random");
      differ_f32((uint32_t)(state >> 32), "f32 random");
    }
  }
  std::printf("x87 ext80 widen: differential ran against %s\n", oracle_name);
#else
  /*
   * Neither a real x87 nor the softfloat build. The table still ran, but a
   * reader has to be told which of the two statements this run made, or a
   * green line here reads as the full check on a host that never took it.
   */
  std::printf("x87 ext80 widen: NO ORACLE on this host -- table only, no "
              "differential. This run did not compare the conversion with "
              "anything.\n");
#endif

  std::printf("x87 ext80 widen: %lld check(s), %d failure(s)\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
