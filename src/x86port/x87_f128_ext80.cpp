#include "x87_f128_ext80.h"

#include <cstdint>
#include <cstring>

namespace {

/* binary128: sign(1) exponent(15) fraction(112). ext80: sign(1) exponent(15)
   and a 64-bit significand whose leading integer bit is explicit. Both use
   exponent bias 16383 over the same 15-bit range, so a finite normal keeps its
   biased exponent unchanged and only the significand is reassembled. */
constexpr uint16_t kExponentMask = 0x7FFFu;
constexpr uint64_t kFractionHighMask = 0x0000FFFFFFFFFFFFULL; /* 48 bits */
constexpr uint64_t kInexactLowMask = 0x0001FFFFFFFFFFFFULL;   /* 49 bits */
constexpr unsigned kLowFractionBits = 49u;
constexpr unsigned kHighFractionShift = 15u;
constexpr uint64_t kIntegerBit = 1ULL << 63;

int is_finite_normal(uint16_t sign_exponent) {
  const uint16_t exponent = static_cast<uint16_t>(sign_exponent & kExponentMask);
  return exponent != 0u && exponent != kExponentMask;
}

} // namespace

extern "C" int x86p_x87_f128_to_ext80_exact(const void *bits, floatx80 *out) {
  uint64_t words[2];
  if (!bits || !out) {
    return 0;
  }
  std::memcpy(words, bits, sizeof words);
  const uint16_t sign_exponent = static_cast<uint16_t>(words[1] >> 48);
  if (!is_finite_normal(sign_exponent) || (words[0] & kInexactLowMask) != 0ULL) {
    return 0;
  }
  out->signExp = sign_exponent;
  out->signif = kIntegerBit | ((words[1] & kFractionHighMask) << kHighFractionShift) |
                (words[0] >> kLowFractionBits);
  return 1;
}

extern "C" int x86p_x87_ext80_to_f128_exact(floatx80 value, void *bits) {
  uint64_t words[2];
  if (!bits || !is_finite_normal(value.signExp) || (value.signif & kIntegerBit) == 0ULL) {
    return 0;
  }
  words[0] = value.signif << kLowFractionBits;
  words[1] = (static_cast<uint64_t>(value.signExp) << 48) |
             ((value.signif >> kHighFractionShift) & kFractionHighMask);
  std::memcpy(bits, words, sizeof words);
  return 1;
}
