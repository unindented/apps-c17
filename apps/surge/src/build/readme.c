#include "build/readme.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "build/page_paths.h"
#include "core/arena.h"
#include "core/ascii.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/str.h"
#include "core/text.h"
#include "domain/repo_model.h"
#include "formats/frontmatter.h"
#include "formats/html.h"
#include "markup/body_html.h"
#include "markup/markdown.h"
#include "markup/org.h"
#include "vcs/filetree.h"
#include "vcs/repository.h"

/** Most base names one kind of document has: `LICENSE`, `LICENCE` and `COPYING`. */
enum { DOCUMENT_BASE_NAME_COUNT_MAX = 3 };

/** One kind of document: how a template names it, and the names its file may have. */
struct DocumentSpec {
  /** Name as a template sees it. */
  struct Str name;

  /** Base names of its file, compared after ASCII case folding. */
  struct Str base_names[DOCUMENT_BASE_NAME_COUNT_MAX];
};

/** The kinds of documents, indexed by `enum DocumentKind`. */
static const struct DocumentSpec document_specs[DOCUMENT_KIND_COUNT] = {
    [DOCUMENT_README] = {STR_INIT("readme"), {STR_INIT("README")}},
    [DOCUMENT_LICENSE] = {STR_INIT("license"),
                          {STR_INIT("LICENSE"), STR_INIT("LICENCE"), STR_INIT("COPYING")}},
    [DOCUMENT_CODE_OF_CONDUCT] = {STR_INIT("code-of-conduct"), {STR_INIT("CODE_OF_CONDUCT")}},
    [DOCUMENT_CONTRIBUTING] = {STR_INIT("contributing"), {STR_INIT("CONTRIBUTING")}},
    [DOCUMENT_SECURITY] = {STR_INIT("security"), {STR_INIT("SECURITY")}},
};

/** Directory below a branch's root whose documents a repo home shows when the root has none. */
#define README_DOCS_DIR S("docs")

/** Extensions a document's file may have after its base name, or none. */
static const struct Str DOCUMENT_EXTENSIONS[] = {
    STR_INIT(""), STR_INIT(".md"), STR_INIT(".markdown"), STR_INIT(".org"), STR_INIT(".txt"),
};

/**
 * Longest link target resolved, in bytes. A tree path is at most `OUTPUT_PATH_RELATIVE_LEN_MAX`
 * long, so a longer target names no file, and the query and fragment of one that does are kept
 * within the rest.
 */
enum { README_LINK_TARGET_LEN_MAX = 64 * 1024 };

/**
 * Bytes of the region each link's URL is built in: the decoded and resolved path and their segment
 * views, a few times `README_LINK_TARGET_LEN_MAX`.
 */
enum { README_LINK_ARENA_LEN = 32 * README_LINK_TARGET_LEN_MAX };

/** What the link callback of one render needs. */
struct ReadmeLinks {
  /** Where the document is rendered. */
  const struct DocumentPlace* place;

  /**
   * Region a link's URL is built in, apart from the renderer's buffers. Each call works in a copy,
   * so nothing it allocates outlives it.
   */
  struct Arena arena;

  /** Relative links that named no path, so far. */
  ptrdiff_t missing_count;
};

/**
 * @brief Returns Markdown with its front matter, a `+++` or `---` block, as a fenced code block of
 *        its language, `toml` or `yaml`, which shows it as written without reading it.
 *
 * The fence is a run of backticks longer than any run in the front matter, so no line of it closes
 * the block.
 *
 * @param markdown Markdown text.
 * @param arena    Arena for the new text. Must not be `NULL`.
 * @return The text, or `markdown` itself when it has no closed front matter.
 */
static struct Str readme_fence_front_matter(struct Str markdown, struct Arena* arena)
    __attribute__((nonnull(2)));

/**
 * @brief Returns the length of `name` without the extension of a document, or `-1` when it has an
 *        extension that is not one.
 *
 * @param name File name.
 * @return The length of the base name, or `-1` for an extension that is not a document's.
 */
static ptrdiff_t document_base_len(struct Str name);

/**
 * @brief Writes the URL of a link's or an image's target, or declines. This is the
 *        `LinkTargetWriteFn` of `readme_render_markup`.
 *
 * @param buffer         Buffer the HTML grows in. Must not be `NULL`.
 * @param type           Org link type, or empty for Markdown.
 * @param target         Target as written.
 * @param is_image       Whether the target is an image's `src`.
 * @param is_written_out Receives whether a URL was written. Must not be `NULL`.
 * @param userdata       `struct ReadmeLinks*`. Must not be `NULL`.
 * @return `0` always: a target the tree does not have is left as written.
 */
static int readme_write_link(struct Buf* buffer,
                             struct Str type,
                             struct Str target,
                             bool is_image,
                             bool* is_written_out,
                             void* userdata) __attribute__((nonnull(1, 5, 6)));

/**
 * @brief Reports whether an Org link type is `file`, in any case, as org4c compares types.
 *
 * @param type Link type.
 * @return `true` for a `file` link.
 */
static bool is_type_file(struct Str type);

/**
 * @brief Reports whether `target` names a place outside the repo: a URL with a scheme, a
 *        protocol-relative URL, or only a query or a fragment of the page itself.
 *
 * @param target Target as written.
 * @return `true` when the target is not a path of the repo.
 */
static bool is_outside_target(struct Str target);

/**
 * @brief Resolves a relative or `/`-rooted path against `dir_path`, with its `%XX` escapes decoded
 *        and its `.` and `..` segments applied.
 *
 * @param dir_path Directory the path is relative to, empty for the root.
 * @param path     Path as written, without its query and fragment.
 * @param arena    Arena that owns the result. Must not be `NULL`.
 * @param path_out Receives the path from the root, empty for the root itself. Must not be `NULL`.
 * @return `true` when the path stays inside the repo.
 */
static bool resolve_path(struct Str dir_path,
                         struct Str path,
                         struct Arena* arena,
                         struct Str* path_out) __attribute__((nonnull(3, 4)));

/**
 * @brief Returns the value of a hex digit.
 *
 * @param c Byte to convert.
 * @return `0` to `15`, or `-1` when `c` is not a hex digit.
 */
static int hex_value(unsigned char c);

/**
 * @brief Finds the linked file at `path` of a branch's tree.
 *
 * @param tree Tree of the branch. Must not be `NULL`.
 * @param path Path from the root.
 * @return The file's entry, or `NULL` when the tree has no linked file there.
 */
static const struct TreeEntry* find_file(const struct BranchTree* tree, struct Str path)
    __attribute__((nonnull(1)));

/**
 * @brief Orders a path and a file's entry bytewise by path, for `bsearch`.
 *
 * @param key   The `struct Str` path. Must not be `NULL`.
 * @param entry The `const struct TreeEntry*`. Must not be `NULL`.
 * @return Negative, zero or positive as the path sorts before, with, or after the entry's.
 */
static int compare_file_path(const void* key, const void* entry) __attribute__((nonnull(1, 2)));

/**
 * @brief Reports whether the sorted `paths` hold `path`.
 *
 * @param paths Sorted paths. Must not be `NULL` unless `count` is `0`.
 * @param count Number of entries in `paths`.
 * @param path  Path to look for.
 * @return `true` when one of `paths` is `path`.
 */
static bool has_path(const struct Str* paths, ptrdiff_t count, struct Str path);

/**
 * @brief Orders two paths bytewise, for `bsearch`.
 *
 * @param a First `struct Str`. Must not be `NULL`.
 * @param b Second `struct Str`. Must not be `NULL`.
 * @return Negative, zero or positive as `a` sorts before, with, or after `b`.
 */
static int compare_paths(const void* a, const void* b) __attribute__((nonnull(1, 2)));

struct Str document_kind_name(enum DocumentKind kind) {
  return document_specs[kind].name;
}

bool readme_is_markup(struct Str name) {
  return text_has_suffix_folded(name, S(".md")) || text_has_suffix_folded(name, S(".markdown")) ||
         readme_is_org(name);
}

bool readme_is_org(struct Str name) {
  return text_has_suffix_folded(name, S(".org"));
}

const struct TreeEntry* readme_find(const struct TreeDir* dir,
                                    enum DocumentKind kind,
                                    ptrdiff_t blob_len_max) {
  // The entries are sorted directories first and then bytewise by name, so the first match among
  // the files is the first in byte order.
  for (ptrdiff_t i = 0; i < dir->entry_count; i++) {
    const struct TreeEntry* entry = &dir->entries[i];
    if (!filetree_entry_has_pages(entry) || entry->len > blob_len_max) {
      continue;
    }
    const ptrdiff_t base_len = document_base_len(entry->name);
    if (base_len < 0) {
      continue;
    }
    const struct Str base = str_head(entry->name, base_len);
    for (ptrdiff_t j = 0; j < DOCUMENT_BASE_NAME_COUNT_MAX; j++) {
      const struct Str candidate = document_specs[kind].base_names[j];
      if (candidate.len > 0 && candidate.len == base.len &&
          text_has_suffix_folded(base, candidate)) {
        return entry;
      }
    }
  }
  return NULL;
}

ptrdiff_t readme_find_documents(const struct BranchTree* tree,
                                const struct TreeDir* dir,
                                ptrdiff_t blob_len_max,
                                const struct TreeEntry* entries_out[static DOCUMENT_KIND_COUNT]) {
  const struct TreeDir* docs_dir = NULL;
  for (ptrdiff_t i = 1; dir == &tree->dirs[0] && i < tree->dir_count; i++) {
    if (str_eq(tree->dirs[i].path, README_DOCS_DIR)) {
      docs_dir = &tree->dirs[i];
    }
  }
  ptrdiff_t count = 0;
  for (ptrdiff_t kind = 0; kind < DOCUMENT_KIND_COUNT; kind++) {
    entries_out[kind] = readme_find(dir, (enum DocumentKind)kind, blob_len_max);
    if (entries_out[kind] == NULL && docs_dir != NULL) {
      entries_out[kind] = readme_find(docs_dir, (enum DocumentKind)kind, blob_len_max);
    }
    count += entries_out[kind] != NULL;
  }
  return count;
}

int readme_render(const struct Repository* repository,
                  const struct TreeEntry* entry,
                  const struct DocumentPlace* place,
                  struct Arena* perm,
                  struct Arena scratch,
                  struct Str* html_out,
                  ptrdiff_t* missing_link_count_out,
                  struct Log* log) {
  // A document is shown as text whatever `git_blob_is_binary` judges it.
  struct Str data = {0};
  bool is_binary = false;
  if (repository_read_blob(repository, entry->oid, &scratch, &data, &is_binary, log) != 0) {
    return -1;
  }
  *missing_link_count_out = 0;
  if (readme_is_markup(entry->name)) {
    *html_out = readme_render_markup(data, readme_is_org(entry->name), place, perm, scratch,
                                     missing_link_count_out);
    return 0;
  }
  struct Buf html = {.arena = perm};
  buf_append(&html, S("<pre>"));
  html_escape_append(&html, data);
  buf_append(&html, S("</pre>\n"));
  *html_out = buf_str(&html);
  return 0;
}

struct Str readme_render_markup(struct Str text,
                                bool is_org,
                                const struct DocumentPlace* place,
                                struct Arena* perm,
                                struct Arena scratch,
                                ptrdiff_t* missing_link_count_out) {
  if (!is_org) {
    text = readme_fence_front_matter(text, &scratch);
  }
  struct ReadmeLinks links = {
      .place = place,
      .arena = arena_split(&scratch, README_LINK_ARENA_LEN),
  };
  // A README often centers its logo in raw HTML, as GitHub shows it, so raw `<img>` and `<a>`
  // targets resolve too.
  const struct LinkTargets link_targets = {
      .write = readme_write_link,
      .userdata = &links,
      .is_raw_html_linked = true,
  };
  const struct Str html =
      is_org ? org_to_html(text, &link_targets, README_TOP_HEADING_LEVEL, perm, scratch)
             : markdown_to_html(text, &link_targets, README_TOP_HEADING_LEVEL, perm, scratch);
  *missing_link_count_out = links.missing_count;
  return html;
}

static struct Str readme_fence_front_matter(struct Str markdown, struct Arena* arena) {
  struct Frontmatter found = {0};
  if (frontmatter_find(markdown, &found) != FRONTMATTER_FOUND) {
    return markdown;
  }
  ptrdiff_t longest_run = 0;
  ptrdiff_t run = 0;
  for (ptrdiff_t i = 0; i < found.text.len; i++) {
    run = found.text.data[i] == '`' ? run + 1 : 0;
    longest_run = run > longest_run ? run : longest_run;
  }
  const ptrdiff_t fence_len = longest_run >= 3 ? longest_run + 1 : 3;
  struct Buf text = {.arena = arena};
  for (ptrdiff_t i = 0; i < fence_len; i++) {
    buf_append_char(&text, '`');
  }
  buf_append(&text, frontmatter_language(found.kind));
  buf_append_char(&text, '\n');
  buf_append(&text, found.text);
  for (ptrdiff_t i = 0; i < fence_len; i++) {
    buf_append_char(&text, '`');
  }
  buf_append(&text, S("\n\n"));
  buf_append(&text, found.body);
  return buf_str(&text);
}

static ptrdiff_t document_base_len(struct Str name) {
  for (ptrdiff_t i = 1;
       i < (ptrdiff_t)(sizeof(DOCUMENT_EXTENSIONS) / sizeof(DOCUMENT_EXTENSIONS[0])); i++) {
    if (text_has_suffix_folded(name, DOCUMENT_EXTENSIONS[i]) &&
        name.len > DOCUMENT_EXTENSIONS[i].len) {
      return name.len - DOCUMENT_EXTENSIONS[i].len;
    }
  }
  // A name with no `.` has no extension, and one with another extension is no document.
  for (ptrdiff_t i = 0; i < name.len; i++) {
    if (name.data[i] == '.') {
      return -1;
    }
  }
  return name.len;
}

static int readme_write_link(struct Buf* buffer,
                             struct Str type,
                             struct Str target,
                             bool is_image,
                             bool* is_written_out,
                             void* userdata) {
  struct ReadmeLinks* links = userdata;
  const struct DocumentPlace* place = links->place;
  *is_written_out = false;
  // An Org link of another type, such as a heading's `fuzzy` link, names no file.
  const bool is_org_file = type.len > 0;
  if ((is_org_file && !is_type_file(type)) || target.len == 0 ||
      target.len > README_LINK_TARGET_LEN_MAX) {
    return 0;
  }
  struct Arena arena = links->arena;
  ptrdiff_t path_len = 0;
  struct Str suffix = {0};
  if (is_org_file) {
    // Org writes a search option after `::`, and only a custom id names an anchor of the page.
    while (path_len < target.len && !str_startswith(str_tail(target, path_len), S("::"))) {
      path_len++;
    }
    const struct Str option =
        path_len < target.len ? str_tail(target, path_len + 2) : (struct Str){0};
    suffix = str_startswith(option, S("#")) ? option : (struct Str){0};
  } else {
    while (path_len < target.len && target.data[path_len] != '?' && target.data[path_len] != '#') {
      path_len++;
    }
    suffix = str_tail(target, path_len);
  }
  // An Org search option holds a `:`, so a scheme is looked for in the path alone.
  if (path_len == 0 || is_outside_target(str_head(target, path_len))) {
    return 0;
  }
  struct Str path = {0};
  if (!resolve_path(place->dir_path, str_head(target, path_len), &arena, &path)) {
    return 0;
  }
  const struct BranchTree* tree = place->tree;
  enum PagePathKind kind = PAGE_PATH_TREE;
  const struct Str md_path = str_concat(path, S(".md"), &arena);
  const struct TreeEntry* file = path.len > 0 ? find_file(tree, path) : NULL;
  if (file != NULL) {
    // A file over the raw limit has no raw copy, so an image of it links its blob page.
    kind = is_image && file->len <= place->raw_len_max ? PAGE_PATH_RAW : PAGE_PATH_BLOB;
  } else if (path.len == 0 || has_path(tree->dir_paths, tree->dir_path_count, path)) {
    // A directory wins over a Markdown file of its name, as `guide/` means the directory.
    kind = PAGE_PATH_TREE;
  } else if (!text_has_suffix_folded(path, S(".md")) && find_file(tree, md_path) != NULL) {
    path = md_path;
    kind = PAGE_PATH_BLOB;
  } else {
    links->missing_count++;
    return 0;
  }
  struct Buf url = {.arena = &arena};
  page_path_append_link(&url, place->path_to_root, kind, place->repo_name, tree->branch->short_name,
                        path);
  buf_append(&url, suffix);
  body_html_url_escape_append(buffer, buf_str(&url));
  *is_written_out = true;
  return 0;
}

static bool is_type_file(struct Str type) {
  return type.len == S("file").len && text_has_suffix_folded(type, S("file"));
}

static bool is_outside_target(struct Str target) {
  if (target.data[0] == '#' || target.data[0] == '?' || str_startswith(target, S("//"))) {
    return true;
  }
  // A scheme is a letter, then letters, digits, `+`, `-` or `.`, then a `:`, as RFC 3986 says.
  if (!ascii_is_alphanumeric((unsigned char)target.data[0]) ||
      ascii_is_digit((unsigned char)target.data[0])) {
    return false;
  }
  for (ptrdiff_t i = 1; i < target.len; i++) {
    const unsigned char c = (unsigned char)target.data[i];
    if (c == ':') {
      return true;
    }
    if (!ascii_is_alphanumeric(c) && c != '+' && c != '-' && c != '.') {
      return false;
    }
  }
  return false;
}

static bool resolve_path(struct Str dir_path,
                         struct Str path,
                         struct Arena* arena,
                         struct Str* path_out) {
  struct Buf decoded = {.arena = arena};
  for (ptrdiff_t i = 0; i < path.len; i++) {
    const unsigned char c = (unsigned char)path.data[i];
    if (c == '%' && i + 2 < path.len) {
      const int high = hex_value((unsigned char)path.data[i + 1]);
      const int low = hex_value((unsigned char)path.data[i + 2]);
      if (high >= 0 && low >= 0) {
        buf_append_char(&decoded, (char)(high * 16 + low));
        i += 2;
        continue;
      }
    }
    buf_append_char(&decoded, (char)c);
  }
  const struct Str relative = buf_str(&decoded);
  const bool is_rooted = str_startswith(relative, S("/"));
  struct Str joined = is_rooted ? relative : str_concat(dir_path, S("/"), arena);
  if (!is_rooted) {
    joined = str_concat(joined, relative, arena);
  }
  // Each kept segment is a view into `joined`, and at most one per `/` of it.
  ptrdiff_t capacity = 1;
  for (ptrdiff_t i = 0; i < joined.len; i++) {
    capacity += joined.data[i] == '/';
  }
  struct Str* segments = arena_new(arena, capacity, struct Str);
  ptrdiff_t count = 0;
  struct StrCut cut = {.tail = joined, .is_found = true};
  while (cut.is_found) {
    cut = str_cut(cut.tail, '/');
    if (cut.head.len == 0 || str_eq(cut.head, S("."))) {
      continue;
    }
    if (str_eq(cut.head, S(".."))) {
      if (count == 0) {
        return false;
      }
      count--;
      continue;
    }
    segments[count++] = cut.head;
  }
  struct Buf resolved = {.arena = arena};
  for (ptrdiff_t i = 0; i < count; i++) {
    if (i > 0) {
      buf_append_char(&resolved, '/');
    }
    buf_append(&resolved, segments[i]);
  }
  *path_out = buf_str(&resolved);
  return true;
}

static int hex_value(unsigned char c) {
  if (ascii_is_digit(c)) {
    return c - '0';
  }
  const unsigned char lower = ascii_to_lower(c);
  return lower >= 'a' && lower <= 'f' ? lower - 'a' + 10 : -1;
}

static const struct TreeEntry* find_file(const struct BranchTree* tree, struct Str path) {
  if (tree->file_count == 0) {
    return NULL;
  }
  const struct TreeEntry* const* found = bsearch(&path, tree->files, (size_t)tree->file_count,
                                                 sizeof(*tree->files), compare_file_path);
  return found != NULL ? *found : NULL;
}

static int compare_file_path(const void* key, const void* entry) {
  return str_compare(*(const struct Str*)key, (*(const struct TreeEntry* const*)entry)->path);
}

static bool has_path(const struct Str* paths, ptrdiff_t count, struct Str path) {
  return count > 0 && bsearch(&path, paths, (size_t)count, sizeof(*paths), compare_paths) != NULL;
}

static int compare_paths(const void* a, const void* b) {
  return str_compare(*(const struct Str*)a, *(const struct Str*)b);
}
