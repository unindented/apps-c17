#include "build/export_writer.h"

#include <stdbool.h>
#include <stddef.h>

#include "build/repo_scan.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/path.h"
#include "core/str.h"
#include "domain/site_config.h"
#include "os/os.h"
#include "output/manifest_builder.h"
#include "output/output_writer.h"

/** What makes one output of a clone export, for `output_writer_produce`. */
struct ExportMake {
  /** Bytes of a small file, or of `_headers` once made. */
  struct Str bytes;

  /** Configuration, for `_headers`, else `NULL`. */
  const struct SiteConfig* site_config;

  /** Output path of a kept pack or idx, which its make names. */
  struct Str file_path;
};

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
 * @brief Records the pack or the idx of the pack this build publishes.
 *
 * @param builder      Prepared manifest builder. Must not be `NULL`.
 * @param scan         Scanned repo. Must not be `NULL`.
 * @param extension    `pack` or `idx`.
 * @param source_label Label of the export's outputs.
 * @param scratch      Arena for the path and the attributes.
 * @param log          Log that receives the refusal. Must not be `NULL`.
 * @return `0` when the path was recorded, or `-1` otherwise.
 */
static int export_writer_register_pack(struct ManifestBuilder* builder,
                                       const struct RepoScan* scan,
                                       struct Str extension,
                                       struct Str source_label,
                                       struct Arena scratch,
                                       struct Log* log) __attribute__((nonnull(1, 2, 6)));

/**
 * @brief Returns the bytes `ExportMake.bytes` holds, as `OutputWriterMakeFn`.
 *
 * @param perm      Unused.
 * @param scratch   Unused.
 * @param bytes_out Receives the bytes. Must not be `NULL`.
 * @param log       Unused.
 * @param userdata  The `struct ExportMake`.
 * @return `0`.
 */
static int export_writer_make_bytes(struct Arena* perm,
                                    struct Arena scratch,
                                    struct Str* bytes_out,
                                    struct Log* log,
                                    void* userdata) __attribute__((nonnull(1, 3, 4, 5)));

/**
 * @brief Fails the make of a pack or an idx the scan kept, which is no longer the file the build
 *        cache recorded, as `OutputWriterMakeFn`.
 *
 * @param perm      Unused.
 * @param scratch   Unused.
 * @param bytes_out Unused.
 * @param log       Log that receives the failure. Must not be `NULL`.
 * @param userdata  The `struct ExportMake`, which names the file.
 * @return `-1`.
 */
static int export_writer_make_kept(struct Arena* perm,
                                   struct Arena scratch,
                                   struct Str* bytes_out,
                                   struct Log* log,
                                   void* userdata) __attribute__((nonnull(1, 3, 4, 5)));

/**
 * @brief Makes the bytes of `_headers`, as `OutputWriterMakeFn`.
 *
 * @param perm      Arena that owns the bytes. Must not be `NULL`.
 * @param scratch   Unused.
 * @param bytes_out Receives the bytes. Must not be `NULL`.
 * @param log       Unused.
 * @param userdata  The `struct ExportMake`, which holds the configuration.
 * @return `0`.
 */
static int export_writer_write_headers_make(struct Arena* perm,
                                            struct Arena scratch,
                                            struct Str* bytes_out,
                                            struct Log* log,
                                            void* userdata) __attribute__((nonnull(1, 3, 4, 5)));

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
  const struct Str extensions[] = {S("pack"), S("idx")};
  for (ptrdiff_t i = 0; export->pack_id.len > 0 && i < 2; i++) {
    if (export_writer_register_pack(builder, scan, extensions[i], source_label, scratch, log) !=
        0) {
      rc = -1;
    }
  }
  // The pack before declares what the output cache recorded for it, so a build that fails before it
  // writes the new pack still records it, with the fact `repo_scan_export_fact` gives.
  const struct Str previous_attributes[] = {export->previous_pack_attributes,
                                            export->previous_idx_attributes};
  for (ptrdiff_t i = 0; export->previous_pack_id.len > 0 && i < 2; i++) {
    struct Arena path_scratch = scratch;
    const struct Str path =
        pack_export_output_path(repo_name, export->previous_pack_id, extensions[i], &path_scratch);
    if (manifest_builder_add_output(builder, path, source_label, previous_attributes[i],
                                    path_scratch, log) != 0) {
      rc = -1;
    }
  }
  return rc;
}

int export_writer_keep_previous(const struct OutputWriter* writer,
                                struct Str output_dir,
                                const struct RepoScan* scan,
                                struct Arena scratch,
                                struct Log* log) {
  const struct Str repo_name = scan->model.config->name;
  const struct RepoExport* export = &scan->export;
  const struct Str extensions[] = {S("pack"), S("idx")};
  for (ptrdiff_t i = 0; export->previous_pack_id.len > 0 && i < 2; i++) {
    struct Arena path_scratch = scratch;
    const struct Str output_path = path_join(
        output_dir,
        pack_export_output_path(repo_name, export->previous_pack_id, extensions[i], &path_scratch),
        &path_scratch);
    if (output_writer_keep(writer, output_path, log) != 0) {
      return -1;
    }
  }
  return 0;
}

int export_writer_write(const struct OutputWriter* writer,
                        struct Str output_dir,
                        const struct RepoScan* scan,
                        const struct OutputCache* cache,
                        struct Arena scratch,
                        struct Log* log) {
  const struct Str repo_name = scan->model.config->name;
  const struct RepoExport* export = &scan->export;
  // The pack and its idx go in before the files that name them, so a client never reads an
  // `info/refs` whose objects are not there yet. The scan has kept a current pack, so only a kept
  // one that changed since reaches its make.
  const struct Str extensions[] = {S("pack"), S("idx")};
  for (ptrdiff_t i = 0; export->pack_id.len > 0 && i < 2; i++) {
    struct Arena path_scratch = scratch;
    struct ExportMake make = {0};
    make.file_path =
        path_join(output_dir,
                  pack_export_output_path(repo_name, export->pack_id, extensions[i], &path_scratch),
                  &path_scratch);
    if (output_writer_produce(writer, make.file_path, cache, export_writer_make_kept, &make,
                              path_scratch, path_scratch, S("failed to write clone pack"),
                              log) != 0) {
      return -1;
    }
  }
  const struct Str files[] = {S("objects/info/packs"), S("info/refs"), S("HEAD")};
  const struct Str contents[] = {export->packs, export->info_refs, export->head};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(files) / sizeof(files[0])); i++) {
    struct Arena file_scratch = scratch;
    const struct Str output_path =
        path_join(output_dir, export_path(repo_name, files[i], &file_scratch), &file_scratch);
    struct ExportMake make = {.bytes = contents[i]};
    if (output_writer_produce(writer, output_path, cache, export_writer_make_bytes, &make,
                              file_scratch, file_scratch, S("failed to write clone export"),
                              log) != 0) {
      return -1;
    }
  }
  return 0;
}

int export_writer_write_headers(const struct OutputWriter* writer,
                                const struct SiteConfig* site_config,
                                const struct OutputCache* cache,
                                struct Arena scratch,
                                struct Log* log) {
  const struct Str file_path =
      path_join(site_config->output_dir, EXPORT_WRITER_HEADERS_PAGE, &scratch);
  struct ExportMake make = {.site_config = site_config};
  // The bytes take one half of the scratch, and the read of the file in the output the other.
  const char* region_end = scratch.limit != NULL ? scratch.limit : scratch.end;
  struct Arena output = arena_split(&scratch, (region_end - scratch.beg) / 2);
  return output_writer_produce(writer, file_path, cache, export_writer_write_headers_make, &make,
                               output, scratch, S("failed to write headers file"), log);
}

static struct Str export_path(struct Str repo_name, struct Str file_name, struct Arena* arena) {
  struct Buf path = {.arena = arena};
  buf_printf(&path, "%.*s.git/%.*s", STR_FMT(repo_name), STR_FMT(file_name));
  return buf_str(&path);
}

static int export_writer_register_pack(struct ManifestBuilder* builder,
                                       const struct RepoScan* scan,
                                       struct Str extension,
                                       struct Str source_label,
                                       struct Arena scratch,
                                       struct Log* log) {
  const struct RepoExport* export = &scan->export;
  const bool is_pack = str_eq(extension, S("pack"));
  const struct Str path =
      pack_export_output_path(scan->model.config->name, export->pack_id, extension, &scratch);
  if (export->is_pack_kept) {
    return manifest_builder_add_output(builder, path, source_label,
                                       is_pack ? export->pack_attributes : export->idx_attributes,
                                       scratch, log);
  }
  if (export->built.pack_id.len > 0) {
    // A pack's name hashes its bytes, so its name and size are all that its copy needs to declare.
    const struct OsStat source_stat = {
        .len = is_pack ? export->built.pack_len : export->built.idx_len,
    };
    const struct Str source_path = is_pack ? export->built.pack_path : export->built.idx_path;
    return manifest_builder_add_copy(builder, path, source_path, path_basename(source_path),
                                     &source_stat, scratch, log);
  }
  struct Buf attributes = {.arena = &scratch};
  buf_printf(&attributes, "refs=%.*s", STR_FMT(export->info_refs_id));
  const struct Str text = buf_str(&attributes);
  return manifest_builder_add_output(builder, path, source_label, text, scratch, log);
}

static int export_writer_make_bytes(struct Arena* perm,
                                    struct Arena scratch,
                                    struct Str* bytes_out,
                                    struct Log* log,
                                    void* userdata) {
  (void)perm;
  (void)scratch;
  (void)log;
  *bytes_out = ((const struct ExportMake*)userdata)->bytes;
  return 0;
}

static int export_writer_make_kept(struct Arena* perm,
                                   struct Arena scratch,
                                   struct Str* bytes_out,
                                   struct Log* log,
                                   void* userdata) {
  (void)perm;
  (void)scratch;
  (void)bytes_out;
  const struct ExportMake* make = userdata;
  return log_error(log, "failed to keep clone pack: changed during the build ('%.*s')",
                   STR_FMT(make->file_path));
}

static int export_writer_write_headers_make(struct Arena* perm,
                                            struct Arena scratch,
                                            struct Str* bytes_out,
                                            struct Log* log,
                                            void* userdata) {
  (void)scratch;
  (void)log;
  *bytes_out = format_headers(((const struct ExportMake*)userdata)->site_config, perm);
  return 0;
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
