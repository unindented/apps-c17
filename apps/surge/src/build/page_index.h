#ifndef SURGE_PAGE_INDEX_H
#define SURGE_PAGE_INDEX_H

#include <stddef.h>

struct Arena;
struct RepoModel;
struct SiteConfig;
struct TemplateValue;

/** Output path of the site index below `output_dir`. */
#define PAGE_INDEX_PAGE S("index.html")

/**
 * @brief Returns the data tree of the site index, `index.html`.
 *
 * `apps/surge/README.md#data-trees` lists its keys, in the `index` row.
 *
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param repos       What the scan found, one per repo of `site_config`. Must not be `NULL`.
 * @param repo_count  Number of entries in `repos`.
 * @param arena       Arena that owns the tree. Must not be `NULL`.
 * @return The tree.
 */
struct TemplateValue* page_index_value(const struct SiteConfig* site_config,
                                       const struct RepoModel* repos,
                                       ptrdiff_t repo_count,
                                       struct Arena* arena)
    __attribute__((nonnull(1, 2, 4), returns_nonnull));

/**
 * @brief Returns the data tree of the not-found page, at `not_found_path`.
 *
 * `apps/surge/README.md#data-trees` lists its keys, in the `not_found` row. A host serves the page
 * at every depth, so its `path_to_root` is the root-relative path of `base_url`, from
 * `output_path_not_found_root`.
 *
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param arena       Arena that owns the tree. Must not be `NULL`.
 * @return The tree.
 */
struct TemplateValue* page_index_not_found_value(const struct SiteConfig* site_config,
                                                 struct Arena* arena)
    __attribute__((nonnull(1, 2), returns_nonnull));

#endif
