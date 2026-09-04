/* Central diagnostic dispatch for the shipping C library. */
#include "diagnostic.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

enum { kDiagnosticMessageCapacity = 512 };

static atomic_flag g_sink_lock = ATOMIC_FLAG_INIT;
static X86pDiagnosticSink g_sink;
static void *g_sink_user;

static void lock_sink(void) {
  while (atomic_flag_test_and_set_explicit(&g_sink_lock, memory_order_acquire)) {
  }
}

static void unlock_sink(void) {
  atomic_flag_clear_explicit(&g_sink_lock, memory_order_release);
}

static void default_sink(const X86pDiagnostic *diagnostic, void *user) {
  const char *component = "library";
  const char *message = "unspecified diagnostic";
  const char *level = "error";

  (void)user;
  if (diagnostic != NULL) {
    if (diagnostic->component != NULL) {
      component = diagnostic->component;
    }
    if (diagnostic->message != NULL) {
      message = diagnostic->message;
    }
    if (diagnostic->level == kX86pDiagnosticFatal) {
      level = "fatal";
    }
  }
  fprintf(stderr, "x86port[%s:%s]: %s\n", component, level, message);
}

void x86p_diagnostic_set_sink(X86pDiagnosticSink sink, void *user) {
  lock_sink();
  g_sink = sink;
  g_sink_user = sink != NULL ? user : NULL;
  unlock_sink();
}

void x86p_diagnostic_report(const X86pDiagnostic *diagnostic) {
  static const X86pDiagnostic kUnspecifiedDiagnostic = {
      .level = kX86pDiagnosticError,
      .component = "library",
      .message = "unspecified diagnostic",
  };
  X86pDiagnosticSink sink;
  void *user;

  lock_sink();
  sink = g_sink != NULL ? g_sink : default_sink;
  user = g_sink_user;
  unlock_sink();
  sink(diagnostic != NULL ? diagnostic : &kUnspecifiedDiagnostic, user);
}

_Noreturn void x86p_diagnostic_fatalf(const char *component, const char *format, ...) {
  char message[kDiagnosticMessageCapacity];
  X86pDiagnostic diagnostic;
  va_list args;

  va_start(args, format);
  if (format == NULL) {
    message[0] = '\0';
  } else {
    (void)vsnprintf(message, sizeof message, format, args);
  }
  va_end(args);

  diagnostic.level = kX86pDiagnosticFatal;
  diagnostic.component = component;
  diagnostic.message = message;
  x86p_diagnostic_report(&diagnostic);
  abort();
}
