// `mkdtemp`, `rmdir` and `unlink` are POSIX rather than C17, so this file asks for them.
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include "vcs/pack_export.h"

#include <errno.h>
#include <git2.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/error.h"
#include "core/log.h"
#include "core/path.h"
#include "core/str.h"
#include "os/os.h"
#include "vcs/refset.h"
#include "vcs/repository.h"

/**
 * Most tags of one ref's chain that the pack takes one at a time. The chain of a nested tag that is
 * longer, which no repository has in practice, goes in whole from the tag past that depth, which is
 * as correct and only slower.
 */
enum { PACK_EXPORT_TAG_CHAIN_COUNT_MAX = 64 };

/**
 * @brief Creates the temporary directory of `export`.
 *
 * @param export     Export whose `dir_path` receives the directory. Must not be `NULL`.
 * @param repository Repository the pack is of, which a failure names. Must not be `NULL`.
 * @param perm       Arena that owns the path. Must not be `NULL`.
 * @param log        Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the directory cannot be created.
 */
static int pack_export_build_dir(struct PackExport* export,
                                 const struct Repository* repository,
                                 struct Arena* perm,
                                 struct Log* log) __attribute__((nonnull(1, 2, 3, 4)));

/**
 * @brief Fills a pack builder, writes the pack and its idx into the export's directory, and reads
 *        their names and sizes. A step of `pack_export_build`.
 *
 * @param export     Export whose directory exists, which receives the paths and sizes. Must not be
 *                   `NULL`.
 * @param builder    Pack builder of `repository`. Must not be `NULL`.
 * @param repository Repository the refs are in. Must not be `NULL`.
 * @param refset     Refs to export. Must not be `NULL`.
 * @param os         Host the sizes are read on. Must not be `NULL`.
 * @param perm       Arena that owns the paths. Must not be `NULL`.
 * @param log        Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when an object cannot be read or the pack cannot be written.
 */
static int pack_export_build_write(struct PackExport* export,
                                   git_packbuilder* builder,
                                   const struct Repository* repository,
                                   const struct Refset* refset,
                                   struct Os* os,
                                   struct Arena* perm,
                                   struct Log* log) __attribute__((nonnull(1, 2, 3, 4, 5, 6, 7)));

/**
 * @brief Joins two path components into a path whose byte after its end is a `NUL`.
 *
 * @param base_path     Leading component.
 * @param relative_path Trailing component.
 * @param arena         Arena that owns the path. Must not be `NULL`.
 * @return The joined path, terminated.
 */
static struct Str terminated_join(struct Str base_path,
                                  struct Str relative_path,
                                  struct Arena* arena) __attribute__((nonnull(3)));

/**
 * @brief Inserts every object the refs reach into a pack builder.
 *
 * @param builder    Pack builder of `repository`. Must not be `NULL`.
 * @param repository Repository the refs are in. Must not be `NULL`.
 * @param refset     Refs to export. Must not be `NULL`.
 * @param log        Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when an object cannot be read.
 */
static int pack_export_build_insert(git_packbuilder* builder,
                                    const struct Repository* repository,
                                    const struct Refset* refset,
                                    struct Log* log) __attribute__((nonnull(1, 2, 3, 4)));

/**
 * @brief Inserts a ref's own object into a pack builder that already holds the history: each tag
 *        along its chain, and the tree or blob that the chain ends in, with every object below it.
 *        A chain that ends in a commit adds only its tags, because the history holds the commit and
 *        its tree.
 *
 * @param builder    Pack builder of `repository`. Must not be `NULL`.
 * @param repository Repository the ref is in. Must not be `NULL`.
 * @param ref_oid    Id of the object the ref points at. Must not be `NULL`.
 * @return `0` on success, or `-1` when an object cannot be read or inserted.
 */
static int pack_export_build_insert_ref(git_packbuilder* builder,
                                        const struct Repository* repository,
                                        const git_oid* ref_oid) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Reads the size of one written file into `len_out`.
 *
 * @param file_path  File to measure.
 * @param repository Repository the pack is of, which a failure names. Must not be `NULL`.
 * @param os         Host the file is on. Must not be `NULL`.
 * @param len_out    Receives the size. Must not be `NULL`.
 * @param log        Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the file cannot be inspected.
 */
static int pack_export_build_measure(struct Str file_path,
                                     const struct Repository* repository,
                                     struct Os* os,
                                     ptrdiff_t* len_out,
                                     struct Log* log) __attribute__((nonnull(2, 3, 4, 5)));

int pack_export_build(struct PackExport* export,
                      const struct Repository* repository,
                      const struct Refset* refset,
                      struct Os* os,
                      struct Arena* perm,
                      struct Log* log) {
  if (refset->count == 0) {
    return 0;
  }
  if (pack_export_build_dir(export, repository, perm, log) != 0) {
    return -1;
  }
  git_packbuilder* builder = NULL;
  if (git_packbuilder_new(&builder, repository->git) != 0) {
    return repository_fail(repository, S("failed to start clone pack"), log);
  }
  // One thread makes the delta search deterministic, so unchanged objects make the same pack.
  (void)git_packbuilder_set_threads(builder, 1);
  const int rc = pack_export_build_write(export, builder, repository, refset, os, perm, log);
  git_packbuilder_free(builder);
  return rc;
}

void pack_export_remove(const struct PackExport* export) {
  // The directory goes last, once it is empty. A failure leaves the files in `TMPDIR`, which is
  // what an interrupted build does too.
  if (export->pack_path.len > 0) {
    (void)unlink(export->pack_path.data);
  }
  if (export->idx_path.len > 0) {
    (void)unlink(export->idx_path.data);
  }
  if (export->dir_path.len > 0) {
    (void)rmdir(export->dir_path.data);
  }
}

struct Str pack_export_format_packs(struct Str pack_id, struct Arena* arena) {
  if (pack_id.len == 0) {
    return (struct Str){0};
  }
  struct Buf text = {.arena = arena};
  buf_printf(&text, "P pack-%.*s.pack\n\n", STR_FMT(pack_id));
  return buf_str(&text);
}

struct Str pack_export_output_path(struct Str repo_name,
                                   struct Str pack_id,
                                   struct Str extension,
                                   struct Arena* arena) {
  struct Buf path = {.arena = arena};
  buf_printf(&path, "%.*s.git/objects/pack/pack-%.*s.%.*s", STR_FMT(repo_name), STR_FMT(pack_id),
             STR_FMT(extension));
  return buf_str(&path);
}

static int pack_export_build_dir(struct PackExport* export,
                                 const struct Repository* repository,
                                 struct Arena* perm,
                                 struct Log* log) {
  const char* tmp_dir = getenv("TMPDIR");
  struct Str base = tmp_dir != NULL && tmp_dir[0] != '\0' ? str_from_cstr(tmp_dir) : S("/tmp");
  while (base.len > 1 && str_endswith(base, S("/"))) {
    base = str_head(base, base.len - 1);
  }
  char* template = str_cstr(path_join(base, S("surge-pack.XXXXXX"), perm), perm);
  if (mkdtemp(template) == NULL) {
    char message[ERROR_SYSTEM_MESSAGE_SIZE];
    return log_error(log, "failed to create clone pack directory for repo '%.*s': %s ('%s')",
                     STR_FMT(repository->name),
                     error_system_message(message, sizeof(message), errno), template);
  }
  export->dir_path = str_from_cstr(template);
  return 0;
}

static int pack_export_build_write(struct PackExport* export,
                                   git_packbuilder* builder,
                                   const struct Repository* repository,
                                   const struct Refset* refset,
                                   struct Os* os,
                                   struct Arena* perm,
                                   struct Log* log) {
  if (pack_export_build_insert(builder, repository, refset, log) != 0) {
    return -1;
  }
  struct Arena scratch = *perm;
  if (git_packbuilder_write(builder, str_cstr(export->dir_path, &scratch), 0, NULL, NULL) != 0) {
    return repository_fail(repository, S("failed to build clone pack"), log);
  }
  export->pack_id = str_clone(str_from_cstr(git_packbuilder_name(builder)), perm);
  struct Buf pack_name = {.arena = perm};
  buf_printf(&pack_name, "pack-%.*s.pack", STR_FMT(export->pack_id));
  export->pack_path = terminated_join(export->dir_path, buf_str(&pack_name), perm);
  struct Buf idx_name = {.arena = perm};
  buf_printf(&idx_name, "pack-%.*s.idx", STR_FMT(export->pack_id));
  export->idx_path = terminated_join(export->dir_path, buf_str(&idx_name), perm);
  if (pack_export_build_measure(export->pack_path, repository, os, &export->pack_len, log) != 0 ||
      pack_export_build_measure(export->idx_path, repository, os, &export->idx_len, log) != 0) {
    return -1;
  }
  return 0;
}

static struct Str terminated_join(struct Str base_path,
                                  struct Str relative_path,
                                  struct Arena* arena) {
  return str_from_cstr(str_cstr(path_join(base_path, relative_path, arena), arena));
}

static int pack_export_build_insert(git_packbuilder* builder,
                                    const struct Repository* repository,
                                    const struct Refset* refset,
                                    struct Log* log) {
  git_revwalk* walk = NULL;
  if (git_revwalk_new(&walk, repository->git) != 0) {
    return repository_fail(repository, S("failed to walk history for clone pack"), log);
  }
  int rc = 0;
  for (ptrdiff_t i = 0; rc == 0 && i < refset->count; i++) {
    const struct Ref* ref = &refset->refs[i];
    git_oid commit_oid;
    if (ref->commit_oid.len > 0 &&
        (git_oid_fromstrn(&commit_oid, ref->commit_oid.data, (size_t)ref->commit_oid.len) != 0 ||
         git_revwalk_push(walk, &commit_oid) != 0)) {
      rc = repository_fail(repository, S("failed to walk history for clone pack"), log);
    }
  }
  if (rc == 0 && git_packbuilder_insert_walk(builder, walk) != 0) {
    rc = repository_fail(repository, S("failed to add history to clone pack"), log);
  }
  git_revwalk_free(walk);
  // A ref's own object goes in after the history, so an annotated or nested tag, and a tag of a
  // tree or a blob, which the walk does not reach, is in the pack too.
  for (ptrdiff_t i = 0; rc == 0 && i < refset->count; i++) {
    const struct Ref* ref = &refset->refs[i];
    git_oid oid;
    if (git_oid_fromstrn(&oid, ref->oid.data, (size_t)ref->oid.len) != 0 ||
        pack_export_build_insert_ref(builder, repository, &oid) != 0) {
      rc = repository_fail(repository, S("failed to add ref to clone pack"), log);
    }
  }
  return rc;
}

static int pack_export_build_insert_ref(git_packbuilder* builder,
                                        const struct Repository* repository,
                                        const git_oid* ref_oid) {
  // `git_packbuilder_insert_recur` walks the whole tree below a commit even when the history put it
  // in already, so it runs only for a chain that ends in a tree or a blob.
  git_oid oid = *ref_oid;
  git_object_t type = GIT_OBJECT_INVALID;
  for (ptrdiff_t i = 0; i < PACK_EXPORT_TAG_CHAIN_COUNT_MAX; i++) {
    git_object* object = NULL;
    if (git_object_lookup(&object, repository->git, &oid, GIT_OBJECT_ANY) != 0) {
      return -1;
    }
    type = git_object_type(object);
    if (type != GIT_OBJECT_TAG) {
      git_object_free(object);
      break;
    }
    const int insert_rc = git_packbuilder_insert(builder, &oid, NULL);
    git_oid_cpy(&oid, git_tag_target_id((const git_tag*)object));
    git_object_free(object);
    if (insert_rc != 0) {
      return -1;
    }
  }
  if (type == GIT_OBJECT_COMMIT) {
    return 0;
  }
  return git_packbuilder_insert_recur(builder, &oid, NULL) != 0 ? -1 : 0;
}

static int pack_export_build_measure(struct Str file_path,
                                     const struct Repository* repository,
                                     struct Os* os,
                                     ptrdiff_t* len_out,
                                     struct Log* log) {
  struct OsStat st;
  if (os_stat(os, file_path, &st) != 0) {
    char message[ERROR_SYSTEM_MESSAGE_SIZE];
    return log_error(log, "failed to measure clone pack of repo '%.*s': %s ('%.*s')",
                     STR_FMT(repository->name),
                     error_system_message(message, sizeof(message), errno), STR_FMT(file_path));
  }
  *len_out = st.len;
  return 0;
}
