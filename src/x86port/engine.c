/* engine.c -- see engine.h for why this is a type with a refusal, not a flag. */
#include "engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */

/*
 * ONE table. Names, routes and the enum are derived from it rather than kept in
 * three switch statements that agree until someone adds an engine to two of
 * them -- which is exactly how an engine becomes selectable but unspellable, or
 * spellable but unroutable.
 */
static const struct {
  const char *name;
  X86pRoute route;
} kEngines[] = {
    {"substrate", kX86pRouteSubstrate},
    {"interpreter", kX86pRouteInterpreter},
    {"jit", kX86pRouteJit},
};

enum { kEngineCount = (int)(sizeof kEngines / sizeof kEngines[0]) };

/* The denominator has to match the enum, or an exhaustive check is counting
   against the wrong total and will report full coverage of a subset. */
_Static_assert(kEngineCount == (int)kX86pEngineCount, "every X86pEngine needs a row in kEngines");

static int in_range(X86pEngine e) {
  return (int)e >= 0 && (int)e < kEngineCount;
}

X86pRoute x86p_route(X86pEngine e) {
  if (!in_range(e)) {
    return kX86pRouteRefuse;
  }
  return kEngines[(int)e].route;
}

const char *x86p_engine_name(X86pEngine e) {
  if (!in_range(e)) {
    return "unknown";
  }
  return kEngines[(int)e].name;
}

const char *x86p_engine_name_list(void) {
  static char list[128];
  int i;
  if (list[0] != '\0') {
    return list;
  }
  for (i = 0; i < kEngineCount; i++) {
    if (i) {
      strncat(list, " | ", sizeof list - strlen(list) - 1);
    }
    strncat(list, kEngines[i].name, sizeof list - strlen(list) - 1);
  }
  return list;
}

int x86p_engine_parse(const char *s, X86pEngine *out) {
  int i;
  if (!s || !*s || !out) {
    return 0;
  }
  for (i = 0; i < kEngineCount; i++) {
    if (strcasecmp(s, kEngines[i].name) == 0) {
      *out = (X86pEngine)i;
      return 1;
    }
  }
  return 0;
}

unsigned x86p_engine_bit(X86pEngine e) {
  return in_range(e) ? (1u << (unsigned)e) : 0u;
}

unsigned x86p_engine_all_bits(void) {
  return (1u << (unsigned)kEngineCount) - 1u;
}

/* The available arms, spelled, for a refusal that has to say what the caller
   COULD have asked for. Built into the caller's buffer: this is the failure
   path, so it allocates nothing and cannot itself fail. */
static void available_list(unsigned available, char *buf, unsigned len) {
  int i, first = 1;
  if (!buf || len == 0) {
    return;
  }
  buf[0] = '\0';
  for (i = 0; i < kEngineCount; i++) {
    if (!(available & x86p_engine_bit((X86pEngine)i))) {
      continue;
    }
    if (!first) {
      strncat(buf, " | ", len - strlen(buf) - 1);
    }
    strncat(buf, kEngines[i].name, len - strlen(buf) - 1);
    first = 0;
  }
  if (first) {
    /* An empty list is a real answer and a distinct defect: the consumer linked
       no engine at all. "(none)" says that; an empty string reads as a
       formatting bug and sends the reader to the wrong place. */
    strncat(buf, "(none)", len - strlen(buf) - 1);
  }
}

int x86p_engine_resolve(
    const char *request, X86pEngine fallback, unsigned available, X86pEngine *out, char *reason, unsigned reason_len) {
  char avail[128];
  X86pEngine want;

  if (reason && reason_len) {
    reason[0] = '\0';
  }
  if (!out) {
    return 0;
  }
  available_list(available, avail, sizeof avail);

  if (!request || !*request) {
    /* Unset is not "anything goes": a consumer whose declared fallback is not
       among its own linked arms is misconfigured, and finding that out at
       startup is far cheaper than at the first guest call. */
    if (!(available & x86p_engine_bit(fallback))) {
      if (reason && reason_len) {
        snprintf(reason,
                 reason_len,
                 "no engine requested and the default '%s' is not one this build can run "
                 "(it has: %s)",
                 x86p_engine_name(fallback),
                 avail);
      }
      return 0;
    }
    *out = fallback;
    return 1;
  }

  if (!x86p_engine_parse(request, &want)) {
    if (reason && reason_len) {
      snprintf(reason,
               reason_len,
               "'%s' names none of the %d engines that exist (%s)",
               request,
               kEngineCount,
               x86p_engine_name_list());
    }
    return 0;
  }
  if (!(available & x86p_engine_bit(want))) {
    /* Spelled right, not linked. Kept separate from a misspelling because the
       two send the reader to completely different places: one is a typo, the
       other is a build that does not contain the engine being asked for. */
    if (reason && reason_len) {
      snprintf(reason,
               reason_len,
               "'%s' is a real engine but this build cannot run it (it has: %s)",
               x86p_engine_name(want),
               avail);
    }
    return 0;
  }
  *out = want;
  return 1;
}

X86pEngine x86p_engine_from_env(const char *var, X86pEngine fallback, unsigned available) {
  char reason[256];
  X86pEngine e = fallback;
  const char *request = var ? getenv(var) : NULL;

  if (x86p_engine_resolve(request, fallback, available, &e, reason, sizeof reason)) {
    return e;
  }
  fprintf(stderr,
          "%s: %s.\n"
          "Refusing to run: falling back silently would make 'the engine was selected' and\n"
          "'the engine never ran' the same run, which is the one measurement this migration\n"
          "depends on.\n",
          var ? var : "(engine)",
          reason);
  abort();
}
