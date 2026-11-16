#include "domain/tree_path.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/str.h"
#include "domain/repo_model.h"

/**
 * @brief Finds the entry at `path` among entries sorted bytewise by path.
 *
 * @param entries Entries, sorted by path. Must not be `NULL` unless `count` is `0`.
 * @param count   Number of entries in `entries`.
 * @param path    Path from the root.
 * @return The entry, or `NULL` when none has the path.
 */
static const struct TreeEntry* find_entry(const struct TreeEntry* const* entries,
                                          ptrdiff_t count,
                                          struct Str path);

/**
 * @brief Orders a path and an entry bytewise by path, for `bsearch`.
 *
 * @param key   The `struct Str` path. Must not be `NULL`.
 * @param entry The `const struct TreeEntry*`. Must not be `NULL`.
 * @return Negative, zero or positive as the path sorts before, with, or after the entry's.
 */
static int compare_entry_path(const void* key, const void* entry) __attribute__((nonnull(1, 2)));

/**
 * @brief Orders two paths bytewise, for `bsearch`.
 *
 * @param a First `struct Str`. Must not be `NULL`.
 * @param b Second `struct Str`. Must not be `NULL`.
 * @return Negative, zero or positive as `a` sorts before, with, or after `b`.
 */
static int compare_paths(const void* a, const void* b) __attribute__((nonnull(1, 2)));

bool tree_path_resolve(struct Str dir_path,
                       struct Str path,
                       struct Arena* arena,
                       struct Str* path_out) {
  const bool is_rooted = str_startswith(path, S("/"));
  struct Str joined = is_rooted ? path : str_concat(dir_path, S("/"), arena);
  if (!is_rooted) {
    joined = str_concat(joined, path, arena);
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

struct Str tree_path_dir(struct Str path) {
  ptrdiff_t len = path.len;
  while (len > 0 && path.data[len - 1] != '/') {
    len--;
  }
  return str_head(path, len > 0 ? len - 1 : 0);
}

const struct TreeEntry* tree_path_find_file(const struct BranchTree* tree, struct Str path) {
  return find_entry(tree->files, tree->file_count, path);
}

const struct TreeEntry* tree_path_find_symlink(const struct BranchTree* tree, struct Str path) {
  return find_entry(tree->symlinks, tree->symlink_count, path);
}

bool tree_path_has_dir(const struct BranchTree* tree, struct Str path) {
  return path.len == 0 ||
         (tree->dir_path_count > 0 && bsearch(&path, tree->dir_paths, (size_t)tree->dir_path_count,
                                              sizeof(*tree->dir_paths), compare_paths) != NULL);
}

static const struct TreeEntry* find_entry(const struct TreeEntry* const* entries,
                                          ptrdiff_t count,
                                          struct Str path) {
  if (count == 0) {
    return NULL;
  }
  const struct TreeEntry* const* found =
      bsearch(&path, entries, (size_t)count, sizeof(*entries), compare_entry_path);
  return found != NULL ? *found : NULL;
}

static int compare_entry_path(const void* key, const void* entry) {
  return str_compare(*(const struct Str*)key, (*(const struct TreeEntry* const*)entry)->path);
}

static int compare_paths(const void* a, const void* b) {
  return str_compare(*(const struct Str*)a, *(const struct Str*)b);
}
