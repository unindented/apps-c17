#ifndef SURGE_RELEASE_WRITER_H
#define SURGE_RELEASE_WRITER_H

#include <stdbool.h>

#include "core/arena.h"

struct Log;
struct ManifestBuilder;
struct Os;
struct OutputWriter;
struct RepoModel;
struct SiteConfig;
struct TemplateSet;

/** What the release outputs of one repo are made from, and which of them the build may keep. */
struct ReleaseWriterInputs {
  /** Configuration of the build. */
  const struct SiteConfig* site_config;

  /** Compiled templates. */
  const struct TemplateSet* template_set;

  /** What the scan found for the repo. */
  const struct RepoModel* repo;

  /** Whether every template matches its row in the build cache. */
  bool is_templates_fresh;

  /** Whether the repo's releases row matches the build cache, so its releases are the earlier
      build's. */
  bool is_releases_fresh;

  /** Whether the repo's repo map row matches the build cache, which its release pages also need, as
      they show the repo's `repo` map. */
  bool is_repo_map_fresh;
};

/**
 * @brief Records the release outputs of one repo: its releases page, each project's page,
 *        `versions.txt` and `versions.json`, and each release's page and assets.
 *
 * A repo without release projects has none of them.
 *
 * @param builder Prepared manifest builder. Must not be `NULL`.
 * @param repo    What the scan found for the repo. Must not be `NULL`.
 * @param scratch Arena for the paths.
 * @param log     Log that receives every refusal. Must not be `NULL`.
 * @return `0` when every path was recorded, or `-1` otherwise.
 */
int release_writer_register(struct ManifestBuilder* builder,
                            const struct RepoModel* repo,
                            struct Arena scratch,
                            struct Log* log) __attribute__((nonnull(1, 2, 4)));

/**
 * @brief Writes the release outputs of one repo, or keeps those that are current.
 *
 * A page or a version list is kept when the templates and the releases are fresh and the file
 * exists. An asset is streamed from `releases_dir` with `OS_SOURCE_FILE`, with no size limit, and
 * is kept when the releases are fresh and the output has the asset's size.
 *
 * @param inputs  What the outputs are made from. Must not be `NULL`.
 * @param writer  Writer of the build. Must not be `NULL`.
 * @param os      Host the output is inspected on. Must not be `NULL`.
 * @param scratch Arena for the pages.
 * @param log     Log that receives every failure. Must not be `NULL`.
 * @return `0` when every output was written or kept, or `-1` otherwise.
 */
int release_writer_write(const struct ReleaseWriterInputs* inputs,
                         const struct OutputWriter* writer,
                         struct Os* os,
                         struct Arena scratch,
                         struct Log* log) __attribute__((nonnull(1, 2, 3, 5)));

#endif
