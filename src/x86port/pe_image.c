#include "pe_image.h"

#include <string.h>

#define PE_DOS_MAGIC 0x5a4du
#define PE_SIGNATURE 0x00004550u
#define PE_MACHINE_I386 0x014cu
#define PE_OPTIONAL_PE32 0x010bu
#define PE_SECTION_HEADER 40u
#define PE_IMPORT_DESCRIPTOR 20u
#define PE_DELAY_DESCRIPTOR 32u
#define PE_ORDINAL_FLAG 0x80000000u
#define PE_DELAY_RVA_BASED 1u
#define PE_RELOC_ABSOLUTE 0u
#define PE_RELOC_HIGHLOW 3u

const char *x86p_pe_status_name(X86pPeStatus status) {
  switch (status) {
  case kX86pPeOk:
    return "ok";
  case kX86pPeMalformed:
    return "malformed";
  case kX86pPeNotI386:
    return "not i386";
  case kX86pPeNotPe32:
    return "not PE32";
  case kX86pPeGuestMemory:
    return "guest memory refused";
  case kX86pPeNoRelocations:
    return "no relocations";
  case kX86pPeRelocationType:
    return "unsupported relocation type";
  case kX86pPeNotFound:
    return "not found";
  }
  return "unknown";
}

static uint16_t file16(const uint8_t *p, size_t at) {
  return (uint16_t)(p[at] | (p[at + 1] << 8));
}

static uint32_t file32(const uint8_t *p, size_t at) {
  return (uint32_t)p[at] | ((uint32_t)p[at + 1] << 8) | ((uint32_t)p[at + 2] << 16) | ((uint32_t)p[at + 3] << 24);
}

static int fits(size_t size, size_t offset, size_t length) {
  return offset <= size && size - offset >= length;
}

X86pPeStatus x86p_pe_parse(const uint8_t *file, size_t size, X86pPeHeaders *out) {
  memset(out, 0, sizeof *out);
  if (!fits(size, 0, 0x40) || file16(file, 0) != PE_DOS_MAGIC) {
    return kX86pPeMalformed;
  }
  const uint32_t pe = file32(file, 0x3c);
  if (!fits(size, pe, 24) || file32(file, pe) != PE_SIGNATURE) {
    return kX86pPeMalformed;
  }
  if (file16(file, pe + 4) != PE_MACHINE_I386) {
    return kX86pPeNotI386;
  }
  const uint32_t section_count = file16(file, pe + 6);
  const uint32_t optional_size = file16(file, pe + 20);
  const uint32_t optional = pe + 24;
  if (optional_size < 96 || !fits(size, optional, optional_size)) {
    return kX86pPeMalformed;
  }
  if (file16(file, optional) != PE_OPTIONAL_PE32) {
    return kX86pPeNotPe32;
  }
  out->entry_rva = file32(file, optional + 16);
  out->image_base = file32(file, optional + 28);
  out->size_of_image = file32(file, optional + 56);
  out->size_of_headers = file32(file, optional + 60);
  const uint32_t directory_count = file32(file, optional + 92);
  for (uint32_t i = 0; i < directory_count && i < kX86pPeDirCount; ++i) {
    const uint32_t entry = optional + 96 + i * 8;
    if (entry + 8 > optional + optional_size) {
      return kX86pPeMalformed;
    }
    out->directories[i].rva = file32(file, entry);
    out->directories[i].size = file32(file, entry + 4);
  }
  const uint32_t table = optional + optional_size;
  if (section_count > X86P_PE_MAX_SECTIONS || !fits(size, table, (size_t)section_count * PE_SECTION_HEADER)) {
    return kX86pPeMalformed;
  }
  for (uint32_t i = 0; i < section_count; ++i) {
    const uint32_t header = table + i * PE_SECTION_HEADER;
    X86pPeSection *section = &out->sections[i];
    memcpy(section->name, file + header, 8);
    section->name[8] = '\0';
    section->virtual_size = file32(file, header + 8);
    section->rva = file32(file, header + 12);
    section->raw_size = file32(file, header + 16);
    section->raw_offset = file32(file, header + 20);
    if (section->raw_size != 0 && !fits(size, section->raw_offset, section->raw_size)) {
      return kX86pPeMalformed;
    }
  }
  out->section_count = section_count;
  if (out->size_of_headers > size || out->size_of_headers > out->size_of_image) {
    return kX86pPeMalformed;
  }
  return kX86pPeOk;
}

/* A mapped image as the table walkers see it: every read is an RVA bounded by size_of_image. */
typedef struct Image {
  const X86pMem *mem;
  uint32_t base;
  uint32_t size;
} Image;

static int image_read(const Image *image, uint32_t rva, uint32_t width, uint32_t *value) {
  return rva <= image->size && image->size - rva >= width && x86p_mem_read(image->mem, image->base + rva, width, value);
}

static int rva32(const Image *image, uint32_t rva, uint32_t *value) {
  return image_read(image, rva, 4, value);
}

static int rva16(const Image *image, uint32_t rva, uint16_t *value) {
  uint32_t wide = 0;
  if (!image_read(image, rva, 2, &wide)) {
    return 0;
  }
  *value = (uint16_t)wide;
  return 1;
}

/* A NUL-terminated string inside the image; refuses an unterminated one. */
static int rva_string(const Image *image, uint32_t rva, char *out, size_t capacity) {
  for (size_t i = 0; i < capacity; ++i) {
    uint32_t byte = 0;
    if (!image_read(image, rva + (uint32_t)i, 1, &byte)) {
      return 0;
    }
    out[i] = (char)byte;
    if (byte == 0) {
      return 1;
    }
  }
  return 0;
}

static X86pPeStatus relocate(const Image *image, const X86pPeHeaders *headers) {
  const X86pPeDataDirectory relocs = headers->directories[kX86pPeDirBaseRelocation];
  if (relocs.size == 0) {
    return kX86pPeNoRelocations;
  }
  const uint32_t delta = image->base - headers->image_base;
  uint32_t block = relocs.rva;
  while (block + 8 <= relocs.rva + relocs.size) {
    uint32_t page = 0, block_size = 0;
    if (!rva32(image, block, &page) || !rva32(image, block + 4, &block_size) || block_size < 8) {
      return kX86pPeMalformed;
    }
    for (uint32_t entry = block + 8; entry + 2 <= block + block_size; entry += 2) {
      uint16_t word = 0;
      if (!rva16(image, entry, &word)) {
        return kX86pPeMalformed;
      }
      const uint32_t type = word >> 12;
      if (type == PE_RELOC_ABSOLUTE) {
        continue;
      }
      if (type != PE_RELOC_HIGHLOW) {
        return kX86pPeRelocationType;
      }
      const uint32_t target = page + (word & 0x0fffu);
      uint32_t value = 0;
      if (!rva32(image, target, &value)) {
        return kX86pPeMalformed;
      }
      if (!x86p_mem_write(image->mem, image->base + target, 4, value + delta)) {
        return kX86pPeGuestMemory;
      }
    }
    block += block_size;
  }
  return kX86pPeOk;
}

X86pPeStatus
x86p_pe_map(const X86pMem *mem, const uint8_t *file, size_t size, const X86pPeHeaders *headers, uint32_t base) {
  if (!x86p_mem_write_bytes(mem, base, file, headers->size_of_headers)) {
    return kX86pPeGuestMemory;
  }
  for (uint32_t i = 0; i < headers->section_count; ++i) {
    const X86pPeSection *section = &headers->sections[i];
    const uint32_t span = section->virtual_size == 0 ? section->raw_size : section->virtual_size;
    const uint32_t copied = section->raw_size < span ? section->raw_size : span;
    if (copied == 0) {
      continue;
    }
    if (!fits(size, section->raw_offset, copied) || section->rva > headers->size_of_image ||
        headers->size_of_image - section->rva < copied) {
      return kX86pPeMalformed;
    }
    if (!x86p_mem_write_bytes(mem, base + section->rva, file + section->raw_offset, copied)) {
      return kX86pPeGuestMemory;
    }
  }
  const Image image = {mem, base, headers->size_of_image};
  return base == headers->image_base ? kX86pPeOk : relocate(&image, headers);
}

typedef struct ThunkWalk {
  const Image *image;
  const char *dll;
  uint32_t names_rva;
  uint32_t slots_rva;
  int delay_loaded;
} ThunkWalk;

/* Returns kX86pPeOk with *stop set when the visitor ends the walk. */
static X86pPeStatus walk_thunks(const ThunkWalk *walk, X86pPeImportVisitor visit, void *user, int *stop) {
  char symbol[X86P_PE_MAX_NAME];
  for (uint32_t i = 0;; ++i) {
    uint32_t thunk = 0;
    if (!rva32(walk->image, walk->names_rva + i * 4, &thunk)) {
      return kX86pPeMalformed;
    }
    if (thunk == 0) {
      return kX86pPeOk;
    }
    X86pPeImport import = {.dll = walk->dll,
                           .symbol = NULL,
                           .ordinal = 0,
                           .slot = walk->image->base + walk->slots_rva + i * 4,
                           .delay_loaded = walk->delay_loaded};
    if ((thunk & PE_ORDINAL_FLAG) != 0) {
      import.ordinal = (uint16_t)(thunk & 0xffffu);
    } else {
      if (!rva16(walk->image, thunk, &import.ordinal) || !rva_string(walk->image, thunk + 2, symbol, sizeof symbol)) {
        return kX86pPeMalformed;
      }
      import.symbol = symbol;
    }
    if (!visit(&import, user)) {
      *stop = 1;
      return kX86pPeOk;
    }
  }
}

X86pPeStatus x86p_pe_imports(
    const X86pMem *mem, uint32_t base, const X86pPeHeaders *headers, X86pPeImportVisitor visit, void *user) {
  const Image image = {mem, base, headers->size_of_image};
  char dll[X86P_PE_MAX_NAME];
  int stop = 0;
  const X86pPeDataDirectory imports = headers->directories[kX86pPeDirImport];
  for (uint32_t descriptor = imports.rva; imports.size != 0 && !stop; descriptor += PE_IMPORT_DESCRIPTOR) {
    uint32_t names = 0, name = 0, slots = 0;
    if (!rva32(&image, descriptor, &names) || !rva32(&image, descriptor + 12, &name) ||
        !rva32(&image, descriptor + 16, &slots)) {
      return kX86pPeMalformed;
    }
    if (name == 0 && slots == 0) {
      break;
    }
    if (!rva_string(&image, name, dll, sizeof dll)) {
      return kX86pPeMalformed;
    }
    const ThunkWalk walk = {&image, dll, names != 0 ? names : slots, slots, 0};
    const X86pPeStatus status = walk_thunks(&walk, visit, user, &stop);
    if (status != kX86pPeOk) {
      return status;
    }
  }
  const X86pPeDataDirectory delayed = headers->directories[kX86pPeDirDelayImport];
  for (uint32_t descriptor = delayed.rva; delayed.size != 0 && !stop; descriptor += PE_DELAY_DESCRIPTOR) {
    uint32_t attributes = 0, name = 0, slots = 0, names = 0;
    if (!rva32(&image, descriptor, &attributes) || !rva32(&image, descriptor + 4, &name) ||
        !rva32(&image, descriptor + 12, &slots) || !rva32(&image, descriptor + 16, &names)) {
      return kX86pPeMalformed;
    }
    if (name == 0) {
      break;
    }
    /* Pre-VC7 descriptors hold VAs against the preferred base. */
    if ((attributes & PE_DELAY_RVA_BASED) == 0) {
      name -= headers->image_base;
      slots -= headers->image_base;
      names -= headers->image_base;
    }
    if (!rva_string(&image, name, dll, sizeof dll)) {
      return kX86pPeMalformed;
    }
    const ThunkWalk walk = {&image, dll, names, slots, 1};
    const X86pPeStatus status = walk_thunks(&walk, visit, user, &stop);
    if (status != kX86pPeOk) {
      return status;
    }
  }
  return kX86pPeOk;
}

typedef struct ExportTable {
  uint32_t ordinal_base;
  uint32_t function_count;
  uint32_t name_count;
  uint32_t functions;
  uint32_t names;
  uint32_t ordinals;
} ExportTable;

static X86pPeStatus export_table(const Image *image, const X86pPeHeaders *headers, ExportTable *table) {
  const X86pPeDataDirectory exports = headers->directories[kX86pPeDirExport];
  if (exports.size == 0) {
    return kX86pPeNotFound;
  }
  const uint32_t at = exports.rva;
  if (!rva32(image, at + 16, &table->ordinal_base) || !rva32(image, at + 20, &table->function_count) ||
      !rva32(image, at + 24, &table->name_count) || !rva32(image, at + 28, &table->functions) ||
      !rva32(image, at + 32, &table->names) || !rva32(image, at + 36, &table->ordinals)) {
    return kX86pPeMalformed;
  }
  return kX86pPeOk;
}

static X86pPeStatus export_at(
    const Image *image, const X86pPeHeaders *headers, const ExportTable *table, uint32_t index, X86pPeExport *out) {
  uint32_t rva = 0;
  memset(out, 0, sizeof *out);
  if (index >= table->function_count) {
    return kX86pPeNotFound;
  }
  if (!rva32(image, table->functions + index * 4, &rva)) {
    return kX86pPeMalformed;
  }
  if (rva == 0) {
    return kX86pPeNotFound;
  }
  const X86pPeDataDirectory exports = headers->directories[kX86pPeDirExport];
  if (rva >= exports.rva && rva - exports.rva < exports.size) {
    return rva_string(image, rva, out->forwarder, sizeof out->forwarder) ? kX86pPeOk : kX86pPeMalformed;
  }
  out->address = image->base + rva;
  return kX86pPeOk;
}

X86pPeStatus x86p_pe_export_by_name(
    const X86pMem *mem, uint32_t base, const X86pPeHeaders *headers, const char *symbol, X86pPeExport *out) {
  const Image image = {mem, base, headers->size_of_image};
  ExportTable table;
  char name[X86P_PE_MAX_NAME];
  X86pPeStatus status = export_table(&image, headers, &table);
  if (status != kX86pPeOk) {
    return status;
  }
  for (uint32_t i = 0; i < table.name_count; ++i) {
    uint32_t name_rva = 0;
    uint16_t index = 0;
    if (!rva32(&image, table.names + i * 4, &name_rva) || !rva16(&image, table.ordinals + i * 2, &index) ||
        !rva_string(&image, name_rva, name, sizeof name)) {
      return kX86pPeMalformed;
    }
    if (strcmp(name, symbol) == 0) {
      return export_at(&image, headers, &table, index, out);
    }
  }
  return kX86pPeNotFound;
}

X86pPeStatus x86p_pe_export_by_ordinal(
    const X86pMem *mem, uint32_t base, const X86pPeHeaders *headers, uint16_t ordinal, X86pPeExport *out) {
  const Image image = {mem, base, headers->size_of_image};
  ExportTable table;
  X86pPeStatus status = export_table(&image, headers, &table);
  if (status != kX86pPeOk) {
    return status;
  }
  if (ordinal < table.ordinal_base) {
    return kX86pPeNotFound;
  }
  return export_at(&image, headers, &table, ordinal - table.ordinal_base, out);
}
