/*
 * test_emit_wasm -- does the encoder emit the module it was asked for?
 *
 * NOT CHECKED BY READING THE BYTES BACK. Hand-written expected byte strings
 * test that the encoder agrees with whoever wrote the test, and both can hold
 * the same misreading of the format -- which for a length-prefixed, structured
 * format is not a subtle wrong instruction but a module no engine will load.
 * So every module here is handed to a REAL WebAssembly engine, which validates
 * it against the specification and runs it, and the value that comes out is
 * compared against what the module was built to compute. The oracle is an
 * independent implementation of the format, not this file's opinion of it.
 *
 * The engine is node, found on PATH or named by X86P_NODE. Without one this
 * test SKIPS (77) rather than passing: a run that instantiated nothing has
 * measured nothing, and must not be readable as the encoder working.
 *
 * The structural half -- sticky overflow, unbalanced regions, unclosed sizes --
 * needs no engine and always runs, because those are the failures that must be
 * caught HERE rather than by an engine rejecting a module far from the cause.
 */
#include "emit_wasm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define x86p_mkdir(path) _mkdir(path)
#else
#include <sys/stat.h>
#define x86p_mkdir(path) mkdir(path, 0777)
#endif

#define CASE_DIR "wasm_emit_cases"
#define MODULE_CAP 4096

static int g_checks;
static int g_failed;

static void check_eq(const char *what, long long got, long long want) {
  g_checks++;
  if (got != want) {
    g_failed++;
    printf("FAIL %s: got %lld, want %lld\n", what, got, want);
  }
}

static void check_text(const char *what, const char *got, const char *want) {
  g_checks++;
  if (strcmp(got, want) != 0) {
    g_failed++;
    printf("FAIL %s: got %s, want %s\n", what, got, want);
  }
}

/* ---- structural checks, which need no engine --------------------------- */

static void test_overflow_is_sticky(void) {
  uint8_t small[4];
  X86pWasmEmit e;
  x86p_wasm_init(&e, small, sizeof(small));
  x86p_wasm_module_begin(&e); /* eight bytes into a four-byte buffer */
  check_eq("overflow set by a module header that does not fit", e.overflow, 1);
  check_eq("overflowed module is not ok", x86p_wasm_ok(&e), 0);
  /* Sticky: a later emit that WOULD fit must not clear it. */
  x86p_wasm_byte(&e, 0x00);
  check_eq("overflow stays set", e.overflow, 1);
  check_eq("nothing was written after overflow", (long long)e.len, 0);
}

static void test_unbalanced_region_is_refused(void) {
  uint8_t buffer[64];
  X86pWasmEmit e;
  x86p_wasm_init(&e, buffer, sizeof(buffer));
  x86p_wasm_block(&e, kWasmVoid);
  check_eq("an open region is not ok", x86p_wasm_ok(&e), 0);
  x86p_wasm_end(&e);
  check_eq("a balanced region is ok", x86p_wasm_ok(&e), 1);
}

static void test_unclosed_size_is_refused(void) {
  uint8_t buffer[64];
  X86pWasmEmit e;
  X86pWasmSize section;
  x86p_wasm_init(&e, buffer, sizeof(buffer));
  section = x86p_wasm_section_begin(&e, kWasmSectionType);
  x86p_wasm_u32(&e, 0);
  check_eq("an unclosed section size is not ok", x86p_wasm_ok(&e), 0);
  x86p_wasm_size_end(&e, section);
  check_eq("a closed section size is ok", x86p_wasm_ok(&e), 1);
}

/*
 * The signed-LEB128 boundaries, checked against a decoder written here from
 * the format's own rule rather than from this encoder's implementation.
 *
 * These are the values where an encoder that stops on "the value reached zero"
 * instead of on the sign bit produces a shorter encoding that reads back as a
 * different number: 64 becomes -64, 8192 becomes -8192. An engine would accept
 * both, and the block would compute with the wrong constant.
 */
static int64_t decode_sleb(const uint8_t *bytes, size_t length) {
  int64_t result = 0;
  unsigned shift = 0;
  size_t i;
  for (i = 0; i < length; i++) {
    result |= (int64_t)(bytes[i] & 0x7Fu) << shift;
    shift += 7;
    if ((bytes[i] & 0x80u) == 0u) {
      if (shift < 64u && (bytes[i] & 0x40u) != 0u) {
        result |= -((int64_t)1 << shift);
      }
      return result;
    }
  }
  return result;
}

static void test_signed_leb_boundaries(void) {
  static const int32_t values[] = {0,
                                   1,
                                   -1,
                                   63,
                                   64,
                                   -64,
                                   -65,
                                   8191,
                                   8192,
                                   -8192,
                                   -8193,
                                   1048575,
                                   1048576,
                                   -1048576,
                                   -1048577,
                                   2147483647,
                                   -2147483647 - 1,
                                   12345678,
                                   -12345678};
  size_t i;
  for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
    uint8_t buffer[16];
    X86pWasmEmit e;
    char label[64];
    x86p_wasm_init(&e, buffer, sizeof(buffer));
    x86p_wasm_i32(&e, values[i]);
    snprintf(label, sizeof(label), "sleb round-trip of %d", values[i]);
    check_eq(label, decode_sleb(buffer, e.len), values[i]);
  }
}

/* ---- modules the engine runs ------------------------------------------- */

typedef void (*BodyFn)(X86pWasmEmit *e);

/*
 * Build a one-function module: two i32 parameters, one i32 result, with the
 * imports every case may use declared whether or not it calls them. Declaring
 * an unused import is legal and keeps the index numbering identical across
 * cases, so a case's function indices do not depend on what it happens to use.
 *
 * Index numbering, which the bodies below depend on:
 *   type 0 = (i32, i32) -> i32     type 1 = () -> ()
 *   func 0 = imported env.helper   func 1 = this module's own function
 */
static size_t build_module(uint8_t *out, size_t cap, const char *export_name, uint32_t local_i32s, BodyFn body) {
  static const X86pWasmType two_i32[2] = {kWasmI32, kWasmI32};
  static const X86pWasmType one_i32[1] = {kWasmI32};
  X86pWasmEmit e;
  X86pWasmSize section;
  X86pWasmSize function_body;
  x86p_wasm_init(&e, out, cap);
  x86p_wasm_module_begin(&e);

  section = x86p_wasm_section_begin(&e, kWasmSectionType);
  x86p_wasm_u32(&e, 1);
  x86p_wasm_functype(&e, two_i32, 2, one_i32, 1);
  x86p_wasm_size_end(&e, section);

  section = x86p_wasm_section_begin(&e, kWasmSectionImport);
  x86p_wasm_u32(&e, 3);
  x86p_wasm_import_func(&e, "env", "helper", 0);
  x86p_wasm_import_memory(&e, "env", "memory", 1, 0, 0);
  x86p_wasm_import_table(&e, "env", "table", 1, 0, 0);
  x86p_wasm_size_end(&e, section);

  section = x86p_wasm_section_begin(&e, kWasmSectionFunction);
  x86p_wasm_u32(&e, 1);
  x86p_wasm_u32(&e, 0); /* our function has type 0 */
  x86p_wasm_size_end(&e, section);

  section = x86p_wasm_section_begin(&e, kWasmSectionExport);
  x86p_wasm_u32(&e, 1);
  x86p_wasm_export_func(&e, export_name, 1);
  x86p_wasm_size_end(&e, section);

  section = x86p_wasm_section_begin(&e, kWasmSectionCode);
  x86p_wasm_u32(&e, 1);
  x86p_wasm_body_begin(&e, &function_body);
  if (local_i32s > 0) {
    x86p_wasm_locals(&e, 1);
    x86p_wasm_local_group(&e, local_i32s, kWasmI32);
  } else {
    x86p_wasm_locals(&e, 0);
  }
  body(&e);
  x86p_wasm_body_end(&e, function_body);
  x86p_wasm_size_end(&e, section);

  if (!x86p_wasm_ok(&e)) {
    return 0;
  }
  return e.len;
}

/* a + b */
static void body_add(X86pWasmEmit *e) {
  x86p_wasm_local_get(e, 0);
  x86p_wasm_local_get(e, 1);
  x86p_wasm_i32_op(e, kWasmI32Add);
}

/* The largest and smallest i32 constants, added: exercises five-byte SLEB. */
static void body_extreme_constants(X86pWasmEmit *e) {
  x86p_wasm_i32_const(e, 2147483647);
  x86p_wasm_i32_const(e, -2147483647 - 1);
  x86p_wasm_i32_op(e, kWasmI32Add);
}

/*
 * Store a, then read it back three ways: as a byte sign-extended, as a
 * halfword zero-extended, and whole. The result folds all three together so a
 * single wrong opcode among the six cannot cancel out.
 */
static void body_memory_roundtrip(X86pWasmEmit *e) {
  x86p_wasm_i32_const(e, 16);
  x86p_wasm_local_get(e, 0);
  x86p_wasm_i32_store(e, 0, 0);

  x86p_wasm_i32_const(e, 16);
  x86p_wasm_i32_load8_s(e, 0, 0);
  x86p_wasm_i32_const(e, 16);
  x86p_wasm_i32_load16_u(e, 0, 0);
  x86p_wasm_i32_op(e, kWasmI32Add);
  x86p_wasm_i32_const(e, 16);
  x86p_wasm_i32_load(e, 0, 0);
  x86p_wasm_i32_op(e, kWasmI32Add);
}

/* An offset immediate rather than an added constant, which is the form a guest
   displacement lowers to. Stores a byte at 32+3 and reads it back. */
static void body_memory_offset(X86pWasmEmit *e) {
  x86p_wasm_i32_const(e, 32);
  x86p_wasm_local_get(e, 0);
  x86p_wasm_i32_store8(e, 0, 3);
  x86p_wasm_i32_const(e, 32);
  x86p_wasm_i32_load8_u(e, 0, 3);
}

/* if a < b then b - a else a - b, signed: a guest conditional, structured. */
static void body_if_else(X86pWasmEmit *e) {
  x86p_wasm_local_get(e, 0);
  x86p_wasm_local_get(e, 1);
  x86p_wasm_i32_op(e, kWasmI32LtS);
  x86p_wasm_if(e, kWasmI32);
  x86p_wasm_local_get(e, 1);
  x86p_wasm_local_get(e, 0);
  x86p_wasm_i32_op(e, kWasmI32Sub);
  x86p_wasm_else(e);
  x86p_wasm_local_get(e, 0);
  x86p_wasm_local_get(e, 1);
  x86p_wasm_i32_op(e, kWasmI32Sub);
  x86p_wasm_end(e);
}

/*
 * Sum a..1 with a loop: local 2 is the accumulator, local 3 the counter.
 *
 * The branch depths are the point. Inside the loop the `br_if 1` names the
 * enclosing block (depth 1) to leave, and the `br 0` names the loop itself to
 * repeat -- a branch to a block goes to its END and a branch to a loop goes to
 * its START, which is the one rule that reads backwards from every other
 * instruction set in this repository.
 */
static void body_loop_sum(X86pWasmEmit *e) {
  x86p_wasm_local_get(e, 0);
  x86p_wasm_local_set(e, 3);
  x86p_wasm_i32_const(e, 0);
  x86p_wasm_local_set(e, 2);
  x86p_wasm_block(e, kWasmVoid);
  x86p_wasm_loop(e, kWasmVoid);
  x86p_wasm_local_get(e, 3);
  x86p_wasm_i32_op(e, kWasmI32Eqz);
  x86p_wasm_br_if(e, 1); /* out of the block */
  x86p_wasm_local_get(e, 2);
  x86p_wasm_local_get(e, 3);
  x86p_wasm_i32_op(e, kWasmI32Add);
  x86p_wasm_local_set(e, 2);
  x86p_wasm_local_get(e, 3);
  x86p_wasm_i32_const(e, 1);
  x86p_wasm_i32_op(e, kWasmI32Sub);
  x86p_wasm_local_set(e, 3);
  x86p_wasm_br(e, 0); /* back to the loop header */
  x86p_wasm_end(e);
  x86p_wasm_end(e);
  x86p_wasm_local_get(e, 2);
}

/* helper(a, b), the imported runtime call. */
static void body_call_import(X86pWasmEmit *e) {
  x86p_wasm_local_get(e, 0);
  x86p_wasm_local_get(e, 1);
  x86p_wasm_call(e, 0);
}

/* table[0](a, b) -- the shape a translated indirect guest call takes. */
static void body_call_indirect(X86pWasmEmit *e) {
  x86p_wasm_local_get(e, 0);
  x86p_wasm_local_get(e, 1);
  x86p_wasm_i32_const(e, 0); /* table index */
  x86p_wasm_call_indirect(e, 0);
}

/* The high 32 bits of an unsigned 32x32 multiply, which is guest MUL's EDX. */
static void body_mul_high(X86pWasmEmit *e) {
  x86p_wasm_local_get(e, 0);
  x86p_wasm_i64_extend_i32_u(e);
  x86p_wasm_local_get(e, 1);
  x86p_wasm_i64_extend_i32_u(e);
  x86p_wasm_i64_mul(e);
  x86p_wasm_i64_const_shift(e, 32);
  x86p_wasm_i32_wrap_i64(e);
}

/* select between the two arguments on a < b, and drop a spare value first. */
static void body_select_and_drop(X86pWasmEmit *e) {
  x86p_wasm_i32_const(e, 999);
  x86p_wasm_drop(e);
  x86p_wasm_local_get(e, 0);
  x86p_wasm_local_get(e, 1);
  x86p_wasm_local_get(e, 0);
  x86p_wasm_local_get(e, 1);
  x86p_wasm_i32_op(e, kWasmI32LtU);
  x86p_wasm_select(e);
}

/* An unsupported form refuses at run time rather than computing a plausible
   answer. The engine must trap, and the test asserts the trap. */
static void body_unreachable(X86pWasmEmit *e) {
  x86p_wasm_unreachable(e);
}

typedef struct Case {
  const char *name;
  const char *export_name;
  uint32_t locals;
  BodyFn body;
  int arg_a;
  int arg_b;
  int wants_table;
  const char *expected; /* decimal, or "!trap" as a prefix match */
} Case;

static const Case kCases[] = {
    {"add", "run", 0, body_add, 7, 35, 0, "42"},
    {"extreme_constants", "run", 0, body_extreme_constants, 0, 0, 0, "-1"},
    /* Store 0x12B4, then fold three reads of it together. Derived, not
       observed: the low byte 0xB4 read SIGNED is -76, the halfword read
       unsigned is 0x12B4 = 4788, and the whole word is 4788, so
       -76 + 4788 + 4788 = 9500. The value has its low byte's high bit set on
       purpose -- with load8_u where load8_s belongs the sum is 9756, so the
       two are distinguishable rather than accidentally equal. */
    {"memory_roundtrip", "run", 0, body_memory_roundtrip, 0x12B4, 0, 0, "9500"},
    {"memory_offset", "run", 0, body_memory_offset, 0xAB, 0, 0, "171"},
    {"if_else", "run", 0, body_if_else, 3, 10, 0, "7"},
    {"if_else_other_arm", "run", 0, body_if_else, 10, 3, 0, "7"},
    {"loop_sum", "run", 2, body_loop_sum, 10, 0, 0, "55"},
    {"call_import", "run", 0, body_call_import, 5, 6, 0, "17"},
    {"call_indirect", "run", 0, body_call_indirect, 5, 6, 1, "11"},
    {"mul_high", "run", 0, body_mul_high, -1, -1, 0, "-2"},
    {"select_and_drop", "run", 0, body_select_and_drop, 4, 9, 0, "4"},
    {"unreachable", "run", 0, body_unreachable, 0, 0, 0, "!trap"},
};

#define CASE_COUNT ((int)(sizeof(kCases) / sizeof(kCases[0])))

/* popen/pclose, spelled once so the two Windows underscores are not repeated
   at every call site. */
static FILE *open_pipe(const char *command) {
#if defined(_WIN32)
  return _popen(command, "r");
#else
  return popen(command, "r");
#endif
}

static int close_pipe(FILE *pipe) {
#if defined(_WIN32)
  return _pclose(pipe);
#else
  return pclose(pipe);
#endif
}

/*
 * Is there a WebAssembly engine at all?
 *
 * Asked BEFORE any case runs, and asked by running the engine rather than by
 * inspecting whether the case output came back empty. A missing engine and a
 * broken encoder both produce no results, and a test that cannot tell them
 * apart reports one as the other -- which is how "the encoder is fine, node
 * just is not installed" gets read as a failure, and how a genuinely broken
 * encoder gets read as a skip.
 */
static int engine_present(const char *node) {
  char command[512];
  FILE *pipe;
  char line[256];
  int have_output;
  snprintf(command, sizeof(command), "\"%s\" --version 2>&1", node);
  pipe = open_pipe(command);
  if (!pipe) {
    return 0;
  }
  have_output = fgets(line, sizeof(line), pipe) != NULL && line[0] == 'v';
  return close_pipe(pipe) == 0 && have_output;
}

static int write_file(const char *path, const uint8_t *bytes, size_t length) {
  FILE *file = fopen(path, "wb");
  if (!file) {
    return 0;
  }
  if (fwrite(bytes, 1, length, file) != length) {
    fclose(file);
    return 0;
  }
  return fclose(file) == 0;
}

/* The module whose export is installed in the table for the indirect case: a
   plain adder, built by this same encoder. */
static int write_table_module(const char *path) {
  uint8_t module[MODULE_CAP];
  size_t length = build_module(module, sizeof(module), "target", 0, body_add);
  if (length == 0) {
    return 0;
  }
  return write_file(path, module, length);
}

static int build_all_cases(void) {
  char path[256];
  FILE *manifest;
  int i;
  int ok = 1;
  if (x86p_mkdir(CASE_DIR) != 0) {
    /* Already there from a previous run is fine; anything else shows up as a
       failed write below, with the path in the message. */
  }
  snprintf(path, sizeof(path), "%s/table.wasm", CASE_DIR);
  if (!write_table_module(path)) {
    printf("FAIL could not write %s\n", path);
    return 0;
  }
  snprintf(path, sizeof(path), "%s/manifest.json", CASE_DIR);
  manifest = fopen(path, "wb");
  if (!manifest) {
    printf("FAIL could not write %s\n", path);
    return 0;
  }
  fprintf(manifest, "{\"cases\":[");
  for (i = 0; i < CASE_COUNT; i++) {
    uint8_t module[MODULE_CAP];
    size_t length = build_module(module, sizeof(module), kCases[i].export_name, kCases[i].locals, kCases[i].body);
    char file[256];
    g_checks++;
    if (length == 0) {
      g_failed++;
      ok = 0;
      printf("FAIL %s: the encoder refused its own module\n", kCases[i].name);
      continue;
    }
    snprintf(file, sizeof(file), "%s/%s.wasm", CASE_DIR, kCases[i].name);
    if (!write_file(file, module, length)) {
      g_failed++;
      ok = 0;
      printf("FAIL could not write %s\n", file);
      continue;
    }
    fprintf(manifest,
            "%s{\"name\":\"%s\",\"file\":\"%s.wasm\",\"export\":\"%s\",\"args\":[%d,%d]",
            i == 0 ? "" : ",",
            kCases[i].name,
            kCases[i].name,
            kCases[i].export_name,
            kCases[i].arg_a,
            kCases[i].arg_b);
    if (kCases[i].wants_table) {
      fprintf(manifest, ",\"table\":{\"file\":\"table.wasm\",\"export\":\"target\"}");
    }
    fprintf(manifest, "}");
  }
  fprintf(manifest, "]}\n");
  if (fclose(manifest) != 0) {
    printf("FAIL could not close the manifest\n");
    return 0;
  }
  return ok;
}

/* The engine's answer for one case, looked up by name in its output. */
static const char *result_for(char *output, const char *name) {
  char needle[128];
  char *found;
  snprintf(needle, sizeof(needle), "\n%s\t", name);
  found = strstr(output, needle);
  if (!found) {
    /* The first line has no leading newline. */
    snprintf(needle, sizeof(needle), "%s\t", name);
    if (strncmp(output, needle, strlen(needle)) == 0) {
      found = output;
    } else {
      return NULL;
    }
  } else {
    found++;
  }
  return found + strlen(name) + 1;
}

int main(void) {
  const char *node = getenv("X86P_NODE");
  const char *oracle = getenv("X86P_WASM_ORACLE");
  char command[1024];
  char output[8192];
  size_t used = 0;
  FILE *pipe;
  int i;
  int engine_cases = 0;

  test_overflow_is_sticky();
  test_unbalanced_region_is_refused();
  test_unclosed_size_is_refused();
  test_signed_leb_boundaries();

  if (!node) {
    node = "node";
  }
  if (!oracle) {
    printf("test_emit_wasm: X86P_WASM_ORACLE is unset, so no module could be "
           "run. The structural checks alone do not establish the encoding.\n");
    return 77;
  }
  if (!engine_present(node)) {
    printf("test_emit_wasm: no WebAssembly engine -- tried \"%s --version\". "
           "Set X86P_NODE to one. The structural checks passed (%d checks, %d "
           "failed) but establish nothing about the encoding, so this SKIPs "
           "rather than passing.\n",
           node,
           g_checks,
           g_failed);
    return g_failed == 0 ? 77 : 1;
  }
  if (!build_all_cases()) {
    printf("test_emit_wasm: FAILED to build the cases (%d checks, %d failed)\n", g_checks, g_failed);
    return 1;
  }

  snprintf(command, sizeof(command), "\"%s\" \"%s\" \"%s/manifest.json\" 2>&1", node, oracle, CASE_DIR);
  pipe = open_pipe(command);
  if (!pipe) {
    printf("test_emit_wasm: the engine answered --version but could not be run "
           "on the oracle (%s)\n",
           command);
    return 1;
  }
  while (used + 1 < sizeof(output)) {
    size_t got = fread(output + used, 1, sizeof(output) - used - 1, pipe);
    if (got == 0) {
      break;
    }
    used += got;
  }
  output[used] = '\0';
  /* The engine is known present, so a nonzero status here is the oracle or the
     modules failing, never an absent toolchain. It is reported per case below,
     with the engine's own words. */
  (void)close_pipe(pipe);

  for (i = 0; i < CASE_COUNT; i++) {
    const char *got = result_for(output, kCases[i].name);
    char actual[256];
    const char *newline;
    if (!got) {
      g_checks++;
      g_failed++;
      printf("FAIL %s: the engine reported no result. Output was:\n%s\n", kCases[i].name, output);
      continue;
    }
    newline = strchr(got, '\n');
    snprintf(actual, sizeof(actual), "%.*s", newline ? (int)(newline - got) : (int)strlen(got), got);
    /* A module the engine REFUSED did not exercise the encoding, so it must
       not be counted towards the denominator that says the encoding was
       tested. A trap did run, and is counted. */
    if (strncmp(actual, "!invalid", 8) != 0 && strncmp(actual, "!missing", 8) != 0) {
      engine_cases++;
    }
    if (strcmp(kCases[i].expected, "!trap") == 0) {
      g_checks++;
      if (strncmp(actual, "!trap", 5) != 0) {
        g_failed++;
        printf("FAIL %s: expected a trap, got %s\n", kCases[i].name, actual);
      }
    } else {
      check_text(kCases[i].name, actual, kCases[i].expected);
    }
  }

  /* A denominator, and a refusal when it is zero: a run that instantiated no
     module has not established the encoding, whatever else passed. */
  if (engine_cases == 0) {
    printf("test_emit_wasm: 0 of %d modules reached the engine -- refusing to "
           "report a pass\n",
           CASE_COUNT);
    return 1;
  }
  printf("test_emit_wasm: %d checks, %d failed; %d of %d modules validated and "
         "ran in a real WebAssembly engine\n",
         g_checks,
         g_failed,
         engine_cases,
         CASE_COUNT);
  return g_failed == 0 ? 0 : 1;
}
