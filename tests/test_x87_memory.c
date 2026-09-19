/*
 * x87 memory operands, over fixtures that make the two paths DIFFERENT paths.
 *
 * x86p_mem_read_bytes reads the whole range in place when it is one span and
 * copies span by span when it is not, so an x87 operand takes one path or the
 * other depending only on where it lies. That is an optimisation only if both
 * produce the same value, and the only way to know is a fixture where each is
 * genuinely taken.
 *
 * Finding one is the whole difficulty, and the first attempt at this file was
 * wrong: on contiguous memory a span does NOT stop where the permission byte
 * merely differs, only where the requested access is not granted -- and there
 * the read would be refused outright. So an operand straddling a read-write
 * page and a read-only one is one span, and a test built on that discriminates
 * nothing. It passed with the span-by-span path compiled out.
 *
 * The case that does take it is SPARSE memory: two adjacent guest ranges from
 * SEPARATE host allocations. Both are readable, so the span walker crosses
 * them; there is no single host pointer that spans both, so the whole-range
 * resolution declines. The checks below assert that premise in both
 * directions rather than believing it.
 *
 * The falsification for the span-by-span path itself lives in
 * test_memory_sparse, which is where that path now is: compiling it out fails
 * 313 of its checks.
 */
#include "cpu.h"
#include "memory_sparse.h"
#include "x87_memory.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { kPageShift = 12, kPage = 1u << kPageShift, kPages = 4, kLo = 0x20000000u };

static unsigned checks, failures;

static void check(int value, const char *message) {
  ++checks;
  if (!value) {
    ++failures;
    printf("FAIL: %s\n", message);
  }
}

/* The guest's own byte order, written by hand rather than by memcpy, so the
   fixture cannot agree with the reader merely by sharing its bug. */
static void put_le(uint8_t *at, uint64_t bits, unsigned width) {
  unsigned i;
  for (i = 0; i < width; i++) {
    at[i] = (uint8_t)(bits >> (8u * i));
  }
}

/* 2.5 in each format the guest can load. Exact in all of them, so a wrong
   conversion cannot hide behind a tolerance. */
static const uint64_t kF64 = 0x4004000000000000ull;
static const uint32_t kF32 = 0x40200000u;

static void contiguous_cases(void) {
  static uint8_t bytes[kPages * kPage];
  uint8_t perms[kPages];
  X86pMem memory = {0};
  long double got = 0.0L;

  memory.host = bytes;
  memory.lo = kLo;
  memory.size = sizeof bytes;
  memory.perms = perms;
  memory.page_shift = kPageShift;

  perms[0] = kX86pMemRead | kX86pMemWrite;
  perms[1] = kX86pMemRead;
  perms[2] = 0u; /* not mapped at all */
  perms[3] = kX86pMemRead | kX86pMemWrite;

  put_le(bytes, kF64, 8);
  check(x86p_x87_read_value(&memory, kLo, 8, 0, &got) == kX86pX87MemoryOk, "a double inside one page reads");
  check(got == 2.5L, "the double is 2.5");

  got = 0.0L;
  put_le(bytes, kF32, 4);
  check(x86p_x87_read_value(&memory, kLo, 4, 0, &got) == kX86pX87MemoryOk, "a float reads");
  check(got == 2.5L, "the float is 2.5");

  /* An integer operand is a third conversion over the same two paths. */
  got = 0.0L;
  put_le(bytes, 0xFFFFFFFFu, 4);
  check(x86p_x87_read_value(&memory, kLo, 4, 1, &got) == kX86pX87MemoryOk, "an integer reads");
  check(got == -1.0L, "FILD read -1, so the integer path is signed");

  /* Crossing a permission change that still grants read is ONE span here --
     recorded because the earlier version of this file assumed the opposite. */
  got = 0.0L;
  put_le(bytes + kPage - 4u, kF64, 8);
  check(x86p_x87_read_value(&memory, kLo + kPage - 4u, 8, 0, &got) == kX86pX87MemoryOk,
        "a double across a read-write page and a read-only one reads");
  check(got == 2.5L, "and it is 2.5");

  /* The refusals still refuse, on both sides of the unmapped page and inside
     it, and none of them may quietly hand back a zero. */
  got = 7.0L;
  check(x86p_x87_read_value(&memory, kLo + 2u * kPage - 4u, 8, 0, &got) == kX86pX87MemoryFault,
        "a double running into an unmapped page faults");
  check(got == 7.0L, "the faulting read left the destination alone");
  check(x86p_x87_read_value(&memory, kLo + 2u * kPage, 8, 0, &got) == kX86pX87MemoryFault,
        "a double wholly inside an unmapped page faults");
  check(x86p_x87_read_value(&memory, kLo + 3u * kPage - 4u, 8, 0, &got) == kX86pX87MemoryFault,
        "a double running out of an unmapped page faults");

  /* An unsupported width is a different answer from a fault, and callers
     switch on which. */
  check(x86p_x87_read_value(&memory, kLo, 6, 0, &got) == kX86pX87MemoryUnsupported, "6 bytes is not an x87 operand");
  check(x86p_x87_read_value(&memory, kLo, 10, 1, &got) == kX86pX87MemoryUnsupported, "there is no 10-byte integer");

  /* With no permission table -- the desktop's shape -- everything resolves. */
  memory.perms = NULL;
  memory.page_shift = 0u;
  got = 0.0L;
  check(x86p_x87_read_value(&memory, kLo + kPage - 4u, 8, 0, &got) == kX86pX87MemoryOk,
        "no permission table: the read works");
  check(got == 2.5L, "no permission table: it is still 2.5");
}

/* The discriminating fixture. Two adjacent guest ranges, two host
   allocations, both readable. */
static void split_allocation_cases(void) {
  static uint8_t low[16], high[16];
  X86pSparseMem *sparse = x86p_sparse_create();
  X86pMem memory = {0};
  const uint32_t straddle = kLo + (uint32_t)sizeof low - 4u;
  uint8_t *host = NULL;
  long double got = 0.0L;

  check(sparse != NULL, "sparse owner created");
  if (!sparse) {
    return;
  }
  memory.sparse = sparse;
  check(x86p_sparse_map(sparse, kLo, low, sizeof low), "low range mapped");
  check(x86p_sparse_map(sparse, kLo + (uint32_t)sizeof low, high, sizeof high), "adjacent high range mapped");

  /* State the fixture's premise as a check rather than believing it: the
     operand must be readable byte by byte and NOT resolvable in one span. */
  check(x86p_mem_accessible(&memory, straddle, 8, kX86pMemRead), "the straddling operand is readable");
  check(!x86p_mem_resolve(&memory, straddle, 8, &host), "the straddling operand is not one host span");

  put_le(low + sizeof low - 4u, kF64 & 0xFFFFFFFFu, 4);
  put_le(high, kF64 >> 32, 4);
  check(x86p_x87_read_value(&memory, straddle, 8, 0, &got) == kX86pX87MemoryOk,
        "a double across two host allocations reads");
  check(got == 2.5L, "the copying path read 2.5, the same value as the mapped path");

  /* And the operand wholly inside one allocation still takes the mapped path
     and agrees, so the two are compared on the same fixture. */
  got = 0.0L;
  put_le(high, kF64, 8);
  check(x86p_mem_resolve(&memory, kLo + (uint32_t)sizeof low, 8, &host), "the contained operand IS one host span");
  check(x86p_x87_read_value(&memory, kLo + (uint32_t)sizeof low, 8, 0, &got) == kX86pX87MemoryOk,
        "a double inside one allocation reads");
  check(got == 2.5L, "the mapped path read 2.5");

  /* A ten-byte operand crossing the same seam, because the f80 branch reads
     its bytes through a different call than the shift-or loop does. */
  /* The operand starts five bytes from the end of `low`, so its bytes 0-4 are
     in `low` and its bytes 5-9 are in `high`. Mantissa byte 7 and the
     sign/exponent word therefore land in `high`. */
  memset(low + sizeof low - 5u, 0, 5u);
  memset(high, 0, 5u);
  high[2] = 0xA0;                /* mantissa 0xA000...0, which is 1.25 */
  put_le(high + 3u, 0x3FFFu, 2); /* exponent for 2^0, sign clear */
  got = 0.0L;
  check(x86p_x87_read_value(&memory, kLo + (uint32_t)sizeof low - 5u, 10, 0, &got) == kX86pX87MemoryOk,
        "an 80-bit operand across two host allocations reads");
  check(got == 1.25L, "the 80-bit copying path read 1.25");

  x86p_sparse_destroy(sparse);
}

/*
 * The operand as bits, which is how a backend that loaded it itself arrives.
 *
 * This is not the memory path with a layer removed -- nothing here touches
 * guest memory -- so it is checked against hand-written literals rather than
 * against the reader that shares the same conversion table. What it is for is
 * the decision the table makes: WHICH conversion a width and an integer flag
 * select, and which widths have no conversion at all.
 */
static void operand_bits_cases(void) {
  X86pX87Reg reg;
  X86pX87Reg narrow;

  check(x86p_x87_reg_from_operand_bits(kF64, 8, 0, &reg) == kX86pX87MemoryOk, "eight bytes of binary64 convert");
  check(x86p_x87_reg_to_long_double(reg) == 2.5L, "and they are 2.5, little-endian");
  check(x86p_x87_reg_from_operand_bits(kF32, 4, 0, &narrow) == kX86pX87MemoryOk, "four bytes of binary32 convert");
  check(x86p_x87_reg_to_long_double(narrow) == 2.5L, "and they are 2.5 too");

  /* The discriminator for the width itself: the SAME bits under the two float
     widths must not agree, or the width is being ignored. 0x40200000 is 2.5 as
     a binary32 and a very small binary64. */
  check(x86p_x87_reg_from_operand_bits(kF32, 8, 0, &reg) == kX86pX87MemoryOk, "the same bits convert at width 8");
  check(x86p_x87_reg_to_long_double(reg) != 2.5L, "width 8 did not read them as a float");

  check(x86p_x87_reg_from_operand_bits(0xFFFFFFFFull, 4, 1, &reg) == kX86pX87MemoryOk, "a 32-bit integer converts");
  check(x86p_x87_reg_to_long_double(reg) == -1.0L, "FILD read -1, so the integer path is signed");
  check(x86p_x87_reg_from_operand_bits(0xFFFFull, 2, 1, &reg) == kX86pX87MemoryOk, "a 16-bit integer converts");
  check(x86p_x87_reg_to_long_double(reg) == -1.0L, "and -1 at 16 bits too, not 65535");
  /* The same bits as a 32-bit integer, to prove the integer width is read. */
  check(x86p_x87_reg_from_operand_bits(0xFFFFull, 4, 1, &reg) == kX86pX87MemoryOk, "0xFFFF converts at width 4");
  check(x86p_x87_reg_to_long_double(reg) == 65535.0L, "width 4 did not sign-extend from bit 15");

  /* The refusals, including the one that separates this entry point from the
     memory reader: ten bytes are an operand there and not bits here. */
  reg = narrow;
  check(x86p_x87_reg_from_operand_bits(0, 10, 0, &reg) == kX86pX87MemoryUnsupported, "an 80-bit operand is not bits");
  check(x86p_x87_reg_from_operand_bits(0, 6, 0, &reg) == kX86pX87MemoryUnsupported, "6 bytes is not an x87 operand");
  check(x86p_x87_reg_from_operand_bits(0, 2, 0, &reg) == kX86pX87MemoryUnsupported, "there is no 16-bit float");
  check(x86p_x87_reg_from_operand_bits(0, 10, 1, &reg) == kX86pX87MemoryUnsupported, "there is no 10-byte integer");
  check(x86p_x87_reg_from_operand_bits(0, 0, 0, &reg) == kX86pX87MemoryUnsupported, "zero bytes is not an operand");
  check(x86p_x87_reg_from_operand_bits(kF64, 8, 0, NULL) == kX86pX87MemoryUnsupported, "no destination is refused");
  check(x86p_x87_reg_to_long_double(reg) == 2.5L, "and every refusal left the destination alone");
}

int main(void) {
  contiguous_cases();
  split_allocation_cases();
  operand_bits_cases();
  printf("x87 memory operands: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
