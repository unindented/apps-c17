#ifndef SURGE_PAGE_BLOB_H
#define SURGE_PAGE_BLOB_H

#include <stdbool.h>

#include "core/arena.h"
#include "core/str.h"

struct BranchTree;
struct HighlightGrammar;
struct RepoModel;
struct SiteConfig;
struct TemplateValue;
struct TreeEntry;

/** What the job of a blob page read of the blob, which the page shows. */
struct BlobContent {
  /** Bytes of the blob, or empty when it is over `blob_len_max` and was not read. */
  struct Str data;

  /** Whether the blob is over `blob_len_max`, so its page has no contents. */
  bool is_too_large;

  /**
   * Whether `git_blob_is_binary` judges the blob binary, so its page shows no lines. Always `false`
   * for a blob over `blob_len_max`, whose bytes a build reads only for its raw copy.
   */
  bool is_binary;

  /** Whether the blob has a raw copy, which it does up to `raw_len_max`. */
  bool has_raw;

  /** The rendered view of a Markdown or Org blob, or empty for another. */
  struct Str rendered_html;
};

/**
 * @brief Reports whether the blob page of a file named `name` shows it with `<img>`: a `.png`,
 *        `.jpg`, `.jpeg`, `.gif`, `.webp`, `.svg`, `.avif`, `.bmp` or `.ico` file, in any case.
 *
 * @param name File name.
 * @return `true` for an image file.
 */
bool page_blob_is_image(struct Str name);

/**
 * @brief Returns the media type of a file named `name` that `page_blob_is_image` judges an image,
 *        such as `image/png`.
 *
 * @param name File name.
 * @return The media type, or empty for a file that is no image.
 */
struct Str page_blob_image_type(struct Str name);

/**
 * @brief Returns the data tree of the blob page of `entry`, `<r>/blob/<ref>/<p>/index.html`.
 *
 * `apps/surge/README.md#data-trees` lists its keys, in the `blob` row.
 *
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param repo        What the scan found for the repo. Must not be `NULL`.
 * @param tree        Tree of the branch. Must not be `NULL`.
 * @param entry       The file. Must not be `NULL`.
 * @param content     What the job read of the blob. Must not be `NULL`.
 * @param perm        Arena that owns the tree. Must not be `NULL`.
 * @param scratch     Arena for the writer of the lines.
 * @return The data tree.
 */
struct TemplateValue* page_blob_value(const struct SiteConfig* site_config,
                                      const struct RepoModel* repo,
                                      const struct BranchTree* tree,
                                      const struct TreeEntry* entry,
                                      const struct BlobContent* content,
                                      struct Arena* perm,
                                      struct Arena scratch)
    __attribute__((nonnull(1, 2, 3, 4, 5, 6), returns_nonnull));

/**
 * @brief Returns text as the `<pre class="surge-code">` of `source_html`, with a
 *        `<span class="tok-…">` around each token `grammar` finds.
 *
 * @param text           Text of the blob.
 * @param grammar        Grammar of the blob's language, or `NULL` to escape the text only.
 * @param perm           Arena that owns the HTML. Must not be `NULL`.
 * @param scratch        Arena for tree-sitter's memory.
 * @param line_count_out Receives the number of lines. Must not be `NULL`.
 * @return The HTML.
 */
struct Str page_blob_lines(struct Str text,
                           const struct HighlightGrammar* grammar,
                           struct Arena* perm,
                           struct Arena scratch,
                           ptrdiff_t* line_count_out) __attribute__((nonnull(3, 5)));

#endif
