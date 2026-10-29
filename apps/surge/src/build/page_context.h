#ifndef SURGE_PAGE_CONTEXT_H
#define SURGE_PAGE_CONTEXT_H

#include "core/str.h"

struct Arena;
struct BranchTree;
struct RepoModel;
struct SiteConfig;
struct TemplateValue;

/**
 * @brief Returns the root map that every page's data tree starts with: `site`, with the site's
 *        `title`, `description`, `author` and `base_url`; `generator`, the name and version of
 *        `surge`; and `path_to_root`, the `../` prefix from the page back to the output root.
 *
 * Every link on a page is relative, so the site works under any prefix: a template writes
 * `{{path_to_root}}<path>` for a page at `<path>` below the output root.
 *
 * @param site_config   Configuration of the build. Must not be `NULL`.
 * @param relative_path Output path of the page below `output_dir`.
 * @param arena         Arena that owns the map. Must not be `NULL`.
 * @return The map, to which the page adds its own keys.
 */
struct TemplateValue* page_context_new(const struct SiteConfig* site_config,
                                       struct Str relative_path,
                                       struct Arena* arena)
    __attribute__((nonnull(1, 3), returns_nonnull));

/**
 * @brief Returns the `repo` map of a repo's pages: `name`, `description`, `default_branch`, `url`
 *        (the repo home), `clone_url`, `branches_url`, `tags_url`, `tags_feed_url`, `commits_url`
 *        (the default branch's log, empty when the default branch names no commit), `has_releases`
 *        (whether the repo has a release project), `releases_url` (its releases page, empty without
 *        one) and `releases_feed_url` (its release feed, empty without one).
 *
 * Besides the repo's name and the page recipe, it shows only what `page_context_repo_text` holds,
 * so every page of the repo is made again when that text changes.
 *
 * @param site_config  Configuration of the build. Must not be `NULL`.
 * @param repo         What the scan found for the repo. Must not be `NULL`.
 * @param path_to_root Prefix from the page back to the output root.
 * @param arena        Arena that owns the map. Must not be `NULL`.
 * @return The map.
 */
struct TemplateValue* page_context_repo(const struct SiteConfig* site_config,
                                        const struct RepoModel* repo,
                                        struct Str path_to_root,
                                        struct Arena* arena)
    __attribute__((nonnull(1, 2, 4), returns_nonnull));

/**
 * @brief Returns what the `repo` map shows of a repo besides its name: the default branch when it
 *        names a commit, whether the repo has a release project, the default branch's name and the
 *        description, as text that two different maps never share.
 *
 * @param repo  What the scan found for the repo. Must not be `NULL`.
 * @param arena Arena that owns the text. Must not be `NULL`.
 * @return The text.
 */
struct Str page_context_repo_text(const struct RepoModel* repo, struct Arena* arena)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Returns the `ref` map of a branch's pages: `name`, `tree_url` (its root's tree page),
 *        `commits_url` (its log) and `commits_feed_url` (its commit feed).
 *
 * @param repo         What the scan found for the repo. Must not be `NULL`.
 * @param tree         Tree of the branch. Must not be `NULL`.
 * @param path_to_root Prefix from the page back to the output root.
 * @param arena        Arena that owns the map. Must not be `NULL`.
 * @return The map.
 */
struct TemplateValue* page_context_branch(const struct RepoModel* repo,
                                          const struct BranchTree* tree,
                                          struct Str path_to_root,
                                          struct Arena* arena)
    __attribute__((nonnull(1, 2, 4), returns_nonnull));

/**
 * @brief Returns the `ref` map of a branch that names no commit, such as the default branch of an
 *        empty repo: the keys of `page_context_branch`, with every link empty, because the branch
 *        has no pages.
 *
 * @param branch_name Name of the branch below `refs/heads/`.
 * @param arena       Arena that owns the map. Must not be `NULL`.
 * @return The map.
 */
struct TemplateValue* page_context_unborn_branch(struct Str branch_name, struct Arena* arena)
    __attribute__((nonnull(2), returns_nonnull));

/**
 * @brief Returns a repo's clone URL, `<base_url>/<repo>.git`, which the site index and every repo
 *        page show.
 *
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param repo_name   Name of the repo.
 * @param arena       Arena that owns the URL. Must not be `NULL`.
 * @return The URL.
 */
struct Str page_context_clone_url(const struct SiteConfig* site_config,
                                  struct Str repo_name,
                                  struct Arena* arena) __attribute__((nonnull(1, 3)));

/**
 * @brief Returns the link to a page of a repo that no ref names, such as `branches/index.html`,
 *        from a page whose way back to the output root is `path_to_root`.
 *
 * @param path_to_root Prefix from the linking page back to the output root.
 * @param repo_name    Name of the repo, which uses only unreserved bytes.
 * @param page         Path of the page below the repo's directory.
 * @param arena        Arena that owns the link. Must not be `NULL`.
 * @return The link.
 */
struct Str page_context_repo_link(struct Str path_to_root,
                                  struct Str repo_name,
                                  struct Str page,
                                  struct Arena* arena) __attribute__((nonnull(4)));

#endif
