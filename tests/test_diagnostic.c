#include "diagnostic.h"
#include "flags.h"

#include <stdio.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#endif

typedef struct Capture {
  int calls;
  X86pDiagnosticLevel level;
  char component[32];
  char message[256];
} Capture;

static int g_checks;
static int g_failures;

#define CHECK(condition)                                                                                               \
  do {                                                                                                                 \
    g_checks++;                                                                                                        \
    if (!(condition)) {                                                                                                \
      g_failures++;                                                                                                    \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                      \
    }                                                                                                                  \
  } while (0)

static void copy_text(char *destination, size_t capacity, const char *source) {
  if (capacity == 0u) {
    return;
  }
  if (source == NULL) {
    destination[0] = '\0';
    return;
  }
  (void)snprintf(destination, capacity, "%s", source);
}

static void capture_sink(const X86pDiagnostic *diagnostic, void *user) {
  Capture *capture = user;

  capture->calls++;
  capture->level = diagnostic->level;
  copy_text(capture->component, sizeof capture->component, diagnostic->component);
  copy_text(capture->message, sizeof capture->message, diagnostic->message);
}

static void test_injected_sink_receives_complete_event(void) {
  Capture capture = {0};
  X86pDiagnostic diagnostic = {
      .level = kX86pDiagnosticError,
      .component = "test",
      .message = "negative control",
  };

  x86p_diagnostic_set_sink(capture_sink, &capture);
  x86p_diagnostic_report(&diagnostic);
  CHECK(capture.calls == 1);
  CHECK(capture.level == kX86pDiagnosticError);
  CHECK(strcmp(capture.component, "test") == 0);
  CHECK(strcmp(capture.message, "negative control") == 0);

  x86p_diagnostic_report(NULL);
  x86p_diagnostic_set_sink(NULL, NULL);

  CHECK(capture.calls == 2);
  CHECK(capture.level == kX86pDiagnosticError);
  CHECK(strcmp(capture.component, "library") == 0);
  CHECK(strcmp(capture.message, "unspecified diagnostic") == 0);
}

#if defined(__unix__) || defined(__APPLE__)
static void pipe_sink(const X86pDiagnostic *diagnostic, void *user) {
  int descriptor = *(const int *)user;
  char line[320];
  int length =
      snprintf(line, sizeof line, "%d|%s|%s", (int)diagnostic->level, diagnostic->component, diagnostic->message);

  if (length > 0) {
    size_t count = (size_t)length < sizeof line ? (size_t)length : sizeof line - 1u;
    (void)write(descriptor, line, count);
  }
}
#endif

static void test_shipping_contract_failure_uses_sink_then_aborts(void) {
#if defined(__unix__) || defined(__APPLE__)
  int descriptors[2];
  pid_t pid;
  char captured[320] = {0};

  if (pipe(descriptors) != 0) {
    CHECK(0 && "pipe creation failed");
    return;
  }
  pid = fork();
  if (pid < 0) {
    CHECK(0 && "fork failed");
    (void)close(descriptors[0]);
    (void)close(descriptors[1]);
    return;
  }
  if (pid == 0) {
    X86pFlags flags = {0};

    (void)close(descriptors[0]);
    x86p_diagnostic_set_sink(pipe_sink, &descriptors[1]);
    x86p_flags_set(&flags, kX86pFlagsAdd, 1u, 2u, 3u, 3);
    _exit(0);
  }
  (void)close(descriptors[1]);
  if (pid > 0) {
    int status = 0;
    ssize_t length = read(descriptors[0], captured, sizeof captured - 1u);

    CHECK(length > 0);
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFSIGNALED(status));
    CHECK(strstr(captured, "1|flags|") != NULL);
    CHECK(strstr(captured, "operand width 3") != NULL);
  }
  (void)close(descriptors[0]);
#else
  printf("test_shipping_contract_failure_uses_sink_then_aborts: skipped (no fork)\n");
#endif
}

int main(void) {
  test_injected_sink_receives_complete_event();
  test_shipping_contract_failure_uses_sink_then_aborts();
  printf("diagnostic: %d check(s), %d failure(s)\n", g_checks, g_failures);
  return g_failures != 0;
}
