#ifndef SURGE_PACK_EXPORT_H
#define SURGE_PACK_EXPORT_H

#include <stddef.h>

#include "core/str.h"

struct Arena;
struct Log;
struct Os;
struct Refset;
struct Repository;

/**
 * A pack of a repository's exported objects, written to a temporary directory of its own. Each path
 * is followed by a `NUL`, so `pack_export_remove` needs no arena.
 */
struct PackExport {
  /** Temporary directory holding the pack and its idx, or empty when nothing was written. */
  struct Str dir_path;

  /** Hex name of the pack, as in `pack-<id>.pack`, or empty when nothing was written. */
  struct Str pack_id;

  /** Path of the pack in `dir_path`. */
  struct Str pack_path;

  /** Path of the idx in `dir_path`. */
  struct Str idx_path;

  /** Size of the pack in bytes. */
  ptrdiff_t pack_len;

  /** Size of the idx in bytes. */
  ptrdiff_t idx_len;
};

/**
 * @brief Writes one pack of every object that a branch or a tag of `refset` reaches, with its idx,
 *        into a new directory below `TMPDIR`, or `/tmp` when it is unset.
 *
 * A revwalk from the commit of every ref feeds the history, and each ref's own object goes in
 * recursively too, because a tag can name a tag, a tree or a blob, which a revwalk skips. The pack
 * builder runs on one thread, so the same objects make the same pack. A repository with no refs
 * writes nothing. The source repository is only read.
 *
 * The directory is outside `os/os.h`, the documented exception that fram's video frames are too,
 * and `pack_export_remove` removes it. An out-of-memory exit or a signal leaves it in `TMPDIR`.
 *
 * @param export     Export that receives the paths and sizes, zeroed. Must not be `NULL`.
 * @param repository Open repository. Must not be `NULL`.
 * @param refset     Refs to export, loaded from `repository`. Must not be `NULL`.
 * @param os         Host the sizes are read on, the real one. Must not be `NULL`.
 * @param perm       Arena that owns the paths. Must not be `NULL`.
 * @param log        Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the directory or the pack cannot be written.
 */
int pack_export_build(struct PackExport* export,
                      const struct Repository* repository,
                      const struct Refset* refset,
                      struct Os* os,
                      struct Arena* perm,
                      struct Log* log) __attribute__((nonnull(1, 2, 3, 4, 5, 6)));

/**
 * @brief Removes the pack, the idx and the temporary directory of `export`, as a best effort.
 *
 * @param export Export to remove, which may have written nothing. Must not be `NULL`.
 */
void pack_export_remove(const struct PackExport* export) __attribute__((nonnull(1)));

/**
 * @brief Returns the `objects/info/packs` file that lists the pack `pack_id`, as
 *        `git update-server-info` writes it, or empty text when `pack_id` is empty.
 *
 * @param pack_id Hex name of the pack, or empty for a repository with nothing to export.
 * @param arena   Arena that owns the text. Must not be `NULL`.
 * @return The text.
 */
struct Str pack_export_format_packs(struct Str pack_id, struct Arena* arena)
    __attribute__((nonnull(2)));

/**
 * @brief Returns the output path of a pack or its idx in a repo's clone export, relative to
 *        `output_dir`: `<r>.git/objects/pack/pack-<id>.<extension>`.
 *
 * @param repo_name Name of the repo.
 * @param pack_id   Hex name of the pack.
 * @param extension `pack` or `idx`.
 * @param arena     Arena that owns the path. Must not be `NULL`.
 * @return The path.
 */
struct Str pack_export_output_path(struct Str repo_name,
                                   struct Str pack_id,
                                   struct Str extension,
                                   struct Arena* arena) __attribute__((nonnull(4)));

#endif
