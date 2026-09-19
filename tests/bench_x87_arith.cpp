/*
 * What the x87 register file's representation costs, measured before anyone
 * changes it.
 *
 * The browser profile puts x86p_x87_arith at 16% of the guest worker and the
 * ext80 arithmetic it calls at about 5%, which says most of the cost is moving
 * values rather than computing them. The suspected reason is that the register
 * file holds `long double` -- IEEE binary128 on this host, which WebAssembly
 * has no register for -- while the arithmetic is ext80, so every operation
 * widens both operands and narrows the result.
 *
 * That is a hypothesis, and rewriting the register file to test it would be a
 * refactor across three host configurations. So measure it here first, with
 * the SHIPPING arithmetic in both arms:
 *
 *   A. through x86p_x87_arith, the real path, storage as it is today.
 *   B. the same operands and the same extF80_* calls, with the values already
 *      in ext80 -- what the path would cost with the storage changed.
 *   D. through the register file again, but with the RAW entry points, which
 *      is the path a JIT backend takes once the storage IS the guest format.
 *      This is the one the change is for; A is what it looked like before.
 *   C. x86p_x87_software_arith, which is what A calls on this host: the same
 *      conversions and the same arithmetic, without the register file's tags,
 *      stack and status word.
 *
 * C must be the function A really calls, and the first version of this got
 * that wrong: it used x86p_x87_arith_portable, which is native + - * / on
 * binary128 followed by a re-round -- a different algorithm, not arm A with a
 * layer removed. It made the register file look like 44% of the path.
 *
 * A minus B is a ceiling, and on its own it is a misleading one, because it
 * charges the storage for bookkeeping that a storage change does not remove.
 * C separates them:
 *
 *   A - C  the register file's own work -- tags, stack, get/push. A storage
 *          change does not remove this.
 *   C - B  converting into and out of the storage type on every operation.
 *          This is the part the refactor removes, and it is the number that
 *          should decide whether to do it.
 *
 * What it said when it was written, four runs under node on the wasm build:
 * conversion 38% of the path, arithmetic 37%, register file 25% -- so changing
 * the storage is worth about 1.62x HERE. That is this path only. It does not
 * cover the guest memory load and store paths, which convert through the same
 * type and were not measured, and it is not a frame rate.
 *
 * This only answers anything on a host whose `long double` is binary128, which
 * is the host the question is about. Where the host has a real 80-bit FPU,
 * x86p_x87_arith does not go through softfloat at all, so there is nothing to
 * compare and the benchmark says so and stops.
 */
#include "softfloat.h"
#include "x87.h"
#include "x87_f128_ext80.h"
#include "x87_softfloat.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
static double now_ms() {
  return emscripten_get_now();
}
#else
#include <chrono>
static double now_ms() {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
#endif

namespace {

constexpr int kValues = 64;
constexpr long kIterations = 300000;

/* Values a game actually multiplies and adds: ordinary normals, no specials,
   so all three arms take their common path and the comparison is not measuring
   a NaN branch. */
void fill(long double *out) {
  for (int i = 0; i < kValues; i++) {
    out[i] = (long double)(i + 1) * 1.37500L + 0.125L;
  }
}

double run_through_register_file(const long double *values, long double *sink) {
  X86pX87 f;
  long double total = 0.0L;
  /* Reset is outside the loop on purpose. It clears the whole file and no
     guest does it once per three operations; leaving it inside charged the
     register file for work that is not on the hot path, and the first version
     of this benchmark did exactly that. FLD and FSTP are a different matter --
     the guest really does push and pop constantly, so those stay in. */
  x86p_x87_reset(&f);
  {
    const double start = now_ms();
    for (long n = 0; n < kIterations; n++) {
      const long double *v = values + (n % (kValues / 2)) * 2;
      x86p_x87_push(&f, v[0]);
      x86p_x87_arith(&f, kX86pX87Mul, 0, v[1], 0);
      x86p_x87_arith(&f, kX86pX87Add, 0, v[0], 0);
      x86p_x87_arith(&f, kX86pX87Sub, 0, v[1], 0);
      {
        /* One pop rather than get-then-pop: FSTP is what the guest emits, and
           it is what balances the push above. */
        long double r = 0.0L;
        x86p_x87_pop(&f, &r);
        total += r;
      }
    }
    *sink = total;
    return now_ms() - start;
  }
}

/* The same three operations, with the operands already in the format the
   arithmetic works in. Nothing here is a faster ALGORITHM -- these are the
   same extF80_* calls that x86p_x87_software_arith makes, reached without
   converting into and out of the storage type on every step.

   It calls them directly rather than through a new x86port entry point,
   because arm B is the hypothetical and not a shipping path: a library export
   whose only caller is a benchmark is production surface for a measurement.
   The environment below is a copy of the one x87_softfloat.cpp builds for
   X86P_X87_CW_INIT, and the two arms agreeing on the accumulated total is
   what catches it if that copy is ever wrong -- a different rounding mode or
   precision would not produce the same sum. */
softfloat_status_t init_environment() {
  softfloat_status_t status{};
  status.softfloat_exceptionMasks = 0x3F;
  status.softfloat_roundingMode = (X86P_X87_CW_INIT >> 10) & 3;
  status.extF80_roundingPrecision = 80;
  return status;
}

double run_in_ext80(const long double *values, long double *sink) {
  floatx80 ext[kValues];
  long double total = 0.0L;
  for (int i = 0; i < kValues; i++) {
    if (!x86p_x87_f128_to_ext80_exact(&values[i], &ext[i])) {
      std::printf("SKIP: this host's long double is not binary128, so x86p_x87_arith\n");
      std::printf("      does not use the software path and there is nothing to compare.\n");
      *sink = 0.0L;
      return -1.0;
    }
  }
  {
    const double start = now_ms();
    for (long n = 0; n < kIterations; n++) {
      const floatx80 *v = ext + (n % (kValues / 2)) * 2;
      floatx80 acc = v[0];
      /* One environment per operation, because that is what arms A and C pay:
         x86p_x87_software_arith builds one on every call. Hoisting it out of
         the loop would credit the ext80 storage with a saving that changing
         the storage does not produce. */
      {
        softfloat_status_t status = init_environment();
        acc = extF80_mul(acc, v[1], &status);
      }
      {
        softfloat_status_t status = init_environment();
        acc = extF80_add(acc, v[0], &status);
      }
      {
        softfloat_status_t status = init_environment();
        acc = extF80_sub(acc, v[1], &status);
      }
      {
        long double r = 0.0L;
        x86p_x87_ext80_to_f128_exact(acc, &r);
        total += r;
      }
    }
    *sink = total;
    return now_ms() - start;
  }
}

/* Exactly what arm A calls once it has the operands out of the register file:
   same widen, same extF80_*, same narrow, none of the tag, stack or status
   work. So the gap between this and arm B is the conversion traffic alone,
   and the gap between arm A and this is the register file's own. */
double run_software_arith(const long double *values, long double *sink) {
  const double start = now_ms();
  long double total = 0.0L;
  for (long n = 0; n < kIterations; n++) {
    const long double *v = values + (n % (kValues / 2)) * 2;
    uint16_t sw = 0;
    long double acc = v[0];
    acc = x86p_x87_software_arith(X86P_X87_CW_INIT, kX86pX87Mul, acc, v[1], &sw);
    acc = x86p_x87_software_arith(X86P_X87_CW_INIT, kX86pX87Add, acc, v[0], &sw);
    acc = x86p_x87_software_arith(X86P_X87_CW_INIT, kX86pX87Sub, acc, v[1], &sw);
    total += acc;
  }
  *sink = total;
  return now_ms() - start;
}

/* The same work through the raw entry points: no host float type appears
   anywhere in the loop, which is the state a JIT backend can actually reach
   because its values come from guest memory and go back to it. */
double run_raw(const long double *values, long double *sink) {
  X86pX87 f;
  X86pX87Reg raw[kValues];
  long double total = 0.0L;
  for (int i = 0; i < kValues; i++) {
    raw[i] = x86p_x87_reg_from_long_double(values[i]);
  }
  x86p_x87_reset(&f);
  {
    const double start = now_ms();
    for (long n = 0; n < kIterations; n++) {
      const X86pX87Reg *v = raw + (n % (kValues / 2)) * 2;
      X86pX87Reg r;
      x86p_x87_push_raw(&f, v[0]);
      x86p_x87_arith_raw(&f, kX86pX87Mul, 0, v[1], 0);
      x86p_x87_arith_raw(&f, kX86pX87Add, 0, v[0], 0);
      x86p_x87_arith_raw(&f, kX86pX87Sub, 0, v[1], 0);
      x86p_x87_pop_raw(&f, &r);
      total += x86p_x87_reg_to_long_double(r);
    }
    *sink = total;
    return now_ms() - start;
  }
}

} // namespace

int main() {
  long double values[kValues];
  long double sink_a = 0.0L, sink_b = 0.0L, sink_c = 0.0L, sink_d = 0.0L;
  fill(values);

  /* Warm all three arms before timing any, so none pays another's cold start
     and a JIT tier-up does not land inside one of them. */
  (void)run_through_register_file(values, &sink_a);
  if (run_in_ext80(values, &sink_b) < 0.0) {
    return 0;
  }
  (void)run_software_arith(values, &sink_c);
  (void)run_raw(values, &sink_d);

  {
    const double a = run_through_register_file(values, &sink_a);
    const double b = run_in_ext80(values, &sink_b);
    const double c = run_software_arith(values, &sink_c);
    const double d = run_raw(values, &sink_d);
    std::printf("x87 arith over %ld iterations of 3 operations:\n", kIterations);
    std::printf("  A  through the register file (long double storage): %8.1f ms\n", a);
    std::printf("  C  x86p_x87_software_arith, no register file      : %8.1f ms\n", c);
    std::printf("  D  through the register file, RAW entry points    : %8.1f ms\n", d);
    std::printf("  B  operands already in ext80                      : %8.1f ms\n", b);
    std::printf("\n");
    std::printf("  register file's own work      A-C: %8.1f ms  (%4.1f%% of A)\n", a - c, 100.0 * (a - c) / a);
    std::printf("  conversion into/out of storage C-B: %8.1f ms  (%4.1f%% of A)\n", c - b, 100.0 * (c - b) / a);
    std::printf("  arithmetic itself              B  : %8.1f ms  (%4.1f%% of A)\n", b, 100.0 * b / a);
    std::printf("\n");
    std::printf("  the raw path against the long double one   A/D: %5.2fx\n", a / d);
    std::printf("  what is left between the raw path and bare ext80: %8.1f ms\n", d - b);
    /* Every arm must have computed the same thing, or the comparison is
       between different amounts of work. */
    if (sink_a != sink_b || sink_a != sink_c || sink_a != sink_d) {
      std::printf("FAIL: the arms disagree (A %.17Lg, B %.17Lg, C %.17Lg, D %.17Lg)\n", sink_a, sink_b, sink_c, sink_d);
      return 1;
    }
    std::printf("  all three arms agree on %.17Lg\n", sink_a);
  }
  return 0;
}
