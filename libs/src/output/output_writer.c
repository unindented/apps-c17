#include "output/output_writer.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/error.h"
#include "core/log.h"
#include "core/path.h"
#include "core/path_list.h"
#include "core/str.h"
#include "os/os.h"
#include "output/manifest.h"
#include "output/output_cache.h"

/**
 * Bytes of scratch that the read of an existing output logs into. A read records one message, which
 * names a path shorter than `PATH_MAX`, so this holds it.
 */
enum { READ_LOG_LEN = 64 * 1024 };

/**
 * @brief Marks `file_path` written with `len` as its size and, unless the build is a dry run,
 *        writes `source` to it, as `output_writer_write` describes.
 *
 * A dry run leaves the file as it is, so its mark records no size.
 *
 * @param writer    Writer of the build. Must not be `NULL`.
 * @param file_path Output path to write, as the manifest records it.
 * @param source    Bytes, or a file to stream, as `os_write` takes them.
 * @param len       Size in bytes of what `source` writes, or `-1` when it is not known.
 * @param operation Operation leading the failure message.
 * @param log       Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the manifest does not name `file_path` or the write fails.
 */
static int write_output(const struct OutputWriter* writer,
                        struct Str file_path,
                        struct OsSource source,
                        ptrdiff_t len,
                        struct Str operation,
                        struct Log* log) __attribute__((nonnull(1, 6)));

/**
 * @brief Marks `file_path` kept with `len` as its size, leaving the file as it is.
 *
 * @param writer    Writer of the build. Must not be `NULL`.
 * @param file_path Output path to keep, as the manifest records it.
 * @param len       Size in bytes of the file at `file_path`, or `-1` when it is not known.
 * @param log       Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the manifest does not name `file_path`.
 */
static int keep_output(const struct OutputWriter* writer,
                       struct Str file_path,
                       ptrdiff_t len,
                       struct Log* log) __attribute__((nonnull(1, 4)));

/**
 * @brief Marks `file_path` in the writer's manifest with `len` as its size, rejecting a path the
 *        manifest does not name.
 *
 * A write the manifest does not name would be removed again by the same build's prune, so it can
 * only be a bug in the site that asked for it.
 *
 * @param writer    Writer whose manifest receives the mark. Must not be `NULL`.
 * @param file_path Output path to mark.
 * @param mark      Mark the path receives.
 * @param len       Size in bytes of the file now at `file_path`, or `-1` when it is not known.
 * @param operation Operation leading the failure message.
 * @param log       Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the manifest does not name `file_path`.
 */
static int mark_output(const struct OutputWriter* writer,
                       struct Str file_path,
                       enum ManifestMark mark,
                       ptrdiff_t len,
                       struct Str operation,
                       struct Log* log) __attribute__((nonnull(1, 6)));

/**
 * @brief Finds the size that `cache` records for the output `file_path` when it vouches for it: the
 *        output declares attributes, the cache records equal ones for its path, and it exists as a
 *        file of the size the cache records.
 *
 * @param writer    Writer of the build. Must not be `NULL`.
 * @param file_path Output path, as the manifest records it.
 * @param output    How the manifest says the output is made. Must not be `NULL`.
 * @param cache     Output cache of the previous build. Must not be `NULL`.
 * @return The size in bytes of the output, which can be kept without being made, or `-1` when the
 *         cache does not vouch for it.
 */
static ptrdiff_t find_cached_len(const struct OutputWriter* writer,
                                 struct Str file_path,
                                 const struct ManifestOutput* output,
                                 const struct OutputCache* cache) __attribute__((nonnull(1, 3, 4)));

/**
 * @brief Reports whether the file at `file_path` holds exactly `bytes`.
 *
 * A file that cannot be read back is written again, which a missing one needs too, so the read's
 * message is dropped.
 *
 * @param os        Host the file is on. Must not be `NULL`.
 * @param file_path Path of the file.
 * @param bytes     Bytes the file must hold.
 * @param scratch   Arena for the file's bytes and the read's message.
 * @return `true` when the file exists, is regular, and holds `bytes`.
 */
static bool is_same_file(struct Os* os,
                         struct Str file_path,
                         struct Str bytes,
                         struct Arena scratch) __attribute__((nonnull(1)));

/**
 * @brief Orders two paths bytewise, for `qsort`.
 *
 * @param a First `struct Str`. Must not be `NULL`.
 * @param b Second `struct Str`. Must not be `NULL`.
 * @return A negative, zero, or positive value per `str_compare` of the two paths.
 */
static int compare_paths(const void* a, const void* b) __attribute__((nonnull(1, 2)));

int output_writer_write(const struct OutputWriter* writer,
                        struct Str file_path,
                        struct OsSource source,
                        struct Str operation,
                        struct Log* log) {
  const ptrdiff_t len = source.kind == OS_SOURCE_BYTES ? source.bytes.len : -1;
  return write_output(writer, file_path, source, len, operation, log);
}

int output_writer_keep(const struct OutputWriter* writer, struct Str file_path, struct Log* log) {
  return keep_output(writer, file_path, -1, log);
}

int output_writer_produce(const struct OutputWriter* writer,
                          struct Str file_path,
                          const struct OutputCache* cache,
                          OutputWriterMakeFn make,
                          void* userdata,
                          struct Arena output,
                          struct Arena scratch,
                          struct Str operation,
                          struct Log* log) {
  struct ManifestOutput manifest_output = {0};
  if (!manifest_find_output(writer->manifest, file_path, &manifest_output)) {
    return log_error(log, "%.*s: not a build output ('%.*s')", STR_FMT(operation),
                     STR_FMT(file_path));
  }
  const ptrdiff_t cached_len =
      writer->is_forced ? -1 : find_cached_len(writer, file_path, &manifest_output, cache);
  if (cached_len >= 0) {
    return keep_output(writer, file_path, cached_len, log);
  }
  if (manifest_output.copy_source_path.len > 0) {
    const struct OsSource source = {.kind = OS_SOURCE_FILE,
                                    .file_path = manifest_output.copy_source_path};
    const ptrdiff_t len =
        manifest_output.copy_source_len > 0 ? manifest_output.copy_source_len : -1;
    return write_output(writer, file_path, source, len, operation, log);
  }
  if (writer->is_dry_run && manifest_output.attributes.len > 0) {
    return output_writer_write(writer, file_path, (struct OsSource){0}, operation, log);
  }
  struct Str bytes = {0};
  if (make(&output, scratch, &bytes, log, userdata) != 0) {
    return -1;
  }
  if (!writer->is_forced && is_same_file(writer->os, file_path, bytes, scratch)) {
    return keep_output(writer, file_path, bytes.len, log);
  }
  return output_writer_write(writer, file_path, (struct OsSource){.bytes = bytes}, operation, log);
}

int output_writer_mkdirs(const struct OutputWriter* writer,
                         struct Str dir_path,
                         struct Str operation,
                         struct Log* log) {
  if (writer->is_dry_run) {
    return 0;
  }
  return os_mkdirs(writer->os, dir_path, operation, log);
}

int output_writer_remove(struct OutputWriter* writer,
                         struct Str file_path,
                         struct Str operation,
                         struct Log* log) {
  if (writer->is_dry_run) {
    path_list_push(&writer->removed_paths, file_path, writer->perm);
    return 0;
  }
  return os_remove(writer->os, file_path, operation, log);
}

int output_writer_remove_dir(struct OutputWriter* writer,
                             struct Str dir_path,
                             struct Str operation,
                             struct Log* log) {
  if (writer->is_dry_run) {
    path_list_push(&writer->removed_paths, dir_path, writer->perm);
    return 0;
  }
  return os_remove_dir(writer->os, dir_path, operation, log);
}

int output_writer_report(struct OutputWriter* writer, struct Arena scratch, struct Log* log) {
  struct Buf listing = {.arena = &scratch};
  int rc = 0;
  struct Str output_path = {0};
  while (manifest_next_output_path(writer->manifest, &output_path)) {
    switch (manifest_find_mark(writer->manifest, output_path)) {
      case MANIFEST_MARK_PENDING:
        rc =
            log_error(log, "failed to report dry run: output was neither written nor kept ('%.*s')",
                      STR_FMT(output_path));
        break;
      case MANIFEST_MARK_WRITTEN:
        buf_printf(&listing, "write\t%.*s\n", STR_FMT(output_path));
        break;
      case MANIFEST_MARK_KEPT:
        buf_printf(&listing, "keep\t%.*s\n", STR_FMT(output_path));
        break;
    }
  }
  if (rc != 0) {
    return -1;
  }
  // The prune meets stale paths in listing order, which no host fixes.
  struct PathList* removed_paths = &writer->removed_paths;
  if (removed_paths->count > 0) {
    qsort(removed_paths->items, (size_t)removed_paths->count, sizeof(*removed_paths->items),
          compare_paths);
  }
  for (ptrdiff_t i = 0; i < removed_paths->count; i++) {
    buf_printf(&listing, "remove\t%.*s\n", STR_FMT(removed_paths->items[i]));
  }
  if (os_print(writer->os, 1, buf_str(&listing)) != 0) {
    char message[ERROR_SYSTEM_MESSAGE_SIZE];
    return log_error(log, "failed to print dry run: %s",
                     error_system_message(message, sizeof(message), errno));
  }
  return 0;
}

static int write_output(const struct OutputWriter* writer,
                        struct Str file_path,
                        struct OsSource source,
                        ptrdiff_t len,
                        struct Str operation,
                        struct Log* log) {
  const ptrdiff_t marked_len = writer->is_dry_run ? -1 : len;
  if (mark_output(writer, file_path, MANIFEST_MARK_WRITTEN, marked_len, operation, log) != 0) {
    return -1;
  }
  if (writer->is_dry_run) {
    return 0;
  }
  if (os_write(writer->os, file_path, source, operation, log) != 0) {
    // A failed write leaves the output unmarked, so the output cache gives it no row, and the next
    // build makes it again rather than keep what the failure left at its path.
    (void)manifest_mark(writer->manifest, file_path, MANIFEST_MARK_PENDING, -1);
    return -1;
  }
  return 0;
}

static int keep_output(const struct OutputWriter* writer,
                       struct Str file_path,
                       ptrdiff_t len,
                       struct Log* log) {
  return mark_output(writer, file_path, MANIFEST_MARK_KEPT, len, S("failed to keep output"), log);
}

static int mark_output(const struct OutputWriter* writer,
                       struct Str file_path,
                       enum ManifestMark mark,
                       ptrdiff_t len,
                       struct Str operation,
                       struct Log* log) {
  if (manifest_mark(writer->manifest, file_path, mark, len) != 0) {
    return log_error(log, "%.*s: not a build output ('%.*s')", STR_FMT(operation),
                     STR_FMT(file_path));
  }
  return 0;
}

static ptrdiff_t find_cached_len(const struct OutputWriter* writer,
                                 struct Str file_path,
                                 const struct ManifestOutput* output,
                                 const struct OutputCache* cache) {
  struct Str relative_path = {0};
  if (output->attributes.len == 0 ||
      !path_is_below(file_path, writer->output_dir, &relative_path)) {
    return -1;
  }
  const struct OutputCacheRow* row = output_cache_find(cache, relative_path);
  struct OsStat st;
  if (row == NULL || !str_eq(row->attributes, output->attributes) ||
      os_stat(writer->os, file_path, &st) != 0 || st.is_dir || st.len != row->len) {
    return -1;
  }
  return row->len;
}

static bool is_same_file(struct Os* os,
                         struct Str file_path,
                         struct Str bytes,
                         struct Arena scratch) {
  struct OsStat st;
  if (os_stat(os, file_path, &st) != 0 || st.is_dir || st.len != bytes.len) {
    return false;
  }
  struct Arena log_arena = arena_split(&scratch, READ_LOG_LEN);
  struct Log read_log = {.buf = {.arena = &log_arena}};
  struct Str existing = {0};
  return os_read(os, file_path, bytes.len, OS_READ_BYTES, &scratch, &existing,
                 S("failed to read output"), &read_log) == 0 &&
         str_eq(existing, bytes);
}

static int compare_paths(const void* a, const void* b) {
  return str_compare(*(const struct Str*)a, *(const struct Str*)b);
}
