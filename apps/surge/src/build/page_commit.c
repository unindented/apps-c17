#include "build/page_commit.h"

#include <stdbool.h>
#include <stddef.h>

#include "build/page_blob.h"
#include "build/page_context.h"
#include "build/page_refs.h"
#include "build/template_value.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "domain/site_config.h"
#include "formats/html.h"
#include "markup/body_html.h"

/** Name of each change status as a template sees it, indexed by `enum ChangeStatus`. */
static const struct Str PAGE_COMMIT_STATUS_NAMES[] = {
    [CHANGE_ADDED] = STR_INIT("added"),
    [CHANGE_DELETED] = STR_INIT("deleted"),
    [CHANGE_MODIFIED] = STR_INIT("modified"),
    [CHANGE_RENAMED] = STR_INIT("renamed"),
    [CHANGE_TYPE_CHANGED] = STR_INIT("type-changed"),
};

/** The `is_<status>` field of each change status, indexed by `enum ChangeStatus`. */
static const struct Str PAGE_COMMIT_STATUS_FIELDS[] = {
    [CHANGE_ADDED] = STR_INIT("is_added"),
    [CHANGE_DELETED] = STR_INIT("is_deleted"),
    [CHANGE_MODIFIED] = STR_INIT("is_modified"),
    [CHANGE_RENAMED] = STR_INIT("is_renamed"),
    [CHANGE_TYPE_CHANGED] = STR_INIT("is_type_changed"),
};

/** Number of change statuses. */
enum { PAGE_COMMIT_STATUS_COUNT = CHANGE_TYPE_CHANGED + 1 };

/**
 * @brief Returns the `commit` map of a commit page.
 *
 * @param repo         What the scan found for the repo. Must not be `NULL`.
 * @param detail       The commit. Must not be `NULL`.
 * @param path_to_root Prefix from the page back to the output root.
 * @param perm         Arena that owns the map. Must not be `NULL`.
 * @return The map.
 */
static struct TemplateValue* page_commit_value_commit(const struct RepoModel* repo,
                                                      const struct CommitDetail* detail,
                                                      struct Str path_to_root,
                                                      struct Arena* perm)
    __attribute__((nonnull(1, 2, 4), returns_nonnull));

/**
 * @brief Returns the map of one changed file.
 *
 * @param file      The file. Must not be `NULL`.
 * @param fragments The HTML of each line of `file`, or `NULL` to escape each line alone.
 * @param image     The bytes of the file's image sides, or `NULL` for none.
 * @param index     Index of the file in the changed-files tree's order.
 * @param perm      Arena that owns the map. Must not be `NULL`.
 * @param scratch   Arena for the diff lines' anchors.
 * @return The map.
 */
static struct TemplateValue* page_commit_value_file(const struct ChangedFile* file,
                                                    const struct Str* fragments,
                                                    const struct ChangedImage* image,
                                                    ptrdiff_t index,
                                                    struct Arena* perm,
                                                    struct Arena scratch)
    __attribute__((nonnull(1, 5), returns_nonnull));

/**
 * @brief Returns a `data:` URI of an image's bytes, in base64.
 *
 * @param media_type The image's media type, such as `image/png`.
 * @param data       The image's bytes.
 * @param arena      Arena that owns the URI. Must not be `NULL`.
 * @return The URI, or empty for no bytes.
 */
static struct Str page_commit_value_file_image_url(struct Str media_type,
                                                   struct Str data,
                                                   struct Arena* arena) __attribute__((nonnull(3)));

/**
 * @brief Returns the nodes of the changed-files tree for the files in `[begin, end)`, which share
 *        their first `depth` path segments and are in the tree's order.
 *
 * @param changeset The changes. Must not be `NULL`.
 * @param maps      Each file's map, in the files' order. Must not be `NULL`.
 * @param begin     Index of the first file.
 * @param end       One past the last file.
 * @param depth     Number of leading segments the files share.
 * @param perm      Arena that owns the nodes. Must not be `NULL`.
 * @return The list of nodes.
 */
static struct TemplateValue* page_commit_value_tree(const struct Changeset* changeset,
                                                    const struct TemplateValue* const* maps,
                                                    ptrdiff_t begin,
                                                    ptrdiff_t end,
                                                    ptrdiff_t depth,
                                                    struct Arena* perm)
    __attribute__((nonnull(1, 2, 6), returns_nonnull));

/**
 * @brief Returns the end of the node that starts at file `begin`: one past the last file below the
 *        directory segment `depth` of `begin` names, or `begin + 1` for a file.
 *
 * @param changeset The changes, in the tree's order. Must not be `NULL`.
 * @param begin     Index of the node's first file.
 * @param end       One past the last file of the enclosing directory.
 * @param depth     Index of the node's segment.
 * @return One past the node's last file.
 */
static ptrdiff_t node_end(const struct Changeset* changeset,
                          ptrdiff_t begin,
                          ptrdiff_t end,
                          ptrdiff_t depth) __attribute__((nonnull(1)));

/**
 * @brief Returns segment `depth` of a changed file's path, and whether more segments follow it.
 *
 * @param file       The file. Must not be `NULL`.
 * @param depth      Index of the segment, from 0.
 * @param is_dir_out Receives whether the segment names a directory. Must not be `NULL`.
 * @return The segment.
 */
static struct Str path_segment(const struct ChangedFile* file, ptrdiff_t depth, bool* is_dir_out)
    __attribute__((nonnull(1, 3)));

/**
 * @brief Appends one diff line as a code line, with its old and new numbers, each linking to
 *        itself, and its origin's class.
 *
 * @param buffer     Buffer that receives the line. Must not be `NULL`.
 * @param line       The line. Must not be `NULL`.
 * @param fragment   The line's HTML, or `NULL` to escape the line alone.
 * @param anchor     The file's anchor, such as `diff-0`.
 * @param hunk_index Index of the line's hunk, from 0.
 * @param scratch    Arena for the line's anchors.
 */
static void append_diff_line(struct Buf* buffer,
                             const struct DiffLine* line,
                             const struct Str* fragment,
                             struct Str anchor,
                             ptrdiff_t hunk_index,
                             struct Arena scratch) __attribute__((nonnull(1, 2)));

/**
 * @brief Returns a decimal number as text.
 *
 * @param number Number to write.
 * @param arena  Arena that owns the text. Must not be `NULL`.
 * @return The text.
 */
static struct Str number_text(ptrdiff_t number, struct Arena* arena) __attribute__((nonnull(2)));

struct Str page_commit_path(struct Str repo_name, struct Str commit_oid, struct Arena* arena) {
  struct Buf path = {.arena = arena};
  buf_printf(&path, "%.*s/commit/%.*s/index.html", STR_FMT(repo_name), STR_FMT(commit_oid));
  return buf_str(&path);
}

struct Str page_commit_link(struct Str path_to_root,
                            struct Str repo_name,
                            struct Str commit_oid,
                            struct Arena* arena) {
  // A repo's name and a commit's id hold no byte a link needs to encode.
  return str_concat(path_to_root, page_commit_path(repo_name, commit_oid, arena), arena);
}

struct TemplateValue* page_commit_value(const struct SiteConfig* site_config,
                                        const struct RepoModel* repo,
                                        const struct CommitDetail* detail,
                                        const struct Changeset* changeset,
                                        const struct Str* const* fragments,
                                        const struct ChangedImage* images,
                                        struct Arena* perm,
                                        struct Arena scratch) {
  const struct Str page = page_commit_path(repo->config->name, detail->summary.oid, perm);
  struct TemplateValue* value = page_context_new(site_config, page, perm);
  const struct Str path_to_root = template_value_find(value, S("path_to_root"))->text;
  template_value_set(value, S("repo"), page_context_repo(site_config, repo, path_to_root, perm),
                     perm);
  template_value_set(value, S("commit"), page_commit_value_commit(repo, detail, path_to_root, perm),
                     perm);
  struct TemplateValue* stats = template_value_map(perm);
  template_value_set_text(stats, S("file_count"), number_text(changeset->file_count, perm), perm);
  template_value_set_text(stats, S("additions"), number_text(changeset->additions, perm), perm);
  template_value_set_text(stats, S("deletions"), number_text(changeset->deletions, perm), perm);
  template_value_set(value, S("stats"), stats, perm);
  template_value_set_bool(value, S("is_truncated"), changeset->is_truncated, perm);
  struct TemplateValue* files = template_value_list(changeset->file_count, perm);
  for (ptrdiff_t i = 0; i < changeset->file_count; i++) {
    files->items[i] =
        page_commit_value_file(&changeset->files[i], fragments != NULL ? fragments[i] : NULL,
                               images != NULL ? &images[i] : NULL, i, perm, scratch);
  }
  // The tree's root is a directory node too, so a template recurses into it as into any other.
  struct TemplateValue* tree = template_value_map(perm);
  template_value_set_text(tree, S("name"), (struct Str){0}, perm);
  template_value_set_bool(tree, S("is_dir"), true, perm);
  template_value_set(
      tree, S("children"),
      page_commit_value_tree(changeset, files->items, 0, changeset->file_count, 0, perm), perm);
  template_value_set(tree, S("file"), template_value_bool(false, perm), perm);
  template_value_set(value, S("tree"), tree, perm);
  template_value_set(value, S("files"), files, perm);
  return value;
}

struct Str page_commit_diff_html(const struct ChangedFile* file,
                                 const struct Str* fragments,
                                 struct Str anchor,
                                 struct Arena* perm,
                                 struct Arena scratch) {
  if (!file->is_shown || file->line_count == 0) {
    return (struct Str){0};
  }
  struct Buf html = {.arena = perm};
  buf_append(&html, S("<pre class=\"surge-diff\"><code>"));
  // A hunk's header has no numbers, but it keeps their columns, which hold the lines' numbers.
  const struct CodeLineNumber no_numbers[2] = {0};
  for (ptrdiff_t h = 0; h < file->hunk_count; h++) {
    const struct DiffHunk* hunk = &file->hunks[h];
    body_html_code_line_open(
        &html, &(struct CodeLine){
                   .class_name = S("surge-hunk"), .numbers = no_numbers, .number_count = 2});
    html_escape_append(&html, hunk->header);
    body_html_code_line_close(&html, true);
    for (ptrdiff_t l = 0; l < hunk->line_count; l++) {
      const ptrdiff_t line_index = hunk->line_index + l;
      append_diff_line(&html, &file->lines[line_index],
                       fragments != NULL ? &fragments[line_index] : NULL, anchor, h, scratch);
    }
  }
  buf_append(&html, S("</code></pre>\n"));
  return buf_str(&html);
}

static struct TemplateValue* page_commit_value_commit(const struct RepoModel* repo,
                                                      const struct CommitDetail* detail,
                                                      struct Str path_to_root,
                                                      struct Arena* perm) {
  struct TemplateValue* map = page_refs_commit(repo, detail->summary, path_to_root, perm);
  template_value_set_text(map, S("body"), detail->body, perm);
  template_value_set_text(map, S("author_date"), detail->author_date, perm);
  template_value_set_text(map, S("committer"), detail->committer, perm);
  template_value_set_bool(map, S("is_root"), detail->parent_count == 0, perm);
  template_value_set_bool(map, S("is_merge"), detail->parent_count > 1, perm);
  struct TemplateValue* parents = template_value_list(detail->parent_count, perm);
  for (ptrdiff_t i = 0; i < detail->parent_count; i++) {
    parents->items[i] = page_refs_commit(repo, (struct CommitSummary){.oid = detail->parents[i]},
                                         path_to_root, perm);
  }
  template_value_set(map, S("parents"), parents, perm);
  return map;
}

static struct TemplateValue* page_commit_value_file(const struct ChangedFile* file,
                                                    const struct Str* fragments,
                                                    const struct ChangedImage* image,
                                                    ptrdiff_t index,
                                                    struct Arena* perm,
                                                    struct Arena scratch) {
  struct TemplateValue* map = template_value_map(perm);
  template_value_set_text(map, S("index"), number_text(index, perm), perm);
  struct Buf anchor = {.arena = perm};
  buf_printf(&anchor, "diff-%td", index);
  template_value_set_text(map, S("anchor"), buf_str(&anchor), perm);
  const struct Str path = file->new_path.len > 0 ? file->new_path : file->old_path;
  template_value_set_text(map, S("path"), path, perm);
  template_value_set_text(map, S("old_path"),
                          file->status == CHANGE_RENAMED ? file->old_path : (struct Str){0}, perm);
  template_value_set_text(
      map, S("similarity"),
      file->status == CHANGE_RENAMED ? number_text(file->similarity, perm) : (struct Str){0}, perm);
  template_value_set_text(map, S("status"), PAGE_COMMIT_STATUS_NAMES[file->status], perm);
  for (ptrdiff_t i = 0; i < PAGE_COMMIT_STATUS_COUNT; i++) {
    template_value_set_bool(map, PAGE_COMMIT_STATUS_FIELDS[i], (ptrdiff_t)file->status == i, perm);
  }
  template_value_set_bool(map, S("is_too_large"), file->is_too_large, perm);
  template_value_set_bool(map, S("is_binary"), file->is_binary, perm);
  template_value_set_bool(map, S("is_shown"), file->is_shown, perm);
  template_value_set_bool(map, S("is_truncated"), file->is_truncated, perm);
  template_value_set_text(map, S("additions"), number_text(file->additions, perm), perm);
  template_value_set_text(map, S("deletions"), number_text(file->deletions, perm), perm);
  // The table is the last thing in the perm while it grows, so it grows in place.
  template_value_set_text(map, S("diff_html"),
                          page_commit_diff_html(file, fragments, buf_str(&anchor), perm, scratch),
                          perm);
  // A file whose type changed may be a symlink on one side, whose blob is a path, not an image.
  const struct Str media_type = file->status != CHANGE_TYPE_CHANGED && file->is_shown
                                    ? page_blob_image_type(path)
                                    : (struct Str){0};
  const struct Str old_data = image != NULL ? image->old_data : (struct Str){0};
  const struct Str new_data = image != NULL ? image->new_data : (struct Str){0};
  template_value_set_bool(map, S("is_image"), media_type.len > 0, perm);
  template_value_set_text(map, S("old_image_url"),
                          page_commit_value_file_image_url(media_type, old_data, perm), perm);
  template_value_set_text(map, S("new_image_url"),
                          page_commit_value_file_image_url(media_type, new_data, perm), perm);
  template_value_set_bool(map, S("is_image_omitted"),
                          media_type.len > 0 && ((file->old_len > 0 && old_data.len == 0) ||
                                                 (file->new_len > 0 && new_data.len == 0)),
                          perm);
  return map;
}

static struct Str page_commit_value_file_image_url(struct Str media_type,
                                                   struct Str data,
                                                   struct Arena* arena) {
  static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if (media_type.len == 0 || data.len == 0) {
    return (struct Str){0};
  }
  struct Buf url = {.arena = arena};
  buf_append(&url, S("data:"));
  buf_append(&url, media_type);
  buf_append(&url, S(";base64,"));
  const unsigned char* bytes = (const unsigned char*)data.data;
  // Each 3 bytes are 4 digits, and the last group pads to 4 with `=`.
  for (ptrdiff_t i = 0; i < data.len; i += 3) {
    const ptrdiff_t left = data.len - i;
    const unsigned long group = ((unsigned long)bytes[i] << 16) |
                                (left > 1 ? (unsigned long)bytes[i + 1] << 8 : 0) |
                                (left > 2 ? (unsigned long)bytes[i + 2] : 0);
    buf_append_char(&url, digits[(group >> 18) & 63]);
    buf_append_char(&url, digits[(group >> 12) & 63]);
    buf_append_char(&url, left > 1 ? digits[(group >> 6) & 63] : '=');
    buf_append_char(&url, left > 2 ? digits[group & 63] : '=');
  }
  return buf_str(&url);
}

static struct TemplateValue* page_commit_value_tree(const struct Changeset* changeset,
                                                    const struct TemplateValue* const* maps,
                                                    ptrdiff_t begin,
                                                    ptrdiff_t end,
                                                    ptrdiff_t depth,
                                                    struct Arena* perm) {
  // The files of one directory are contiguous in the tree's order, so each node is a run.
  ptrdiff_t node_count = 0;
  for (ptrdiff_t i = begin; i < end; i = node_end(changeset, i, end, depth)) {
    node_count++;
  }
  struct TemplateValue* list = template_value_list(node_count, perm);
  ptrdiff_t index = 0;
  for (ptrdiff_t i = begin; i < end;) {
    const ptrdiff_t next = node_end(changeset, i, end, depth);
    bool is_dir = false;
    const struct Str name = path_segment(&changeset->files[i], depth, &is_dir);
    struct TemplateValue* node = template_value_map(perm);
    template_value_set_text(node, S("name"), name, perm);
    template_value_set_bool(node, S("is_dir"), is_dir, perm);
    template_value_set(node, S("children"),
                       is_dir ? page_commit_value_tree(changeset, maps, i, next, depth + 1, perm)
                              : template_value_list(0, perm),
                       perm);
    template_value_set(node, S("file"), is_dir ? template_value_bool(false, perm) : maps[i], perm);
    list->items[index++] = node;
    i = next;
  }
  return list;
}

static ptrdiff_t node_end(const struct Changeset* changeset,
                          ptrdiff_t begin,
                          ptrdiff_t end,
                          ptrdiff_t depth) {
  bool is_dir = false;
  const struct Str name = path_segment(&changeset->files[begin], depth, &is_dir);
  ptrdiff_t next = begin + 1;
  while (is_dir && next < end) {
    bool is_next_dir = false;
    const struct Str next_name = path_segment(&changeset->files[next], depth, &is_next_dir);
    if (!is_next_dir || !str_eq(next_name, name)) {
      break;
    }
    next++;
  }
  return next;
}

static struct Str path_segment(const struct ChangedFile* file, ptrdiff_t depth, bool* is_dir_out) {
  struct StrCut cut = {.tail = file->new_path.len > 0 ? file->new_path : file->old_path};
  for (ptrdiff_t i = 0; i <= depth; i++) {
    cut = str_cut(cut.tail, '/');
  }
  *is_dir_out = cut.is_found;
  return cut.head;
}

static void append_diff_line(struct Buf* buffer,
                             const struct DiffLine* line,
                             const struct Str* fragment,
                             struct Str anchor,
                             ptrdiff_t hunk_index,
                             struct Arena scratch) {
  struct Buf old_prefix = {.arena = &scratch};
  if (line->old_line > 0) {
    buf_printf(&old_prefix, "%.*s-hunk-%td-L", STR_FMT(anchor), hunk_index);
  }
  struct Buf new_prefix = {.arena = &scratch};
  if (line->new_line > 0) {
    buf_printf(&new_prefix, "%.*s-hunk-%td-R", STR_FMT(anchor), hunk_index);
  }
  const struct CodeLineNumber numbers[] = {
      {.value = line->old_line, .anchor_prefix = buf_str(&old_prefix)},
      {.value = line->new_line, .anchor_prefix = buf_str(&new_prefix)},
  };
  const struct Str class_name = line->origin == '+'   ? S("surge-add")
                                : line->origin == '-' ? S("surge-del")
                                : line->origin == ' ' ? S("surge-context")
                                                      : S("surge-no-newline");
  body_html_code_line_open(
      buffer, &(struct CodeLine){.class_name = class_name, .numbers = numbers, .number_count = 2});
  buf_append_char(buffer, line->origin == '\\' ? ' ' : line->origin);
  if (fragment != NULL) {
    buf_append(buffer, *fragment);
  } else {
    html_escape_append(buffer, line->text);
  }
  body_html_code_line_close(buffer, true);
}

static struct Str number_text(ptrdiff_t number, struct Arena* arena) {
  struct Buf text = {.arena = arena};
  buf_printf(&text, "%td", number);
  return buf_str(&text);
}
