#include "core/log.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#include "core/buf.h"
#include "core/str.h"

/**
 * @brief Ends the error whose message was just appended to `log->buf`: appends the attribution to
 *        `file` and the newline, and counts the error.
 *
 * @param log  Log to append to. Must not be `NULL`.
 * @param file File the error was found in, or empty to name none.
 * @return `-1` always.
 */
static int log_error_finish(struct Log* log, struct Str file) __attribute__((nonnull(1)));

int log_error(struct Log* log, const char* format, ...) {
  // The first pass measures the message, and the second appends it, as `buf_printf` does.
  va_list args;
  va_start(args, format);
  const ptrdiff_t message_len = vsnprintf(NULL, 0, format, args);
  va_end(args);
  va_start(args, format);
  buf_vprintf_measured(&log->buf, message_len, format, args);
  va_end(args);
  return log_error_finish(log, (struct Str){0});
}

int log_error_in(struct Log* log, struct Str file, const char* format, ...) {
  // The first pass measures the message, and the second appends it, as `buf_printf` does.
  va_list args;
  va_start(args, format);
  const ptrdiff_t message_len = vsnprintf(NULL, 0, format, args);
  va_end(args);
  va_start(args, format);
  buf_vprintf_measured(&log->buf, message_len, format, args);
  va_end(args);
  return log_error_finish(log, file);
}

void log_warn(struct Log* log, const char* format, ...) {
  // The first pass measures the message, and the second appends it, as `buf_printf` does.
  struct Buf* warning_buf = log_warning_buf(log);
  buf_append(warning_buf, S("warning: "));
  va_list args;
  va_start(args, format);
  const ptrdiff_t message_len = vsnprintf(NULL, 0, format, args);
  va_end(args);
  va_start(args, format);
  buf_vprintf_measured(warning_buf, message_len, format, args);
  va_end(args);
  buf_append_char(warning_buf, '\n');
  log->warnings++;
}

void log_append(struct Log* log, const struct Log* source) {
  log_append_errors(log, source);
  log_append_warnings(log, source);
}

void log_append_errors(struct Log* log, const struct Log* source) {
  buf_append(&log->buf, buf_str(&source->buf));
  log->errors += source->errors;
}

void log_append_warnings(struct Log* log, const struct Log* source) {
  if (source->warning_buf.len > 0) {
    buf_append(log_warning_buf(log), buf_str(&source->warning_buf));
  }
  log->warnings += source->warnings;
}

struct Buf* log_warning_buf(struct Log* log) {
  if (log->warning_buf.arena == NULL) {
    log->warning_buf.arena = log->buf.arena;
  }
  return &log->warning_buf;
}

static int log_error_finish(struct Log* log, struct Str file) {
  if (file.len > 0) {
    buf_printf(&log->buf, " (in '%.*s')", STR_FMT(file));
  }
  buf_append_char(&log->buf, '\n');
  log->errors++;
  return -1;
}
