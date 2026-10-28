#include "build/output_manifest.h"

#include <stddef.h>

#include "core/arena.h"
#include "core/path.h"
#include "core/path_list.h"
#include "core/str.h"
#include "domain/build_cache.h"
#include "domain/content_entry.h"
#include "domain/site_config.h"
#include "os/os.h"
#include "output/manifest_builder.h"

/** Number of input roots no output may enter: `content_dir`, `templates_dir` and `static_dir`. */
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

/**
 * @brief Records an output given by its path joined onto `output_dir`, as the build plans it.
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

int output_manifest_populate(struct Manifest* manifest,
                             const struct SiteConfig* site_config,
                             struct Str config_path,
                             const struct PathList* source_paths,
                             const struct PathList* static_paths,
                             const struct PathList* asset_paths,
                             const struct PathList* template_paths,
                             const struct ContentEntry* const* content_entries,
                             ptrdiff_t content_entry_count,
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
  const struct PathList* input_lists[] = {source_paths, static_paths, asset_paths, template_paths};
  const struct PathList inputs = output_manifest_populate_inputs(
      config_path, input_lists, (ptrdiff_t)(sizeof(input_lists) / sizeof(input_lists[0])),
      &input_scratch);
  struct ManifestBuilder builder;
  manifest_builder_init(&builder, manifest, site_config->output_dir, roots,
                        OUTPUT_MANIFEST_ROOT_COUNT, &inputs, os, perm);

  // Every output is checked even after one fails, so each collision in the site is reported in one
  // run. A rejected path is not recorded, so it cannot cause a second report against a later one.
  int rc = 0;
  for (ptrdiff_t i = 0; i < content_entry_count; i++) {
    if (output_manifest_populate_add(&builder, content_entries[i]->output_path,
                                     content_entries[i]->source_path, scratch, log) != 0) {
      rc = -1;
    }
  }
  if (manifest_builder_add_template_list(
          &builder, S("aggregate_templates"), site_config->aggregate_templates,
          site_config->aggregate_template_count, scratch, log) != 0 ||
      manifest_builder_add_template_list(&builder, S("feed_templates"), site_config->feed_templates,
                                         site_config->feed_template_count, scratch, log) != 0) {
    rc = -1;
  }
  if (manifest_builder_add_files(&builder, static_paths, site_config->static_dir, NULL, scratch,
                                 log) != 0) {
    rc = -1;
  }
  if (manifest_builder_add_files(&builder, asset_paths, site_config->content_dir, NULL, scratch,
                                 log) != 0) {
    rc = -1;
  }
  // The build cache is recorded last, so a dry run lists it last. The path is made before the call,
  // so the copy of the scratch the add gets starts past it.
  const struct Str cache_path = build_cache_path(site_config->output_dir, &scratch);
  if (output_manifest_populate_add(&builder, cache_path, S("build cache"), scratch, log) != 0) {
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

static int output_manifest_populate_add(struct ManifestBuilder* builder,
                                        struct Str output_path,
                                        struct Str label,
                                        struct Arena scratch,
                                        struct Log* log) {
  struct Str relative = {0};
  (void)path_is_below(output_path, builder->output_dir, &relative);
  return manifest_builder_add(builder, relative, label, scratch, log);
}

static void fill_roots(const struct SiteConfig* site_config, struct ManifestRoot* roots) {
  roots[0] =
      (struct ManifestRoot){.label = S("'content_dir'"), .dir_path = site_config->content_dir};
  roots[1] =
      (struct ManifestRoot){.label = S("'templates_dir'"), .dir_path = site_config->templates_dir};
  roots[2] = (struct ManifestRoot){.label = S("'static_dir'"), .dir_path = site_config->static_dir};
}
