#ifndef APPS_RUNTIME_VERBOSE_H
#define APPS_RUNTIME_VERBOSE_H

#include <stdbool.h>

#include "core/arena.h"

struct Os;

/** Where a command's status lines go, and whether it prints them. */
struct Verbose {
  /** Host the lines print on, to `stderr`. Must not be `NULL` when `is_enabled`. */
  struct Os* os;

  /** Whether the command prints its status lines, as `--verbose` asks. */
  bool is_enabled;

  /**
   * Scratch of the thread that prints, of which each line takes a copy, so a line is dead once it
   * is printed. Must not be `NULL` when `is_enabled`.
   */
  const struct Arena* scratch;
};

/**
 * @brief Prints a formatted status line to `stderr`, with its newline, when `verbose` is enabled.
 *
 * The line prints in one `os_print` call, so the lines of two threads do not interleave within a
 * line.
 *
 * @param verbose Where the line goes. Must not be `NULL`.
 * @param format  `printf` format of the line, without the newline. Must not be `NULL`.
 * @param ...     Arguments that the format converts.
 */
void verbose_printf(const struct Verbose* verbose, const char* format, ...)
    __attribute__((format(printf, 2, 3), nonnull(1, 2)));

#endif
