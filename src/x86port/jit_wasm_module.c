/*
 * jit_wasm_module.c -- the module builder. See jit_wasm_module.h for why a
 * translated block on this host is a module rather than a run of bytes.
 */
#include "jit_wasm_module.h"
#include "jit_wasm_imports.h"

#include <string.h>

/*
 * The export names, written out rather than formed at run time.
 *
 * Spelling them is longer than a two-digit formatter and is the reason the
 * formatter is not here: these strings are a WIRE FORMAT shared with whatever
 * instantiates the module, and a literal table cannot drift from itself the
 * way two copies of a formatting rule can.
 */
static const char *const kBodyNames[X86P_WASM_MAX_BODIES] = {
    "b0",  "b1",  "b2",  "b3",  "b4",  "b5",  "b6",  "b7",  "b8",  "b9",  "b10", "b11", "b12", "b13", "b14", "b15",
    "b16", "b17", "b18", "b19", "b20", "b21", "b22", "b23", "b24", "b25", "b26", "b27", "b28", "b29", "b30", "b31",
    "b32", "b33", "b34", "b35", "b36", "b37", "b38", "b39", "b40", "b41", "b42", "b43", "b44", "b45", "b46", "b47",
    "b48", "b49", "b50", "b51", "b52", "b53", "b54", "b55", "b56", "b57", "b58", "b59", "b60", "b61", "b62", "b63",
};

const char *x86p_wasm_body_name(unsigned index) {
  return index < X86P_WASM_MAX_BODIES ? kBodyNames[index] : NULL;
}

static void write_types(X86pWasmEmit *e) {
  static const X86pWasmType integers[8] = {
      kWasmI32, kWasmI32, kWasmI32, kWasmI32, kWasmI32, kWasmI32, kWasmI32, kWasmI32};
  X86pWasmSize section = x86p_wasm_section_begin(e, kWasmSectionType);
  x86p_wasm_u32(e, 12u);
  for (unsigned params = 1; params <= 8; ++params) {
    x86p_wasm_functype(e, integers, params, integers, 1);
  }
  for (unsigned params = 1; params <= 4; ++params) {
    x86p_wasm_functype(e, integers, params, NULL, 0);
  }
  x86p_wasm_size_end(e, section);
}

static void write_imports(X86pWasmEmit *e) {
  X86pWasmSize section = x86p_wasm_section_begin(e, kWasmSectionImport);
  unsigned i;
  /* The memory is the one extra entry beyond the functions. */
  x86p_wasm_u32(e, (uint32_t)kX86pWasmImportCount + 1u);
  for (i = 0; i < (unsigned)kX86pWasmImportCount; i++) {
    x86p_wasm_import_func(
        e, "env", x86p_wasm_import_field((X86pWasmImport)i), x86p_wasm_import_type((X86pWasmImport)i));
  }
  /*
   * One page minimum and no maximum. The minimum is a floor the instantiating
   * memory must meet, not a request: the block's own addresses are checked
   * against the mapping the block was translated for, which is a far narrower
   * claim than "the memory is at least this big".
   */
#if defined(__EMSCRIPTEN_PTHREADS__)
  /* The imported maximum is an upper bound, so this accepts the product's
   * configured memory ceiling anywhere within wasm32's 65,536 pages. */
  x86p_wasm_import_shared_memory(e, X86P_WASM_MEMORY_MODULE, X86P_WASM_MEMORY_FIELD, 1u, 65536u);
#else
  x86p_wasm_import_memory(e, X86P_WASM_MEMORY_MODULE, X86P_WASM_MEMORY_FIELD, 1u, 0, 0u);
#endif
  x86p_wasm_size_end(e, section);
}

static void write_functions(X86pWasmModule *m) {
  X86pWasmSize section = x86p_wasm_section_begin(&m->e, kWasmSectionFunction);
  unsigned i;
  x86p_wasm_u32(&m->e, m->promised);
  for (i = 0; i < m->promised; i++) {
    x86p_wasm_u32(&m->e, X86P_WASM_BLOCK_TYPE);
  }
  x86p_wasm_size_end(&m->e, section);
}

static void write_exports(X86pWasmModule *m) {
  X86pWasmSize section = x86p_wasm_section_begin(&m->e, kWasmSectionExport);
  unsigned i;
  x86p_wasm_u32(&m->e, m->promised);
  for (i = 0; i < m->promised; i++) {
    x86p_wasm_export_func(&m->e, kBodyNames[i], (uint32_t)kX86pWasmImportCount + i);
  }
  x86p_wasm_size_end(&m->e, section);
}

void x86p_wasm_module_init(X86pWasmModule *m, void *buf, size_t cap, unsigned functions) {
  if (!m) {
    return;
  }
  memset(m, 0, sizeof *m);
  x86p_wasm_init(&m->e, buf, cap);
  if (functions == 0u || functions > X86P_WASM_MAX_BODIES) {
    /*
     * Poison the emitter rather than clamping. A module promising a different
     * number of bodies than the caller believes it asked for would validate
     * and export the wrong function under the right name.
     */
    m->e.overflow = 1;
    return;
  }
  m->promised = functions;
  x86p_wasm_module_begin(&m->e);
  write_types(&m->e);
  write_imports(&m->e);
  write_functions(m);
  write_exports(m);
  m->code_section = x86p_wasm_section_begin(&m->e, kWasmSectionCode);
  x86p_wasm_u32(&m->e, m->promised);
}

const char *x86p_wasm_module_export_name(const X86pWasmModule *m, unsigned index) {
  if (!m || index >= m->promised) {
    return NULL;
  }
  return x86p_wasm_body_name(index);
}

X86pWasmEmit *x86p_wasm_module_emitter(X86pWasmModule *m) {
  return m ? &m->e : NULL;
}

int x86p_wasm_module_body_begin(X86pWasmModule *m) {
  X86pWasmSize body;
  if (!m || m->open || m->finished || m->written >= m->promised || !x86p_wasm_intact(&m->e)) {
    return -1;
  }
  x86p_wasm_body_begin(&m->e, &body);
  /*
   * The body's size slot is held in the module rather than handed back to the
   * caller, because a caller holding it could close bodies out of order. The
   * encoder counts opened and closed regions, so an unclosed one already
   * refuses -- this makes mismatched ORDER impossible as well.
   */
  m->body = body;
  x86p_wasm_locals(&m->e, 1);
  x86p_wasm_local_group(&m->e, (uint32_t)kX86pWasmLocalCount - 1u, kWasmI32);
  m->open = 1;
  return (int)m->written;
}

void x86p_wasm_module_body_end(X86pWasmModule *m) {
  if (!m || !m->open) {
    return;
  }
  x86p_wasm_body_end(&m->e, m->body);
  m->open = 0;
  m->written++;
}

size_t x86p_wasm_module_finish(X86pWasmModule *m) {
  if (!m || m->open || m->finished) {
    return 0;
  }
  m->finished = 1;
  if (m->written != m->promised) {
    /* Fewer bodies than the function and export sections declared. The engine
       would reject this, but with a message about a section length rather than
       about the lowering that stopped early. */
    return 0;
  }
  x86p_wasm_size_end(&m->e, m->code_section);
  if (!x86p_wasm_ok(&m->e)) {
    return 0;
  }
  return m->e.len;
}
