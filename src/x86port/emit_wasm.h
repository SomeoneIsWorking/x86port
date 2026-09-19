/*
 * emit_wasm.h -- writing a WebAssembly module, byte by byte.
 *
 * The bottom half of the third JIT backend, and the counterpart of
 * emit_x64.h and emit_arm64.h: this file knows nothing about the guest, only
 * how to encode host instructions. Guest meaning lives in jit_wasm.c, so the
 * encoder can be tested by a real WebAssembly engine without a guest program
 * existing.
 *
 * WHY A THIRD BACKEND AT ALL. A browser host cannot execute the other two.
 * There is no operation in WebAssembly that transfers control to a buffer of
 * bytes, which is what both existing backends produce -- the compiler says so
 * outright, refusing jit-common's code region with "llvm.clear_cache is not
 * supported on wasm". Translated code on this host is not a byte buffer that
 * is jumped to; it is a MODULE that is handed to the engine, compiled by it,
 * and called through a function reference. That difference is the whole reason
 * this encoder looks unlike the other two.
 *
 * WHAT IS DIFFERENT, AND WHY IT SHAPES THIS INTERFACE:
 *
 *   - THERE ARE NO REGISTERS. A wasm function has an operand stack and
 *     numbered locals, so there is no register allocation and none of the
 *     REX/ModRM/SIB traps emit_x64.h enumerates. The traps here are structural
 *     instead, and the two below are the ones that produce a module an engine
 *     rejects wholesale rather than a subtly wrong instruction.
 *
 *   - EVERY SECTION AND EVERY FUNCTION BODY IS LENGTH-PREFIXED, and the length
 *     is not known until the contents are written. Rather than build each
 *     section in its own buffer and copy, every size here is written as a
 *     five-byte padded LEB128 and patched in place afterwards. Five bytes is
 *     the maximum width the format allows for a u32, so a padded encoding is
 *     legal and no engine has to be persuaded to accept it. The patch is
 *     issued by value and closed by name, exactly as emit_x64.h issues and
 *     binds a jump site, and for the same reason: an unclosed size is a
 *     section header claiming a length that was never written.
 *
 *   - CONTROL FLOW IS STRUCTURED AND MUST BALANCE. `block`, `loop` and `if`
 *     each open a region that `end` closes, and a body whose regions do not
 *     balance is not a subtly wrong program -- it fails validation and nothing
 *     runs. So opens and closes are counted the way jump sites are counted,
 *     and x86p_wasm_ok() refuses a module with an open region for the same
 *     reason its x64 counterpart refuses an unbound jump.
 *
 * THE SAME OVERFLOW DISCIPLINE AS THE OTHER TWO ENCODERS: every emit checks
 * capacity first, a failed emit sets a sticky flag, and the caller checks once
 * at the end. A module that overflowed is DISCARDED, never handed to an engine
 * truncated -- a truncated module is a validation error at best, and at worst
 * a shorter module that validates and computes something else.
 *
 * The test does not read these bytes back and agree with itself. It hands each
 * module to a real WebAssembly engine, which validates it against the spec and
 * runs it, and compares the value that comes out. The oracle is an independent
 * implementation of the format, not this file's opinion of it.
 */
#ifndef X86PORT_EMIT_WASM_H
#define X86PORT_EMIT_WASM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The value types, spelled as the binary format spells them. The number IS the
 * encoding, so this cannot drift from the specification.
 */
typedef enum X86pWasmType {
  kWasmI32 = 0x7F,
  kWasmI64 = 0x7E,
  kWasmF32 = 0x7D,
  kWasmF64 = 0x7C,
  kWasmV128 = 0x7B, /* the fixed-width SIMD proposal's one value type */
  kWasmVoid = 0x40  /* the empty block type; not a value type */
} X86pWasmType;

/*
 * Section ids, in the order a module must present them. A module whose
 * sections are out of order is rejected, so the order below is the order the
 * caller writes them in.
 */
typedef enum X86pWasmSection {
  kWasmSectionType = 1,
  kWasmSectionImport = 2,
  kWasmSectionFunction = 3,
  kWasmSectionTable = 4,
  kWasmSectionMemory = 5,
  kWasmSectionGlobal = 6,
  kWasmSectionExport = 7,
  kWasmSectionStart = 8,
  kWasmSectionElement = 9,
  kWasmSectionCode = 10,
  kWasmSectionData = 11
} X86pWasmSection;

/* What an import or export names. Again, the number is the encoding. */
typedef enum X86pWasmExternal {
  kWasmExternalFunc = 0,
  kWasmExternalTable = 1,
  kWasmExternalMemory = 2,
  kWasmExternalGlobal = 3
} X86pWasmExternal;

/*
 * The output buffer.
 *
 * `overflow` is sticky: once set it stays set, so a caller that emits a whole
 * module and checks once at the end cannot miss a failure in the middle.
 *
 * The three counter pairs exist so x86p_wasm_ok() can answer "is this module
 * finished", not merely "did it fit". Each pair counts a thing that was opened
 * and the same thing closed; an unequal pair is a structurally invalid module
 * that would otherwise be discovered by an engine, at run time, far from here.
 */
typedef struct X86pWasmEmit {
  uint8_t *buf;
  size_t cap;
  size_t len;
  int overflow;
  unsigned sizes_opened;  /* five-byte size slots reserved ... */
  unsigned sizes_closed;  /* ... and filled in */
  unsigned blocks_opened; /* block/loop/if regions entered ... */
  unsigned blocks_closed; /* ... and ended */
  unsigned bodies_opened; /* function bodies begun ... */
  unsigned bodies_closed; /* ... and finished */
} X86pWasmEmit;

/*
 * A reserved five-byte size slot, returned by value and closed by name.
 *
 * `at` is where the padded LEB128 begins; the size written is the distance
 * from just past it to wherever the buffer has reached when it is closed.
 */
typedef struct X86pWasmSize {
  size_t at;
} X86pWasmSize;

void x86p_wasm_init(X86pWasmEmit *e, void *buf, size_t cap);

/*
 * Did every emit fit, does every region balance, and is every size filled in?
 * Ask this before handing the module to an engine. A module that fails here
 * must be DISCARDED, not repaired and run.
 */
int x86p_wasm_ok(const X86pWasmEmit *e);

/*
 * Is the buffer still GOOD -- nothing has overflowed -- regardless of whether
 * the module is finished?
 *
 * A different question from x86p_wasm_ok, and the two are easy to confuse in a
 * way that is silent: `ok` also requires every region to be CLOSED, so it is
 * false for a perfectly healthy module that is still being written. A caller
 * that gated mid-construction work on `ok` would find itself refusing to add
 * anything the moment it opened a section. Ask this while building and `ok`
 * when finished.
 */
int x86p_wasm_intact(const X86pWasmEmit *e);

/* Where the next byte will land, as an offset from the start of the buffer. */
size_t x86p_wasm_here(const X86pWasmEmit *e);

/* ---- primitives -------------------------------------------------------- */

void x86p_wasm_byte(X86pWasmEmit *e, uint8_t b);
void x86p_wasm_bytes(X86pWasmEmit *e, const void *p, size_t n);

/* Unsigned LEB128, canonical (as short as the value allows). */
void x86p_wasm_u32(X86pWasmEmit *e, uint32_t value);
/* Signed LEB128, which is what every immediate in the instruction set uses. */
void x86p_wasm_i32(X86pWasmEmit *e, int32_t value);
void x86p_wasm_i64(X86pWasmEmit *e, int64_t value);

/* A length-prefixed UTF-8 name, for imports and exports. */
void x86p_wasm_name(X86pWasmEmit *e, const char *name);

/* ---- module and sections ----------------------------------------------- */

/* The eight-byte header: "\0asm" and version 1. */
void x86p_wasm_module_begin(X86pWasmEmit *e);

/*
 * Open a section, write its contents, then close it with the value returned.
 *
 * Sections must appear in increasing id order; this does not enforce that,
 * because a backend emits them in a fixed order and an engine reports the
 * violation precisely. What it does enforce is that every size opened is
 * closed, which an engine cannot report precisely because the module it reads
 * is already nonsense by then.
 */
X86pWasmSize x86p_wasm_section_begin(X86pWasmEmit *e, X86pWasmSection id);
void x86p_wasm_size_end(X86pWasmEmit *e, X86pWasmSize size);

/*
 * One function type in the type section: `count` of them, each spelled
 * params-then-results. Types are numbered by the order they are written, and
 * the caller holds those numbers -- the same contract the format itself has.
 */
void x86p_wasm_functype(
    X86pWasmEmit *e, const X86pWasmType *params, size_t param_count, const X86pWasmType *results, size_t result_count);

/* One imported function: module.field with the given type index. */
void x86p_wasm_import_func(X86pWasmEmit *e, const char *module, const char *field, uint32_t type_index);

/*
 * One imported memory, with a minimum and an optional maximum in 64 KiB pages.
 *
 * The guest arena is the host's memory, imported rather than defined, because
 * a translated block reads and writes the SAME memory the rest of the runtime
 * does. A module that defined its own memory would compute correct arithmetic
 * on a private address space nobody else can see.
 */
void x86p_wasm_import_memory(X86pWasmEmit *e,
                             const char *module,
                             const char *field,
                             uint32_t minimum_pages,
                             int has_maximum,
                             uint32_t maximum_pages);

/* Shared linear memory requires an explicit maximum in the binary format. */
void x86p_wasm_import_shared_memory(
    X86pWasmEmit *e, const char *module, const char *field, uint32_t minimum_pages, uint32_t maximum_pages);

/* One imported table of function references, for indirect calls. */
void x86p_wasm_import_table(
    X86pWasmEmit *e, const char *module, const char *field, uint32_t minimum, int has_maximum, uint32_t maximum);

/* One exported function, by index. */
void x86p_wasm_export_func(X86pWasmEmit *e, const char *field, uint32_t func_index);

/*
 * A function body: begin, declare locals, emit instructions, end.
 *
 * Locals are declared in RUN-LENGTH GROUPS -- "n of this type" -- and the
 * groups must all be written before the first instruction. Local indices
 * continue the parameter numbering, so a function with two parameters has its
 * first declared local at index 2.
 */
void x86p_wasm_body_begin(X86pWasmEmit *e, X86pWasmSize *body);
void x86p_wasm_locals(X86pWasmEmit *e, uint32_t group_count);
void x86p_wasm_local_group(X86pWasmEmit *e, uint32_t count, X86pWasmType type);
void x86p_wasm_body_end(X86pWasmEmit *e, X86pWasmSize body);

/* ---- instructions ------------------------------------------------------ */

void x86p_wasm_i32_const(X86pWasmEmit *e, int32_t value);
void x86p_wasm_i64_const(X86pWasmEmit *e, int64_t value);

void x86p_wasm_local_get(X86pWasmEmit *e, uint32_t index);
void x86p_wasm_local_set(X86pWasmEmit *e, uint32_t index);
void x86p_wasm_local_tee(X86pWasmEmit *e, uint32_t index);
void x86p_wasm_global_get(X86pWasmEmit *e, uint32_t index);
void x86p_wasm_global_set(X86pWasmEmit *e, uint32_t index);

/*
 * Memory access. `align` is the ALIGNMENT HINT as a power of two exponent, not
 * a byte count: 0 means one-byte alignment, 2 means four. It is a promise, not
 * a request, and the guest makes no alignment promises, so the JIT passes 0
 * and lets the engine do the unaligned access it is required to support. A
 * larger hint than the address deserves is not a validation error and not a
 * fault; it is permission for the engine to assume something untrue.
 */
void x86p_wasm_i32_load(X86pWasmEmit *e, uint32_t align, uint32_t offset);
void x86p_wasm_i32_load8_s(X86pWasmEmit *e, uint32_t align, uint32_t offset);
void x86p_wasm_i32_load8_u(X86pWasmEmit *e, uint32_t align, uint32_t offset);
void x86p_wasm_i32_load16_s(X86pWasmEmit *e, uint32_t align, uint32_t offset);
void x86p_wasm_i32_load16_u(X86pWasmEmit *e, uint32_t align, uint32_t offset);
void x86p_wasm_i64_load(X86pWasmEmit *e, uint32_t align, uint32_t offset);
void x86p_wasm_i32_store(X86pWasmEmit *e, uint32_t align, uint32_t offset);
void x86p_wasm_i32_store8(X86pWasmEmit *e, uint32_t align, uint32_t offset);
void x86p_wasm_i32_store16(X86pWasmEmit *e, uint32_t align, uint32_t offset);
void x86p_wasm_i64_store(X86pWasmEmit *e, uint32_t align, uint32_t offset);

/*
 * The i32 operations, by their opcode. The number IS the encoding, and they
 * are contiguous in the specification, so this cannot drift from it.
 */
typedef enum X86pWasmI32Op {
  kWasmI32Eqz = 0x45,
  kWasmI32Eq = 0x46,
  kWasmI32Ne = 0x47,
  kWasmI32LtS = 0x48,
  kWasmI32LtU = 0x49,
  kWasmI32GtS = 0x4A,
  kWasmI32GtU = 0x4B,
  kWasmI32LeS = 0x4C,
  kWasmI32LeU = 0x4D,
  kWasmI32GeS = 0x4E,
  kWasmI32GeU = 0x4F,
  kWasmI32Clz = 0x67,
  kWasmI32Ctz = 0x68,
  kWasmI32Popcnt = 0x69,
  kWasmI32Add = 0x6A,
  kWasmI32Sub = 0x6B,
  kWasmI32Mul = 0x6C,
  kWasmI32DivS = 0x6D,
  kWasmI32DivU = 0x6E,
  kWasmI32RemS = 0x6F,
  kWasmI32RemU = 0x70,
  kWasmI32And = 0x71,
  kWasmI32Or = 0x72,
  kWasmI32Xor = 0x73,
  kWasmI32Shl = 0x74,
  kWasmI32ShrS = 0x75,
  kWasmI32ShrU = 0x76,
  kWasmI32Rotl = 0x77,
  kWasmI32Rotr = 0x78
} X86pWasmI32Op;

void x86p_wasm_i32_op(X86pWasmEmit *e, X86pWasmI32Op op);

/* The i64 forms the guest needs: widening a 32-bit multiply or divide. */
void x86p_wasm_i64_extend_i32_s(X86pWasmEmit *e);
void x86p_wasm_i64_extend_i32_u(X86pWasmEmit *e);
void x86p_wasm_i32_wrap_i64(X86pWasmEmit *e);
void x86p_wasm_i64_mul(X86pWasmEmit *e);
void x86p_wasm_i64_shr_u(X86pWasmEmit *e);
/* The bitwise forms an ext80 widening needs: assembling a significand and an
   exponent field out of the bits of a binary32 or binary64 operand. */
void x86p_wasm_i64_and(X86pWasmEmit *e);
void x86p_wasm_i64_or(X86pWasmEmit *e);
void x86p_wasm_i64_shl(X86pWasmEmit *e);
/* And the three an ext80 NARROWING needs: rounding to nearest is adding half
   an ulp to a 64-bit significand, comparing the discarded part against exactly
   half to find a tie, and clearing one bit to send that tie to even. */
void x86p_wasm_i64_add(X86pWasmEmit *e);
void x86p_wasm_i64_eq(X86pWasmEmit *e);
void x86p_wasm_i64_xor(X86pWasmEmit *e);
void x86p_wasm_i64_const_shift(X86pWasmEmit *e, int64_t amount);

/* ---- 128-bit SIMD ------------------------------------------------------ */

/*
 * The fixed-width SIMD instructions the guest's packed-float family maps onto.
 *
 * ENCODED DIFFERENTLY FROM EVERYTHING ABOVE, which is why they are their own
 * type rather than more entries in X86pWasmI32Op: each is the prefix byte 0xFD
 * followed by an opcode as a u32 LEB, so the ones past 127 are two bytes and a
 * caller that wrote the number as a byte would emit a different instruction.
 * The numbers below are the specification's opcode indices, so this cannot
 * drift from it, and the emitter owns the encoding.
 *
 * ROUNDING IS NOT SELECTABLE. f32x4 arithmetic is IEEE 754 binary32
 * round-to-nearest-even with no flush-to-zero, which is the one mode SSE's
 * MXCSR can also name -- so a lowering that uses these is equivalent to the
 * scalar helper it replaces only while the guest asks for that mode. Whoever
 * emits them owns that question; see jit_wasm_simd_inline.h.
 */
typedef enum X86pWasmSimdOp {
  kWasmV128Not = 77,
  kWasmV128And = 78,
  kWasmV128AndNot = 79, /* a & ~b -- note the operand order against x86's ANDNPS */
  kWasmV128Or = 80,
  kWasmV128Xor = 81,
  kWasmF32x4Add = 228,
  kWasmF32x4Sub = 229,
  kWasmF32x4Mul = 230,
  kWasmF32x4Div = 231
} X86pWasmSimdOp;

void x86p_wasm_v128_op(X86pWasmEmit *e, X86pWasmSimdOp op);

/* v128.load / v128.store. `align` is the hint, as for the i32 forms above. */
void x86p_wasm_v128_load(X86pWasmEmit *e, uint32_t align, uint32_t offset);
void x86p_wasm_v128_store(X86pWasmEmit *e, uint32_t align, uint32_t offset);

/*
 * i8x16.shuffle: two v128 operands and SIXTEEN immediate byte indices, each
 * selecting one byte of the 32-byte concatenation. The selection is part of the
 * INSTRUCTION, so a guest shuffle whose control is a decode-time immediate
 * becomes one instruction with nothing computed at run time. `lanes` must hold
 * sixteen indices, each 0..31; a larger one is a validation error in the engine
 * rather than a wrap, so the caller builds them and this does not clamp.
 */
void x86p_wasm_v128_shuffle(X86pWasmEmit *e, const uint8_t lanes[16]);

/* ---- control ----------------------------------------------------------- */

/*
 * Structured control. Each of these opens a region that x86p_wasm_end closes;
 * the counters make an unbalanced body a refusal here rather than a validation
 * failure in the engine.
 *
 * `type` is the region's RESULT type -- kWasmVoid for a region that leaves
 * nothing on the stack, which is what a translated guest branch wants.
 */
void x86p_wasm_block(X86pWasmEmit *e, X86pWasmType type);
void x86p_wasm_loop(X86pWasmEmit *e, X86pWasmType type);
void x86p_wasm_if(X86pWasmEmit *e, X86pWasmType type);
void x86p_wasm_else(X86pWasmEmit *e);
void x86p_wasm_end(X86pWasmEmit *e);

/*
 * Branches name a region by DEPTH, counting outwards from the innermost: 0 is
 * the region the branch is directly inside. A branch out of a `block` jumps to
 * its end and a branch to a `loop` jumps to its start, which is the one place
 * this reads unlike every other instruction set here.
 */
void x86p_wasm_br(X86pWasmEmit *e, uint32_t depth);
void x86p_wasm_br_if(X86pWasmEmit *e, uint32_t depth);
void x86p_wasm_return(X86pWasmEmit *e);
void x86p_wasm_drop(X86pWasmEmit *e);
void x86p_wasm_select(X86pWasmEmit *e);

/*
 * A trap. This is how an unsupported form refuses at run time instead of
 * computing something plausible: the engine stops the call rather than
 * continuing past it.
 */
void x86p_wasm_unreachable(X86pWasmEmit *e);

void x86p_wasm_call(X86pWasmEmit *e, uint32_t func_index);
/* Indirect through the imported table, by type index. Table 0 is assumed. */
void x86p_wasm_call_indirect(X86pWasmEmit *e, uint32_t type_index);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_EMIT_WASM_H */
