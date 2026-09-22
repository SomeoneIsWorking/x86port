/*
 * FNSAVE'S TAG WORD, WHICH NOTHING USED TO READ BACK.
 *
 * The model keeps a byte of tag per register and the image packs two bits per
 * register, and no test looked at the result: the whole 108-byte round trip
 * was covered only by whatever a guest happened to do with it. That is the
 * one field whose value is DERIVED -- it says what each register's bits mean
 * -- so it is the field a change to how the model tracks that meaning can
 * break in silence.
 *
 * Every class the encoding distinguishes appears here, in a known physical
 * register, and the assertions are on the packed word rather than on the
 * model's own bytes, so the test reads the image a guest would read.
 */
#include "x87.h"
#include "x87_state.h"

#include <stdio.h>
#include <string.h>

enum { kPageShift = 12, kPage = 1u << kPageShift, kPages = 2, kLo = 0x30000000u };

static unsigned checks, failures;

static void check(int value, const char *message) {
  ++checks;
  if (!value) {
    ++failures;
    printf("FAIL: %s\n", message);
  }
}

/* The architectural pair for physical register `i`, out of the saved image. */
static unsigned tag_of_image(const uint8_t *bytes, unsigned i) {
  const uint16_t word = (uint16_t)(bytes[8] | (bytes[9] << 8));
  return (word >> (2u * i)) & 3u;
}

/* An ext80 value built from its fields, so the fixture cannot agree with the
   classifier merely by sharing its arithmetic. */
static long double ext80(uint64_t significand, uint16_t sign_exponent) {
  uint8_t raw[10];
  unsigned i;
  for (i = 0; i < 8u; i++) {
    raw[i] = (uint8_t)(significand >> (8u * i));
  }
  raw[8] = (uint8_t)sign_exponent;
  raw[9] = (uint8_t)(sign_exponent >> 8);
  return x86p_x87_reg_to_long_double(x86p_x87_reg_from_f80(raw));
}

/*
 * EVERY ENCODING CLASS, THROUGH THE WORD A GUEST READS.
 *
 * This used to assert on the model's own tag byte, which is a representation
 * and not a behaviour -- a model that classified elsewhere, or later, would
 * fail it while writing a perfectly correct image. The four encodings after the
 * ordinary ones are why the class is taken from the architectural fields
 * rather than from `isnan`/`isinf` -- a pseudo-denormal, an unnormal, a
 * pseudo-infinity and a pseudo-NaN are shapes no arithmetic predicate is
 * required to have an opinion about, and the hardware tags them by exponent
 * and significand like everything else. They are built here as bytes, because
 * no C literal produces one.
 */
static void every_encoding_class(const X86pMem *memory, const uint8_t *bytes) {
  static const struct {
    const char *what;
    uint8_t raw[10];
    X86pX87Tag tag;
  } kCases[] = {
      {"+0", {0, 0, 0, 0, 0, 0, 0, 0x00, 0x00, 0x00}, kX86pX87TagZero},
      {"-0", {0, 0, 0, 0, 0, 0, 0, 0x00, 0x00, 0x80}, kX86pX87TagZero},
      {"+1.0", {0, 0, 0, 0, 0, 0, 0, 0x80, 0xFF, 0x3F}, kX86pX87TagValid},
      {"-1.0", {0, 0, 0, 0, 0, 0, 0, 0x80, 0xFF, 0xBF}, kX86pX87TagValid},
      /* Exponent 0, significand non-zero, leading bit clear: a subnormal. */
      {"smallest subnormal", {1, 0, 0, 0, 0, 0, 0, 0x00, 0x00, 0x00}, kX86pX87TagValid},
      {"+inf", {0, 0, 0, 0, 0, 0, 0, 0x80, 0xFF, 0x7F}, kX86pX87TagSpecial},
      {"-inf", {0, 0, 0, 0, 0, 0, 0, 0x80, 0xFF, 0xFF}, kX86pX87TagSpecial},
      {"quiet NaN", {0, 0, 0, 0, 0, 0, 0, 0xC0, 0xFF, 0x7F}, kX86pX87TagSpecial},
      {"signalling NaN", {1, 0, 0, 0, 0, 0, 0, 0x80, 0xFF, 0x7F}, kX86pX87TagSpecial},
      /* Exponent 0 with the leading significand bit SET: pseudo-denormal. */
      {"pseudo-denormal", {0, 0, 0, 0, 0, 0, 0, 0x80, 0x00, 0x00}, kX86pX87TagValid},
      /* A normal exponent with the leading bit CLEAR: unnormal. */
      {"unnormal", {0, 0, 0, 0, 0, 0, 0, 0x40, 0xFF, 0x3F}, kX86pX87TagValid},
      /* Exponent all ones with the leading bit clear: pseudo-infinity and
         pseudo-NaN, which the 387 stopped producing and still tags special. */
      {"pseudo-infinity", {0, 0, 0, 0, 0, 0, 0, 0x00, 0xFF, 0x7F}, kX86pX87TagSpecial},
      {"pseudo-NaN", {1, 0, 0, 0, 0, 0, 0, 0x00, 0xFF, 0x7F}, kX86pX87TagSpecial},
  };
  size_t i;

  if (!x86p_x87_precision_is_exact()) {
    printf("  (skipped: this host's long double is not the ten-byte x87 object, "
           "so these encodings cannot be built)\n");
    return;
  }
  for (i = 0; i < sizeof kCases / sizeof kCases[0]; i++) {
    X86pX87 f;
    char message[128];

    snprintf(message, sizeof message, "%s is tagged %u in the saved word", kCases[i].what, (unsigned)kCases[i].tag);

    x86p_x87_reset(&f);
    check(x86p_x87_push_raw(&f, x86p_x87_reg_from_f80(kCases[i].raw)) != 0, "the encoding pushes");
    check(x86p_x87_save_state(&f, memory, kLo) != 0, "the state saves");
    check(tag_of_image(bytes, 7) == (unsigned)kCases[i].tag, message);

    /* And through the position-named write, which is the other caller. */
    x86p_x87_reset(&f);
    check(x86p_x87_push(&f, 1.0L) != 0, "a placeholder pushes");
    check(x86p_x87_set_raw(&f, 0, x86p_x87_reg_from_f80(kCases[i].raw)) != 0, "the encoding is set");
    check(x86p_x87_save_state(&f, memory, kLo) != 0, "the state saves");
    check(tag_of_image(bytes, 7) == (unsigned)kCases[i].tag, message);
  }
}

int main(void) {
  static uint8_t bytes[kPages * kPage];
  uint8_t perms[kPages];
  X86pMem memory = {0};
  X86pX87 fpu;

  memory.host = bytes;
  memory.lo = kLo;
  memory.size = sizeof bytes;
  memory.perms = perms;
  memory.page_shift = kPageShift;
  perms[0] = kX86pMemRead | kX86pMemWrite;
  perms[1] = kX86pMemRead | kX86pMemWrite;

  x86p_x87_reset(&fpu);

  /* Pushed newest-first, so these land in physical 7, 6, 5 and 4: the file
     grows downward from TOP and the image is in physical order. */
  check(x86p_x87_push(&fpu, 1.5L), "an ordinary value pushes");
  check(x86p_x87_push(&fpu, 0.0L), "a zero pushes");
  check(x86p_x87_push(&fpu, ext80(0x8000000000000000ull, 0x7FFFu)), "an infinity pushes");
  check(x86p_x87_push(&fpu, ext80(0xC000000000000000ull, 0x7FFFu)), "a NaN pushes");
  check(x86p_x87_depth(&fpu) == 4, "four registers are occupied");

  check(x86p_x87_save_state(&fpu, &memory, kLo) != 0, "the state saves");

  check(tag_of_image(bytes, 7) == (unsigned)kX86pX87TagValid, "1.5 is tagged valid");
  check(tag_of_image(bytes, 6) == (unsigned)kX86pX87TagZero, "0.0 is tagged zero");
  check(tag_of_image(bytes, 5) == (unsigned)kX86pX87TagSpecial, "an infinity is tagged special");
  check(tag_of_image(bytes, 4) == (unsigned)kX86pX87TagSpecial, "a NaN is tagged special");
  check(tag_of_image(bytes, 3) == (unsigned)kX86pX87TagEmpty, "an untouched register is tagged empty");
  check(tag_of_image(bytes, 0) == (unsigned)kX86pX87TagEmpty, "and so is physical zero");

  /* A value that ARRIVES by arithmetic rather than by being pushed: the
     result of 1.5 - 1.5 is a zero the model never saw written as one. */
  x86p_x87_reset(&fpu);
  check(x86p_x87_push(&fpu, 1.5L), "1.5 pushes again");
  check(x86p_x87_push(&fpu, 1.5L), "and a second time");
  {
    long double top = 0.0L;
    check(x86p_x87_pop(&fpu, &top), "the operand pops");
    check(x86p_x87_arith(&fpu, kX86pX87Sub, 0, top, 0) != 0, "ST(0) -= 1.5");
  }
  check(x86p_x87_save_state(&fpu, &memory, kLo) != 0, "the state saves again");
  /* The pop left physical 7 holding the result; physical 6 is the one it
     freed. Stack order and physical order differ, and the image is in the
     second -- getting that backwards is exactly what this file exists for. */
  check(tag_of_image(bytes, 7) == (unsigned)kX86pX87TagZero, "a zero PRODUCED by arithmetic is tagged zero, not valid");
  check(tag_of_image(bytes, 6) == (unsigned)kX86pX87TagEmpty, "and the popped register is empty");

  /* And the word survives the round trip, which is the pair of packings that
     a hand-written save/restore gets subtly wrong in one direction only. */
  check(x86p_x87_restore_state(&fpu, &memory, kLo) != 0, "the state restores");
  check(x86p_x87_depth(&fpu) == 1, "one register came back occupied");
  check(x86p_x87_save_state(&fpu, &memory, kLo) != 0, "and saves once more");
  check(tag_of_image(bytes, 7) == (unsigned)kX86pX87TagZero, "still a zero after the round trip");
  check(tag_of_image(bytes, 6) == (unsigned)kX86pX87TagEmpty, "and the emptied register is still empty");

  every_encoding_class(&memory, bytes);

  printf("%s: %u check(s), %u failure(s)\n", failures ? "FAIL" : "ok", checks, failures);
  return failures ? 1 : 0;
}
