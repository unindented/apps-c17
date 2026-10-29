#include "vcs/repository.h"

#include <git2.h>
#include <git2/sys/alloc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "core/arena.h"
#include "core/log.h"
#include "core/str.h"

/** Config levels whose search path is emptied, so no file outside a repository is read. */
static const int REPOSITORY_EMPTY_CONFIG_LEVELS[] = {
    GIT_CONFIG_LEVEL_PROGRAMDATA,
    GIT_CONFIG_LEVEL_SYSTEM,
    GIT_CONFIG_LEVEL_XDG,
    GIT_CONFIG_LEVEL_GLOBAL,
};

/**
 * @brief Allocates for libgit2, and ends the process through `os_oom` when `malloc` fails.
 *
 * @param len  Bytes to allocate. `0` allocates one byte, so a `NULL` always means failure.
 * @param file Source file of the call. Unused.
 * @param line Source line of the call. Unused.
 * @return The allocation, never `NULL`.
 */
static void* repository_malloc(size_t len, const char* file, int line);

/**
 * @brief Reallocates for libgit2, and ends the process through `os_oom` when `realloc` fails.
 *
 * @param data Allocation to resize, or `NULL` for a new one.
 * @param len  Bytes the allocation must hold. `0` keeps one byte, so a `NULL` always means failure.
 * @param file Source file of the call. Unused.
 * @param line Source line of the call. Unused.
 * @return The resized allocation, never `NULL`.
 */
static void* repository_realloc(void* data, size_t len, const char* file, int line);

/**
 * @brief Records a failed setup step with libgit2's own message.
 *
 * @param operation Operation leading the message, such as `failed to set up libgit2`.
 * @param log       Log that receives the error. Must not be `NULL`.
 * @return `-1` always.
 */
static int repository_setup_fail(struct Str operation, struct Log* log) __attribute__((nonnull(2)));

/**
 * @brief Returns `path` without its trailing `/`, copied into `arena`.
 *
 * @param path  Terminated path libgit2 returned, or `NULL`.
 * @param arena Arena that owns the copy. Must not be `NULL`.
 * @return The path, empty for `NULL`.
 */
static struct Str copy_dir_path(const char* path, struct Arena* arena) __attribute__((nonnull(2)));

/**
 * @brief Returns libgit2's message for the last failure on this thread.
 *
 * @return The message, or `unknown error` when libgit2 recorded none.
 */
static const char* last_error_message(void) __attribute__((returns_nonnull));

int repository_setup(struct Log* log) {
  int major = 0;
  int minor = 0;
  int revision = 0;
  (void)git_libgit2_version(&major, &minor, &revision);
  if (major != LIBGIT2_VERSION_MAJOR || minor != LIBGIT2_VERSION_MINOR) {
    return log_error(log, "failed to set up libgit2: linked version must be %d.%d: '%d.%d.%d'",
                     LIBGIT2_VERSION_MAJOR, LIBGIT2_VERSION_MINOR, major, minor, revision);
  }
  if ((git_libgit2_features() & GIT_FEATURE_THREADS) == 0) {
    return log_error(log, "failed to set up libgit2: it was built without thread support");
  }
  // libgit2 copies the table, so it can live on the stack. It takes effect before the first
  // allocation of `git_libgit2_init`.
  git_allocator allocator = {
      .gmalloc = repository_malloc,
      .grealloc = repository_realloc,
      .gfree = free,
  };
  if (git_libgit2_opts(GIT_OPT_SET_ALLOCATOR, &allocator) != 0) {
    return repository_setup_fail(S("failed to set libgit2's allocator"), log);
  }
  if (git_libgit2_init() < 0) {
    return repository_setup_fail(S("failed to set up libgit2"), log);
  }
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(REPOSITORY_EMPTY_CONFIG_LEVELS) /
                                        sizeof(REPOSITORY_EMPTY_CONFIG_LEVELS[0]));
       i++) {
    if (git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, REPOSITORY_EMPTY_CONFIG_LEVELS[i], "") != 0) {
      return repository_setup_fail(S("failed to empty libgit2's config search path"), log);
    }
  }
  if (git_libgit2_opts(GIT_OPT_SET_OWNER_VALIDATION, 0) != 0) {
    return repository_setup_fail(S("failed to turn off libgit2's owner validation"), log);
  }
  if (git_libgit2_opts(GIT_OPT_SET_MWINDOW_FILE_LIMIT, (size_t)REPOSITORY_MWINDOW_FILE_LIMIT) !=
      0) {
    return repository_setup_fail(S("failed to limit libgit2's mapped pack files"), log);
  }
  return 0;
}

int repository_open(struct Repository* repository,
                    struct Str path,
                    struct Str name,
                    struct Arena* perm,
                    struct Log* log) {
  struct Arena scratch = *perm;
  const char* path_cstr = str_cstr(path, &scratch);
  repository->name = name;
  // No search and no `GIT_DIR`: the config names exactly the repository the build reads.
  git_repository* git = NULL;
  if (git_repository_open_ext(&git, path_cstr, GIT_REPOSITORY_OPEN_NO_SEARCH, NULL) != 0) {
    return log_error(log, "failed to open repo '%.*s': %s ('%.*s')", STR_FMT(name),
                     last_error_message(), STR_FMT(path));
  }
  repository->git = git;
  // A linked worktree's own git directory holds only its `HEAD` and index, and the objects, the
  // refs and the description that a build reads are in the common one.
  repository->git_dir = copy_dir_path(git_repository_commondir(git), perm);
  repository->work_dir = copy_dir_path(git_repository_workdir(git), perm);
  return 0;
}

void repository_close(struct Repository* repository) {
  if (repository->git != NULL) {
    git_repository_free(repository->git);
    repository->git = NULL;
  }
}

int repository_read_blob(const struct Repository* repository,
                         struct Str oid,
                         struct Arena* perm,
                         struct Str* data_out,
                         bool* is_binary_out,
                         struct Log* log) {
  git_oid blob_oid;
  git_blob* blob = NULL;
  if (git_oid_fromstrn(&blob_oid, oid.data, (size_t)oid.len) != 0 ||
      git_blob_lookup(&blob, repository->git, &blob_oid) != 0) {
    return repository_fail(repository, S("failed to read blob"), log);
  }
  const struct Str data = {git_blob_rawcontent(blob), (ptrdiff_t)git_blob_rawsize(blob)};
  *data_out = str_clone(data, perm);
  *is_binary_out = git_blob_is_binary(blob) != 0;
  git_blob_free(blob);
  return 0;
}

int repository_fail(const struct Repository* repository, struct Str operation, struct Log* log) {
  return log_error(log, "%.*s: %s (in repo '%.*s')", STR_FMT(operation), last_error_message(),
                   STR_FMT(repository->name));
}

struct Str repository_oid_text(const git_oid* oid, struct Arena* arena) {
  char* hex = arena_new(arena, GIT_OID_SHA1_HEXSIZE, char);
  (void)git_oid_fmt(hex, oid);
  return (struct Str){hex, GIT_OID_SHA1_HEXSIZE};
}

struct Str repository_text_id(struct Str text, struct Arena* arena) {
  git_oid oid;
  // Hashing a buffer as a blob cannot fail for a SHA-1 repository but on an argument error, which a
  // counted string never makes.
  (void)git_odb_hash(&oid, text.data, (size_t)text.len, GIT_OBJECT_BLOB);
  return repository_oid_text(&oid, arena);
}

static void* repository_malloc(size_t len, const char* file, int line) {
  (void)file;
  (void)line;
  void* data = malloc(len == 0 ? 1 : len);
  if (data == NULL) {
    os_oom();
  }
  return data;
}

static void* repository_realloc(void* data, size_t len, const char* file, int line) {
  (void)file;
  (void)line;
  void* resized = realloc(data, len == 0 ? 1 : len);
  if (resized == NULL) {
    os_oom();
  }
  return resized;
}

static int repository_setup_fail(struct Str operation, struct Log* log) {
  return log_error(log, "%.*s: %s", STR_FMT(operation), last_error_message());
}

static struct Str copy_dir_path(const char* path, struct Arena* arena) {
  if (path == NULL) {
    return (struct Str){0};
  }
  struct Str dir_path = str_from_cstr(path);
  while (dir_path.len > 1 && str_endswith(dir_path, S("/"))) {
    dir_path = str_head(dir_path, dir_path.len - 1);
  }
  return str_clone(dir_path, arena);
}

static const char* last_error_message(void) {
  const git_error* error = git_error_last();
  return error != NULL && error->message != NULL ? error->message : "unknown error";
}
