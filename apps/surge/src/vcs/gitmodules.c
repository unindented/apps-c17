#include "vcs/gitmodules.h"

#include <git2.h>
#include <git2/sys/config.h>
#include <stddef.h>
#include <stdlib.h>

#include "core/arena.h"
#include "core/str.h"

/** The keys of a submodule that a gitlink's row reads, as libgit2 names them. */
#define GITMODULES_KEY_PATTERN "^submodule\\..+\\.(path|url)$"

/** What the parse of one file collects, for `gitmodules_parse_entry`. */
struct GitmodulesParse {
  /** Arena that owns the submodules. */
  struct Arena* perm;

  /** Submodules found so far, in the order their names first appear. */
  struct Gitmodule* gitmodules;

  /** Number of entries in `gitmodules`. */
  ptrdiff_t count;

  /** Slots allocated at `gitmodules`. */
  ptrdiff_t capacity;
};

/**
 * @brief Records one `submodule.<name>.path` or `submodule.<name>.url` in its submodule. This is
 *        the `git_config_foreach_cb` of `gitmodules_parse`.
 *
 * @param entry   The config entry, whose name libgit2 matched. Must not be `NULL`.
 * @param payload `struct GitmodulesParse*`. Must not be `NULL`.
 * @return `0` always.
 */
static int gitmodules_parse_entry(const git_config_entry* entry, void* payload)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Returns libgit2's message for the last failure, in `arena`.
 *
 * @param arena Arena that owns the message. Must not be `NULL`.
 * @return The message.
 */
static struct Str last_error_message(struct Arena* arena) __attribute__((nonnull(1)));

/**
 * @brief Orders two submodules bytewise by path, for `qsort`.
 *
 * @param a First `struct Gitmodule`. Must not be `NULL`.
 * @param b Second `struct Gitmodule`. Must not be `NULL`.
 * @return Negative, zero or positive as `a` sorts before, with, or after `b`.
 */
static int compare_paths(const void* a, const void* b) __attribute__((nonnull(1, 2)));

int gitmodules_parse(struct Str text,
                     struct Arena* perm,
                     struct Gitmodule** gitmodules_out,
                     ptrdiff_t* count_out,
                     struct Str* reason_out) {
  *gitmodules_out = NULL;
  *count_out = 0;
  git_config* config = NULL;
  git_config_backend* backend = NULL;
  // The memory backend parses when it is added, and only an allocation can fail before that.
  if (git_config_new(&config) != 0 ||
      git_config_backend_from_string(&backend, text.data, (size_t)text.len, NULL) != 0) {
    git_config_free(config);
    *reason_out = last_error_message(perm);
    return -1;
  }
  if (git_config_add_backend(config, backend, GIT_CONFIG_LEVEL_APP, NULL, 0) != 0) {
    // A backend that did not open is still the caller's.
    *reason_out = last_error_message(perm);
    backend->free(backend);
    git_config_free(config);
    return -1;
  }
  struct GitmodulesParse parse = {.perm = perm};
  const int rc =
      git_config_foreach_match(config, GITMODULES_KEY_PATTERN, gitmodules_parse_entry, &parse);
  if (rc != 0) {
    *reason_out = last_error_message(perm);
  }
  git_config_free(config);
  if (rc != 0) {
    return -1;
  }
  // A submodule without a path names no gitlink.
  ptrdiff_t count = 0;
  for (ptrdiff_t i = 0; i < parse.count; i++) {
    if (parse.gitmodules[i].path.len > 0) {
      parse.gitmodules[count++] = parse.gitmodules[i];
    }
  }
  if (count > 0) {
    qsort(parse.gitmodules, (size_t)count, sizeof(*parse.gitmodules), compare_paths);
  }
  *gitmodules_out = parse.gitmodules;
  *count_out = count;
  return 0;
}

static int gitmodules_parse_entry(const git_config_entry* entry, void* payload) {
  struct GitmodulesParse* parse = payload;
  // The pattern matched `submodule.<name>.<key>`, and a name can hold `.`, so the key is after the
  // last one.
  const struct Str rest = str_tail(str_from_cstr(entry->name), S("submodule.").len);
  ptrdiff_t dot = rest.len - 1;
  while (rest.data[dot] != '.') {
    dot--;
  }
  const struct Str name = str_head(rest, dot);
  const struct Str key = str_tail(rest, dot + 1);
  const struct Str value = entry->value != NULL ? str_from_cstr(entry->value) : (struct Str){0};
  struct Gitmodule* gitmodule = NULL;
  for (ptrdiff_t i = 0; i < parse->count && gitmodule == NULL; i++) {
    if (str_eq(parse->gitmodules[i].name, name)) {
      gitmodule = &parse->gitmodules[i];
    }
  }
  if (gitmodule == NULL) {
    if (parse->count == parse->capacity) {
      parse->gitmodules =
          arena_grow(parse->perm, parse->gitmodules, &parse->capacity,
                     (ptrdiff_t)sizeof(struct Gitmodule), (ptrdiff_t)_Alignof(struct Gitmodule));
    }
    gitmodule = &parse->gitmodules[parse->count++];
    *gitmodule = (struct Gitmodule){.name = str_clone(name, parse->perm)};
  }
  if (str_eq(key, S("path"))) {
    gitmodule->path = str_clone(value, parse->perm);
  } else {
    gitmodule->url = str_clone(value, parse->perm);
  }
  return 0;
}

static struct Str last_error_message(struct Arena* arena) {
  const git_error* error = git_error_last();
  const char* message = error != NULL && error->message != NULL ? error->message : "unknown error";
  return str_clone(str_from_cstr(message), arena);
}

static int compare_paths(const void* a, const void* b) {
  return str_compare(((const struct Gitmodule*)a)->path, ((const struct Gitmodule*)b)->path);
}
