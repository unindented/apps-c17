#include "build/export_writer.h"

#include <stdbool.h>
#include <stddef.h>

#include "build/repo_scan.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/path.h"
#include "core/str.h"
#include "domain/build_cache.h"
#include "domain/site_config.h"
#include "os/os.h"
#include "output/manifest_builder.h"
#include "output/output_writer.h"

/** Largest small file of a clone export read back, in bytes, to compare it with the new one. */
enum { EXPORT_WRITER_FILE_LEN_MAX = 64 * 1024 * 1024 };

/** Bytes of the log a read-back failure goes to: one message that names a path. */
enum { EXPORT_WRITER_LOG_LEN = 64 * 1024 };

/**
 * @brief Returns the path of one file of a repo's clone export, relative to `output_dir`.
 *
 * @param repo_name Name of the repo.
 * @param file_name Path below `<r>.git/`, such as `info/refs`.
 * @param arena     Arena that owns the path. Must not be `NULL`.
 * @return The path.
 */
static struct Str export_path(struct Str repo_name, struct Str file_name, struct Arena* arena)
    __attribute__((nonnull(3)));

/**
 * @brief Writes `bytes` to `file_path`, or keeps the file when it already holds them.
 *
 * @param writer    Writer of the build. Must not be `NULL`.
 * @param file_path Output path.
 * @param bytes     Bytes the file must hold.
 * @param os        Host the file is read on. Must not be `NULL`.
 * @param scratch   Arena for the existing bytes.
 * @param operation Operation leading the write's failure message.
 * @param log       Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the write fails.
 */
static int write_or_keep(const struct OutputWriter* writer,
                         struct Str file_path,
                         struct Str bytes,
                         struct Os* os,
                         struct Arena scratch,
                         struct Str operation,
                         struct Log* log) __attribute__((nonnull(1, 4, 7)));

/**
 * @brief Returns the bytes of `_headers`, as `export_writer_write_headers` describes them.
 *
 * @param site_config Loaded configuration. Must not be `NULL`.
 * @param arena       Arena that owns the bytes. Must not be `NULL`.
 * @return The bytes.
 */
static struct Str format_headers(const struct SiteConfig* site_config, struct Arena* arena)
    __attribute__((nonnull(1, 2)));

int export_writer_register(struct ManifestBuilder* builder,
                           const struct RepoScan* scan,
                           struct Arena scratch,
                           struct Log* log) {
  const struct Str repo_name = scan->model.config->name;
  const struct RepoExport* export = &scan->export;
  // The label is the export's directory, as a static file's is its path.
  const struct Str source_label = str_concat(repo_name, S(".git"), &scratch);
  int rc = 0;
  const struct Str files[] = {S("HEAD"), S("info/refs"), S("objects/info/packs")};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(files) / sizeof(files[0])); i++) {
    // The path is made before the call, so the copy of the scratch the add gets starts past it: C
    // leaves the order in which a call evaluates its arguments unspecified.
    struct Arena path_scratch = scratch;
    const struct Str path = export_path(repo_name, files[i], &path_scratch);
    if (manifest_builder_add(builder, path, source_label, path_scratch, log) != 0) {
      rc = -1;
    }
  }
  const struct Str pack_ids[] = {export->pack_id, export->previous_pack_id};
  const struct Str extensions[] = {S("pack"), S("idx")};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(pack_ids) / sizeof(pack_ids[0])); i++) {
    for (ptrdiff_t j = 0; pack_ids[i].len > 0 && j < 2; j++) {
      struct Arena path_scratch = scratch;
      const struct Str path =
          pack_export_output_path(repo_name, pack_ids[i], extensions[j], &path_scratch);
      if (manifest_builder_add(builder, path, source_label, path_scratch, log) != 0) {
        rc = -1;
      }
    }
  }
  return rc;
}

int export_writer_write(const struct OutputWriter* writer,
                        struct Str output_dir,
                        const struct RepoScan* scan,
                        struct Os* os,
                        struct Arena scratch,
                        struct Log* log) {
  const struct Str repo_name = scan->model.config->name;
  const struct RepoExport* export = &scan->export;
  // The pack and its idx go in before the files that name them, so a client never reads an
  // `info/refs` whose objects are not there yet.
  const struct Str extensions[] = {S("pack"), S("idx")};
  const struct Str built_paths[] = {export->built.pack_path, export->built.idx_path};
  for (ptrdiff_t i = 0; export->pack_id.len > 0 && i < 2; i++) {
    const struct Str output_path = path_join(
        output_dir, pack_export_output_path(repo_name, export->pack_id, extensions[i], &scratch),
        &scratch);
    const int rc = export->is_pack_kept
                       ? output_writer_keep(writer, output_path, log)
                       : output_writer_write(
                             writer, output_path,
                             (struct OsSource){.kind = OS_SOURCE_FILE, .file_path = built_paths[i]},
                             S("failed to write clone pack"), log);
    if (rc != 0) {
      return -1;
    }
  }
  for (ptrdiff_t i = 0; export->previous_pack_id.len > 0 && i < 2; i++) {
    const struct Str output_path = path_join(
        output_dir,
        pack_export_output_path(repo_name, export->previous_pack_id, extensions[i], &scratch),
        &scratch);
    if (output_writer_keep(writer, output_path, log) != 0) {
      return -1;
    }
  }
  const struct Str files[] = {S("objects/info/packs"), S("info/refs"), S("HEAD")};
  const struct Str contents[] = {export->packs, export->info_refs, export->head};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(files) / sizeof(files[0])); i++) {
    const struct Str output_path =
        path_join(output_dir, export_path(repo_name, files[i], &scratch), &scratch);
    if (write_or_keep(writer, output_path, contents[i], os, scratch,
                      S("failed to write clone export"), log) != 0) {
      return -1;
    }
  }
  return 0;
}

int export_writer_write_headers(const struct OutputWriter* writer,
                                const struct SiteConfig* site_config,
                                struct Os* os,
                                struct Arena scratch,
                                struct Log* log) {
  const struct Str file_path =
      path_join(site_config->output_dir, EXPORT_WRITER_HEADERS_PAGE, &scratch);
  const struct Str bytes = format_headers(site_config, &scratch);
  return write_or_keep(writer, file_path, bytes, os, scratch, S("failed to write headers file"),
                       log);
}

struct BuildCacheRow export_writer_row(const struct RepoScan* scan) {
  const struct RepoExport* export = &scan->export;
  return (struct BuildCacheRow){
      .kind = BUILD_CACHE_KIND_EXPORT,
      .name = scan->model.config->name,
      .id = export->pack_id,
      .pack_len = export->pack_len,
      .idx_len = export->idx_len,
      .info_refs_id = export->info_refs_id,
      .previous_pack_id = export->previous_pack_id,
  };
}

static struct Str export_path(struct Str repo_name, struct Str file_name, struct Arena* arena) {
  struct Buf path = {.arena = arena};
  buf_printf(&path, "%.*s.git/%.*s", STR_FMT(repo_name), STR_FMT(file_name));
  return buf_str(&path);
}

static int write_or_keep(const struct OutputWriter* writer,
                         struct Str file_path,
                         struct Str bytes,
                         struct Os* os,
                         struct Arena scratch,
                         struct Str operation,
                         struct Log* log) {
  // A file that cannot be read back is written again, which is what a missing one needs too, so the
  // read's message is not kept. The message grows in a region of its own, apart from the read.
  struct Arena log_arena = arena_split(&scratch, EXPORT_WRITER_LOG_LEN);
  struct Log read_log = {.buf = {.arena = &log_arena}};
  struct Str existing = {0};
  struct OsStat st;
  if (os_stat(os, file_path, &st) == 0 && !st.is_dir && st.len == bytes.len &&
      os_read(os, file_path, EXPORT_WRITER_FILE_LEN_MAX, OS_READ_BYTES, &scratch, &existing,
              S("failed to read clone export"), &read_log) == 0 &&
      str_eq(existing, bytes)) {
    return output_writer_keep(writer, file_path, log);
  }
  return output_writer_write(writer, file_path, (struct OsSource){.bytes = bytes}, operation, log);
}

static struct Str format_headers(const struct SiteConfig* site_config, struct Arena* arena) {
  // A rule's path starts at the host's root, so a site below a path of `base_url` puts it first.
  // One splat a rule is what Cloudflare Pages takes, so each repo's packs get a rule of their own,
  // and the small files one rule each for every repo.
  const struct Str authority_and_path = str_tail(str_cut(site_config->base_url, ':').tail, 2);
  const struct StrCut host = str_cut(authority_and_path, '/');
  const struct Str prefix =
      host.is_found ? str_tail(authority_and_path, host.head.len) : (struct Str){0};
  static const char* const no_cache[] = {"HEAD", "info/refs", "objects/info/packs"};
  struct Buf text = {.arena = arena};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(no_cache) / sizeof(no_cache[0])); i++) {
    buf_printf(&text, "%.*s/*.git/%s\n  Cache-Control: no-cache\n", STR_FMT(prefix), no_cache[i]);
  }
  for (ptrdiff_t i = 0; i < site_config->repo_count; i++) {
    const struct Str name = site_config->repos[i].name;
    buf_printf(&text,
               "%.*s/%.*s.git/objects/pack/*\n  Cache-Control: public, max-age=31536000, "
               "immutable\n",
               STR_FMT(prefix), STR_FMT(name));
  }
  return buf_str(&text);
}
