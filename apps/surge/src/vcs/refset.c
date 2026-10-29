#include "vcs/refset.h"

#include <git2.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "core/arena.h"
#include "core/ascii.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/str.h"
#include "core/utf8.h"
#include "vcs/repository.h"

#define REFSET_BRANCH_PREFIX S("refs/heads/")
#define REFSET_TAG_PREFIX S("refs/tags/")

/** A branch's name, or a directory above it in its pages, with the branch it comes from. */
struct RefsetFoldedPath {
  /** The full name, `refs/heads/...`, cut at a `/` or whole. */
  struct Str path;

  /** The branch's full name. */
  struct Str branch;
};

/**
 * @brief Adds one ref to the refs being loaded, unless it is neither a branch nor a tag, or is
 *        symbolic.
 *
 * @param refset     Refs loaded so far, which receives the ref. Must not be `NULL`.
 * @param capacity   Slots allocated at `refset->refs`, updated when they grow. Must not be `NULL`.
 * @param reference  Ref to add. Must not be `NULL`.
 * @param repository Repository the ref is in. Must not be `NULL`.
 * @param perm       Arena that owns the refs. Must not be `NULL`.
 * @param log        Log that receives the failure. Must not be `NULL`.
 * @return `0` when the ref was added or skipped, or `-1` when its name is invalid or it does not
 *         peel.
 */
static int refset_load_add(struct Refset* refset,
                           ptrdiff_t* capacity,
                           git_reference* reference,
                           const struct Repository* repository,
                           struct Arena* perm,
                           struct Log* log) __attribute__((nonnull(1, 2, 3, 4, 5, 6)));

/**
 * @brief Refuses branches that collide after ASCII case folding, and a ref that is a `/`-prefix of
 *        another.
 *
 * @param refset     Loaded refs, sorted by name. Must not be `NULL`.
 * @param repository Repository the refs are in, which the messages name. Must not be `NULL`.
 * @param scratch    Arena for the folded names.
 * @param log        Log that receives every collision. Must not be `NULL`.
 * @return `0` when no refs collide, or `-1` otherwise.
 */
static int refset_load_check_collisions(const struct Refset* refset,
                                        const struct Repository* repository,
                                        struct Arena scratch,
                                        struct Log* log) __attribute__((nonnull(1, 2, 4)));

/**
 * @brief Orders two refs bytewise by full name, for `qsort` and `bsearch`.
 *
 * @param a First `struct Ref`. Must not be `NULL`.
 * @param b Second `struct Ref`. Must not be `NULL`.
 * @return Negative, zero or positive as `a` sorts before, with, or after `b`.
 */
static int compare_refs(const void* a, const void* b) __attribute__((nonnull(1, 2)));

/**
 * @brief Orders two strings bytewise after ASCII case folding, for `qsort`.
 *
 * @param a First `struct Str`. Must not be `NULL`.
 * @param b Second `struct Str`. Must not be `NULL`.
 * @return Negative, zero or positive as `a` sorts before, with, or after `b`.
 */
static int compare_folded(const void* a, const void* b) __attribute__((nonnull(1, 2)));

/**
 * @brief Orders two `struct RefsetFoldedPath` by path after ASCII case folding, then bytewise, for
 *        `qsort`.
 *
 * @param a First `struct RefsetFoldedPath`. Must not be `NULL`.
 * @param b Second `struct RefsetFoldedPath`. Must not be `NULL`.
 * @return Negative, zero or positive as `a` sorts before, with, or after `b`.
 */
static int compare_folded_paths(const void* a, const void* b) __attribute__((nonnull(1, 2)));

int refset_load(struct Refset* refset,
                const struct Repository* repository,
                struct Arena* perm,
                struct Log* log) {
  git_reference_iterator* iterator = NULL;
  if (git_reference_iterator_new(&iterator, repository->git) != 0) {
    return repository_fail(repository, S("failed to list refs"), log);
  }
  struct Arena arena = *perm;
  struct Refset refs = {0};
  ptrdiff_t capacity = 0;
  // A refused ref leaves the others to check, so one run reports every bad ref and every collision
  // of the rest.
  int rc = 0;
  git_reference* reference = NULL;
  int next_rc = 0;
  while ((next_rc = git_reference_next(&reference, iterator)) == 0) {
    if (refset_load_add(&refs, &capacity, reference, repository, &arena, log) != 0) {
      rc = -1;
    }
    git_reference_free(reference);
  }
  git_reference_iterator_free(iterator);
  if (next_rc != GIT_ITEROVER) {
    return repository_fail(repository, S("failed to list refs"), log);
  }
  if (refs.count > 0) {
    qsort(refs.refs, (size_t)refs.count, sizeof(*refs.refs), compare_refs);
  }
  if (refset_load_check_collisions(&refs, repository, arena, log) != 0 || rc != 0) {
    return -1;
  }
  *perm = arena;
  *refset = refs;
  return 0;
}

const struct Ref* refset_find_branch(const struct Refset* refset, struct Str short_name) {
  for (ptrdiff_t i = 0; i < refset->count; i++) {
    if (refset->refs[i].kind == REF_KIND_BRANCH && str_eq(refset->refs[i].short_name, short_name)) {
      return &refset->refs[i];
    }
  }
  return NULL;
}

struct Str refset_format_info_refs(const struct Refset* refset, struct Arena* arena) {
  struct Buf text = {.arena = arena};
  for (ptrdiff_t i = 0; i < refset->count; i++) {
    const struct Ref* ref = &refset->refs[i];
    buf_printf(&text, "%.*s\t%.*s\n", STR_FMT(ref->oid), STR_FMT(ref->name));
    if (ref->peeled_oid.len > 0) {
      buf_printf(&text, "%.*s\t%.*s^{}\n", STR_FMT(ref->peeled_oid), STR_FMT(ref->name));
    }
  }
  return buf_str(&text);
}

static int refset_load_add(struct Refset* refset,
                           ptrdiff_t* capacity,
                           git_reference* reference,
                           const struct Repository* repository,
                           struct Arena* perm,
                           struct Log* log) {
  const struct Str name = str_from_cstr(git_reference_name(reference));
  enum RefKind kind = REF_KIND_BRANCH;
  struct Str short_name = {0};
  if (str_startswith(name, REFSET_BRANCH_PREFIX)) {
    short_name = str_tail(name, REFSET_BRANCH_PREFIX.len);
  } else if (str_startswith(name, REFSET_TAG_PREFIX)) {
    kind = REF_KIND_TAG;
    short_name = str_tail(name, REFSET_TAG_PREFIX.len);
  } else {
    return 0;
  }
  // A symbolic ref has no id of its own for `info/refs` to list.
  if (git_reference_type(reference) != GIT_REFERENCE_DIRECT) {
    return 0;
  }
  // A name git refuses can hold a tab or a newline, which would break the lines of `info/refs`, so
  // only a hand-written `packed-refs` holds one, and it refuses the build.
  int is_valid = 0;
  if (git_reference_name_is_valid(&is_valid, git_reference_name(reference)) != 0 || !is_valid) {
    return log_error(log, "ref name is not valid: '%.*s' (in repo '%.*s')", STR_FMT(name),
                     STR_FMT(repository->name));
  }
  // A ref's name is a directory of its pages, which a host that takes only UTF-8 file names, such
  // as macOS, refuses otherwise. git accepts any byte past ASCII in a name.
  if (!utf8_is_valid(name)) {
    struct Arena scratch = *perm;
    struct Buf shown = {.arena = &scratch};
    for (ptrdiff_t i = 0; i < name.len; i++) {
      const unsigned char c = (unsigned char)name.data[i];
      if (c >= 0x80) {
        buf_printf(&shown, "\\x%02X", c);
      } else {
        buf_append_char(&shown, (char)c);
      }
    }
    const struct Str shown_name = buf_str(&shown);
    return log_error(log, "ref name is not valid UTF-8: '%.*s' (in repo '%.*s')",
                     STR_FMT(shown_name), STR_FMT(repository->name));
  }
  git_object* peeled = NULL;
  if (git_reference_peel(&peeled, reference, GIT_OBJECT_ANY) != 0) {
    return repository_fail(repository, S("failed to peel ref"), log);
  }
  const git_oid* target = git_reference_target(reference);
  const git_oid* peeled_id = git_object_id(peeled);
  struct Ref ref = {
      .name = str_clone(name, perm),
      .kind = kind,
      .oid = repository_oid_text(target, perm),
  };
  ref.short_name = str_tail(ref.name, name.len - short_name.len);
  if (git_oid_cmp(target, peeled_id) != 0) {
    ref.peeled_oid = repository_oid_text(peeled_id, perm);
  }
  if (git_object_type(peeled) == GIT_OBJECT_COMMIT) {
    ref.commit_oid = ref.peeled_oid.len > 0 ? ref.peeled_oid : ref.oid;
  }
  git_object_free(peeled);
  if (refset->count == *capacity) {
    refset->refs = arena_grow(perm, refset->refs, capacity, (ptrdiff_t)sizeof(struct Ref),
                              (ptrdiff_t)_Alignof(struct Ref));
  }
  refset->refs[refset->count++] = ref;
  return 0;
}

static int refset_load_check_collisions(const struct Refset* refset,
                                        const struct Repository* repository,
                                        struct Arena scratch,
                                        struct Log* log) {
  int rc = 0;
  // Branches become directories of their pages, so two whose names, or the directories above them,
  // differ only in case would share one directory on a case-insensitive host: `main` and `Main`,
  // `a` and `A/b`, or `A/x` and `a/y`. Each branch gives its name and every directory above it, and
  // sorting them folded, then bytewise, puts two spellings of one folded path side by side.
  ptrdiff_t path_count = 0;
  for (ptrdiff_t i = 0; i < refset->count; i++) {
    const struct Ref* ref = &refset->refs[i];
    for (ptrdiff_t j = 0; ref->kind == REF_KIND_BRANCH && j <= ref->short_name.len; j++) {
      path_count += j == ref->short_name.len || ref->short_name.data[j] == '/';
    }
  }
  struct RefsetFoldedPath* paths = arena_new(&scratch, path_count, struct RefsetFoldedPath);
  ptrdiff_t index = 0;
  for (ptrdiff_t i = 0; i < refset->count; i++) {
    const struct Ref* ref = &refset->refs[i];
    const ptrdiff_t prefix_len = ref->name.len - ref->short_name.len;
    for (ptrdiff_t j = 0; ref->kind == REF_KIND_BRANCH && j <= ref->short_name.len; j++) {
      if (j == ref->short_name.len || ref->short_name.data[j] == '/') {
        paths[index++] = (struct RefsetFoldedPath){
            .path = str_head(ref->name, prefix_len + j),
            .branch = ref->name,
        };
      }
    }
  }
  if (path_count > 0) {
    qsort(paths, (size_t)path_count, sizeof(*paths), compare_folded_paths);
  }
  for (ptrdiff_t i = 1; i < path_count; i++) {
    if (compare_folded(&paths[i - 1].path, &paths[i].path) == 0 &&
        !str_eq(paths[i - 1].path, paths[i].path)) {
      rc = log_error(log, "branches '%.*s' and '%.*s' collide after case folding (in repo '%.*s')",
                     STR_FMT(paths[i - 1].branch), STR_FMT(paths[i].branch),
                     STR_FMT(repository->name));
    }
  }
  // A ref that is a `/`-prefix of another would be a file and a directory at once in a clone's
  // `refs/`, and on the pages.
  for (ptrdiff_t i = 0; i < refset->count; i++) {
    const struct Str name = refset->refs[i].name;
    for (ptrdiff_t j = name.len - 1; j > 0; j--) {
      if (name.data[j] != '/') {
        continue;
      }
      const struct Ref key = {.name = str_head(name, j)};
      const struct Ref* prefix =
          bsearch(&key, refset->refs, (size_t)refset->count, sizeof(*refset->refs), compare_refs);
      if (prefix != NULL) {
        rc = log_error(log, "ref '%.*s' is a prefix of ref '%.*s' (in repo '%.*s')",
                       STR_FMT(prefix->name), STR_FMT(name), STR_FMT(repository->name));
      }
    }
  }
  return rc;
}

static int compare_refs(const void* a, const void* b) {
  const struct Ref* left = a;
  const struct Ref* right = b;
  return str_compare(left->name, right->name);
}

static int compare_folded(const void* a, const void* b) {
  const struct Str* left = a;
  const struct Str* right = b;
  const ptrdiff_t len = left->len < right->len ? left->len : right->len;
  for (ptrdiff_t i = 0; i < len; i++) {
    const unsigned char left_byte = ascii_to_lower((unsigned char)left->data[i]);
    const unsigned char right_byte = ascii_to_lower((unsigned char)right->data[i]);
    if (left_byte != right_byte) {
      return left_byte < right_byte ? -1 : 1;
    }
  }
  return left->len < right->len ? -1 : left->len > right->len ? 1 : 0;
}

static int compare_folded_paths(const void* a, const void* b) {
  const struct RefsetFoldedPath* left = a;
  const struct RefsetFoldedPath* right = b;
  const int order = compare_folded(&left->path, &right->path);
  if (order != 0) {
    return order;
  }
  const int bytes = str_compare(left->path, right->path);
  return bytes != 0 ? bytes : str_compare(left->branch, right->branch);
}
