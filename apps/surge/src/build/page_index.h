#ifndef SURGE_PAGE_INDEX_H
#define SURGE_PAGE_INDEX_H

#include <stddef.h>

struct Arena;
struct RepoModel;
struct SiteConfig;
struct TemplateValue;

/** Name of the site index's template, and of its output below `output_dir`. */
#define PAGE_INDEX_TEMPLATE S("index.html")

/** Name of the not-found page's template, and of its output below `output_dir`. */
#define PAGE_INDEX_NOT_FOUND_TEMPLATE S("404.html")

/**
 * @brief Returns the data tree of the site index, `index.html`.
 *
 * Besides `site`, `generator` and `path_to_root`, it holds `repos`, one map per repo in config
 * order, each with `name`, `description`, `url` (the repo home), `clone_url`, `default_branch`,
 * `modified` and `projects`. A project has `name`, `description`, `url` (its directory's tree page
 * on the default branch), `has_releases` and `releases_url`.
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
 * @brief Returns the data tree of the not-found page, `404.html`, which holds `site`, `generator`
 *        and `path_to_root`.
 *
 * A host serves the page for any missing path, at any depth, so its relative links would break: a
 * template links with `{{site.base_url}}` instead.
 *
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param arena       Arena that owns the tree. Must not be `NULL`.
 * @return The tree.
 */
struct TemplateValue* page_index_not_found_value(const struct SiteConfig* site_config,
                                                 struct Arena* arena)
    __attribute__((nonnull(1, 2), returns_nonnull));

#endif
