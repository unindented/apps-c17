#ifndef SURGE_PAGE_COMMIT_H
#define SURGE_PAGE_COMMIT_H

#include <stddef.h>

#include "core/arena.h"
#include "core/str.h"

struct Changeset;
struct ChangedFile;
struct CommitDetail;
struct RepoModel;
struct SiteConfig;
struct TemplateValue;

/** The bytes of the two sides of one changed image, which a commit page embeds. */
struct ChangedImage {
  /** The blob before the commit, or empty for an added file or a side left out. */
  struct Str old_data;

  /** The blob after the commit, or empty for a deleted file or a side left out. */
  struct Str new_data;
};

/**
 * @brief Returns the output path of a commit page below `output_dir`,
 *        `<r>/commit/<oid>/index.html`.
 *
 * @param repo_name  Name of the repo.
 * @param commit_oid Hex id of the commit.
 * @param arena      Arena that owns the path. Must not be `NULL`.
 * @return The path.
 */
struct Str page_commit_path(struct Str repo_name, struct Str commit_oid, struct Arena* arena)
    __attribute__((nonnull(3)));

/**
 * @brief Returns the link from a page to a commit page, its output path after `path_to_root`.
 *
 * @param path_to_root Prefix from the page back to the output root, or an absolute one.
 * @param repo_name    Name of the repo.
 * @param commit_oid   Hex id of the commit.
 * @param arena        Arena that owns the link. Must not be `NULL`.
 * @return The link.
 */
struct Str page_commit_link(struct Str path_to_root,
                            struct Str repo_name,
                            struct Str commit_oid,
                            struct Arena* arena) __attribute__((nonnull(4)));

/**
 * @brief Returns the data tree of a commit page.
 *
 * `apps/surge/README.md#data-trees` lists its keys, in the `commit` rows, and describes
 * `diff_html`.
 *
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param repo        What the scan found for the repo. Must not be `NULL`.
 * @param detail      The commit. Must not be `NULL`.
 * @param changeset   Its changes. Must not be `NULL`.
 * @param fragments   Each file's fragments, as `page_commit_diff_html` takes them, in the files'
 *                    order, or `NULL` when no file is highlighted.
 * @param images      Each file's image bytes, in the files' order, or `NULL` when the page embeds
 *                    none.
 * @param perm        Arena that owns the tree. Must not be `NULL`.
 * @param scratch     Arena for the diff lines' anchors.
 * @return The data tree.
 */
struct TemplateValue* page_commit_value(const struct SiteConfig* site_config,
                                        const struct RepoModel* repo,
                                        const struct CommitDetail* detail,
                                        const struct Changeset* changeset,
                                        const struct Str* const* fragments,
                                        const struct ChangedImage* images,
                                        struct Arena* perm,
                                        struct Arena scratch)
    __attribute__((nonnull(1, 2, 3, 4, 7), returns_nonnull));

/**
 * @brief Returns one file's diff as the `diff_html` of `page_commit_value`.
 *
 * @param file      The file. Must not be `NULL`.
 * @param fragments The HTML of each line of `file`, as `highlight_diff_file` returns it, or `NULL`
 *                  to escape each line alone.
 * @param anchor    The file's anchor, such as `diff-0`.
 * @param perm      Arena that owns the HTML, where nothing else grows while it is written. Must not
 *                  be `NULL`.
 * @param scratch   Arena for each line's anchors.
 * @return The HTML, empty for a file whose diff is not shown or has no lines.
 */
struct Str page_commit_diff_html(const struct ChangedFile* file,
                                 const struct Str* fragments,
                                 struct Str anchor,
                                 struct Arena* perm,
                                 struct Arena scratch) __attribute__((nonnull(1, 4)));

#endif
