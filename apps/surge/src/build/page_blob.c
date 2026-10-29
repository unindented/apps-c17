#include "build/page_blob.h"

#include <stdbool.h>
#include <stddef.h>

#include "build/page_context.h"
#include "build/page_paths.h"
#include "build/page_tree.h"
#include "build/readme.h"
#include "build/template_value.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/str.h"
#include "core/text.h"
#include "domain/grammar_path.h"
#include "domain/repo_model.h"
#include "domain/site_config.h"
#include "markup/body_html.h"
#include "markup/highlight.h"

/** An extension of the files a blob page shows with `<img>`, and the media type of its files. */
struct PageBlobImageType {
  /** The extension, with its `.`, compared after ASCII case folding. */
  struct Str extension;

  /** The media type. */
  struct Str media_type;
};

/** Every image extension. */
static const struct PageBlobImageType PAGE_BLOB_IMAGE_TYPES[] = {
    {STR_INIT(".png"), STR_INIT("image/png")},
    {STR_INIT(".jpg"), STR_INIT("image/jpeg")},
    {STR_INIT(".jpeg"), STR_INIT("image/jpeg")},
    {STR_INIT(".gif"), STR_INIT("image/gif")},
    {STR_INIT(".webp"), STR_INIT("image/webp")},
    {STR_INIT(".svg"), STR_INIT("image/svg+xml")},
    {STR_INIT(".avif"), STR_INIT("image/avif")},
    {STR_INIT(".bmp"), STR_INIT("image/bmp")},
    {STR_INIT(".ico"), STR_INIT("image/vnd.microsoft.icon")},
};

/**
 * @brief Writes the start of one code line, with its number, which links to itself. This is the
 *        `open` callback of `page_blob_lines`.
 *
 * @param buffer   Buffer the lines grow in. Must not be `NULL`.
 * @param line     Index of the line, from 0.
 * @param userdata `ptrdiff_t*` that counts the lines. Must not be `NULL`.
 */
static void page_blob_lines_open(struct Buf* buffer, ptrdiff_t line, void* userdata)
    __attribute__((nonnull(1, 3)));

/**
 * @brief Writes the end of one code line, with its line end. This is the `close` callback of
 *        `page_blob_lines`.
 *
 * @param buffer   Buffer the lines grow in. Must not be `NULL`.
 * @param line     Index of the line, from 0.
 * @param is_ended Whether the line ends in `\n`.
 * @param userdata `ptrdiff_t*` that counts the lines.
 */
static void page_blob_lines_close(struct Buf* buffer, ptrdiff_t line, bool is_ended, void* userdata)
    __attribute__((nonnull(1)));

bool page_blob_is_image(struct Str name) {
  return page_blob_image_type(name).len > 0;
}

struct Str page_blob_image_type(struct Str name) {
  const ptrdiff_t count =
      (ptrdiff_t)(sizeof(PAGE_BLOB_IMAGE_TYPES) / sizeof(PAGE_BLOB_IMAGE_TYPES[0]));
  for (ptrdiff_t i = 0; i < count; i++) {
    const struct Str extension = PAGE_BLOB_IMAGE_TYPES[i].extension;
    if (name.len > extension.len && text_has_suffix_folded(name, extension)) {
      return PAGE_BLOB_IMAGE_TYPES[i].media_type;
    }
  }
  return (struct Str){0};
}

struct TemplateValue* page_blob_value(const struct SiteConfig* site_config,
                                      const struct RepoModel* repo,
                                      const struct BranchTree* tree,
                                      const struct TreeEntry* entry,
                                      const struct BlobContent* content,
                                      struct Arena* perm,
                                      struct Arena scratch) {
  const struct Str repo_name = repo->config->name;
  const struct Str branch = tree->branch->short_name;
  const struct Str page = page_path(PAGE_PATH_BLOB, repo_name, branch, entry->path, perm);
  struct TemplateValue* value = page_context_new(site_config, page, perm);
  const struct Str path_to_root = template_value_find(value, S("path_to_root"))->text;
  template_value_set(value, S("repo"), page_context_repo(site_config, repo, path_to_root, perm),
                     perm);
  template_value_set(value, S("ref"), page_context_branch(repo, tree, path_to_root, perm), perm);
  template_value_set_text(value, S("path"), entry->path, perm);
  template_value_set_text(value, S("name"), entry->name, perm);
  template_value_set(value, S("breadcrumbs"),
                     page_tree_breadcrumbs(repo, tree, entry->path, path_to_root, perm), perm);
  const bool is_executable = entry->kind == TREE_ENTRY_EXECUTABLE;
  template_value_set_text(value, S("mode"), is_executable ? S("100755") : S("100644"), perm);
  template_value_set_bool(value, S("is_executable"), is_executable, perm);
  struct Buf size = {.arena = perm};
  buf_printf(&size, "%td", entry->len);
  template_value_set_text(value, S("size"), buf_str(&size), perm);
  template_value_set_text(value, S("raw_url"),
                          content->has_raw ? page_path_link(path_to_root, PAGE_PATH_RAW, repo_name,
                                                            branch, entry->path, perm)
                                           : (struct Str){0},
                          perm);
  template_value_set_bool(value, S("has_raw"), content->has_raw, perm);
  template_value_set_bool(value, S("is_too_large"), content->is_too_large, perm);
  template_value_set_bool(value, S("is_binary"), content->is_binary, perm);
  template_value_set_bool(value, S("is_image"), page_blob_is_image(entry->name), perm);
  template_value_set_bool(value, S("is_markup"), readme_is_markup(entry->name), perm);
  template_value_set_text(value, S("rendered_html"), content->rendered_html, perm);
  ptrdiff_t line_count = 0;
  struct Str source_html = {0};
  if (!content->is_too_large && !content->is_binary) {
    const struct HighlightGrammar* grammar =
        highlight_grammar_find(grammar_path_language(entry->path));
    source_html = page_blob_lines(content->data, grammar, perm, scratch, &line_count);
  }
  template_value_set_text(value, S("source_html"), source_html, perm);
  struct Buf lines = {.arena = perm};
  buf_printf(&lines, "%td", line_count);
  template_value_set_text(value, S("line_count"), buf_str(&lines), perm);
  return value;
}

struct Str page_blob_lines(struct Str text,
                           const struct HighlightGrammar* grammar,
                           struct Arena* perm,
                           struct Arena scratch,
                           ptrdiff_t* line_count_out) {
  ptrdiff_t line_count = 0;
  const struct HighlightLines lines = {
      .open = page_blob_lines_open,
      .close = page_blob_lines_close,
      .userdata = &line_count,
  };
  struct Buf html = {.arena = perm};
  buf_append(&html, S("<pre class=\"surge-code\"><code>"));
  highlight_write(&html, grammar, text, &lines, scratch);
  buf_append(&html, S("</code></pre>\n"));
  *line_count_out = line_count;
  return buf_str(&html);
}

static void page_blob_lines_open(struct Buf* buffer, ptrdiff_t line, void* userdata) {
  ptrdiff_t* line_count = userdata;
  *line_count = line + 1;
  const struct CodeLineNumber number = {.value = line + 1, .anchor_prefix = S("L")};
  body_html_code_line_open(buffer, &(struct CodeLine){.numbers = &number, .number_count = 1});
}

static void page_blob_lines_close(struct Buf* buffer,
                                  ptrdiff_t line,
                                  bool is_ended,
                                  void* userdata) {
  (void)line;
  (void)userdata;
  body_html_code_line_close(buffer, is_ended);
}
