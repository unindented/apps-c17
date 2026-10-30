#ifndef APPS_OUTPUT_OUTPUT_PRUNER_H
#define APPS_OUTPUT_OUTPUT_PRUNER_H

#include <stddef.h>

#include "core/arena.h"
#include "core/str.h"

struct Log;
struct Os;
struct OutputWriter;

/**
 * @brief Removes every entry below `output_dir` that the build did not write and does not pass
 *        through, so the output tree holds only what the current inputs produce.
 *
 * Run it once every output is written. An entry is kept when following it reaches a file the
 * manifest names or a directory between `output_dir` and one, compared by `(device, inode)` rather
 * than by name: a case-insensitive filesystem keeps the case an earlier build created, so a name
 * comparison would remove a current output whose name changed only in case. A symlink is removed as
 * a link and never descended, so what it points to is never touched, and removing a hard link
 * leaves the file under its other names. A stale directory is removed once everything inside it is.
 * No input root can be inside `output_dir`, because `manifest_builder_check_output_dir` rejects
 * that layout, except one bind-mounted there, which the prune empties.
 *
 * Every removal goes through `writer`, so a dry run removes nothing and records what it would
 * remove instead, and an `output_dir` that does not exist, which only a dry run leaves, has nothing
 * to remove.
 *
 * A failure to list a directory or remove an entry is reported, and the prune goes on with the rest
 * of the tree. A directory that a failure left non-empty is not reported again, because its removal
 * would fail for the same cause.
 *
 * @param writer            Writer of the build, whose manifest names every output path, each joined
 *                          onto `output_dir`, and which makes the removals. Must not be `NULL`.
 * @param output_dir        Directory to prune.
 * @param os                Host the output tree is read from. Must not be `NULL`.
 * @param scratch           Arena for the kept identities and the listings.
 * @param removed_count_out Receives the number of entries removed, directories included. Must not
 *                          be `NULL`.
 * @param log               Log that receives every failure. Must not be `NULL`.
 * @return `0` when every stale entry was removed, or `-1` when one could not be listed or removed.
 */
int output_pruner_prune(struct OutputWriter* writer,
                        struct Str output_dir,
                        struct Os* os,
                        struct Arena scratch,
                        ptrdiff_t* removed_count_out,
                        struct Log* log) __attribute__((nonnull(1, 3, 5, 6)));

#endif
