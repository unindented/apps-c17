#include "build/output_manifest.h"

#include <stddef.h>

#include "core/arena.h"
#include "core/path.h"
#include "core/path_list.h"
#include "core/str.h"
#include "domain/album.h"
#include "domain/build_cache.h"
#include "domain/media_item.h"
#include "domain/site_config.h"
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
static void output_manifest_roots(const struct SiteConfig* site_config, struct ManifestRoot* roots)
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
 * @param builder   Prepared builder. Must not be `NULL`.
 * @param album     Album to record. Must not be `NULL`.
 * @param media_dir Directory the album's source directory is below, which labels its page.
 * @param scratch   Arena for the labels and the checks.
 * @param log       Log that receives every refusal. Must not be `NULL`.
 * @return `0` when every output was recorded, or `-1` otherwise.
 */
static int output_manifest_populate_album(struct ManifestBuilder* builder,
                                          const struct Album* album,
                                          struct Str media_dir,
                                          struct Arena scratch,
                                          struct Log* log) __attribute__((nonnull(1, 2, 5)));

int output_manifest_check_output_dir(const struct SiteConfig* site_config,
                                     struct Str config_path,
                                     struct Os* os,
                                     struct Arena scratch,
                                     struct Log* log) {
  struct ManifestRoot roots[OUTPUT_MANIFEST_ROOT_COUNT];
  output_manifest_roots(site_config, roots);
  return manifest_builder_check_output_dir(
      site_config->output_dir, roots, OUTPUT_MANIFEST_ROOT_COUNT, config_path, os, scratch, log);
}

int output_manifest_populate(struct Manifest* manifest,
                             const struct SiteConfig* site_config,
                             struct Str config_path,
                             const struct PathList* source_paths,
                             const struct PathList* static_paths,
                             const struct PathList* template_paths,
                             const struct Album* const* albums,
                             ptrdiff_t album_count,
                             struct Os* os,
                             struct Arena* perm,
                             struct Arena scratch,
                             struct Log* log) {
  struct ManifestRoot* roots = arena_new(perm, OUTPUT_MANIFEST_ROOT_COUNT, struct ManifestRoot);
  output_manifest_roots(site_config, roots);
  // The input files are needed only until the builder has their identities, so they go into a copy
  // of the scratch that the checks below reuse. The config is one of them, so no output can
  // overwrite the file that configured the build.
  struct Arena input_scratch = scratch;
  struct PathList inputs = {0};
  path_list_push(&inputs, config_path, &input_scratch);
  const struct PathList* input_lists[] = {source_paths, static_paths, template_paths};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(input_lists) / sizeof(input_lists[0])); i++) {
    for (ptrdiff_t j = 0; j < input_lists[i]->count; j++) {
      path_list_push(&inputs, input_lists[i]->items[j], &input_scratch);
    }
  }
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
          &builder, S("aggregate_templates"), site_config->aggregate_templates,
          site_config->aggregate_template_count, scratch, log) != 0) {
    rc = -1;
  }
  if (manifest_builder_add_files(&builder, static_paths, site_config->static_dir, NULL, scratch,
                                 log) != 0) {
    rc = -1;
  }
  // The build cache is recorded last, so a dry run lists it last.
  if (output_manifest_populate_add(&builder, build_cache_path(site_config->output_dir, &scratch),
                                   S("build cache"), scratch, log) != 0) {
    rc = -1;
  }
  // Every path is now recorded, so the prefix scan sees all of them.
  if (manifest_builder_finish(&builder, log) != 0) {
    rc = -1;
  }
  return rc;
}

static void output_manifest_roots(const struct SiteConfig* site_config,
                                  struct ManifestRoot* roots) {
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
    int media_rc = 0;
    for (ptrdiff_t j = 0; media_rc == 0 && j < media->derivative_count; j++) {
      media_rc = output_manifest_populate_add(builder, media->derivatives[j].path,
                                              media->source_path, scratch, log);
    }
    if (media_rc == 0) {
      media_rc = output_manifest_populate_add(builder, media->original_path, media->source_path,
                                              scratch, log);
    }
    if (media_rc != 0) {
      rc = -1;
    }
  }
  return rc;
}
