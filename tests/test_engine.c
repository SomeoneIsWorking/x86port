/*
 * test_engine -- the engine selector's refusals.
 *
 * The happy path here is one line and barely worth a test. What is worth
 * testing is everything this exists to REFUSE: a misspelled engine, an engine
 * that is spelled right but was never linked, a default the consumer does not
 * itself have, and any value outside the enum. Each of those, if it resolved to
 * the substrate instead, would produce a run indistinguishable from one where
 * the requested engine worked -- and the whole migration is measured by
 * comparing engines, so that failure would not be a bug, it would be a false
 * measurement.
 */
#include "engine.h"

#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failed;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    g_checks++;                                                                                                        \
    if (!(cond)) {                                                                                                     \
      g_failed++;                                                                                                      \
      printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                       \
    }                                                                                                                  \
  } while (0)

#define CHECK_EQ_U(got, want)                                                                                          \
  do {                                                                                                                 \
    unsigned long long g_ = (unsigned long long)(got);                                                                 \
    unsigned long long w_ = (unsigned long long)(want);                                                                \
    g_checks++;                                                                                                        \
    if (g_ != w_) {                                                                                                    \
      g_failed++;                                                                                                      \
      printf("    FAIL %s:%d: %s: got %llu want %llu\n", __FILE__, __LINE__, #got, g_, w_);                            \
    }                                                                                                                  \
  } while (0)

static int g_test_failed;
#define RUN(fn)                                                                                                        \
  do {                                                                                                                 \
    int before = g_failed;                                                                                             \
    printf("test %s\n", #fn);                                                                                          \
    fn();                                                                                                              \
    if (g_failed != before) {                                                                                          \
      g_test_failed++;                                                                                                 \
      printf("  FAIL\n");                                                                                              \
    } else {                                                                                                           \
      printf("  PASS\n");                                                                                              \
    }                                                                                                                  \
  } while (0)

/*
 * Exhaustive over the enum, counted against kX86pEngineCount rather than a
 * literal -- adding an engine and forgetting its row must fail here, which it
 * cannot do if the test writes its own total.
 */
static void test_every_engine_routes_and_names(void) {
  int i;
  int routed = 0;
  for (i = 0; i < (int)kX86pEngineCount; i++) {
    X86pEngine e = (X86pEngine)i;
    const char *n = x86p_engine_name(e);
    X86pEngine back = kX86pEngineCount;
    CHECK(n != NULL && n[0] != '\0');
    CHECK(strcmp(n, "unknown") != 0); /* a real engine must not be spelled "unknown" */
    CHECK(x86p_route(e) != kX86pRouteRefuse);
    /* Round trip: every engine is spellable, and its own spelling parses back
       to itself. An engine nobody can name is an engine nobody can select. */
    CHECK(x86p_engine_parse(n, &back) == 1);
    CHECK_EQ_U(back, e);
    CHECK(strstr(x86p_engine_name_list(), n) != NULL);
    routed++;
  }
  CHECK_EQ_U(routed, (int)kX86pEngineCount); /* the denominator, stated */
}

/* Totality. A value outside the enum is the case a switch without a default
   falls through, and falling through here means running the wrong engine. */
static void test_out_of_range_refuses(void) {
  static const int outside[] = {(int)kX86pEngineCount, (int)kX86pEngineCount + 1, 99, -1, -12345};
  const int n = (int)(sizeof outside / sizeof outside[0]);
  int i;
  for (i = 0; i < n; i++) {
    X86pEngine e = (X86pEngine)outside[i];
    CHECK(x86p_route(e) == kX86pRouteRefuse);
    CHECK(strcmp(x86p_engine_name(e), "unknown") == 0); /* names it, does not crash */
    CHECK_EQ_U(x86p_engine_bit(e), 0u);                 /* and is never "available" */
  }
}

static void test_parse_refusals_leave_out_untouched(void) {
  static const char *bad[] = {
      NULL,
      "",
      "subs",
      "substrate ",
      "interp",
      "JITT",
      "0",
      "1",
      "true",
      "recomp",
  };
  const int n = (int)(sizeof bad / sizeof bad[0]);
  int i;
  int refused = 0;
  X86pEngine out = kX86pEngineJit; /* sentinel: NOT the default, so a silent
                                      write to the default would show up */
  for (i = 0; i < n; i++) {
    if (!x86p_engine_parse(bad[i], &out)) {
      refused++;
    }
  }
  CHECK_EQ_U(refused, n);
  CHECK_EQ_U(out, kX86pEngineJit);

  /* "substrate " with a trailing space is in that list on purpose: a parser
     that trims looks friendlier and makes a typo in a script silently work,
     which is how a run ends up on an engine nobody asked for. */

  CHECK(x86p_engine_parse(NULL, NULL) == 0);
  CHECK(x86p_engine_parse("substrate", NULL) == 0);
}

static void test_parse_is_case_insensitive(void) {
  X86pEngine e = kX86pEngineCount;
  CHECK(x86p_engine_parse("INTERPRETER", &e) == 1);
  CHECK_EQ_U(e, kX86pEngineInterpreter);
  CHECK(x86p_engine_parse("Jit", &e) == 1);
  CHECK_EQ_U(e, kX86pEngineJit);
  CHECK(x86p_engine_parse("sUbStRaTe", &e) == 1);
  CHECK_EQ_U(e, kX86pEngineSubstrate);
}

static void test_bits(void) {
  CHECK_EQ_U(x86p_engine_bit(kX86pEngineSubstrate), 1u);
  CHECK_EQ_U(x86p_engine_bit(kX86pEngineInterpreter), 2u);
  CHECK_EQ_U(x86p_engine_bit(kX86pEngineJit), 4u);
  /* all_bits is every engine and nothing else, derived from the same count. */
  CHECK_EQ_U(x86p_engine_all_bits(), (1u << (unsigned)kX86pEngineCount) - 1u);
  {
    int i;
    unsigned acc = 0;
    for (i = 0; i < (int)kX86pEngineCount; i++) {
      acc |= x86p_engine_bit((X86pEngine)i);
    }
    CHECK_EQ_U(acc, x86p_engine_all_bits());
  }
}

/* The state xmen2 is actually in today: the substrate is linked, the
   interpreter is not yet written. Asking for it must fail loudly. */
static void test_resolve_real_but_unlinked(void) {
  const unsigned only_substrate = x86p_engine_bit(kX86pEngineSubstrate);
  X86pEngine out = kX86pEngineJit;
  char reason[256];

  CHECK(x86p_engine_resolve("interpreter", kX86pEngineSubstrate, only_substrate, &out, reason, sizeof reason) == 0);
  CHECK_EQ_U(out, kX86pEngineJit); /* untouched -- no silent substrate */
  CHECK(reason[0] != '\0');
  /* The reason has to distinguish this from a typo: one is fixed by editing a
     command line, the other by building the engine. */
  CHECK(strstr(reason, "real engine") != NULL);
  CHECK(strstr(reason, "interpreter") != NULL);
  CHECK(strstr(reason, "substrate") != NULL); /* and says what IS available */
}

static void test_resolve_misspelled(void) {
  X86pEngine out = kX86pEngineJit;
  char reason[256];
  CHECK(x86p_engine_resolve("interperter", kX86pEngineSubstrate, x86p_engine_all_bits(), &out, reason, sizeof reason) ==
        0);
  CHECK_EQ_U(out, kX86pEngineJit);
  CHECK(strstr(reason, "none of the") != NULL);
  CHECK(strstr(reason, x86p_engine_name_list()) != NULL); /* prints the valid set */
}

static void test_resolve_unset_uses_fallback(void) {
  X86pEngine out = kX86pEngineJit;
  char reason[256];
  CHECK(x86p_engine_resolve(NULL, kX86pEngineSubstrate, x86p_engine_all_bits(), &out, reason, sizeof reason) == 1);
  CHECK_EQ_U(out, kX86pEngineSubstrate);
  CHECK_EQ_U(reason[0], '\0'); /* success says nothing */

  out = kX86pEngineJit;
  CHECK(x86p_engine_resolve("", kX86pEngineInterpreter, x86p_engine_all_bits(), &out, reason, sizeof reason) == 1);
  CHECK_EQ_U(out, kX86pEngineInterpreter); /* empty == unset, not == default */
}

/* A consumer whose declared default is not among its own linked arms is
   misconfigured. Caught at selection, not at the first guest call. */
static void test_resolve_unset_with_unavailable_fallback(void) {
  X86pEngine out = kX86pEngineSubstrate;
  char reason[256];
  CHECK(x86p_engine_resolve(
            NULL, kX86pEngineInterpreter, x86p_engine_bit(kX86pEngineSubstrate), &out, reason, sizeof reason) == 0);
  CHECK(strstr(reason, "default") != NULL);
  CHECK(strstr(reason, "interpreter") != NULL);
}

/* The degenerate consumer: nothing linked. The report must say so in words
   rather than printing an empty list, which reads as a formatting bug and
   sends the reader looking in the wrong place. */
static void test_resolve_with_no_engines_available(void) {
  X86pEngine out = kX86pEngineJit;
  char reason[256];
  CHECK(x86p_engine_resolve("substrate", kX86pEngineSubstrate, 0u, &out, reason, sizeof reason) == 0);
  CHECK(strstr(reason, "(none)") != NULL);
  CHECK_EQ_U(out, kX86pEngineJit);
}

/* A caller that does not want the reason must still get the verdict, and must
   not be written through. */
static void test_resolve_without_reason_buffer(void) {
  X86pEngine out = kX86pEngineJit;
  CHECK(x86p_engine_resolve("substrate", kX86pEngineSubstrate, x86p_engine_all_bits(), &out, NULL, 0) == 1);
  CHECK_EQ_U(out, kX86pEngineSubstrate);
  CHECK(x86p_engine_resolve("nonsense", kX86pEngineSubstrate, x86p_engine_all_bits(), &out, NULL, 0) == 0);
  CHECK(x86p_engine_resolve("substrate", kX86pEngineSubstrate, x86p_engine_all_bits(), NULL, NULL, 0) == 0);
}

int main(void) {
  RUN(test_every_engine_routes_and_names);
  RUN(test_out_of_range_refuses);
  RUN(test_parse_refusals_leave_out_untouched);
  RUN(test_parse_is_case_insensitive);
  RUN(test_bits);
  RUN(test_resolve_real_but_unlinked);
  RUN(test_resolve_misspelled);
  RUN(test_resolve_unset_uses_fallback);
  RUN(test_resolve_unset_with_unavailable_fallback);
  RUN(test_resolve_with_no_engines_available);
  RUN(test_resolve_without_reason_buffer);
  printf("%d check(s), %d failed, %d failing test(s)\n", g_checks, g_failed, g_test_failed);
  return g_test_failed ? 1 : 0;
}
