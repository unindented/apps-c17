#ifndef SURGE_PAGE_LOG_H
#define SURGE_PAGE_LOG_H

#include <stddef.h>

#include "core/str.h"

struct Arena;
struct Buf;
struct BranchTree;
struct CommitSummary;
struct RepoModel;
struct SiteConfig;
struct TemplateValue;

/**
 * @brief Returns the number of log pages of a branch: its commits in pages of `page_size`, and one
 *        page for none.
 *
 * @param commit_count Number of the branch's commits.
 * @param page_size    Commits a page lists, positive.
 * @return The number of pages.
 */
ptrdiff_t page_log_count(ptrdiff_t commit_count, ptrdiff_t page_size);

/**
 * @brief Returns the output path of a log page below `output_dir`: `<r>/commits/<ref>/index.html`
 *        for the first, and `page-<n>.html` beside it for the `n`th, from 2.
 *
 * @param repo_name  Name of the repo.
 * @param ref_name   Name of the branch below `refs/heads/`.
 * @param page_index Index of the page, from 0.
 * @param arena      Arena that owns the path. Must not be `NULL`.
 * @return The path.
 */
struct Str page_log_path(struct Str repo_name,
                         struct Str ref_name,
                         ptrdiff_t page_index,
                         struct Arena* arena) __attribute__((nonnull(4)));

/**
 * @brief Returns the data tree of one log page.
 *
 * `apps/surge/README.md#data-trees` lists its keys, in the `commit_list` row.
 *
 * @param site_config  Configuration of the build. Must not be `NULL`.
 * @param repo         What the scan found for the repo. Must not be `NULL`.
 * @param tree         Tree of the branch. Must not be `NULL`.
 * @param page_index   Index of the page, from 0.
 * @param page_count   Number of the branch's log pages.
 * @param commits      The page's commits, newest first. Must not be `NULL` unless `commit_count` is
 *                     `0`.
 * @param commit_count Number of entries in `commits`.
 * @param arena        Arena that owns the tree. Must not be `NULL`.
 * @return The data tree.
 */
struct TemplateValue* page_log_value(const struct SiteConfig* site_config,
                                     const struct RepoModel* repo,
                                     const struct BranchTree* tree,
                                     ptrdiff_t page_index,
                                     ptrdiff_t page_count,
                                     const struct CommitSummary* commits,
                                     ptrdiff_t commit_count,
                                     struct Arena* arena)
    __attribute__((nonnull(1, 2, 3, 8), returns_nonnull));

/**
 * @brief Appends the names of the refs that name `commit_oid`, each `<kind> <name>` and a newline,
 *        in refset order: what the commit's log entry shows beside it.
 *
 * @param buffer     Buffer that receives the names. Must not be `NULL`.
 * @param repo       What the scan found for the repo. Must not be `NULL`.
 * @param commit_oid Hex id of a commit.
 */
void page_log_append_refs(struct Buf* buffer, const struct RepoModel* repo, struct Str commit_oid)
    __attribute__((nonnull(1, 2)));

#endif
