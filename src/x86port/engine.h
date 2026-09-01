/*
 * engine.h -- WHICH ENGINE EXECUTES GUEST CODE, decided in exactly one place.
 *
 * This is x86port's copy of the decision psxport settled in
 * runtime/recomp/engine_select.h (jit-common I001). The frameworks are
 * deliberately separate -- a shared engine interface across four guest
 * architectures was the thing the user ruled out -- but the VOCABULARY is
 * shared, because "substrate", "interpreter" and "jit" have to mean the same
 * thing in a report from either framework or the migration cannot be read.
 *
 * WHY IT IS A TYPE AND NOT A BOOLEAN. psxport had `int Core::use_interp`, and
 * two things were wrong with it. It could not name a third engine. And "any
 * non-zero means interpreter" routes an engine it does not know into the
 * shipping path, so "the new engine is selected" and "the new engine never ran"
 * produce an identical run -- the one failure mode a diagnostic must not have.
 * x86p_route() is therefore TOTAL and REFUSES an unknown value.
 *
 * WHAT IS DIFFERENT HERE, AND WHY IT NEEDED MORE THAN A COPY. In psxport all
 * three arms are compiled into every build, so selection only has to reject a
 * misspelling. In x86port they are not: the substrate is generated C that lives
 * in the CONSUMING TITLE, and the interpreter does not exist yet. An engine
 * that is spelled correctly but cannot run is exactly as dangerous as a
 * misspelled one, so selection takes an availability mask from the caller --
 * the consumer owns which arms it linked; this header owns the refusal.
 */
#ifndef X86PORT_ENGINE_H
#define X86PORT_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

/* The engines an x86-32 guest can be executed with. Substrate is 0 so an
   unconfigured consumer keeps the behaviour it had before this existed. */
typedef enum X86pEngine {
  kX86pEngineSubstrate = 0,   /* statically recompiled C -- what the migration is removing */
  kX86pEngineInterpreter = 1, /* decode-and-execute; the correctness authority */
  kX86pEngineJit = 2,         /* runtime translation; escalated to only on measurement */
  kX86pEngineCount            /* MUST stay last: the denominator of every exhaustive check */
} X86pEngine;

/* The shipping default while the substrate is still what runs the game. */
#define X86P_ENGINE_DEFAULT kX86pEngineSubstrate

/*
 * Where a call goes. Distinct from X86pEngine so a future engine that shares an
 * existing execution arm does not have to lie about its identity.
 */
typedef enum X86pRoute {
  kX86pRouteSubstrate,
  kX86pRouteInterpreter,
  kX86pRouteJit,
  kX86pRouteRefuse, /* not an engine this build knows -- fail fast, never guess */
  kX86pRouteCount
} X86pRoute;

/* The one routing decision. Total: every input, including a value outside the
   enum, produces a route, and the route for an unknown value is a refusal. */
X86pRoute x86p_route(X86pEngine e);

/* Name for diagnostics and refusals. Never returns null, including for a value
   outside the enum -- a null here would crash the code whose only job is to
   report that something was wrong. */
const char *x86p_engine_name(X86pEngine e);

/* The valid spellings joined with " | ", for a refusal message and for a
   caller's --help. Never null. */
const char *x86p_engine_name_list(void);

/*
 * Text -> engine. Returns 1 on success. Returns 0 and leaves *out UNTOUCHED for
 * anything unrecognised, including null and empty: an unrecognised engine name
 * must never resolve to a plausible engine. Case-insensitive.
 */
int x86p_engine_parse(const char *s, X86pEngine *out);

/* Membership in an availability mask. Defined for values outside the enum too,
   where it is always 0 -- an unknown engine is never available. */
unsigned x86p_engine_bit(X86pEngine e);

/* Every engine, for a caller that has linked all of them. */
unsigned x86p_engine_all_bits(void);

/*
 * Resolve the engine for this run from an environment variable.
 *
 * `available` is a mask of x86p_engine_bit() values: the arms the CONSUMER
 * actually linked. `fallback` is used when the variable is unset or empty, and
 * must itself be available.
 *
 * ABORTS, with a report naming the request, the spellings this build knows and
 * the arms it can actually run, when the variable names something unparseable
 * or something real but unlinked. Refusing is the point: silently falling back
 * to the substrate would make "I selected the interpreter" and "the interpreter
 * never ran" the same run, and that is the measurement this whole migration
 * turns on.
 */
X86pEngine x86p_engine_from_env(const char *var, X86pEngine fallback, unsigned available);

/*
 * The pure half of the above, so the refusal policy is testable without setting
 * environment variables or crashing a test process.
 *
 * Returns 1 and writes *out when `request` (null or empty meaning "unset")
 * names an available engine. Returns 0 and writes a human-readable reason into
 * `reason`/`reason_len` otherwise; *out is untouched. x86p_engine_from_env is
 * this function plus getenv plus abort, so what the test covers is what runs.
 */
int x86p_engine_resolve(
    const char *request, X86pEngine fallback, unsigned available, X86pEngine *out, char *reason, unsigned reason_len);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* X86PORT_ENGINE_H */
