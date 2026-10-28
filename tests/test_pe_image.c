#include "cpu.h"
#include "pe_image.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;

static void check(int value, const char *what) {
  ++checks;
  if (!value) {
    ++failures;
    printf("FAIL: %s\n", what);
  }
}

enum {
  kFileSize = 0x600,
  kImageSize = 0x2000,
  kPreferred = 0x00400000,
  kRaw = 0x200, /* file offset of the one section, mapped at RVA 0x1000 */
};

static uint8_t file[kFileSize];

static void put16(uint32_t at, uint32_t value) {
  file[at] = (uint8_t)value;
  file[at + 1] = (uint8_t)(value >> 8);
}

static void put32(uint32_t at, uint32_t value) {
  put16(at, value);
  put16(at + 2, value >> 16);
}

/* Section bytes are written by RVA. */
static void sec32(uint32_t rva, uint32_t value) {
  put32(kRaw + rva - 0x1000, value);
}

static void sec_text(uint32_t rva, const char *text) {
  memcpy(file + kRaw + rva - 0x1000, text, strlen(text) + 1);
}

static void build(int with_relocations) {
  memset(file, 0, sizeof file);
  put16(0, 0x5a4d);
  put32(0x3c, 0x80);
  put32(0x80, 0x00004550);
  put16(0x84, 0x014c);
  put16(0x86, 1);
  put16(0x94, 0xe0);
  const uint32_t opt = 0x98;
  put16(opt, 0x010b);
  put32(opt + 16, 0x1000);
  put32(opt + 28, kPreferred);
  put32(opt + 56, kImageSize);
  put32(opt + 60, 0x200);
  put32(opt + 92, 16);
  put32(opt + 96 + 0 * 8, 0x1100); /* export */
  put32(opt + 96 + 0 * 8 + 4, 0x80);
  put32(opt + 96 + 1 * 8, 0x1040); /* import */
  put32(opt + 96 + 1 * 8 + 4, 40);
  if (with_relocations) {
    put32(opt + 96 + 5 * 8, 0x1180);
    put32(opt + 96 + 5 * 8 + 4, 12);
  }
  put32(opt + 96 + 13 * 8, 0x1200); /* delay import */
  put32(opt + 96 + 13 * 8 + 4, 64);
  const uint32_t section = opt + 0xe0;
  memcpy(file + section, ".text", 5);
  put32(section + 8, 0x1000);
  put32(section + 12, 0x1000);
  put32(section + 16, 0x400);
  put32(section + 20, kRaw);

  sec32(0x1000, kPreferred + 0x1234); /* an absolute pointer the relocation fixes */

  sec32(0x1040, 0x1080); /* KERNEL32.dll: lookup table, name, IAT */
  sec32(0x104c, 0x10a0);
  sec32(0x1050, 0x1090);
  sec32(0x1080, 0x10b0);
  sec32(0x1084, 0x80000007u);
  sec32(0x1090, 0x10b0);
  sec32(0x1094, 0x80000007u);
  sec_text(0x10a0, "KERNEL32.dll");
  put16(kRaw + 0xb0, 0x11);
  sec_text(0x10b2, "Sleep");

  sec32(0x1110, 5); /* exports: ordinal base 5, two functions, two names */
  sec32(0x1114, 2);
  sec32(0x1118, 2);
  sec32(0x111c, 0x1140);
  sec32(0x1120, 0x1150);
  sec32(0x1124, 0x1158);
  sec32(0x1140, 0x1000);
  sec32(0x1144, 0x1170);
  sec32(0x1150, 0x1160);
  sec32(0x1154, 0x1166);
  put16(kRaw + 0x158, 0);
  put16(kRaw + 0x15a, 1);
  sec_text(0x1160, "Alpha");
  sec_text(0x1166, "Beta");
  sec_text(0x1170, "NTDLL.RtlBeta");

  sec32(0x1180, 0x1000); /* one HIGHLOW at RVA 0x1000, one ABSOLUTE pad */
  sec32(0x1184, 12);
  put16(kRaw + 0x188, 0x3000);

  sec32(0x1200, 1); /* PHYS.dll, RVA-based delay descriptor */
  sec32(0x1204, 0x1240);
  sec32(0x120c, 0x1250);
  sec32(0x1210, 0x1260);
  sec_text(0x1240, "PHYS.dll");
  sec32(0x1250, 0x80000003u);
  sec32(0x1260, 0x80000003u);
}

typedef struct Seen {
  unsigned count;
  char dll[3][16];
  char symbol[3][16];
  uint16_t ordinal[3];
  uint32_t slot[3];
  int delay[3];
} Seen;

static int record(const X86pPeImport *import, void *user) {
  Seen *seen = user;
  if (seen->count < 3) {
    snprintf(seen->dll[seen->count], sizeof seen->dll[0], "%s", import->dll);
    snprintf(seen->symbol[seen->count], sizeof seen->symbol[0], "%s", import->symbol ? import->symbol : "");
    seen->ordinal[seen->count] = import->ordinal;
    seen->slot[seen->count] = import->slot;
    seen->delay[seen->count] = import->delay_loaded;
  }
  ++seen->count;
  return 1;
}

static void map_and_walk(uint32_t base) {
  static uint8_t image[kImageSize];
  X86pMem mem = {.host = image, .lo = base, .size = kImageSize};
  X86pPeHeaders headers;
  X86pPeExport out;
  uint32_t value = 0;
  memset(image, 0, sizeof image);
  build(1);
  check(x86p_pe_parse(file, sizeof file, &headers) == kX86pPeOk, "parse a well-formed image");
  check(headers.image_base == kPreferred && headers.size_of_image == kImageSize && headers.section_count == 1,
        "header fields");
  check(strcmp(headers.sections[0].name, ".text") == 0, "section name");
  check(x86p_pe_map(&mem, file, sizeof file, &headers, base) == kX86pPeOk, "map");
  check(x86p_mem_read(&mem, base + 0x1000, 4, &value) && value == base + 0x1234, "absolute pointer follows the image");

  Seen seen = {0};
  check(x86p_pe_imports(&mem, base, &headers, record, &seen) == kX86pPeOk, "walk imports");
  check(seen.count == 3, "two ordinary and one delay-load import");
  check(strcmp(seen.dll[0], "KERNEL32.dll") == 0 && strcmp(seen.symbol[0], "Sleep") == 0 && seen.ordinal[0] == 0x11 &&
            seen.slot[0] == base + 0x1090 && !seen.delay[0],
        "named import with hint and slot");
  check(seen.symbol[1][0] == '\0' && seen.ordinal[1] == 7 && seen.slot[1] == base + 0x1094, "ordinal import");
  check(strcmp(seen.dll[2], "PHYS.dll") == 0 && seen.ordinal[2] == 3 && seen.slot[2] == base + 0x1250 && seen.delay[2],
        "delay-load import");

  check(x86p_pe_export_by_name(&mem, base, &headers, "Alpha", &out) == kX86pPeOk && out.address == base + 0x1000,
        "named export");
  check(x86p_pe_export_by_name(&mem, base, &headers, "Beta", &out) == kX86pPeOk && out.address == 0 &&
            strcmp(out.forwarder, "NTDLL.RtlBeta") == 0,
        "forwarded export");
  check(x86p_pe_export_by_ordinal(&mem, base, &headers, 5, &out) == kX86pPeOk && out.address == base + 0x1000,
        "export by ordinal");
  check(x86p_pe_export_by_name(&mem, base, &headers, "Gamma", &out) == kX86pPeNotFound, "missing export");
  check(x86p_pe_export_by_ordinal(&mem, base, &headers, 4, &out) == kX86pPeNotFound, "ordinal below base");
}

static void refusals(void) {
  static uint8_t image[kImageSize];
  X86pPeHeaders headers;
  build(1);
  check(x86p_pe_parse(file, 0x100, &headers) == kX86pPeMalformed, "truncated file");
  put16(0x84, 0x8664);
  check(x86p_pe_parse(file, sizeof file, &headers) == kX86pPeNotI386, "x64 machine");
  build(0);
  X86pMem mem = {.host = image, .lo = 0x10000000, .size = kImageSize};
  check(x86p_pe_parse(file, sizeof file, &headers) == kX86pPeOk, "parse without relocations");
  check(x86p_pe_map(&mem, file, sizeof file, &headers, 0x10000000) == kX86pPeNoRelocations,
        "moved image without relocations");
  X86pMem small = {.host = image, .lo = kPreferred, .size = 0x100};
  check(x86p_pe_map(&small, file, sizeof file, &headers, kPreferred) == kX86pPeGuestMemory,
        "image larger than guest memory");
}

int main(void) {
  map_and_walk(kPreferred);
  map_and_walk(0x10000000);
  refusals();
  printf("pe image: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
