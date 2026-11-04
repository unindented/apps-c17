#ifndef SURGE_PAGE_TREE_H
#define SURGE_PAGE_TREE_H

#include <stdbool.h>
#include <stddef.h>

#include "build/readme.h"
#include "core/str.h"

struct Arena;
struct BranchTree;
struct RepoModel;
struct SiteConfig;
struct TemplateValue;
struct TreeDir;
struct TreeEntry;

/** Name of the template of a repo's home and of every tree page. */
#define PAGE_TREE_TEMPLATE S("tree.html")

/** Output path of a repo home below the repo's directory. */
#define PAGE_TREE_HOME_PAGE S("index.html")

/** One document of a directory, found and rendered. */
struct TreeDocument {
  /** Kind of the document. */
  enum DocumentKind kind;

  /** The document's file. Must not be `NULL`. */
  const struct TreeEntry* entry;

  /** The document, rendered. */
  struct Str rendered_html;
};

/**
 * @brief Returns the data tree of a repo's home, `<r>/index.html`, or of the tree page of `dir`,
 *        `<r>/tree/<ref>/<p>/index.html`, which one template renders alike.
 *
 * `apps/surge/README.md#data-trees` lists its keys, in the `tree.html` rows.
 *
 * @param site_config    Configuration of the build. Must not be `NULL`.
 * @param repo           What the scan found for the repo. Must not be `NULL`.
 * @param tree           Tree of the branch, or `NULL` for the home of an empty repo.
 * @param dir            Directory of the page, the root for a home, or `NULL` when `tree` is.
 * @param is_home        Whether the page is the repo's home, which shows the default branch's root.
 * @param documents      The documents found, in kind order. Must not be `NULL` unless
 *                       `document_count` is `0`.
 * @param document_count Number of entries in `documents`.
 * @param arena          Arena that owns the tree. Must not be `NULL`.
 * @return The data tree.
 */
struct TemplateValue* page_tree_value(const struct SiteConfig* site_config,
                                      const struct RepoModel* repo,
                                      const struct BranchTree* tree,
                                      const struct TreeDir* dir,
                                      bool is_home,
                                      const struct TreeDocument* documents,
                                      ptrdiff_t document_count,
                                      struct Arena* arena)
    __attribute__((nonnull(1, 2, 8), returns_nonnull));

/**
 * @brief Returns the output path of a repo's home, `<r>/index.html`.
 *
 * @param repo_name Name of the repo.
 * @param arena     Arena that owns the path. Must not be `NULL`.
 * @return The path.
 */
struct Str page_tree_home_path(struct Str repo_name, struct Arena* arena)
    __attribute__((nonnull(2)));

/**
 * @brief Returns the list of a directory's entries as `page_tree_value` holds it, with links from a
 *        page whose way back to the output root is `path_to_root`.
 *
 * @param repo         What the scan found for the repo. Must not be `NULL`.
 * @param tree         Tree of the branch. Must not be `NULL`.
 * @param dir          Directory to list. Must not be `NULL`.
 * @param path_to_root Prefix from the page back to the output root.
 * @param arena        Arena that owns the list. Must not be `NULL`.
 * @return The list.
 */
struct TemplateValue* page_tree_entries(const struct RepoModel* repo,
                                        const struct BranchTree* tree,
                                        const struct TreeDir* dir,
                                        struct Str path_to_root,
                                        struct Arena* arena)
    __attribute__((nonnull(1, 2, 3, 5), returns_nonnull));

/**
 * @brief Returns the breadcrumbs from the repo to `path`, the last one, `path` itself, with an
 *        empty `url`.
 *
 * @param repo         What the scan found for the repo. Must not be `NULL`.
 * @param tree         Tree of the branch. Must not be `NULL`.
 * @param path         Path in the tree, empty for the root.
 * @param path_to_root Prefix from the page back to the output root.
 * @param arena        Arena that owns the list. Must not be `NULL`.
 * @return The list.
 */
struct TemplateValue* page_tree_breadcrumbs(const struct RepoModel* repo,
                                            const struct BranchTree* tree,
                                            struct Str path,
                                            struct Str path_to_root,
                                            struct Arena* arena)
    __attribute__((nonnull(1, 2, 5), returns_nonnull));

#endif
