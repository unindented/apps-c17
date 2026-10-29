#ifndef SURGE_README_H
#define SURGE_README_H

#include <stdbool.h>
#include <stddef.h>

#include "core/arena.h"
#include "core/str.h"

struct BranchTree;
struct Log;
struct Repository;
struct TreeDir;
struct TreeEntry;

/** The documents a repo home shows as tabs, in their order there. */
enum DocumentKind {
  /** `README`, the home's main content. */
  DOCUMENT_README,

  /** `LICENSE`, `LICENCE` or `COPYING`. */
  DOCUMENT_LICENSE,

  /** `CODE_OF_CONDUCT`. */
  DOCUMENT_CODE_OF_CONDUCT,

  /** `CONTRIBUTING`. */
  DOCUMENT_CONTRIBUTING,

  /** `SECURITY`. */
  DOCUMENT_SECURITY,
};

/** Number of document kinds. */
enum { DOCUMENT_KIND_COUNT = DOCUMENT_SECURITY + 1 };

/**
 * HTML level of a level 1 heading in a document, a blob, release notes or a feed: `#` is `<h1>`, as
 * GitHub renders it.
 */
enum { README_TOP_HEADING_LEVEL = 1 };

/** Where a document is rendered, which its relative links resolve against. */
struct DocumentPlace {
  /** Tree of the branch the document is on. Must not be `NULL`. */
  const struct BranchTree* tree;

  /** Name of the repo. */
  struct Str repo_name;

  /** Prefix that leads from the page back to the output root, such as `../../`. */
  struct Str path_to_root;

  /** Directory of the document in the tree, empty for the root. */
  struct Str dir_path;

  /** Largest blob given a raw copy, which an image link to a larger file cannot point at. */
  ptrdiff_t raw_len_max;
};

/**
 * @brief Returns the kind's name as a template sees it, such as `code-of-conduct`.
 *
 * @param kind Document kind.
 * @return The name.
 */
struct Str document_kind_name(enum DocumentKind kind);

/**
 * @brief Reports whether a file named `name` is Markdown (`.md`, `.markdown`) or Org (`.org`), in
 *        any case, which renders rather than showing as text.
 *
 * @param name File name.
 * @return `true` for a Markdown or Org file.
 */
bool readme_is_markup(struct Str name);

/**
 * @brief Reports whether a file named `name` is Org (`.org`), in any case.
 *
 * @param name File name.
 * @return `true` for an Org file.
 */
bool readme_is_org(struct Str name);

/**
 * @brief Finds the document of `kind` in `dir`: the first file in byte order whose name is one of
 *        the kind's base names in any case, with no extension or `.md`, `.markdown`, `.org` or
 *        `.txt`.
 *
 * A symlink, a file without pages, and a file over `blob_len_max` are skipped.
 *
 * @param dir          Directory to search. Must not be `NULL`.
 * @param kind         Kind to find.
 * @param blob_len_max Largest blob read for a page.
 * @return The entry, or `NULL` when the directory has none.
 */
const struct TreeEntry* readme_find(const struct TreeDir* dir,
                                    enum DocumentKind kind,
                                    ptrdiff_t blob_len_max) __attribute__((nonnull(1)));

/**
 * @brief Finds the documents of every kind that the page of `dir` shows, as `readme_find` finds
 *        one: each in `dir`, and for the root, else in its `docs/`.
 *
 * @param tree         Tree of the branch. Must not be `NULL`.
 * @param dir          Directory of the page, one of `tree`'s. Must not be `NULL`.
 * @param blob_len_max Largest blob read for a page.
 * @param entries_out  Receives each kind's entry, or `NULL` for a kind not found, indexed by
 *                     `enum DocumentKind`. Must not be `NULL`.
 * @return The number of documents found.
 */
ptrdiff_t readme_find_documents(const struct BranchTree* tree,
                                const struct TreeDir* dir,
                                ptrdiff_t blob_len_max,
                                const struct TreeEntry* entries_out[static DOCUMENT_KIND_COUNT])
    __attribute__((nonnull(1, 2, 4)));

/**
 * @brief Reads a document and renders it: Markdown through `markdown_to_html`, Org through
 *        `org_to_html`, and anything else HTML-escaped in a `<pre>`.
 *
 * A relative link resolves against the document's directory, and a `/`-rooted one against the
 * repo's root, to the blob page of a file, the tree page of a directory, the blob page of
 * `<target>.md` for a target that is neither and leaves out `.md`, and the raw copy of a file an
 * image shows, or its blob page when it is over `raw_len_max`. Its query and fragment are kept, and
 * an Org `file:` link's `::#<id>` search option becomes the fragment `#<id>`, while its other
 * search options, which name places the page does not anchor, are dropped. Any other target is left
 * as written, and counted in `missing_link_count_out` when it is relative and names no path of the
 * tree.
 *
 * @param repository             Open repository the document is in. Must not be `NULL`.
 * @param entry                  The document's entry. Must not be `NULL`.
 * @param place                  Where the document is rendered. Must not be `NULL`.
 * @param perm                   Arena that owns the HTML. Must not be `NULL`.
 * @param scratch                Arena for the document's bytes and the renderer's temporaries.
 * @param html_out               Receives the HTML. Must not be `NULL`.
 * @param missing_link_count_out Receives how many relative links name no path. Must not be `NULL`.
 * @param log                    Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the blob cannot be read.
 */
int readme_render(const struct Repository* repository,
                  const struct TreeEntry* entry,
                  const struct DocumentPlace* place,
                  struct Arena* perm,
                  struct Arena scratch,
                  struct Str* html_out,
                  ptrdiff_t* missing_link_count_out,
                  struct Log* log) __attribute__((nonnull(1, 2, 3, 4, 6, 7, 8)));

/**
 * @brief Renders Markdown or Org text whose links resolve as `readme_render` resolves them.
 *
 * @param text                   Document text, in any encoding.
 * @param is_org                 Whether the text is Org rather than Markdown.
 * @param place                  Where the text is rendered. Must not be `NULL`.
 * @param perm                   Arena that owns the HTML. Must not be `NULL`.
 * @param scratch                Arena for the renderer's temporaries.
 * @param missing_link_count_out Receives how many relative links name no path. Must not be `NULL`.
 * @return The HTML.
 */
struct Str readme_render_markup(struct Str text,
                                bool is_org,
                                const struct DocumentPlace* place,
                                struct Arena* perm,
                                struct Arena scratch,
                                ptrdiff_t* missing_link_count_out)
    __attribute__((nonnull(3, 4, 6)));

#endif
