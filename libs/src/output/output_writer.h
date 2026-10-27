#ifndef APPS_OUTPUT_OUTPUT_WRITER_H
#define APPS_OUTPUT_OUTPUT_WRITER_H

#include <stdbool.h>

#include "core/arena.h"
#include "core/path_list.h"
#include "core/str.h"
#include "os/os.h"

struct Log;
struct Manifest;

/**
 * Every change one build makes under `output_dir`. A dry run withholds each change and a real build
 * makes it, and either way each output path is marked written or kept.
 *
 * Every site that changes `output_dir` goes through it, so a dry run cannot miss one: a write that
 * bypasses it leaves its path pending in the manifest, which `output_writer_report` rejects.
 */
struct OutputWriter {
  /** Every output path of the build, whose entries receive the marks. */
  struct Manifest* manifest;

  /** Host the changes are made on. */
  struct Os* os;

  /** Whether the changes are withheld. */
  bool is_dry_run;

  /** Paths a dry run's prune would have removed, in `perm`, in the order the prune met them. */
  struct PathList removed_paths;

  /** The main thread's perm, which owns `removed_paths`. */
  struct Arena* perm;
};

/**
 * @brief Marks `file_path` written and, unless the build is a dry run, writes `source` to it as
 *        `os_write` does.
 *
 * Jobs running at once may call it, each for a path of its own. A dry run never reads `source`.
 *
 * @param writer    Writer of the build. Must not be `NULL`.
 * @param file_path Output path to write, as the manifest records it.
 * @param source    Bytes, or a file to stream, as `os_write` takes them.
 * @param operation Operation leading the failure message.
 * @param log       Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the manifest does not name `file_path` or the write fails.
 */
int output_writer_write(const struct OutputWriter* writer,
                        struct Str file_path,
                        struct OsSource source,
                        struct Str operation,
                        struct Log* log) __attribute__((nonnull(1, 5)));

/**
 * @brief Marks `file_path` kept, leaving the file as it is.
 *
 * A keep reads no file and writes none, so its one failure is a site that names a path the manifest
 * does not record, which its message names.
 *
 * @param writer    Writer of the build. Must not be `NULL`.
 * @param file_path Output path to keep, as the manifest records it.
 * @param log       Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the manifest does not name `file_path`.
 */
int output_writer_keep(const struct OutputWriter* writer, struct Str file_path, struct Log* log)
    __attribute__((nonnull(1, 3)));

/**
 * @brief Reports whether a source matches what the build cache recorded for it.
 *
 * @param name        Path of the source below the root of the copies.
 * @param source_stat What `os_stat` reported for the source. Must not be `NULL`.
 * @param userdata    Opaque pointer the copy was given.
 * @return `true` when the build cache records the source with that stat.
 */
typedef bool (*OutputWriterIsUnchangedFn)(struct Str name,
                                          const struct OsStat* source_stat,
                                          void* userdata);

/**
 * @brief Copies each of `source_paths` to the same path below `output_dir` that it has below
 *        `root_dir`, or keeps a copy that is current.
 *
 * A copy is current when `is_unchanged` says that its source matches what the build cache recorded,
 * and the output exists as a file of the source's size. A current copy is kept, as
 * `output_writer_keep` keeps it, so its modification time and a deploy's upload are left alone, and
 * any other is streamed from its source. A tool that changes a file but keeps its size and
 * modification time defeats the check, as it does every row of a build cache. Each copy names its
 * source to `oom_set_job`, which names no file once every copy succeeds.
 *
 * @param writer       Writer of the build. Must not be `NULL`.
 * @param source_paths Files to copy, each below `root_dir`. Must not be `NULL`.
 * @param source_stats Stat of each of `source_paths`, parallel to them. Must not be `NULL` unless
 *                     `is_unchanged` is `NULL`.
 * @param is_unchanged Asked for each source whether it matches the build cache, or `NULL` when none
 *                     does, as in a build that ignores its cache.
 * @param userdata     Opaque pointer passed to `is_unchanged`.
 * @param root_dir     Directory the sources are below.
 * @param output_dir   Directory the copies go below.
 * @param scratch      Arena for each output path.
 * @param operation    Operation leading the message of a copy that fails.
 * @param log          Log that receives the first failure. Must not be `NULL`.
 * @return `0` on success, or `-1` on the first copy or keep that fails.
 */
int output_writer_copy_files(const struct OutputWriter* writer,
                             const struct PathList* source_paths,
                             const struct OsStat* source_stats,
                             OutputWriterIsUnchangedFn is_unchanged,
                             void* userdata,
                             struct Str root_dir,
                             struct Str output_dir,
                             struct Arena scratch,
                             struct Str operation,
                             struct Log* log) __attribute__((nonnull(1, 2, 10)));

/**
 * @brief Creates `dir_path` and any missing parents, as `os_mkdirs` does, unless the build is a dry
 *        run.
 *
 * @param writer    Writer of the build. Must not be `NULL`.
 * @param dir_path  Directory to create.
 * @param operation Operation leading the failure message.
 * @param log       Log that receives the failure. Must not be `NULL`.
 * @return `0` on success or in a dry run, or `-1` when a component cannot be created.
 */
int output_writer_mkdirs(const struct OutputWriter* writer,
                         struct Str dir_path,
                         struct Str operation,
                         struct Log* log) __attribute__((nonnull(1, 4)));

/**
 * @brief Removes the file or symlink `file_path` names, as `os_remove` does, or in a dry run
 *        records it in `removed_paths`.
 *
 * @param writer    Writer of the build. Must not be `NULL`.
 * @param file_path Path of the file or symlink to remove.
 * @param operation Operation leading the failure message.
 * @param log       Log that receives the failure. Must not be `NULL`.
 * @return `0` on success or in a dry run, or `-1` when the removal fails.
 */
int output_writer_remove(struct OutputWriter* writer,
                         struct Str file_path,
                         struct Str operation,
                         struct Log* log) __attribute__((nonnull(1, 4)));

/**
 * @brief Removes the empty directory `dir_path` names, as `os_remove_dir` does, or in a dry run
 *        records it in `removed_paths`.
 *
 * @param writer    Writer of the build. Must not be `NULL`.
 * @param dir_path  Path of the directory to remove.
 * @param operation Operation leading the failure message.
 * @param log       Log that receives the failure. Must not be `NULL`.
 * @return `0` on success or in a dry run, or `-1` when the removal fails.
 */
int output_writer_remove_dir(struct OutputWriter* writer,
                             struct Str dir_path,
                             struct Str operation,
                             struct Log* log) __attribute__((nonnull(1, 4)));

/**
 * @brief Prints to standard output what a dry run would have written, kept, and removed.
 *
 * Prints a `write` or `keep` line for each output path in manifest order, then a `remove` line for
 * each path in `removed_paths`, sorted bytewise, each a mark, a tab, and the path. The text is
 * formatted whole and printed in one `os_print` call. A path still pending is one a site wrote
 * without the writer, so the dry run may already have changed `output_dir` and a listing would
 * leave that change out: each such path is reported and nothing is printed.
 *
 * @param writer  Writer of a dry run whose writes and prune are done, whose `removed_paths` this
 *                sorts. Must not be `NULL`.
 * @param scratch Arena for the listing.
 * @param log     Log that receives every pending path, or the failed print. Must not be `NULL`.
 * @return `0` once the listing is printed, or `-1` when an output path was neither written nor
 *         kept, or the print fails.
 */
int output_writer_report(struct OutputWriter* writer, struct Arena scratch, struct Log* log)
    __attribute__((nonnull(1, 3)));

#endif
