#include "vcs/filetree.h"

#include <git2.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "core/arena.h"
#include "core/ascii.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/path.h"
#include "core/str.h"
#include "core/text.h"
#include "core/utf8.h"
#include "domain/repo_model.h"
#include "vcs/repository.h"

/**
 * Longest symlink target read for its row, in bytes. A target is a path, so `PATH_MAX` bounds it,
 * and a longer one is cut with a warning.
 */
enum { FILETREE_LINK_TARGET_LEN_MAX = 4096 };

/** What the walk of one branch shares between its directories. */
struct FiletreeWalk {
  /** Repository the tree is in. */
  const struct Repository* repository;

  /** Object database of the repository, which the entries' sizes are read from. */
  git_odb* odb;

  /** Branch whose tree is walked. */
  const struct Ref* branch;

  /** Directories found so far, the next to read at `dir_next`. */
  struct TreeDir* dirs;

  /** Number of entries in `dirs`. */
  ptrdiff_t dir_count;

  /** Slots allocated at `dirs`. */
  ptrdiff_t dir_capacity;

  /** Arena that owns the tree. */
  struct Arena* perm;

  /** Log that receives the warnings and the failure. */
  struct Log* log;
};

/**
 * @brief Fills `tree` from a finished walk: its directories, and the sorted paths of every linked
 *        file and every directory but the root, which links resolve to.
 *
 * @param tree Tree to fill. Must not be `NULL`.
 * @param walk The finished walk. Must not be `NULL`.
 */
static void filetree_load_index(struct BranchTree* tree, const struct FiletreeWalk* walk)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Reads one directory's entries, checks their names, sorts them, and queues the linked
 *        subdirectories.
 *
 * @param walk  Walk in progress. Must not be `NULL`.
 * @param index Index in `walk->dirs` of the directory.
 * @return `0` on success, or `-1` when an object cannot be read.
 */
static int filetree_load_dir(struct FiletreeWalk* walk, ptrdiff_t index)
    __attribute__((nonnull(1)));

/**
 * @brief Fills one entry from libgit2's tree entry: its kind, id, size, link target, and whether
 *        its name and its pages' paths can be used.
 *
 * @param walk     Walk in progress. Must not be `NULL`.
 * @param dir_path Path of the directory the entry is in.
 * @param source   libgit2's entry. Must not be `NULL`.
 * @param entry    Entry to fill. Must not be `NULL`.
 * @return `0` on success, or `-1` when the entry's object cannot be read.
 */
static int filetree_load_entry(struct FiletreeWalk* walk,
                               struct Str dir_path,
                               const git_tree_entry* source,
                               struct TreeEntry* entry) __attribute__((nonnull(1, 3, 4)));

/**
 * @brief Unlinks an entry whose name a page cannot use, or whose pages pass an output-path limit,
 *        with a warning.
 *
 * @param walk  Walk in progress. Must not be `NULL`.
 * @param entry Entry whose name, path and kind are set. Must not be `NULL`.
 */
static void filetree_load_entry_check_name(const struct FiletreeWalk* walk, struct TreeEntry* entry)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Returns the target of a symlink as its row shows it: the first
 *        `FILETREE_LINK_TARGET_LEN_MAX` bytes at most, cut before a UTF-8 continuation byte, with a
 *        warning when it is cut.
 *
 * @param walk   Walk in progress. Must not be `NULL`.
 * @param entry  The symlink, whose path and size are set. Must not be `NULL`.
 * @param target Every byte of the target, `entry->len` of them.
 * @return A view of `target`.
 */
static struct Str filetree_load_entry_cut_target(const struct FiletreeWalk* walk,
                                                 const struct TreeEntry* entry,
                                                 struct Str target) __attribute__((nonnull(1, 2)));

/**
 * @brief Reports whether a page can use `name` as a path segment.
 *
 * @param name Name of a tree entry.
 * @return `false` for `.`, `..`, `.git` in any case, a name with `/`, `\` or a control byte, and a
 *         name that is not UTF-8.
 */
static bool is_usable_name(struct Str name);

/**
 * @brief Appends `path` to `buffer` with each control byte written `\xNN`, so a warning prints it
 *        on one line, and each byte past ASCII too when the path is not UTF-8.
 *
 * @param buffer Buffer that receives the text. Must not be `NULL`.
 * @param path   Path to append.
 */
static void append_printable(struct Buf* buffer, struct Str path) __attribute__((nonnull(1)));

/**
 * @brief Unlinks every linked entry whose name collides with another's after ASCII case folding,
 *        with one warning per group.
 *
 * @param walk        Walk in progress. Must not be `NULL`.
 * @param entries     Entries of one directory. Must not be `NULL` unless `entry_count` is `0`.
 * @param entry_count Number of entries.
 */
static void filetree_load_unlink_collisions(struct FiletreeWalk* walk,
                                            struct TreeEntry* entries,
                                            ptrdiff_t entry_count) __attribute__((nonnull(1)));

/**
 * @brief Orders two entries bytewise by ASCII-folded name, for finding collisions.
 *
 * @param a First `struct TreeEntry*`. Must not be `NULL`.
 * @param b Second `struct TreeEntry*`. Must not be `NULL`.
 * @return Negative, zero or positive as `a` sorts before, with, or after `b`.
 */
static int compare_folded_names(const void* a, const void* b) __attribute__((nonnull(1, 2)));

/**
 * @brief Orders two entries by ASCII-folded name, then bytewise, for `qsort`, so the names of a
 *        collision are listed in one order whatever the C library's sort does with equal elements.
 *
 * @param a First `struct TreeEntry*`. Must not be `NULL`.
 * @param b Second `struct TreeEntry*`. Must not be `NULL`.
 * @return Negative, zero or positive as `a` sorts before, with, or after `b`.
 */
static int compare_folded_names_then_bytes(const void* a, const void* b)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Orders two entries: directories first, then the rest, each group bytewise by name.
 *
 * @param a First `struct TreeEntry`. Must not be `NULL`.
 * @param b Second `struct TreeEntry`. Must not be `NULL`.
 * @return Negative, zero or positive as `a` sorts before, with, or after `b`.
 */
static int compare_entries(const void* a, const void* b) __attribute__((nonnull(1, 2)));

/**
 * @brief Orders two entries bytewise by path, for `qsort`.
 *
 * @param a First `const struct TreeEntry*`. Must not be `NULL`.
 * @param b Second `const struct TreeEntry*`. Must not be `NULL`.
 * @return Negative, zero or positive as `a` sorts before, with, or after `b`.
 */
static int compare_file_paths(const void* a, const void* b) __attribute__((nonnull(1, 2)));

/**
 * @brief Orders two paths bytewise, for `qsort`.
 *
 * @param a First `struct Str`. Must not be `NULL`.
 * @param b Second `struct Str`. Must not be `NULL`.
 * @return Negative, zero or positive as `a` sorts before, with, or after `b`.
 */
static int compare_paths(const void* a, const void* b) __attribute__((nonnull(1, 2)));

int filetree_load(struct BranchTree* tree,
                  const struct Repository* repository,
                  const struct Ref* branch,
                  struct Arena* perm,
                  struct Log* log) {
  git_oid commit_oid;
  git_commit* commit = NULL;
  if (git_oid_fromstrn(&commit_oid, branch->commit_oid.data, (size_t)branch->commit_oid.len) != 0 ||
      git_commit_lookup(&commit, repository->git, &commit_oid) != 0) {
    return repository_fail(repository, S("failed to read a branch's commit"), log);
  }
  const struct Str root_oid = repository_oid_text(git_commit_tree_id(commit), perm);
  git_commit_free(commit);
  git_odb* odb = NULL;
  if (git_repository_odb(&odb, repository->git) != 0) {
    return repository_fail(repository, S("failed to open the object database"), log);
  }
  struct FiletreeWalk walk = {
      .repository = repository, .odb = odb, .branch = branch, .perm = perm, .log = log};
  walk.dirs = arena_grow(perm, walk.dirs, &walk.dir_capacity, (ptrdiff_t)sizeof(struct TreeDir),
                         (ptrdiff_t)_Alignof(struct TreeDir));
  walk.dirs[walk.dir_count++] = (struct TreeDir){.oid = root_oid};
  int rc = 0;
  for (ptrdiff_t i = 0; rc == 0 && i < walk.dir_count; i++) {
    rc = filetree_load_dir(&walk, i);
  }
  git_odb_free(odb);
  if (rc != 0) {
    return -1;
  }
  filetree_load_index(tree, &walk);
  return 0;
}

bool filetree_entry_has_pages(const struct TreeEntry* entry) {
  return entry->is_linked &&
         (entry->kind == TREE_ENTRY_FILE || entry->kind == TREE_ENTRY_EXECUTABLE);
}

static void filetree_load_index(struct BranchTree* tree, const struct FiletreeWalk* walk) {
  // The paths a link resolves to: every linked file, and every linked directory but the root.
  ptrdiff_t file_count = 0;
  for (ptrdiff_t i = 0; i < walk->dir_count; i++) {
    for (ptrdiff_t j = 0; j < walk->dirs[i].entry_count; j++) {
      const struct TreeEntry* entry = &walk->dirs[i].entries[j];
      file_count += filetree_entry_has_pages(entry);
    }
  }
  *tree = (struct BranchTree){
      .branch = walk->branch,
      .dirs = walk->dirs,
      .dir_count = walk->dir_count,
      .files = arena_new(walk->perm, file_count, const struct TreeEntry*),
      .dir_paths = arena_new(walk->perm, walk->dir_count, struct Str),
  };
  for (ptrdiff_t i = 0; i < walk->dir_count; i++) {
    if (i > 0) {
      tree->dir_paths[tree->dir_path_count++] = walk->dirs[i].path;
    }
    for (ptrdiff_t j = 0; j < walk->dirs[i].entry_count; j++) {
      const struct TreeEntry* entry = &walk->dirs[i].entries[j];
      if (filetree_entry_has_pages(entry)) {
        tree->files[tree->file_count++] = entry;
      }
    }
  }
  if (tree->file_count > 0) {
    qsort(tree->files, (size_t)tree->file_count, sizeof(*tree->files), compare_file_paths);
  }
  if (tree->dir_path_count > 0) {
    qsort(tree->dir_paths, (size_t)tree->dir_path_count, sizeof(*tree->dir_paths), compare_paths);
  }
}

static int filetree_load_dir(struct FiletreeWalk* walk, ptrdiff_t index) {
  git_oid oid;
  git_tree* git_tree_object = NULL;
  const struct Str dir_oid = walk->dirs[index].oid;
  if (git_oid_fromstrn(&oid, dir_oid.data, (size_t)dir_oid.len) != 0 ||
      git_tree_lookup(&git_tree_object, walk->repository->git, &oid) != 0) {
    return repository_fail(walk->repository, S("failed to read a tree"), walk->log);
  }
  const ptrdiff_t count = (ptrdiff_t)git_tree_entrycount(git_tree_object);
  struct TreeEntry* entries = arena_new(walk->perm, count, struct TreeEntry);
  int rc = 0;
  for (ptrdiff_t i = 0; rc == 0 && i < count; i++) {
    rc = filetree_load_entry(walk, walk->dirs[index].path,
                             git_tree_entry_byindex(git_tree_object, (size_t)i), &entries[i]);
  }
  git_tree_free(git_tree_object);
  if (rc != 0) {
    return -1;
  }
  filetree_load_unlink_collisions(walk, entries, count);
  if (count > 0) {
    qsort(entries, (size_t)count, sizeof(*entries), compare_entries);
  }
  walk->dirs[index].entries = entries;
  walk->dirs[index].entry_count = count;
  for (ptrdiff_t i = 0; i < count; i++) {
    if (entries[i].kind != TREE_ENTRY_DIR || !entries[i].is_linked) {
      continue;
    }
    if (walk->dir_count == walk->dir_capacity) {
      walk->dirs =
          arena_grow(walk->perm, walk->dirs, &walk->dir_capacity, (ptrdiff_t)sizeof(struct TreeDir),
                     (ptrdiff_t)_Alignof(struct TreeDir));
    }
    walk->dirs[walk->dir_count++] =
        (struct TreeDir){.path = entries[i].path, .oid = entries[i].oid};
  }
  return 0;
}

static int filetree_load_entry(struct FiletreeWalk* walk,
                               struct Str dir_path,
                               const git_tree_entry* source,
                               struct TreeEntry* entry) {
  const struct Str name = str_clone(str_from_cstr(git_tree_entry_name(source)), walk->perm);
  struct Buf path = {.arena = walk->perm};
  if (dir_path.len > 0) {
    buf_append(&path, dir_path);
    buf_append_char(&path, '/');
  }
  buf_append(&path, name);
  *entry = (struct TreeEntry){
      .name = name,
      .path = buf_str(&path),
      .oid = repository_oid_text(git_tree_entry_id(source), walk->perm),
      .is_linked = true,
  };
  switch (git_tree_entry_filemode(source)) {
    case GIT_FILEMODE_TREE:
      entry->kind = TREE_ENTRY_DIR;
      break;
    case GIT_FILEMODE_BLOB_EXECUTABLE:
      entry->kind = TREE_ENTRY_EXECUTABLE;
      break;
    case GIT_FILEMODE_LINK:
      entry->kind = TREE_ENTRY_SYMLINK;
      break;
    case GIT_FILEMODE_COMMIT:
      entry->kind = TREE_ENTRY_GITLINK;
      break;
    case GIT_FILEMODE_BLOB:
    case GIT_FILEMODE_UNREADABLE:
      entry->kind = TREE_ENTRY_FILE;
      break;
  }
  filetree_load_entry_check_name(walk, entry);
  // A gitlink names a commit of another repository, so its header is never read.
  if (entry->kind == TREE_ENTRY_DIR || entry->kind == TREE_ENTRY_GITLINK) {
    return 0;
  }
  size_t len = 0;
  git_object_t type = GIT_OBJECT_INVALID;
  if (git_odb_read_header(&len, &type, walk->odb, git_tree_entry_id(source)) != 0) {
    return repository_fail(walk->repository, S("failed to read a blob's header"), walk->log);
  }
  entry->len = (ptrdiff_t)len;
  if (entry->kind == TREE_ENTRY_SYMLINK) {
    git_blob* blob = NULL;
    if (git_blob_lookup(&blob, walk->repository->git, git_tree_entry_id(source)) != 0) {
      return repository_fail(walk->repository, S("failed to read a symlink"), walk->log);
    }
    const struct Str target = filetree_load_entry_cut_target(
        walk, entry, (struct Str){git_blob_rawcontent(blob), entry->len});
    // The blob's bytes go with it, so the target is copied.
    entry->link_target = str_clone(target, walk->perm);
    git_blob_free(blob);
  }
  return 0;
}

static void filetree_load_entry_check_name(const struct FiletreeWalk* walk,
                                           struct TreeEntry* entry) {
  const struct Str branch_name = walk->branch->short_name;
  // A directory's tree page is `<p>/index.html`, so one named `index.html` would put its page below
  // its parent's page file.
  const bool is_index_dir = entry->kind == TREE_ENTRY_DIR &&
                            text_has_suffix_folded(entry->name, S("index.html")) &&
                            entry->name.len == S("index.html").len;
  if (!is_usable_name(entry->name) || is_index_dir) {
    entry->is_linked = false;
    struct Arena scratch = *walk->perm;
    struct Buf printable = {.arena = &scratch};
    append_printable(&printable, entry->path);
    const struct Str shown = buf_str(&printable);
    log_warn(walk->log,
             "tree entry name cannot be a path, so it has no pages: '%.*s' (in repo '%.*s', "
             "branch '%.*s')",
             STR_FMT(shown), STR_FMT(walk->repository->name), STR_FMT(branch_name));
  } else if (entry->kind != TREE_ENTRY_SYMLINK && entry->kind != TREE_ENTRY_GITLINK) {
    // The longest page of an entry is its blob or tree page, `<p>/index.html` below the ref.
    struct Arena scratch = *walk->perm;
    struct Buf page = {.arena = &scratch};
    buf_printf(&page, "%.*s/%s/%.*s/%.*s/index.html", STR_FMT(walk->repository->name),
               entry->kind == TREE_ENTRY_DIR ? "tree" : "blob", STR_FMT(branch_name),
               STR_FMT(entry->path));
    struct Str segment = {0};
    const enum PathOutputVerdict verdict = path_check_output_limits(buf_str(&page), &segment);
    if (verdict != PATH_OUTPUT_OK) {
      // Each message names the tree path, and ends with the value it measured.
      entry->is_linked = false;
      const struct Str page_path = buf_str(&page);
      if (verdict == PATH_OUTPUT_TOO_LONG) {
        log_warn(walk->log,
                 "output path for tree path '%.*s' exceeds max output path length (%d bytes) at "
                 "%td bytes, so it has no pages: '%.*s' (in repo '%.*s', branch '%.*s')",
                 STR_FMT(entry->path), OUTPUT_PATH_RELATIVE_LEN_MAX, page_path.len,
                 STR_FMT(page_path), STR_FMT(walk->repository->name), STR_FMT(branch_name));
      } else {
        log_warn(walk->log,
                 "output path for tree path '%.*s' exceeds max filename length (%d bytes) at %td "
                 "bytes, so it has no pages: '%.*s' (in repo '%.*s', branch '%.*s')",
                 STR_FMT(entry->path), FILENAME_LEN_MAX, segment.len, STR_FMT(segment),
                 STR_FMT(walk->repository->name), STR_FMT(branch_name));
      }
    }
  }
}

static struct Str filetree_load_entry_cut_target(const struct FiletreeWalk* walk,
                                                 const struct TreeEntry* entry,
                                                 struct Str target) {
  if (target.len <= FILETREE_LINK_TARGET_LEN_MAX) {
    return target;
  }
  // A UTF-8 sequence is at most four bytes, so the cut moves back over at most three continuation
  // bytes to the start of the sequence it would split.
  ptrdiff_t len = FILETREE_LINK_TARGET_LEN_MAX;
  for (int i = 0; i < 3 && ((unsigned char)target.data[len] & 0xC0) == 0x80; i++) {
    len--;
  }
  struct Arena scratch = *walk->perm;
  struct Buf printable = {.arena = &scratch};
  append_printable(&printable, entry->path);
  const struct Str shown = buf_str(&printable);
  log_warn(walk->log,
           "symlink target exceeds max symlink target length (%d bytes) at %td bytes, so its row "
           "shows it cut short: '%.*s' (in repo '%.*s', branch '%.*s')",
           FILETREE_LINK_TARGET_LEN_MAX, target.len, STR_FMT(shown),
           STR_FMT(walk->repository->name), STR_FMT(walk->branch->short_name));
  return str_head(target, len);
}

static bool is_usable_name(struct Str name) {
  if (name.len == 0 || str_eq(name, S(".")) || str_eq(name, S(".."))) {
    return false;
  }
  for (ptrdiff_t i = 0; i < name.len; i++) {
    const unsigned char c = (unsigned char)name.data[i];
    if (c < 0x20 || c == 0x7F || c == '/' || c == '\\') {
      return false;
    }
  }
  // A name that is not UTF-8 is a filename some filesystems refuse, so its pages would not be
  // written alike on every host.
  return utf8_is_valid(name) && !str_eq_folded(name, S(".git"));
}

static void append_printable(struct Buf* buffer, struct Str path) {
  // In a path that is not UTF-8, a byte past ASCII prints as itself in no terminal.
  const bool is_valid = utf8_is_valid(path);
  for (ptrdiff_t i = 0; i < path.len; i++) {
    const unsigned char c = (unsigned char)path.data[i];
    if (c < 0x20 || c == 0x7F || (!is_valid && c >= 0x80)) {
      buf_printf(buffer, "\\x%02X", c);
    } else {
      buf_append_char(buffer, (char)c);
    }
  }
}

static void filetree_load_unlink_collisions(struct FiletreeWalk* walk,
                                            struct TreeEntry* entries,
                                            ptrdiff_t entry_count) {
  // An entry that already lost its pages has had its warning, and the checks before treat names
  // that differ only in case alike, so only the linked entries can still collide.
  struct Arena scratch = *walk->perm;
  struct TreeEntry** sorted = arena_new(&scratch, entry_count, struct TreeEntry*);
  ptrdiff_t count = 0;
  for (ptrdiff_t i = 0; i < entry_count; i++) {
    if (entries[i].is_linked) {
      sorted[count++] = &entries[i];
    }
  }
  if (count > 0) {
    qsort(sorted, (size_t)count, sizeof(*sorted), compare_folded_names_then_bytes);
  }
  for (ptrdiff_t start = 0; start < count;) {
    ptrdiff_t end = start + 1;
    while (end < count && compare_folded_names(&sorted[start], &sorted[end]) == 0) {
      end++;
    }
    if (end - start > 1) {
      // No winner is picked: every name of the group loses its pages.
      struct Buf paths = {.arena = &scratch};
      for (ptrdiff_t i = start; i < end; i++) {
        sorted[i]->is_linked = false;
        buf_append(&paths, i > start ? S("', '") : S(""));
        append_printable(&paths, sorted[i]->path);
      }
      const struct Str listed = buf_str(&paths);
      log_warn(walk->log,
               "tree paths collide after case folding, so none of them has pages: '%.*s' (in "
               "repo '%.*s', branch '%.*s')",
               STR_FMT(listed), STR_FMT(walk->repository->name), STR_FMT(walk->branch->short_name));
    }
    start = end;
  }
}

static int compare_folded_names(const void* a, const void* b) {
  const struct TreeEntry* left = *(struct TreeEntry* const*)a;
  const struct TreeEntry* right = *(struct TreeEntry* const*)b;
  const ptrdiff_t len = left->name.len < right->name.len ? left->name.len : right->name.len;
  for (ptrdiff_t i = 0; i < len; i++) {
    const unsigned char left_byte = ascii_to_lower((unsigned char)left->name.data[i]);
    const unsigned char right_byte = ascii_to_lower((unsigned char)right->name.data[i]);
    if (left_byte != right_byte) {
      return left_byte < right_byte ? -1 : 1;
    }
  }
  return left->name.len < right->name.len ? -1 : left->name.len > right->name.len ? 1 : 0;
}

static int compare_folded_names_then_bytes(const void* a, const void* b) {
  const int folded = compare_folded_names(a, b);
  if (folded != 0) {
    return folded;
  }
  return str_compare((*(struct TreeEntry* const*)a)->name, (*(struct TreeEntry* const*)b)->name);
}

static int compare_entries(const void* a, const void* b) {
  const struct TreeEntry* left = a;
  const struct TreeEntry* right = b;
  const bool is_left_dir = left->kind == TREE_ENTRY_DIR;
  const bool is_right_dir = right->kind == TREE_ENTRY_DIR;
  if (is_left_dir != is_right_dir) {
    return is_left_dir ? -1 : 1;
  }
  return str_compare(left->name, right->name);
}

static int compare_file_paths(const void* a, const void* b) {
  return str_compare((*(const struct TreeEntry* const*)a)->path,
                     (*(const struct TreeEntry* const*)b)->path);
}

static int compare_paths(const void* a, const void* b) {
  return str_compare(*(const struct Str*)a, *(const struct Str*)b);
}
