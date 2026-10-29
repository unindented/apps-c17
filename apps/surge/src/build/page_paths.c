#include "build/page_paths.h"

#include <stdbool.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/str.h"
#include "domain/url_segment.h"

/** Directory below the repo's of each kind of page, indexed by `enum PagePathKind`. */
static const struct Str PAGE_PATH_DIRS[] = {
    [PAGE_PATH_TREE] = STR_INIT("tree"),
    [PAGE_PATH_BLOB] = STR_INIT("blob"),
    [PAGE_PATH_RAW] = STR_INIT("raw"),
    [PAGE_PATH_COMMITS] = STR_INIT("commits"),
};

/**
 * @brief Reports whether a kind of page is a directory's `index.html` rather than a file of its
 *        own.
 *
 * @param kind Kind of page.
 * @return `false` for a raw copy, `true` otherwise.
 */
static bool is_index_page(enum PagePathKind kind);

struct Str page_path(enum PagePathKind kind,
                     struct Str repo_name,
                     struct Str ref_name,
                     struct Str path,
                     struct Arena* arena) {
  struct Buf text = {.arena = arena};
  buf_append(&text, page_path_dir(kind, repo_name, ref_name, arena));
  if (path.len > 0) {
    buf_append_char(&text, '/');
    buf_append(&text, path);
  }
  if (is_index_page(kind)) {
    buf_append(&text, S("/index.html"));
  }
  return buf_str(&text);
}

struct Str page_path_dir(enum PagePathKind kind,
                         struct Str repo_name,
                         struct Str ref_name,
                         struct Arena* arena) {
  struct Buf text = {.arena = arena};
  buf_printf(&text, "%.*s/%.*s/%.*s", STR_FMT(repo_name), STR_FMT(PAGE_PATH_DIRS[kind]),
             STR_FMT(ref_name));
  return buf_str(&text);
}

void page_path_append_dir_link(struct Buf* buffer,
                               struct Str path_to_root,
                               enum PagePathKind kind,
                               struct Str repo_name,
                               struct Str ref_name) {
  // A repo name uses only unreserved bytes, so only the ref's name needs encoding.
  buf_printf(buffer, "%.*s%.*s/%.*s/", STR_FMT(path_to_root), STR_FMT(repo_name),
             STR_FMT(PAGE_PATH_DIRS[kind]));
  url_segment_append_path(buffer, ref_name);
}

void page_path_append_link(struct Buf* buffer,
                           struct Str path_to_root,
                           enum PagePathKind kind,
                           struct Str repo_name,
                           struct Str ref_name,
                           struct Str path) {
  page_path_append_dir_link(buffer, path_to_root, kind, repo_name, ref_name);
  if (path.len > 0) {
    buf_append_char(buffer, '/');
    url_segment_append_path(buffer, path);
  }
  if (is_index_page(kind)) {
    buf_append(buffer, S("/index.html"));
  }
}

struct Str page_path_link(struct Str path_to_root,
                          enum PagePathKind kind,
                          struct Str repo_name,
                          struct Str ref_name,
                          struct Str path,
                          struct Arena* arena) {
  struct Buf link = {.arena = arena};
  page_path_append_link(&link, path_to_root, kind, repo_name, ref_name, path);
  return buf_str(&link);
}

static bool is_index_page(enum PagePathKind kind) {
  return kind != PAGE_PATH_RAW;
}
