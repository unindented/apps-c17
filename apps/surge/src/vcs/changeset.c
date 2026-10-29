#include "vcs/changeset.h"

#include <git2.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "domain/site_config.h"
#include "vcs/repository.h"

/**
 * Most sources libgit2 tries for each target in rename detection. libgit2 compares every target
 * with up to this many sources, even for an exact match, so the bound keeps a commit of a hundred
 * thousand changed files from costing their square, while a rename of up to this many files is
 * found whole, as git finds it.
 */
enum { CHANGESET_RENAME_LIMIT_MAX = 10000 };

/**
 * Most pairs of a deleted file and an added one whose contents the rename search compares, git's
 * `diff.renameLimit` of 1000 squared. A commit with more pairs finds only exact renames, as git
 * does.
 */
enum { CHANGESET_SIMILAR_PAIRS_MAX = 1000 * 1000 };

/** Similarity, in percent, from which a deleted file and an added one are a rename, git's
    default. */
enum { CHANGESET_RENAME_THRESHOLD = 50 };

/** Text of a diff line that says its side has no newline at its end, as git prints it. */
#define CHANGESET_NO_NEWLINE S("No newline at end of file")

/** One changed file before its diff is read: where libgit2 holds it, and the path it sorts by. */
struct ChangesetEntry {
  /** Index of the delta in libgit2's diff. */
  ptrdiff_t delta_index;

  /** Path the file sorts by: its new path, or its old one when deleted. */
  struct Str path;
};

/** What reading the files' diffs in order shares: the caps and how much of them is spent. */
struct ChangesetBudget {
  /** The config's limits. */
  const struct SurgeLimits* limits;

  /** Files shown so far. */
  ptrdiff_t file_count;

  /** Diff lines shown so far. */
  ptrdiff_t line_count;

  /** Diff bytes shown so far. */
  ptrdiff_t len;

  /** Whether a cap was reached, so no later file shows its diff. */
  bool is_spent;
};

/**
 * @brief Sorts the files of a diff into the changed-files tree's order and reads their diffs.
 *
 * @param changeset  Changeset to fill. Must not be `NULL`.
 * @param diff       libgit2's diff of the commit. Must not be `NULL`.
 * @param repository Repository the diff is of, which a failure names. Must not be `NULL`.
 * @param limits     The config's limits. Must not be `NULL`.
 * @param perm       Arena that owns the changes. Must not be `NULL`.
 * @param log        Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when a file's diff cannot be read.
 */
static int changeset_load_diff(struct Changeset* changeset,
                               git_diff* diff,
                               const struct Repository* repository,
                               const struct SurgeLimits* limits,
                               struct Arena* perm,
                               struct Log* log) __attribute__((nonnull(1, 2, 3, 4, 5, 6)));

/**
 * @brief Reads one file's diff into `file`, within what is left of the budget.
 *
 * @param file       File to fill. Must not be `NULL`.
 * @param diff       libgit2's diff. Must not be `NULL`.
 * @param entry      Where libgit2 holds the file. Must not be `NULL`.
 * @param repository Repository the diff is of, which a failure names. Must not be `NULL`.
 * @param budget     The caps and what is spent of them. Must not be `NULL`.
 * @param perm       Arena that owns the lines. Must not be `NULL`.
 * @param log        Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the diff cannot be read.
 */
static int changeset_load_file(struct ChangedFile* file,
                               git_diff* diff,
                               const struct ChangesetEntry* entry,
                               const struct Repository* repository,
                               struct ChangesetBudget* budget,
                               struct Arena* perm,
                               struct Log* log) __attribute__((nonnull(1, 2, 3, 4, 5, 6, 7)));

/**
 * @brief Returns libgit2's text without its trailing newline, copied into `arena`.
 *
 * @param text  Bytes libgit2 owns.
 * @param arena Arena that owns the copy. Must not be `NULL`.
 * @return The copy.
 */
static struct Str copy_line(struct Str text, struct Arena* arena) __attribute__((nonnull(2)));

/**
 * @brief Orders two changed files as the changed-files tree lists them: at the first path segment
 *        where they differ, a directory before a file, and otherwise bytewise.
 *
 * @param a First `struct ChangesetEntry`. Must not be `NULL`.
 * @param b Second `struct ChangesetEntry`. Must not be `NULL`.
 * @return Negative, zero or positive as `a` sorts before, with, or after `b`.
 */
static int compare_tree_order(const void* a, const void* b) __attribute__((nonnull(1, 2)));

int changeset_load(struct Changeset* changeset,
                   const struct Repository* repository,
                   struct Str commit_oid,
                   const struct SurgeLimits* limits,
                   struct Arena* perm,
                   struct Log* log) {
  *changeset = (struct Changeset){0};
  git_oid oid;
  git_commit* commit = NULL;
  if (git_oid_fromstrn(&oid, commit_oid.data, (size_t)commit_oid.len) != 0 ||
      git_commit_lookup(&commit, repository->git, &oid) != 0) {
    return repository_fail(repository, S("failed to read commit"), log);
  }
  git_tree* new_tree = NULL;
  git_commit* parent = NULL;
  git_tree* old_tree = NULL;
  git_diff* diff = NULL;
  git_diff_options options;
  (void)git_diff_options_init(&options, GIT_DIFF_OPTIONS_VERSION);
  options.flags |= GIT_DIFF_INCLUDE_TYPECHANGE;
  // libgit2 takes a `max_size` of 0 for its default of 512 MiB, so the smallest it gets is 1, and a
  // file over `blob_len_max` is told apart from a binary one after its patch loads.
  options.max_size = (git_off_t)(limits->blob_len_max > 0 ? limits->blob_len_max : 1);
  git_diff_find_options find_options;
  (void)git_diff_find_options_init(&find_options, GIT_DIFF_FIND_OPTIONS_VERSION);
  find_options.flags = GIT_DIFF_FIND_RENAMES | GIT_DIFF_FIND_EXACT_MATCH_ONLY;
  int rc = -1;
  if (git_commit_tree(&new_tree, commit) != 0 ||
      (git_commit_parentcount(commit) > 0 &&
       (git_commit_parent(&parent, commit, 0) != 0 || git_commit_tree(&old_tree, parent) != 0))) {
    rc = repository_fail(repository, S("failed to read commit tree"), log);
  } else if (git_diff_tree_to_tree(&diff, repository->git, old_tree, new_tree, &options) != 0) {
    rc = repository_fail(repository, S("failed to diff commit"), log);
  } else {
    // A deleted file and an added one are a rename when their contents are alike, as git finds
    // them, while the pairs to compare stay under `CHANGESET_SIMILAR_PAIRS_MAX`. Past it, as in
    // git, only exact renames count, which hashing finds without comparing contents. libgit2 tries
    // at most `rename_limit` sources for each target, even for an exact match. A limit of the
    // source count, or of the delta count for exact renames, tries every source, up to
    // `CHANGESET_RENAME_LIMIT_MAX`, and a limit of 0 would read the repository's own
    // `diff.renameLimit`.
    size_t source_count = 0;
    size_t target_count = 0;
    const size_t delta_count = git_diff_num_deltas(diff);
    for (size_t i = 0; i < delta_count; i++) {
      const git_delta_t status = git_diff_get_delta(diff, i)->status;
      source_count += status == GIT_DELTA_DELETED;
      target_count += status == GIT_DELTA_ADDED;
    }
    const bool is_similar_searched =
        target_count == 0 || source_count <= CHANGESET_SIMILAR_PAIRS_MAX / target_count;
    const size_t searched_count = is_similar_searched ? source_count : delta_count;
    if (is_similar_searched) {
      find_options.flags = GIT_DIFF_FIND_RENAMES;
      find_options.rename_threshold = CHANGESET_RENAME_THRESHOLD;
    }
    find_options.rename_limit = searched_count < 1 ? 1
                                : searched_count > CHANGESET_RENAME_LIMIT_MAX
                                    ? CHANGESET_RENAME_LIMIT_MAX
                                    : searched_count;
    if (git_diff_find_similar(diff, &find_options) != 0) {
      rc = repository_fail(repository, S("failed to diff commit"), log);
    } else {
      rc = changeset_load_diff(changeset, diff, repository, limits, perm, log);
    }
  }
  git_diff_free(diff);
  git_tree_free(old_tree);
  git_commit_free(parent);
  git_tree_free(new_tree);
  git_commit_free(commit);
  return rc;
}

static int changeset_load_diff(struct Changeset* changeset,
                               git_diff* diff,
                               const struct Repository* repository,
                               const struct SurgeLimits* limits,
                               struct Arena* perm,
                               struct Log* log) {
  // The sort keys stay in the perm with the files, a few bytes for each.
  const ptrdiff_t count = (ptrdiff_t)git_diff_num_deltas(diff);
  struct ChangesetEntry* entries = arena_new(perm, count, struct ChangesetEntry);
  for (ptrdiff_t i = 0; i < count; i++) {
    const git_diff_delta* delta = git_diff_get_delta(diff, (size_t)i);
    const char* path =
        delta->status == GIT_DELTA_DELETED ? delta->old_file.path : delta->new_file.path;
    entries[i] = (struct ChangesetEntry){.delta_index = i, .path = str_from_cstr(path)};
  }
  if (count > 0) {
    qsort(entries, (size_t)count, sizeof(*entries), compare_tree_order);
  }
  struct ChangedFile* files = arena_new(perm, count, struct ChangedFile);
  struct ChangesetBudget budget = {.limits = limits};
  for (ptrdiff_t i = 0; i < count; i++) {
    if (changeset_load_file(&files[i], diff, &entries[i], repository, &budget, perm, log) != 0) {
      return -1;
    }
  }
  *changeset = (struct Changeset){.files = files, .file_count = count};
  for (ptrdiff_t i = 0; i < count; i++) {
    changeset->additions += files[i].additions;
    changeset->deletions += files[i].deletions;
    changeset->is_truncated =
        changeset->is_truncated || files[i].is_truncated || !files[i].is_shown;
  }
  return 0;
}

static int changeset_load_file(struct ChangedFile* file,
                               git_diff* diff,
                               const struct ChangesetEntry* entry,
                               const struct Repository* repository,
                               struct ChangesetBudget* budget,
                               struct Arena* perm,
                               struct Log* log) {
  const git_diff_delta* delta = git_diff_get_delta(diff, (size_t)entry->delta_index);
  *file = (struct ChangedFile){
      .old_path = delta->status != GIT_DELTA_ADDED
                      ? str_clone(str_from_cstr(delta->old_file.path), perm)
                      : (struct Str){0},
      .new_path = delta->status != GIT_DELTA_DELETED
                      ? str_clone(str_from_cstr(delta->new_file.path), perm)
                      : (struct Str){0},
      .similarity = delta->status == GIT_DELTA_RENAMED ? (ptrdiff_t)delta->similarity : 0,
      // A gitlink's id names a commit of another repository, so it is no blob to read.
      .old_oid = delta->status != GIT_DELTA_ADDED && delta->old_file.mode != GIT_FILEMODE_COMMIT
                     ? repository_oid_text(&delta->old_file.id, perm)
                     : (struct Str){0},
      .new_oid = delta->status != GIT_DELTA_DELETED && delta->new_file.mode != GIT_FILEMODE_COMMIT
                     ? repository_oid_text(&delta->new_file.id, perm)
                     : (struct Str){0},
  };
  switch (delta->status) {
    case GIT_DELTA_ADDED:
      file->status = CHANGE_ADDED;
      break;
    case GIT_DELTA_DELETED:
      file->status = CHANGE_DELETED;
      break;
    case GIT_DELTA_RENAMED:
      file->status = CHANGE_RENAMED;
      break;
    case GIT_DELTA_TYPECHANGE:
      file->status = CHANGE_TYPE_CHANGED;
      break;
    default:
      file->status = CHANGE_MODIFIED;
      break;
  }
  const struct SurgeLimits* limits = budget->limits;
  if (budget->is_spent || budget->file_count == limits->diff_file_count_max) {
    budget->is_spent = true;
    return 0;
  }
  budget->file_count++;
  file->is_shown = true;
  git_patch* patch = NULL;
  if (git_patch_from_diff(&patch, diff, (size_t)entry->delta_index) != 0) {
    struct Arena scratch = *perm;
    struct Buf operation = {.arena = &scratch};
    buf_printf(&operation, "failed to diff file '%.*s'", STR_FMT(entry->path));
    return repository_fail(repository, buf_str(&operation), log);
  }
  // A patch is `NULL` only for a delta that the diff's options skip, such as an unmodified one,
  // which a diff of two trees never holds. A mode change or a binary file has a patch without
  // hunks. The patch's delta has the sizes once the blobs are loaded.
  const git_diff_delta* loaded = patch != NULL ? git_patch_get_delta(patch) : delta;
  file->old_len = (ptrdiff_t)loaded->old_file.size;
  file->new_len = (ptrdiff_t)loaded->new_file.size;
  file->is_too_large = loaded->old_file.size > (git_object_size_t)limits->blob_len_max ||
                       loaded->new_file.size > (git_object_size_t)limits->blob_len_max;
  file->is_binary = !file->is_too_large && (loaded->flags & GIT_DIFF_FLAG_BINARY) != 0;
  if (patch == NULL || file->is_too_large || file->is_binary) {
    git_patch_free(patch);
    return 0;
  }
  const ptrdiff_t hunk_count = (ptrdiff_t)git_patch_num_hunks(patch);
  file->hunks = arena_new(perm, hunk_count, struct DiffHunk);
  // No more lines are kept than the line cap leaves.
  ptrdiff_t line_total = 0;
  for (ptrdiff_t h = 0; h < hunk_count; h++) {
    line_total += (ptrdiff_t)git_patch_num_lines_in_hunk(patch, (size_t)h);
  }
  const ptrdiff_t line_room = limits->diff_line_count_max - budget->line_count;
  file->lines = arena_new(perm, line_total < line_room ? line_total : line_room, struct DiffLine);
  for (ptrdiff_t h = 0; h < hunk_count && !budget->is_spent; h++) {
    const git_diff_hunk* hunk = NULL;
    size_t line_count = 0;
    (void)git_patch_get_hunk(&hunk, &line_count, patch, (size_t)h);
    struct DiffHunk* shown = &file->hunks[file->hunk_count++];
    *shown = (struct DiffHunk){
        .header = copy_line((struct Str){hunk->header, (ptrdiff_t)hunk->header_len}, perm),
        .line_index = file->line_count,
    };
    for (ptrdiff_t l = 0; l < (ptrdiff_t)line_count; l++) {
      const git_diff_line* line = NULL;
      (void)git_patch_get_line_in_hunk(&line, patch, (size_t)h, (size_t)l);
      const bool is_marker = line->origin == GIT_DIFF_LINE_CONTEXT_EOFNL ||
                             line->origin == GIT_DIFF_LINE_ADD_EOFNL ||
                             line->origin == GIT_DIFF_LINE_DEL_EOFNL;
      if (!is_marker && line->origin != GIT_DIFF_LINE_CONTEXT &&
          line->origin != GIT_DIFF_LINE_ADDITION && line->origin != GIT_DIFF_LINE_DELETION) {
        continue;
      }
      if (budget->line_count == limits->diff_line_count_max ||
          budget->len + (ptrdiff_t)line->content_len > limits->diff_len_max) {
        budget->is_spent = true;
        file->is_truncated = true;
        break;
      }
      budget->line_count++;
      budget->len += (ptrdiff_t)line->content_len;
      // The marker belongs to the line before it, whose number it would repeat.
      file->lines[file->line_count++] = (struct DiffLine){
          .origin = is_marker ? '\\' : line->origin,
          .old_line = !is_marker && line->old_lineno > 0 ? line->old_lineno : 0,
          .new_line = !is_marker && line->new_lineno > 0 ? line->new_lineno : 0,
          .text = is_marker
                      ? CHANGESET_NO_NEWLINE
                      : copy_line((struct Str){line->content, (ptrdiff_t)line->content_len}, perm),
      };
      file->additions += line->origin == GIT_DIFF_LINE_ADDITION;
      file->deletions += line->origin == GIT_DIFF_LINE_DELETION;
      shown->line_count++;
    }
    // A cap reached at a hunk's first line leaves the hunk with no lines to show under its header.
    if (shown->line_count == 0) {
      file->hunk_count--;
    }
  }
  git_patch_free(patch);
  return 0;
}

static struct Str copy_line(struct Str text, struct Arena* arena) {
  while (text.len > 0 && (text.data[text.len - 1] == '\n' || text.data[text.len - 1] == '\r')) {
    text = str_head(text, text.len - 1);
  }
  return str_clone(text, arena);
}

static int compare_tree_order(const void* a, const void* b) {
  struct StrCut left = {.tail = ((const struct ChangesetEntry*)a)->path, .is_found = true};
  struct StrCut right = {.tail = ((const struct ChangesetEntry*)b)->path, .is_found = true};
  while (left.is_found && right.is_found) {
    left = str_cut(left.tail, '/');
    right = str_cut(right.tail, '/');
    // A segment with more after it is a directory, which sorts first among its siblings.
    if (left.is_found != right.is_found) {
      return left.is_found ? -1 : 1;
    }
    const int order = str_compare(left.head, right.head);
    if (order != 0) {
      return order;
    }
  }
  // A path deleted and added again is two deltas, which keep libgit2's order.
  const ptrdiff_t left_index = ((const struct ChangesetEntry*)a)->delta_index;
  const ptrdiff_t right_index = ((const struct ChangesetEntry*)b)->delta_index;
  return left_index < right_index ? -1 : left_index > right_index ? 1 : 0;
}
