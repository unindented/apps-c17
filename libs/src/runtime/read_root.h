#ifndef APPS_RUNTIME_READ_ROOT_H
#define APPS_RUNTIME_READ_ROOT_H

#include <stdbool.h>
#include <stddef.h>

#include "core/str.h"

struct Log;
struct Os;

/** A directory that a build reads, as its configuration names it. */
struct ReadRoot {
  /** Configured path, already slash-trimmed by the config load. */
  struct Str dir_path;

  /** Config key naming `dir_path`, which every diagnostic quotes, so the user knows what to
      edit. */
  struct Str key;

  /** Whether a build may legitimately not have this directory. */
  bool is_optional;

  /** Whether the config load rejected `key`, so `dir_path` is its default and is not checked. */
  bool is_rejected;

  /**
   * Receives whether the build can read the directory: it is a directory, or it is optional and
   * missing. `false` for a rejected key.
   */
  bool is_usable;
};

/**
 * @brief Checks that each directory of `roots` exists and is a directory, and reports each one that
 *        does not.
 *
 * A root whose key the config load rejected is skipped, because it holds its default and the load
 * has reported it. An optional root that does not exist is no error. Every other root is checked,
 * so one run reports every unusable root:
 * `failed to resolve config directory '<key>': <cause> ('<path>')`, where the cause is the system's
 * for a root that cannot be inspected, and `not a directory` for one that is something else.
 *
 * @param roots      Roots to check, each of which receives `is_usable`. Must not be `NULL` unless
 *                   `root_count` is `0`.
 * @param root_count Number of entries in `roots`.
 * @param os         Host the roots are on. Must not be `NULL`.
 * @param log        Log that receives every failure. Must not be `NULL`.
 * @return `0` when every root that was checked is usable, or `-1` otherwise.
 */
int read_root_require(struct ReadRoot* roots, ptrdiff_t root_count, struct Os* os, struct Log* log)
    __attribute__((nonnull(3, 4)));

#endif
