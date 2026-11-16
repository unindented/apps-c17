#ifndef SURGE_GITMODULES_H
#define SURGE_GITMODULES_H

#include <stddef.h>

#include "core/str.h"

struct Arena;

/** One submodule of a `.gitmodules` file that has a `path`. */
struct Gitmodule {
  /** Name, the subsection of its `[submodule "<name>"]`, as written. */
  struct Str name;

  /** Its `path`, the path of its gitlink in the tree. */
  struct Str path;

  /** Its `url` as written, or empty when it has none. */
  struct Str url;
};

/**
 * @brief Parses the text of a `.gitmodules` file with libgit2's config parser, which handles
 *        git-config quoting, escapes, continuation lines, comments and the case of section names,
 *        and reads no file an `include.path` names.
 *
 * libgit2 must be set up. A key given twice in one submodule keeps its last value, as git does.
 *
 * @param text           The file's bytes.
 * @param perm           Arena that owns the submodules. Must not be `NULL`.
 * @param gitmodules_out Receives the submodules with a `path`, sorted bytewise by path, so the
 *                       submodules of one path are next to each other. Must not be `NULL`.
 * @param count_out      Receives the number of submodules. Must not be `NULL`.
 * @param reason_out     Receives libgit2's message when the text does not parse, in `perm`. Must
 *                       not be `NULL`.
 * @return `0` on success, or `-1` when the text does not parse.
 */
int gitmodules_parse(struct Str text,
                     struct Arena* perm,
                     struct Gitmodule** gitmodules_out,
                     ptrdiff_t* count_out,
                     struct Str* reason_out) __attribute__((nonnull(2, 3, 4, 5)));

#endif
