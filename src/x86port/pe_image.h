/* PE32 (i386) images: header parsing, placement into guest memory, base
 * relocation, and the import, delay-import and export tables read back from
 * guest memory. Which range an image gets and which modules are loaded belong
 * to the consumer; this module only moves bytes through X86pMem. */
#ifndef X86PORT_PE_IMAGE_H
#define X86PORT_PE_IMAGE_H

#include "cpu.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The PE/COFF section limit. */
#define X86P_PE_MAX_SECTIONS 96
#define X86P_PE_MAX_NAME 256

typedef enum X86pPeStatus {
  kX86pPeOk = 0,
  kX86pPeMalformed,      /* a header, table or string runs past its bounds */
  kX86pPeNotI386,        /* machine field is not i386 */
  kX86pPeNotPe32,        /* optional header is not PE32 */
  kX86pPeGuestMemory,    /* a guest read or write was refused */
  kX86pPeNoRelocations,  /* placed away from its preferred base without relocations */
  kX86pPeRelocationType, /* a base relocation other than ABSOLUTE or HIGHLOW */
  kX86pPeNotFound,       /* export lookup found nothing */
} X86pPeStatus;

const char *x86p_pe_status_name(X86pPeStatus status);

typedef enum X86pPeDirectory {
  kX86pPeDirExport = 0,
  kX86pPeDirImport = 1,
  kX86pPeDirBaseRelocation = 5,
  kX86pPeDirDelayImport = 13,
  kX86pPeDirCount = 16,
} X86pPeDirectory;

typedef struct X86pPeDataDirectory {
  uint32_t rva;
  uint32_t size;
} X86pPeDataDirectory;

typedef struct X86pPeSection {
  char name[9];
  uint32_t rva;
  uint32_t virtual_size;
  uint32_t raw_offset;
  uint32_t raw_size;
} X86pPeSection;

typedef struct X86pPeHeaders {
  uint32_t image_base;
  uint32_t size_of_image;
  uint32_t size_of_headers;
  uint32_t entry_rva;
  X86pPeDataDirectory directories[kX86pPeDirCount];
  uint32_t section_count;
  X86pPeSection sections[X86P_PE_MAX_SECTIONS];
} X86pPeHeaders;

/* Parse and bounds-check the headers of a PE32 file held in host memory. */
X86pPeStatus x86p_pe_parse(const uint8_t *file, size_t size, X86pPeHeaders *out);

/* Copy headers and sections to `base` (which the consumer has made writable
 * for size_of_image bytes) and apply base relocations when base differs from
 * the preferred image base. */
X86pPeStatus
x86p_pe_map(const X86pMem *mem, const uint8_t *file, size_t size, const X86pPeHeaders *headers, uint32_t base);

typedef struct X86pPeImport {
  const char *dll;
  const char *symbol; /* NULL when imported by ordinal */
  uint16_t ordinal;   /* the ordinal, or the hint of a named import */
  uint32_t slot;      /* guest address of the IAT entry */
  int delay_loaded;
} X86pPeImport;

/* Return nonzero to continue the walk. */
typedef int (*X86pPeImportVisitor)(const X86pPeImport *import, void *user);

/* Visit every ordinary import, then every delay-load import, of an image
 * mapped at `base`. Strings are valid only during the call. */
X86pPeStatus
x86p_pe_imports(const X86pMem *mem, uint32_t base, const X86pPeHeaders *headers, X86pPeImportVisitor visit, void *user);

typedef struct X86pPeExport {
  uint32_t address;                 /* zero for a forwarder */
  char forwarder[X86P_PE_MAX_NAME]; /* "DLL.Symbol" or "DLL.#ordinal", else empty */
} X86pPeExport;

X86pPeStatus x86p_pe_export_by_name(
    const X86pMem *mem, uint32_t base, const X86pPeHeaders *headers, const char *symbol, X86pPeExport *out);
X86pPeStatus x86p_pe_export_by_ordinal(
    const X86pMem *mem, uint32_t base, const X86pPeHeaders *headers, uint16_t ordinal, X86pPeExport *out);

#ifdef __cplusplus
}
#endif

#endif
