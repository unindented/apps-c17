#ifndef APPS_OUTPUT_OUTPUT_WRITER_H
#define APPS_OUTPUT_OUTPUT_WRITER_H

#include <stdbool.h>

#include "core/arena.h"
#include "core/path_list.h"
#include "core/str.h"
#include "os/os.h"

struct Log;
struct Manifest;
struct OutputCache;

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

  /**
   * Directory every output path is below, which `output_writer_produce` and the output cache take
   * each output's relative path from.
   */
  struct Str output_dir;

  /** Whether the changes are withheld. */
  bool is_dry_run;

  /**
   * Whether `output_writer_produce` keeps nothing, neither an output its output cache vouches for
   * nor one whose bytes are unchanged, and writes every output.
   */
  bool is_forced;

  /** Paths a dry run's prune would have removed, in `perm`, in the order the prune met them. */
  struct PathList removed_paths;

  /** The main thread's perm, which owns `removed_paths`. */
  struct Arena* perm;
};

/**
 * @brief Marks `file_path` written and, unless the build is a dry run, writes `source` to it as
 *        `os_write` does.
 *
 * Jobs running at once may call it, each for a path of its own. A dry run never reads `source`. A
 * write that fails leaves the path unmarked, so the output cache records no row for it.
 *
 * The mark records the size of the bytes it writes, which the output cache reads instead of a stat.
 * A file that it streams, and every write of a dry run, records no size, so the output cache stats
 * that output.
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
 * does not record, which its message names. Its mark records no size, so the output cache stats a
 * kept output, as it must for one that `output_writer_produce` did not keep.
 *
 * @param writer    Writer of the build. Must not be `NULL`.
 * @param file_path Output path to keep, as the manifest records it.
 * @param log       Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the manifest does not name `file_path`.
 */
int output_writer_keep(const struct OutputWriter* writer, struct Str file_path, struct Log* log)
    __attribute__((nonnull(1, 3)));

/**
 * @brief Makes the bytes of one output for `output_writer_produce`.
 *
 * @param perm      Arena that owns the bytes until they are written. Must not be `NULL`.
 * @param scratch   Arena for temporaries.
 * @param bytes_out Receives the bytes on success. Must not be `NULL`.
 * @param log       Log that receives the failure. Must not be `NULL`.
 * @param userdata  Opaque pointer the produce was given.
 * @return `0` on success, or `-1` when the output cannot be made.
 */
typedef int (*OutputWriterMakeFn)(struct Arena* perm,
                                  struct Arena scratch,
                                  struct Str* bytes_out,
                                  struct Log* log,
                                  void* userdata);

/**
 * @brief Keeps, makes, or writes the output `file_path`, so that every output of a build goes
 *        through one rule.
 *
 * An output is kept without being made when the manifest declares attributes for it, `cache` has a
 * row for its path below `output_dir` with equal attributes, and the output exists as a file of the
 * size the row records. Otherwise `make` makes it, and an existing file of equal bytes is kept and
 * left untouched, while any other is written. A copy that the manifest records from
 * `manifest_builder_add_copy` or `manifest_builder_add_copies` is never made or compared: it is
 * kept under the same rule, or streamed from its source. `is_forced` keeps nothing and writes every
 * output.
 *
 * Its mark records the size of the file it leaves at `file_path`, so the output cache stats no
 * output that it handled: the size the cache row records for an output kept on its attributes, the
 * size of the made bytes for one kept or written, and for a copy streamed from its source the size
 * in the source's stat, `ManifestOutput.copy_source_len`. A write that a dry run withholds records
 * none, because the file at `file_path` is the old one.
 *
 * A dry run writes nothing. It marks an output with attributes kept when the cache vouches for it,
 * and otherwise written without calling `make`, because such an output can be expensive to make. It
 * makes an output without attributes and marks it kept or written as a real build would.
 *
 * Jobs running at once may call it, each for a path of its own, while no one changes `cache`.
 *
 * @param writer    Writer of the build. Must not be `NULL`.
 * @param file_path Output path, as the manifest records it.
 * @param cache     Output cache of the previous build, or an empty one. Must not be `NULL`.
 * @param make      Makes the output's bytes. Must not be `NULL` unless the output is a copy.
 * @param userdata  Opaque pointer passed to `make`.
 * @param output    Arena whose free space holds the made bytes until they are written.
 * @param scratch   Arena for the temporaries of `make`, and then for the read of an existing file
 *                  of the made size, so it must hold that many bytes and 64 KiB more.
 * @param operation Operation leading the message of a write that fails.
 * @param log       Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the manifest does not name `file_path`, `make` fails, or the
 *         write fails.
 */
int output_writer_produce(const struct OutputWriter* writer,
                          struct Str file_path,
                          const struct OutputCache* cache,
                          OutputWriterMakeFn make,
                          void* userdata,
                          struct Arena output,
                          struct Arena scratch,
                          struct Str operation,
                          struct Log* log) __attribute__((nonnull(1, 3, 9)));

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
