#ifndef SURGE_CHANGESET_H
#define SURGE_CHANGESET_H

#include "core/str.h"

struct Arena;
struct Changeset;
struct Log;
struct Repository;
struct SurgeLimits;

/**
 * @brief Diffs the commit `commit_oid` against its first parent, or against nothing for a root
 *        commit, with exact-match rename detection that finds every exact rename.
 *
 * A file whose blob is over `blob_len_max` on either side shows no lines, and git's binary
 * detection decides the others. The repository's own `diff.renameLimit` is not read. The files come
 * in the changed-files tree's order, and the caps apply in it: a file past `diff_file_count_max` is
 * listed without its diff, and the file whose lines would pass `diff_line_count_max` or
 * `diff_len_max` is cut short there, with every file after it listed without its diff.
 *
 * @param changeset  Changeset to fill. Must not be `NULL`.
 * @param repository Open repository. Must not be `NULL`.
 * @param commit_oid Hex id of a commit.
 * @param limits     The config's limits. Must not be `NULL`.
 * @param perm       Arena that owns the changes. Must not be `NULL`.
 * @param log        Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when an object cannot be read.
 */
int changeset_load(struct Changeset* changeset,
                   const struct Repository* repository,
                   struct Str commit_oid,
                   const struct SurgeLimits* limits,
                   struct Arena* perm,
                   struct Log* log) __attribute__((nonnull(1, 2, 4, 5, 6)));

#endif
