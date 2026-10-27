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
#include "os/oom.h"
#include "os/os.h"
#include "output/manifest.h"

/**
 * @brief Marks `file_path` in the writer's manifest, rejecting a path the manifest does not name.
 *
 * A write the manifest does not name would be removed again by the same build's prune, so it can
 * only be a bug in the site that asked for it.
 *
 * @param writer    Writer whose manifest receives the mark. Must not be `NULL`.
 * @param file_path Output path to mark.
 * @param mark      Mark the path receives.
 * @param operation Operation leading the failure message.
 * @param log       Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the manifest does not name `file_path`.
 */
static int mark_output(const struct OutputWriter* writer,
                       struct Str file_path,
                       enum ManifestMark mark,
                       struct Str operation,
                       struct Log* log) __attribute__((nonnull(1, 5)));

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
  if (mark_output(writer, file_path, MANIFEST_MARK_WRITTEN, operation, log) != 0) {
    return -1;
  }
  if (writer->is_dry_run) {
    return 0;
  }
  return os_write(writer->os, file_path, source, operation, log);
}

int output_writer_keep(const struct OutputWriter* writer, struct Str file_path, struct Log* log) {
  return mark_output(writer, file_path, MANIFEST_MARK_KEPT, S("failed to keep output"), log);
}

int output_writer_copy_files(const struct OutputWriter* writer,
                             const struct PathList* source_paths,
                             const struct OsStat* source_stats,
                             OutputWriterIsUnchangedFn is_unchanged,
                             void* userdata,
                             struct Str root_dir,
                             struct Str output_dir,
                             struct Arena scratch,
                             struct Str operation,
                             struct Log* log) {
  for (ptrdiff_t i = 0; i < source_paths->count; i++) {
    const struct Str source_path = source_paths->items[i];
    oom_set_job(source_path, NULL);
    // Each copy works in its own copy of `scratch`, so one file's path is dead before the next.
    struct Arena copy_scratch = scratch;
    struct Str relative = {0};
    (void)path_is_below(source_path, root_dir, &relative);
    const struct Str output_path = path_join(output_dir, relative, &copy_scratch);
    struct OsStat output_stat;
    const bool is_current = is_unchanged != NULL &&
                            is_unchanged(relative, &source_stats[i], userdata) &&
                            os_stat(writer->os, output_path, &output_stat) == 0 &&
                            !output_stat.is_dir && output_stat.len == source_stats[i].len;
    // The copy names the path that failed, the source or its destination, which mirrors it.
    const int rc = is_current ? output_writer_keep(writer, output_path, log)
                              : output_writer_write(writer, output_path,
                                                    (struct OsSource){.kind = OS_SOURCE_FILE,
                                                                      .file_path = source_path},
                                                    operation, log);
    if (rc != 0) {
      return -1;
    }
  }
  oom_set_job((struct Str){0}, NULL);
  return 0;
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

static int mark_output(const struct OutputWriter* writer,
                       struct Str file_path,
                       enum ManifestMark mark,
                       struct Str operation,
                       struct Log* log) {
  if (manifest_mark(writer->manifest, file_path, mark) != 0) {
    return log_error(log, "%.*s: not a build output ('%.*s')", STR_FMT(operation),
                     STR_FMT(file_path));
  }
  return 0;
}

static int compare_paths(const void* a, const void* b) {
  return str_compare(*(const struct Str*)a, *(const struct Str*)b);
}
