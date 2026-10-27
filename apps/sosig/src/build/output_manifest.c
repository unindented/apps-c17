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
static void output_manifest_roots(const struct SiteConfig* site_config, struct ManifestRoot* roots)
    __attribute__((nonnull(1, 2)));

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
                             const struct PathList* asset_paths,
                             const struct PathList* template_paths,
                             const struct ContentEntry* const* content_entries,
                             ptrdiff_t content_entry_count,
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
  const struct PathList* input_lists[] = {source_paths, static_paths, asset_paths, template_paths};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(input_lists) / sizeof(input_lists[0])); i++) {
    for (ptrdiff_t j = 0; j < input_lists[i]->count; j++) {
      path_list_push(&inputs, input_lists[i]->items[j], &input_scratch);
    }
  }
  struct ManifestBuilder builder;
  manifest_builder_init(&builder, manifest, site_config->output_dir, roots,
                        OUTPUT_MANIFEST_ROOT_COUNT, &inputs, os, perm);

  // Every output is checked even after one fails, so each collision in the site is reported in one
  // run. A rejected path is not recorded, so it cannot cause a second report against a later one.
  int rc = 0;
  for (ptrdiff_t i = 0; i < content_entry_count; i++) {
    const struct ContentEntry* entry = content_entries[i];
    struct Str relative = {0};
    (void)path_is_below(entry->output_path, site_config->output_dir, &relative);
    if (manifest_builder_add(&builder, relative, entry->source_path, scratch, log) != 0) {
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
  // The build cache is recorded last, so a dry run lists it last.
  struct Str cache_relative = {0};
  (void)path_is_below(build_cache_path(site_config->output_dir, &scratch), site_config->output_dir,
                      &cache_relative);
  if (manifest_builder_add(&builder, cache_relative, S("build cache"), scratch, log) != 0) {
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
  roots[0] =
      (struct ManifestRoot){.label = S("'content_dir'"), .dir_path = site_config->content_dir};
  roots[1] =
      (struct ManifestRoot){.label = S("'templates_dir'"), .dir_path = site_config->templates_dir};
  roots[2] = (struct ManifestRoot){.label = S("'static_dir'"), .dir_path = site_config->static_dir};
}
