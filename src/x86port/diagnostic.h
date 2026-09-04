/* One configurable reporting boundary for the shipping x86port library. */
#ifndef X86PORT_DIAGNOSTIC_H
#define X86PORT_DIAGNOSTIC_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum X86pDiagnosticLevel {
  kX86pDiagnosticError = 0,
  kX86pDiagnosticFatal,
} X86pDiagnosticLevel;

typedef struct X86pDiagnostic {
  X86pDiagnosticLevel level;
  const char *component;
  const char *message;
} X86pDiagnostic;

typedef void (*X86pDiagnosticSink)(const X86pDiagnostic *diagnostic, void *user);

/*
 * Replace the process-wide library sink. Configure this during application
 * composition, before execution threads start. A null sink restores the
 * default standard-error sink. The library never reads process configuration.
 */
void x86p_diagnostic_set_sink(X86pDiagnosticSink sink, void *user);

/* Route a complete event through the configured sink. */
void x86p_diagnostic_report(const X86pDiagnostic *diagnostic);

/*
 * Report a violated programming contract through the same sink, then abort.
 * Guest faults and unsupported instructions use typed runtime statuses instead
 * and must never come through this fatal boundary.
 */
#if defined(__cplusplus)
[[noreturn]] void x86p_diagnostic_fatalf(const char *component, const char *format, ...);
#else
_Noreturn void x86p_diagnostic_fatalf(const char *component, const char *format, ...);
#endif

#ifdef __cplusplus
}
#endif

#endif
