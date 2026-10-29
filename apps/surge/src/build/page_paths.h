#ifndef SURGE_PAGE_PATHS_H
#define SURGE_PAGE_PATHS_H

#include "core/str.h"

struct Arena;
struct Buf;

/** The kinds of per-ref pages, each a prefix below the repo's directory. */
enum PagePathKind {
  /** `<r>/tree/<ref>/<p>/index.html`, a directory's listing. */
  PAGE_PATH_TREE,

  /** `<r>/blob/<ref>/<p>/index.html`, a file's page. */
  PAGE_PATH_BLOB,

  /** `<r>/raw/<ref>/<p>`, a file's raw copy. */
  PAGE_PATH_RAW,

  /** `<r>/commits/<ref>/index.html`, a branch's log. */
  PAGE_PATH_COMMITS,
};

/**
 * @brief Returns the output path of a per-ref page below `output_dir`, with the names as they are.
 *
 * GitHub's paths, served as directory index pages: the ref's name is a run of nested directories,
 * so `feature/x` is `tree/feature/x/`, and a path below it nests further.
 *
 * @param kind      Kind of page.
 * @param repo_name Name of the repo.
 * @param ref_name  Name of the ref below `refs/heads/`.
 * @param path      Path in the ref's tree, empty for the root.
 * @param arena     Arena that owns the path. Must not be `NULL`.
 * @return The path relative to `output_dir`.
 */
struct Str page_path(enum PagePathKind kind,
                     struct Str repo_name,
                     struct Str ref_name,
                     struct Str path,
                     struct Arena* arena) __attribute__((nonnull(5)));

/**
 * @brief Returns the directory of a ref's pages of `kind` below `output_dir`, `<r>/<kind>/<ref>`,
 *        with the names as they are, for a page beside the ref's index, such as a log's
 *        `page-2.html`.
 *
 * @param kind      Kind of page.
 * @param repo_name Name of the repo.
 * @param ref_name  Name of the ref below `refs/heads/`.
 * @param arena     Arena that owns the path. Must not be `NULL`.
 * @return The directory, relative to `output_dir`.
 */
struct Str page_path_dir(enum PagePathKind kind,
                         struct Str repo_name,
                         struct Str ref_name,
                         struct Arena* arena) __attribute__((nonnull(4)));

/**
 * @brief Appends the link to the directory of a ref's pages of `kind`,
 *        `<path_to_root><r>/<kind>/<ref>`, with each segment of the ref's name percent-encoded and
 *        no `/` after it, for a page beside the ref's index, such as a log's `page-2.html`.
 *
 * @param buffer       Buffer that receives the link. Must not be `NULL`.
 * @param path_to_root Prefix from the linking page back to the output root, such as `../../`.
 * @param kind         Kind of page.
 * @param repo_name    Name of the repo.
 * @param ref_name     Name of the ref below `refs/heads/`.
 */
void page_path_append_dir_link(struct Buf* buffer,
                               struct Str path_to_root,
                               enum PagePathKind kind,
                               struct Str repo_name,
                               struct Str ref_name) __attribute__((nonnull(1)));

/**
 * @brief Appends the link to a per-ref page from a page whose way back to the output root is
 *        `path_to_root`, with each segment of the ref's name and of the path percent-encoded.
 *
 * @param buffer       Buffer that receives the link. Must not be `NULL`.
 * @param path_to_root Prefix from the linking page back to the output root, such as `../../`.
 * @param kind         Kind of page.
 * @param repo_name    Name of the repo.
 * @param ref_name     Name of the ref below `refs/heads/`.
 * @param path         Path in the ref's tree, empty for the root.
 */
void page_path_append_link(struct Buf* buffer,
                           struct Str path_to_root,
                           enum PagePathKind kind,
                           struct Str repo_name,
                           struct Str ref_name,
                           struct Str path) __attribute__((nonnull(1)));

/**
 * @brief Returns the link that `page_path_append_link` appends, in `arena`.
 *
 * @param path_to_root Prefix from the linking page back to the output root.
 * @param kind         Kind of page.
 * @param repo_name    Name of the repo.
 * @param ref_name     Name of the ref below `refs/heads/`.
 * @param path         Path in the ref's tree, empty for the root.
 * @param arena        Arena that owns the link. Must not be `NULL`.
 * @return The link.
 */
struct Str page_path_link(struct Str path_to_root,
                          enum PagePathKind kind,
                          struct Str repo_name,
                          struct Str ref_name,
                          struct Str path,
                          struct Arena* arena) __attribute__((nonnull(6)));

#endif
