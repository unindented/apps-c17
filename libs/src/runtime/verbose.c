#include "runtime/verbose.h"

#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#include "core/arena.h"
#include "core/str.h"
#include "os/os.h"

void verbose_printf(const struct Verbose* verbose, const char* format, ...) {
  if (!verbose->is_enabled) {
    return;
  }
  // The first pass measures the line and the second writes it, as `buf_printf` does. Starting the
  // list twice needs no `va_copy`, and forwarding it to a formatter that copies it with `va_copy`
  // instead makes Clang's analyzer report a false `va_end` on AArch64 Linux.
  va_list args;
  va_start(args, format);
  const int line_len = vsnprintf(NULL, 0, format, args);
  va_end(args);
  assert(line_len >= 0);
  struct Arena scratch = *verbose->scratch;
  char* line = arena_new(&scratch, (ptrdiff_t)line_len + 1, char);
  va_start(args, format);
  (void)vsnprintf(line, (size_t)line_len + 1, format, args);
  va_end(args);
  // The terminator `vsnprintf` wrote becomes the newline, so the line prints in one call.
  line[line_len] = '\n';
  (void)os_print(verbose->os, 2, (struct Str){line, (ptrdiff_t)line_len + 1});
}
