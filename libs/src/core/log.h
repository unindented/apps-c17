#ifndef APPS_CORE_LOG_H
#define APPS_CORE_LOG_H

#include <stddef.h>

#include "core/buf.h"
#include "core/str.h"

/**
 * Growable diagnostic log: every error and every warning a run finds, one line each, in the order
 * they were found.
 *
 * Start one as `(struct Log){.buf = {.arena = arena}}`. The arena must be one nothing else
 * allocates from through a copy while the log is in use, because a function given a copy of an
 * arena by value would hand out the same bytes the log grows into. A region of its own is the
 * simple way to guarantee that. A message is never truncated, so it can name every value in full,
 * and a caller that keeps going after an error collects every error in one run and checks `errors`
 * before doing anything it cannot undo.
 *
 * Two logs never share an arena, so each thread can keep its own, and the owner concatenates them
 * in a fixed order once the threads are done.
 *
 * A warning is a problem that does not stop the run, so it is kept apart from the errors: a check
 * of `errors` never sees one, and a command prints its warnings after its errors.
 */
struct Log {
  /** Error messages appended so far, each ending in `\n`. Print it as it stands. */
  struct Buf buf;

  /** Number of errors `log_error` recorded. */
  ptrdiff_t errors;

  /**
   * Warning messages appended so far, each ending in `\n`. A line `log_warn` records starts with
   * `warning: `, and a line appended through `log_warning_buf`, such as a count of warnings already
   * recorded, does not. It grows in the arena of `buf`, which the first warning gives it, so a log
   * started with only `buf` takes warnings too.
   */
  struct Buf warning_buf;

  /** Number of warnings `log_warn` recorded. */
  ptrdiff_t warnings;
};

/**
 * @brief Records one error: appends the text that the format produces as one line, and counts it.
 *
 * Use it as `log_error(log, "failed to read '%.*s': %.*s", STR_FMT(path), STR_FMT(reason))`. The
 * compiler checks the arguments against the format, as for `buf_printf`. Returns `-1`, so an action
 * can write `return log_error(log, ...);`.
 *
 * @param log    Log to append to. Must not be `NULL`.
 * @param format `printf` format of the message, without the newline. Must not be `NULL`.
 * @param ...    Arguments that the format converts.
 * @return `-1` always.
 */
int log_error(struct Log* log, const char* format, ...)
    __attribute__((format(printf, 2, 3), nonnull(1, 2)));

/**
 * @brief Records one error found inside `file`: the message, then ` (in '<file>')`, as one line.
 *
 * This is the attribution for a failure located inside a file, such as a parse error, as in
 * `frontmatter key 'draft' must be a boolean (in 'content/post.md')`. An empty `file` adds nothing,
 * so a caller with no file to name passes `(struct Str){0}`.
 *
 * @param log    Log to append to. Must not be `NULL`.
 * @param file   File the error was found in, or empty to name none.
 * @param format `printf` format of the message, without the attribution. Must not be `NULL`.
 * @param ...    Arguments that the format converts.
 * @return `-1` always.
 */
int log_error_in(struct Log* log, struct Str file, const char* format, ...)
    __attribute__((format(printf, 3, 4), nonnull(1, 3)));

/**
 * @brief Records one warning: appends `warning: ` and the text that the format produces as one line
 *        of `warning_buf`, and counts it in `warnings`.
 *
 * A warning leaves `errors` as it is, so a run that has only warnings still succeeds.
 *
 * @param log    Log to append to. Must not be `NULL`.
 * @param format `printf` format of the message, without the prefix and the newline. Must not be
 *               `NULL`.
 * @param ...    Arguments that the format converts.
 */
void log_warn(struct Log* log, const char* format, ...)
    __attribute__((format(printf, 2, 3), nonnull(1, 2)));

/**
 * @brief Appends every error and every warning of `source` to `log`, and adds both counts.
 *
 * @param log    Log to append to. Must not be `NULL`.
 * @param source Log whose messages are copied. Left unchanged. Must not be `NULL`.
 */
void log_append(struct Log* log, const struct Log* source) __attribute__((nonnull(1, 2)));

/**
 * @brief Appends every error of `source` to `log`, and adds its error count.
 *
 * @param log    Log to append to. Must not be `NULL`.
 * @param source Log whose errors are copied. Left unchanged. Must not be `NULL`.
 */
void log_append_errors(struct Log* log, const struct Log* source) __attribute__((nonnull(1, 2)));

/**
 * @brief Appends every warning of `source` to `log`, and adds its warning count.
 *
 * @param log    Log to append to. Must not be `NULL`.
 * @param source Log whose warnings are copied. Left unchanged. Must not be `NULL`.
 */
void log_append_warnings(struct Log* log, const struct Log* source) __attribute__((nonnull(1, 2)));

/**
 * @brief Returns the buffer of `log`'s warnings, after giving it the arena of `buf` when it has
 *        none yet.
 *
 * For a caller that appends a line that is no warning of its own, such as a count of warnings
 * already recorded, and so must not raise `warnings`.
 *
 * @param log Log whose warning buffer is returned. Must not be `NULL`.
 * @return `&log->warning_buf`, ready to append to.
 */
struct Buf* log_warning_buf(struct Log* log) __attribute__((nonnull(1), returns_nonnull));

#endif
