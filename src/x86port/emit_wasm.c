/*
 * emit_wasm.c -- the WebAssembly binary format, written out.
 *
 * See emit_wasm.h for why this backend's encoder differs in shape from the
 * other two. This file is the alphabet only: no guest state, no register
 * allocation, no opinion about what a translated block should contain.
 */
#include "emit_wasm.h"

#include <string.h>

/*
 * The width a padded size slot always occupies.
 *
 * A u32 in LEB128 needs at most five bytes, and the format permits an encoding
 * padded out to that maximum. Reserving five and patching means a section's
 * contents are written straight into the output buffer instead of into a
 * scratch buffer that is measured and copied -- and a copy is exactly where a
 * size and its contents get to disagree.
 */
#define X86P_WASM_SIZE_WIDTH 5

static int fits(X86pWasmEmit *e, size_t n) {
  if (e->overflow) {
    return 0;
  }
  if (e->len + n > e->cap) {
    e->overflow = 1;
    return 0;
  }
  return 1;
}

void x86p_wasm_init(X86pWasmEmit *e, void *buf, size_t cap) {
  memset(e, 0, sizeof(*e));
  e->buf = (uint8_t *)buf;
  e->cap = cap;
}

int x86p_wasm_ok(const X86pWasmEmit *e) {
  return !e->overflow && e->sizes_opened == e->sizes_closed && e->blocks_opened == e->blocks_closed &&
         e->bodies_opened == e->bodies_closed;
}

int x86p_wasm_intact(const X86pWasmEmit *e) {
  return !e->overflow;
}

size_t x86p_wasm_here(const X86pWasmEmit *e) {
  return e->len;
}

void x86p_wasm_byte(X86pWasmEmit *e, uint8_t b) {
  if (!fits(e, 1)) {
    return;
  }
  e->buf[e->len++] = b;
}

void x86p_wasm_bytes(X86pWasmEmit *e, const void *p, size_t n) {
  if (!fits(e, n)) {
    return;
  }
  memcpy(e->buf + e->len, p, n);
  e->len += n;
}

void x86p_wasm_u32(X86pWasmEmit *e, uint32_t value) {
  do {
    uint8_t byte = (uint8_t)(value & 0x7Fu);
    value >>= 7;
    if (value != 0u) {
      byte |= 0x80u;
    }
    x86p_wasm_byte(e, byte);
  } while (value != 0u);
}

/*
 * Signed LEB128 terminates on the SIGN, not on the value reaching zero.
 *
 * A loop that stops when the value is 0 or -1 without checking that the last
 * byte's sign bit already says so encodes 0x40 (64) as one byte, which reads
 * back as -64. The `more` condition below is the whole correctness of this
 * function, and it is why the test sweeps the boundaries at every byte width
 * rather than sampling values.
 */
static void emit_sleb(X86pWasmEmit *e, int64_t value) {
  for (;;) {
    uint8_t byte = (uint8_t)(value & 0x7F);
    int sign_bit_set = (byte & 0x40u) != 0u;
    value >>= 7; /* arithmetic: the sign propagates */
    if ((value == 0 && !sign_bit_set) || (value == -1 && sign_bit_set)) {
      x86p_wasm_byte(e, byte);
      return;
    }
    x86p_wasm_byte(e, (uint8_t)(byte | 0x80u));
  }
}

void x86p_wasm_i32(X86pWasmEmit *e, int32_t value) {
  emit_sleb(e, value);
}

void x86p_wasm_i64(X86pWasmEmit *e, int64_t value) {
  emit_sleb(e, value);
}

void x86p_wasm_name(X86pWasmEmit *e, const char *name) {
  size_t length = strlen(name);
  x86p_wasm_u32(e, (uint32_t)length);
  x86p_wasm_bytes(e, name, length);
}

void x86p_wasm_module_begin(X86pWasmEmit *e) {
  static const uint8_t header[8] = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};
  x86p_wasm_bytes(e, header, sizeof(header));
}

/* Reserve a padded size slot at the current position. */
static X86pWasmSize open_size(X86pWasmEmit *e) {
  X86pWasmSize size;
  int i;
  size.at = e->len;
  e->sizes_opened++;
  for (i = 0; i < X86P_WASM_SIZE_WIDTH; i++) {
    x86p_wasm_byte(e, 0x00);
  }
  return size;
}

X86pWasmSize x86p_wasm_section_begin(X86pWasmEmit *e, X86pWasmSection id) {
  x86p_wasm_byte(e, (uint8_t)id);
  return open_size(e);
}

void x86p_wasm_size_end(X86pWasmEmit *e, X86pWasmSize size) {
  size_t contents_start = size.at + X86P_WASM_SIZE_WIDTH;
  uint64_t length;
  int i;
  e->sizes_closed++;
  if (e->overflow) {
    return;
  }
  /* A slot opened past the end of the buffer has no bytes to patch; the sticky
     overflow flag already condemns the module, so do not write outside it. */
  if (contents_start > e->len || contents_start > e->cap) {
    e->overflow = 1;
    return;
  }
  length = (uint64_t)(e->len - contents_start);
  if (length > 0xFFFFFFFFu) {
    e->overflow = 1;
    return;
  }
  for (i = 0; i < X86P_WASM_SIZE_WIDTH; i++) {
    uint8_t byte = (uint8_t)(length & 0x7Fu);
    length >>= 7;
    /* Every byte but the last keeps its continuation bit, which is what makes
       a short value legal in a five-byte slot. */
    if (i != X86P_WASM_SIZE_WIDTH - 1) {
      byte |= 0x80u;
    }
    e->buf[size.at + (size_t)i] = byte;
  }
}

void x86p_wasm_functype(
    X86pWasmEmit *e, const X86pWasmType *params, size_t param_count, const X86pWasmType *results, size_t result_count) {
  size_t i;
  x86p_wasm_byte(e, 0x60); /* the one function-type form */
  x86p_wasm_u32(e, (uint32_t)param_count);
  for (i = 0; i < param_count; i++) {
    x86p_wasm_byte(e, (uint8_t)params[i]);
  }
  x86p_wasm_u32(e, (uint32_t)result_count);
  for (i = 0; i < result_count; i++) {
    x86p_wasm_byte(e, (uint8_t)results[i]);
  }
}

void x86p_wasm_import_func(X86pWasmEmit *e, const char *module, const char *field, uint32_t type_index) {
  x86p_wasm_name(e, module);
  x86p_wasm_name(e, field);
  x86p_wasm_byte(e, (uint8_t)kWasmExternalFunc);
  x86p_wasm_u32(e, type_index);
}

/* Limits: a flag byte, a minimum, and a maximum only when the flag says so. */
static void emit_limits(X86pWasmEmit *e, uint32_t minimum, int has_maximum, uint32_t maximum) {
  x86p_wasm_byte(e, has_maximum ? 0x01u : 0x00u);
  x86p_wasm_u32(e, minimum);
  if (has_maximum) {
    x86p_wasm_u32(e, maximum);
  }
}

void x86p_wasm_import_memory(X86pWasmEmit *e,
                             const char *module,
                             const char *field,
                             uint32_t minimum_pages,
                             int has_maximum,
                             uint32_t maximum_pages) {
  x86p_wasm_name(e, module);
  x86p_wasm_name(e, field);
  x86p_wasm_byte(e, (uint8_t)kWasmExternalMemory);
  emit_limits(e, minimum_pages, has_maximum, maximum_pages);
}

void x86p_wasm_import_shared_memory(
    X86pWasmEmit *e, const char *module, const char *field, uint32_t minimum_pages, uint32_t maximum_pages) {
  x86p_wasm_name(e, module);
  x86p_wasm_name(e, field);
  x86p_wasm_byte(e, (uint8_t)kWasmExternalMemory);
  x86p_wasm_byte(e, 0x03u); /* shared memory, with required maximum */
  x86p_wasm_u32(e, minimum_pages);
  x86p_wasm_u32(e, maximum_pages);
}

void x86p_wasm_import_table(
    X86pWasmEmit *e, const char *module, const char *field, uint32_t minimum, int has_maximum, uint32_t maximum) {
  x86p_wasm_name(e, module);
  x86p_wasm_name(e, field);
  x86p_wasm_byte(e, (uint8_t)kWasmExternalTable);
  x86p_wasm_byte(e, 0x70); /* funcref: the only element type this needs */
  emit_limits(e, minimum, has_maximum, maximum);
}

void x86p_wasm_export_func(X86pWasmEmit *e, const char *field, uint32_t func_index) {
  x86p_wasm_name(e, field);
  x86p_wasm_byte(e, (uint8_t)kWasmExternalFunc);
  x86p_wasm_u32(e, func_index);
}

void x86p_wasm_body_begin(X86pWasmEmit *e, X86pWasmSize *body) {
  e->bodies_opened++;
  *body = open_size(e);
}

void x86p_wasm_locals(X86pWasmEmit *e, uint32_t group_count) {
  x86p_wasm_u32(e, group_count);
}

void x86p_wasm_local_group(X86pWasmEmit *e, uint32_t count, X86pWasmType type) {
  x86p_wasm_u32(e, count);
  x86p_wasm_byte(e, (uint8_t)type);
}

void x86p_wasm_body_end(X86pWasmEmit *e, X86pWasmSize body) {
  /* Every body ends with an explicit `end`, and it is emitted here rather than
     left to the caller: a body missing it is not a wrong program, it is a
     module the engine refuses to load, and forgetting it is invisible in the
     caller's source. */
  x86p_wasm_byte(e, 0x0B);
  e->bodies_closed++;
  x86p_wasm_size_end(e, body);
}

void x86p_wasm_i32_const(X86pWasmEmit *e, int32_t value) {
  x86p_wasm_byte(e, 0x41);
  x86p_wasm_i32(e, value);
}

void x86p_wasm_i64_const(X86pWasmEmit *e, int64_t value) {
  x86p_wasm_byte(e, 0x42);
  x86p_wasm_i64(e, value);
}

static void emit_index_op(X86pWasmEmit *e, uint8_t opcode, uint32_t index) {
  x86p_wasm_byte(e, opcode);
  x86p_wasm_u32(e, index);
}

void x86p_wasm_local_get(X86pWasmEmit *e, uint32_t index) {
  emit_index_op(e, 0x20, index);
}
void x86p_wasm_local_set(X86pWasmEmit *e, uint32_t index) {
  emit_index_op(e, 0x21, index);
}
void x86p_wasm_local_tee(X86pWasmEmit *e, uint32_t index) {
  emit_index_op(e, 0x22, index);
}
void x86p_wasm_global_get(X86pWasmEmit *e, uint32_t index) {
  emit_index_op(e, 0x23, index);
}
void x86p_wasm_global_set(X86pWasmEmit *e, uint32_t index) {
  emit_index_op(e, 0x24, index);
}

/* Every memory instruction carries the same two immediates, in this order. */
static void emit_mem_op(X86pWasmEmit *e, uint8_t opcode, uint32_t align, uint32_t offset) {
  x86p_wasm_byte(e, opcode);
  x86p_wasm_u32(e, align);
  x86p_wasm_u32(e, offset);
}

void x86p_wasm_i32_load(X86pWasmEmit *e, uint32_t align, uint32_t offset) {
  emit_mem_op(e, 0x28, align, offset);
}
void x86p_wasm_i64_load(X86pWasmEmit *e, uint32_t align, uint32_t offset) {
  emit_mem_op(e, 0x29, align, offset);
}
void x86p_wasm_i32_load8_s(X86pWasmEmit *e, uint32_t align, uint32_t offset) {
  emit_mem_op(e, 0x2C, align, offset);
}
void x86p_wasm_i32_load8_u(X86pWasmEmit *e, uint32_t align, uint32_t offset) {
  emit_mem_op(e, 0x2D, align, offset);
}
void x86p_wasm_i32_load16_s(X86pWasmEmit *e, uint32_t align, uint32_t offset) {
  emit_mem_op(e, 0x2E, align, offset);
}
void x86p_wasm_i32_load16_u(X86pWasmEmit *e, uint32_t align, uint32_t offset) {
  emit_mem_op(e, 0x2F, align, offset);
}
void x86p_wasm_i32_store(X86pWasmEmit *e, uint32_t align, uint32_t offset) {
  emit_mem_op(e, 0x36, align, offset);
}
void x86p_wasm_i64_store(X86pWasmEmit *e, uint32_t align, uint32_t offset) {
  emit_mem_op(e, 0x37, align, offset);
}
void x86p_wasm_i32_store8(X86pWasmEmit *e, uint32_t align, uint32_t offset) {
  emit_mem_op(e, 0x3A, align, offset);
}
void x86p_wasm_i32_store16(X86pWasmEmit *e, uint32_t align, uint32_t offset) {
  emit_mem_op(e, 0x3B, align, offset);
}

void x86p_wasm_i32_op(X86pWasmEmit *e, X86pWasmI32Op op) {
  x86p_wasm_byte(e, (uint8_t)op);
}

void x86p_wasm_i64_extend_i32_s(X86pWasmEmit *e) {
  x86p_wasm_byte(e, 0xAC);
}
void x86p_wasm_i64_extend_i32_u(X86pWasmEmit *e) {
  x86p_wasm_byte(e, 0xAD);
}
void x86p_wasm_i32_wrap_i64(X86pWasmEmit *e) {
  x86p_wasm_byte(e, 0xA7);
}
void x86p_wasm_i64_mul(X86pWasmEmit *e) {
  x86p_wasm_byte(e, 0x7E);
}
void x86p_wasm_i64_shr_u(X86pWasmEmit *e) {
  x86p_wasm_byte(e, 0x88);
}
void x86p_wasm_i64_and(X86pWasmEmit *e) {
  x86p_wasm_byte(e, 0x83);
}
void x86p_wasm_i64_or(X86pWasmEmit *e) {
  x86p_wasm_byte(e, 0x84);
}
void x86p_wasm_i64_shl(X86pWasmEmit *e) {
  x86p_wasm_byte(e, 0x86);
}
void x86p_wasm_i64_add(X86pWasmEmit *e) {
  x86p_wasm_byte(e, 0x7C);
}
void x86p_wasm_i64_eq(X86pWasmEmit *e) {
  x86p_wasm_byte(e, 0x51);
}
void x86p_wasm_f64_op(X86pWasmEmit *e, X86pWasmF64Op op) {
  x86p_wasm_byte(e, (uint8_t)op);
}
void x86p_wasm_f64_convert_i64_u(X86pWasmEmit *e) {
  x86p_wasm_byte(e, 0xBA);
}
void x86p_wasm_f64_promote_f32(X86pWasmEmit *e) {
  x86p_wasm_byte(e, 0xBB);
}
void x86p_wasm_i64_reinterpret_f64(X86pWasmEmit *e) {
  x86p_wasm_byte(e, 0xBD);
}
void x86p_wasm_f32_reinterpret_i32(X86pWasmEmit *e) {
  x86p_wasm_byte(e, 0xBE);
}
void x86p_wasm_f64_reinterpret_i64(X86pWasmEmit *e) {
  x86p_wasm_byte(e, 0xBF);
}
void x86p_wasm_i64_xor(X86pWasmEmit *e) {
  x86p_wasm_byte(e, 0x85);
}

void x86p_wasm_i64_const_shift(X86pWasmEmit *e, int64_t amount) {
  x86p_wasm_i64_const(e, amount);
  x86p_wasm_i64_shr_u(e);
}

/*
 * SIMD's prefix byte, then the opcode as a u32 LEB rather than a byte -- the
 * indices this backend uses run past 127, so a byte would encode a different
 * instruction. Every SIMD instruction below goes through here.
 */
enum { kWasmSimdPrefix = 0xFD };

static void emit_simd_op(X86pWasmEmit *e, uint32_t opcode) {
  x86p_wasm_byte(e, (uint8_t)kWasmSimdPrefix);
  x86p_wasm_u32(e, opcode);
}

void x86p_wasm_v128_op(X86pWasmEmit *e, X86pWasmSimdOp op) {
  emit_simd_op(e, (uint32_t)op);
}

void x86p_wasm_v128_load(X86pWasmEmit *e, uint32_t align, uint32_t offset) {
  emit_simd_op(e, 0u);
  x86p_wasm_u32(e, align);
  x86p_wasm_u32(e, offset);
}

void x86p_wasm_v128_store(X86pWasmEmit *e, uint32_t align, uint32_t offset) {
  emit_simd_op(e, 11u);
  x86p_wasm_u32(e, align);
  x86p_wasm_u32(e, offset);
}

void x86p_wasm_v128_shuffle(X86pWasmEmit *e, const uint8_t lanes[16]) {
  emit_simd_op(e, 13u);
  x86p_wasm_bytes(e, lanes, 16u);
}

static void open_block(X86pWasmEmit *e, uint8_t opcode, X86pWasmType type) {
  e->blocks_opened++;
  x86p_wasm_byte(e, opcode);
  x86p_wasm_byte(e, (uint8_t)type);
}

void x86p_wasm_block(X86pWasmEmit *e, X86pWasmType type) {
  open_block(e, 0x02, type);
}
void x86p_wasm_loop(X86pWasmEmit *e, X86pWasmType type) {
  open_block(e, 0x03, type);
}
void x86p_wasm_if(X86pWasmEmit *e, X86pWasmType type) {
  open_block(e, 0x04, type);
}

/* `else` neither opens nor closes a region: it divides the one `if` opened. */
void x86p_wasm_else(X86pWasmEmit *e) {
  x86p_wasm_byte(e, 0x05);
}

void x86p_wasm_end(X86pWasmEmit *e) {
  e->blocks_closed++;
  x86p_wasm_byte(e, 0x0B);
}

void x86p_wasm_br(X86pWasmEmit *e, uint32_t depth) {
  emit_index_op(e, 0x0C, depth);
}
void x86p_wasm_br_if(X86pWasmEmit *e, uint32_t depth) {
  emit_index_op(e, 0x0D, depth);
}
void x86p_wasm_return(X86pWasmEmit *e) {
  x86p_wasm_byte(e, 0x0F);
}
void x86p_wasm_drop(X86pWasmEmit *e) {
  x86p_wasm_byte(e, 0x1A);
}
void x86p_wasm_select(X86pWasmEmit *e) {
  x86p_wasm_byte(e, 0x1B);
}
void x86p_wasm_unreachable(X86pWasmEmit *e) {
  x86p_wasm_byte(e, 0x00);
}

void x86p_wasm_call(X86pWasmEmit *e, uint32_t func_index) {
  emit_index_op(e, 0x10, func_index);
}

void x86p_wasm_return_call(X86pWasmEmit *e, uint32_t func_index) {
  emit_index_op(e, 0x12, func_index);
}

static void emit_indirect(X86pWasmEmit *e, uint8_t opcode, uint32_t type_index) {
  x86p_wasm_byte(e, opcode);
  x86p_wasm_u32(e, type_index);
  x86p_wasm_u32(e, 0u); /* table 0 */
}

void x86p_wasm_call_indirect(X86pWasmEmit *e, uint32_t type_index) {
  emit_indirect(e, 0x11, type_index);
}

void x86p_wasm_return_call_indirect(X86pWasmEmit *e, uint32_t type_index) {
  emit_indirect(e, 0x13, type_index);
}
