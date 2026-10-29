#ifndef SURGE_RELEASE_WRITER_H
#define SURGE_RELEASE_WRITER_H

#include "core/arena.h"
#include "core/str.h"

struct Log;
struct ManifestBuilder;
struct OutputCache;
struct OutputWriter;
struct RepoModel;
struct RepoScan;
struct SiteConfig;
struct TemplateSet;

/** What the release outputs of one repo are made from. */
struct ReleaseWriterInputs {
  /** Configuration of the build. */
  const struct SiteConfig* site_config;

  /** Compiled templates. */
  const struct TemplateSet* template_set;

  /** What the scan found for the repo. */
  const struct RepoModel* repo;

  /** Output cache of the previous build. */
  const struct OutputCache* cache;
};

/**
 * @brief Records the release outputs of one repo: its releases page, each project's page,
 *        `versions.txt` and `versions.json`, and each release's page and assets.
 *
 * A repo without release projects has none of them. A page declares the page recipe, the repo's
 * `repo` map and its releases, a version list the recipe and the releases, and an asset is a copy
 * of its file.
 *
 * @param builder   Prepared manifest builder. Must not be `NULL`.
 * @param scan      What the scan found for the repo. Must not be `NULL`.
 * @param recipe_id Id of the page recipe.
 * @param scratch   Arena for the paths and the attributes.
 * @param log       Log that receives every refusal. Must not be `NULL`.
 * @return `0` when every path was recorded, or `-1` otherwise.
 */
int release_writer_register(struct ManifestBuilder* builder,
                            const struct RepoScan* scan,
                            struct Str recipe_id,
                            struct Arena scratch,
                            struct Log* log) __attribute__((nonnull(1, 2, 5)));

/**
 * @brief Writes the release outputs of one repo through `output_writer_produce`, or keeps those
 *        that are current.
 *
 * An asset is streamed from `releases_dir`, with no size limit.
 *
 * @param inputs  What the outputs are made from. Must not be `NULL`.
 * @param writer  Writer of the build. Must not be `NULL`.
 * @param scratch Arena for the pages.
 * @param log     Log that receives every failure. Must not be `NULL`.
 * @return `0` when every output was written or kept, or `-1` otherwise.
 */
int release_writer_write(const struct ReleaseWriterInputs* inputs,
                         const struct OutputWriter* writer,
                         struct Arena scratch,
                         struct Log* log) __attribute__((nonnull(1, 2, 4)));

#endif
