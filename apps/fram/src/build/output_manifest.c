#include "build/output_manifest.h"

#include <inttypes.h>
#include <stddef.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/path.h"
#include "core/path_list.h"
#include "core/str.h"
#include "domain/album.h"
#include "domain/media_item.h"
#include "domain/site_config.h"
#include "media/image.h"
#include "os/os.h"
#include "output/manifest_builder.h"

/** Number of input roots no output may enter: `media_dir`, `templates_dir` and `static_dir`. */
enum { OUTPUT_MANIFEST_ROOT_COUNT = 3 };

/**
 * @brief Fills `roots` with the input roots of `site_config`, each labelled by its quoted config
 *        key, which every diagnostic names.
 *
 * @param site_config Configuration supplying the roots. Must not be `NULL`.
 * @param roots       `OUTPUT_MANIFEST_ROOT_COUNT` roots to fill. Must not be `NULL`.
 */
static void fill_roots(const struct SiteConfig* site_config, struct ManifestRoot* roots)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Records an output given by its path joined onto `output_dir`, as the album skeleton plans
 *        it.
 *
 * @param builder     Prepared builder. Must not be `NULL`.
 * @param output_path Output path, below `output_dir`.
 * @param label       What produces the output, which a diagnostic names.
 * @param scratch     Arena for the check.
 * @param log         Log that receives the refusal. Must not be `NULL`.
 * @return `0` when the path was recorded, or `-1` when it was refused.
 */
static int output_manifest_populate_add(struct ManifestBuilder* builder,
                                        struct Str output_path,
                                        struct Str label,
                                        struct Arena scratch,
                                        struct Log* log) __attribute__((nonnull(1, 5)));

/**
 * @brief Records an album's page, and each derivative and original of its media, which stops at a
 *        media item's first refused output, so each item is reported once.
 *
 * A derivative's attributes name its source's size and modification time. Its settings and the
 * recipe version are in its path, so they need not be named again. The original is a copy of its
 * source.
 *
 * @param builder   Prepared builder. Must not be `NULL`.
 * @param album     Album to record. Must not be `NULL`.
 * @param media_dir Directory the album's source directory is below, which labels its page, and
 *                  below which each source is named in its outputs' attributes.
 * @param scratch   Arena for the labels and the checks.
 * @param log       Log that receives every refusal. Must not be `NULL`.
 * @return `0` when every output was recorded, or `-1` otherwise.
 */
static int output_manifest_populate_album(struct ManifestBuilder* builder,
                                          const struct Album* album,
                                          struct Str media_dir,
                                          struct Arena scratch,
                                          struct Log* log) __attribute__((nonnull(1, 2, 5)));

/**
 * @brief Lists the build's input files: the config, which no output may overwrite, and every file
 *        of `input_lists`.
 *
 * @param config_path      Path the configuration was loaded from.
 * @param input_lists      Lists of input files. Must not be `NULL`.
 * @param input_list_count Number of entries in `input_lists`.
 * @param arena            Arena that owns the list. Must not be `NULL`.
 * @return The list.
 */
static struct PathList output_manifest_populate_inputs(struct Str config_path,
                                                       const struct PathList* const* input_lists,
                                                       ptrdiff_t input_list_count,
                                                       struct Arena* arena)
    __attribute__((nonnull(2, 4)));

int output_manifest_check_output_dir(const struct SiteConfig* site_config,
                                     struct Str config_path,
                                     struct Os* os,
                                     struct Arena scratch,
                                     struct Log* log) {
  struct ManifestRoot roots[OUTPUT_MANIFEST_ROOT_COUNT];
  fill_roots(site_config, roots);
  return manifest_builder_check_output_dir(
      site_config->output_dir, roots, OUTPUT_MANIFEST_ROOT_COUNT, config_path, os, scratch, log);
}

struct Str output_manifest_cache_path(struct Str output_dir, struct Arena* arena) {
  return path_join(output_dir, S("_fram/output-cache.tsv"), arena);
}

int output_manifest_populate(struct Manifest* manifest,
                             const struct SiteConfig* site_config,
                             struct Str config_path,
                             const struct PathList* source_paths,
                             const struct PathList* static_paths,
                             const struct OsStat* static_stats,
                             const struct PathList* template_paths,
                             const struct Album* const* albums,
                             ptrdiff_t album_count,
                             struct Os* os,
                             struct Arena* perm,
                             struct Arena scratch,
                             struct Log* log) {
  // The builder is local, so the roots it points to live as long on the stack.
  struct ManifestRoot roots[OUTPUT_MANIFEST_ROOT_COUNT];
  fill_roots(site_config, roots);
  // The input files are needed only until the builder has their identities, so they go into a copy
  // of the scratch that the checks below reuse.
  struct Arena input_scratch = scratch;
  const struct PathList* input_lists[] = {source_paths, static_paths, template_paths};
  const struct PathList inputs = output_manifest_populate_inputs(
      config_path, input_lists, (ptrdiff_t)(sizeof(input_lists) / sizeof(input_lists[0])),
      &input_scratch);
  struct ManifestBuilder builder;
  manifest_builder_init(&builder, manifest, site_config->output_dir, roots,
                        OUTPUT_MANIFEST_ROOT_COUNT, &inputs, os, perm);

  // Every output is checked even after one fails, so each collision in the gallery is reported in
  // one run. A rejected path is not recorded, so it cannot cause a second report against a later
  // one.
  int rc = 0;
  for (ptrdiff_t i = 0; i < album_count; i++) {
    if (output_manifest_populate_album(&builder, albums[i], site_config->media_dir, scratch, log) !=
        0) {
      rc = -1;
    }
  }
  if (manifest_builder_add_template_list(
          &builder, S("templates.aggregates"), site_config->templates.aggregates,
          site_config->templates.aggregate_template_count, scratch, log) != 0) {
    rc = -1;
  }
  if (manifest_builder_add_copies(&builder, static_paths, static_stats, site_config->static_dir,
                                  NULL, scratch, log) != 0) {
    rc = -1;
  }
  // The output cache is recorded last, so a dry run lists it last. The path is made before the
  // call, so the copy of the scratch the add gets starts past it.
  const struct Str cache_path = output_manifest_cache_path(site_config->output_dir, &scratch);
  if (output_manifest_populate_add(&builder, cache_path, S("output cache"), scratch, log) != 0) {
    rc = -1;
  }
  // Every path is now recorded, so the prefix scan sees all of them.
  if (manifest_builder_finish(&builder, log) != 0) {
    rc = -1;
  }
  return rc;
}

static struct PathList output_manifest_populate_inputs(struct Str config_path,
                                                       const struct PathList* const* input_lists,
                                                       ptrdiff_t input_list_count,
                                                       struct Arena* arena) {
  struct PathList inputs = {0};
  path_list_push(&inputs, config_path, arena);
  for (ptrdiff_t i = 0; i < input_list_count; i++) {
    for (ptrdiff_t j = 0; j < input_lists[i]->count; j++) {
      path_list_push(&inputs, input_lists[i]->items[j], arena);
    }
  }
  return inputs;
}

static void fill_roots(const struct SiteConfig* site_config, struct ManifestRoot* roots) {
  roots[0] = (struct ManifestRoot){.label = S("'media_dir'"), .dir_path = site_config->media_dir};
  roots[1] =
      (struct ManifestRoot){.label = S("'templates_dir'"), .dir_path = site_config->templates_dir};
  roots[2] = (struct ManifestRoot){.label = S("'static_dir'"), .dir_path = site_config->static_dir};
}

static int output_manifest_populate_add(struct ManifestBuilder* builder,
                                        struct Str output_path,
                                        struct Str label,
                                        struct Arena scratch,
                                        struct Log* log) {
  struct Str relative = {0};
  (void)path_is_below(output_path, builder->output_dir, &relative);
  return manifest_builder_add(builder, relative, label, scratch, log);
}

static int output_manifest_populate_album(struct ManifestBuilder* builder,
                                          const struct Album* album,
                                          struct Str media_dir,
                                          struct Arena scratch,
                                          struct Log* log) {
  const struct Str album_label =
      album_is_root(album) ? media_dir : path_join(media_dir, album->source_dir, &scratch);
  int rc = output_manifest_populate_add(builder, album->output_path, album_label, scratch, log);
  for (ptrdiff_t i = 0; i < album->media_count; i++) {
    const struct MediaItem* media = album->media[i];
    // Each item's attributes live in a copy of `scratch`, which the next item reuses.
    struct Arena media_scratch = scratch;
    const struct OsStat* source_stat = &media->source_stat;
    // A source is named by its path below `media_dir`, so a renamed source makes its outputs again,
    // and moving the whole gallery makes none.
    struct Str source_name = {0};
    (void)path_is_below(media->source_path, media_dir, &source_name);
    // The codecs' versions are part of a derivative's recipe, so an upgrade of either makes it
    // again. A copied original holds the source's bytes, so its attributes need no versions.
    const struct Str codec_versions = image_codec_versions(&media_scratch);
    struct Buf attributes = {.arena = &media_scratch};
    buf_printf(&attributes, "src=%.*s len=%td mtime=%" PRId64 ".%09" PRId64 " %.*s",
               STR_FMT(source_name), source_stat->len, source_stat->mtime_s, source_stat->mtime_ns,
               STR_FMT(codec_versions));
    int media_rc = 0;
    for (ptrdiff_t j = 0; media_rc == 0 && j < media->derivative_count; j++) {
      struct Str relative = {0};
      (void)path_is_below(media->derivatives[j].path, builder->output_dir, &relative);
      media_rc = manifest_builder_add_output(builder, relative, media->source_path,
                                             buf_str(&attributes), media_scratch, log);
    }
    if (media_rc == 0) {
      struct Str relative = {0};
      (void)path_is_below(media->original_path, builder->output_dir, &relative);
      media_rc = manifest_builder_add_copy(builder, relative, media->source_path, source_name,
                                           source_stat, media_scratch, log);
    }
    if (media_rc != 0) {
      rc = -1;
    }
  }
  return rc;
}
