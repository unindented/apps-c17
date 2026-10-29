#ifndef SURGE_REFSET_H
#define SURGE_REFSET_H

#include <stdbool.h>
#include <stddef.h>

#include "core/str.h"
#include "domain/repo_model.h"

struct Arena;
struct Log;
struct Repository;

/**
 * @brief Loads every branch and tag of `repository`, peeling each tag to the object it names.
 *
 * Only the refs below `refs/heads/` and `refs/tags/` are loaded, and a symbolic ref among them is
 * skipped, because a dumb-HTTP clone reads every listed ref as an object id. The load refuses a
 * repository whose branches collide after ASCII case folding, which a case-insensitive filesystem
 * cannot hold as pages, and a ref that is a `/`-prefix of another, which only a hand-written
 * `packed-refs` can hold, and a clone could not store. Each message names both refs.
 *
 * @param refset     Refset that receives the refs, zeroed. Must not be `NULL`.
 * @param repository Open repository. Must not be `NULL`.
 * @param perm       Arena that owns the refs. Must not be `NULL`.
 * @param log        Log that receives every failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when a ref cannot be read or peeled, or two refs collide.
 */
int refset_load(struct Refset* refset,
                const struct Repository* repository,
                struct Arena* perm,
                struct Log* log) __attribute__((nonnull(1, 2, 3, 4)));

/**
 * @brief Finds the branch named `short_name`.
 *
 * @param refset     Loaded refs. Must not be `NULL`.
 * @param short_name Branch name below `refs/heads/`.
 * @return The branch, or `NULL` when the repository has none of that name.
 */
const struct Ref* refset_find_branch(const struct Refset* refset, struct Str short_name)
    __attribute__((nonnull(1)));

/**
 * @brief Returns the `info/refs` file of a dumb-HTTP clone, as `git update-server-info` writes it.
 *
 * One `<oid>\t<name>` line per ref, sorted by name, and after an annotated tag a
 * `<peeled-oid>\t<name>^{}` line.
 *
 * @param refset Loaded refs. Must not be `NULL`.
 * @param arena  Arena that owns the text. Must not be `NULL`.
 * @return The text, empty for a repository with no refs.
 */
struct Str refset_format_info_refs(const struct Refset* refset, struct Arena* arena)
    __attribute__((nonnull(1, 2)));

#endif
