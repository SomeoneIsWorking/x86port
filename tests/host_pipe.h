/*
 * host_pipe.h -- read a shell command's output, the same way on every host.
 *
 * On Windows _popen hands the line to `cmd /c`, which strips the first and
 * last quote of a line that starts with one. A command naming a quoted engine
 * and quoted arguments -- "node" "oracle.js" "manifest.json" -- then reaches
 * the shell as node" "oracle.js" "manifest.json and runs nothing. Wrapping the
 * whole line in one extra pair of quotes gives cmd those two to strip.
 */
#ifndef X86PORT_TESTS_HOST_PIPE_H
#define X86PORT_TESTS_HOST_PIPE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline FILE *open_pipe(const char *command) {
#if defined(_WIN32)
  const size_t len = strlen(command);
  char *wrapped = (char *)malloc(len + 3u);
  if (!wrapped) {
    return NULL;
  }
  wrapped[0] = '"';
  memcpy(wrapped + 1, command, len);
  wrapped[len + 1u] = '"';
  wrapped[len + 2u] = '\0';
  FILE *pipe = _popen(wrapped, "r");
  free(wrapped);
  return pipe;
#else
  return popen(command, "r");
#endif
}

static inline int close_pipe(FILE *pipe) {
#if defined(_WIN32)
  return _pclose(pipe);
#else
  return pclose(pipe);
#endif
}

#endif
