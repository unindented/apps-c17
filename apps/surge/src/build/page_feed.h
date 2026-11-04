#ifndef SURGE_PAGE_FEED_H
#define SURGE_PAGE_FEED_H

#include <stddef.h>

#include "core/arena.h"
#include "core/str.h"

struct BranchTree;
struct CommitDetail;
struct RepoModel;
struct SiteConfig;
struct TemplateValue;

/** Name of a branch's commit feed's template. */
#define PAGE_FEED_COMMITS_TEMPLATE S("commits.atom")

/** Name of a repo's tag feed's template. */
#define PAGE_FEED_TAGS_TEMPLATE S("tags.atom")

/** Name of a repo's release feed's template. */
#define PAGE_FEED_RELEASES_TEMPLATE S("releases.atom")

/** The feed templates every site needs: a branch's commits and a repo's tags. */
#define PAGE_FEED_TEMPLATES STR_INIT("commits.atom"), STR_INIT("tags.atom")

/** The feed template a site with a release project needs. */
#define PAGE_FEED_RELEASE_TEMPLATES STR_INIT("releases.atom")

/** Output path of a repo's tag feed below the repo's directory. */
#define PAGE_FEED_TAGS_PAGE S("tags.atom")

/** Output path of a repo's release feed below the repo's directory. */
#define PAGE_FEED_RELEASES_PAGE S("releases.atom")

/**
 * @brief Returns the output path of a branch's commit feed below `output_dir`,
 *        `<r>/commits/<ref>.atom`.
 *
 * @param repo_name   Name of the repo.
 * @param branch_name Name of the branch below `refs/heads/`.
 * @param arena       Arena that owns the path. Must not be `NULL`.
 * @return The path.
 */
struct Str page_feed_commits_path(struct Str repo_name, struct Str branch_name, struct Arena* arena)
    __attribute__((nonnull(3)));

/**
 * @brief Returns the data tree of a branch's commit feed, `<r>/commits/<ref>.atom`.
 *
 * Its entries are the branch's newest commits. `apps/surge/README.md#feeds` describes them, and
 * `apps/surge/README.md#data-trees` lists the keys, in the Feeds row.
 *
 * @param site_config  Configuration of the build. Must not be `NULL`.
 * @param repo         What the scan found for the repo. Must not be `NULL`.
 * @param tree         The branch's tree. Must not be `NULL`.
 * @param commits      The branch's newest commits, newest first, at most `feed_count`. Must not be
 *                     `NULL` unless `commit_count` is `0`.
 * @param commit_count Number of entries in `commits`.
 * @param arena        Arena that owns the tree. Must not be `NULL`.
 * @return The data tree.
 */
struct TemplateValue* page_feed_commits_value(const struct SiteConfig* site_config,
                                              const struct RepoModel* repo,
                                              const struct BranchTree* tree,
                                              const struct CommitDetail* commits,
                                              ptrdiff_t commit_count,
                                              struct Arena* arena)
    __attribute__((nonnull(1, 2, 3, 6), returns_nonnull));

/**
 * @brief Returns the data tree of a repo's tag feed, `<r>/tags.atom`, as `page_feed_commits_value`
 *        lays it out, without `ref`.
 *
 * Its entries are the repo's newest tags, as `apps/surge/README.md#feeds` describes.
 *
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param repo        What the scan found for the repo. Must not be `NULL`.
 * @param arena       Arena that owns the tree. Must not be `NULL`.
 * @return The data tree.
 */
struct TemplateValue* page_feed_tags_value(const struct SiteConfig* site_config,
                                           const struct RepoModel* repo,
                                           struct Arena* arena)
    __attribute__((nonnull(1, 2, 3), returns_nonnull));

/**
 * @brief Returns the data tree of a repo's release feed, `<r>/releases.atom`, as
 *        `page_feed_commits_value` lays it out, without `ref`.
 *
 * Its entries are the repo's newest releases, as `apps/surge/README.md#feeds` describes.
 *
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param repo        What the scan found for the repo. Must not be `NULL`.
 * @param perm        Arena that owns the tree. Must not be `NULL`.
 * @param scratch     Arena for rendering the notes and the sort.
 * @return The data tree.
 */
struct TemplateValue* page_feed_releases_value(const struct SiteConfig* site_config,
                                               const struct RepoModel* repo,
                                               struct Arena* perm,
                                               struct Arena scratch)
    __attribute__((nonnull(1, 2, 3), returns_nonnull));

#endif
