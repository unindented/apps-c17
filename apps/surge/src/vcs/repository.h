#ifndef SURGE_REPOSITORY_H
#define SURGE_REPOSITORY_H

#include <stdbool.h>

#include "core/str.h"

struct Arena;
struct Log;
struct git_oid;
struct git_repository;

/** One open repository: libgit2's handle, and the names a diagnostic and the overlap check use. */
struct Repository {
  /** libgit2's handle, which `repository_close` frees, or `NULL` when nothing is open. */
  struct git_repository* git;

  /** Name of the repo in the config, which every diagnostic about it names. */
  struct Str name;

  /**
   * Common git directory, without a trailing `/`: the git directory of the repository, and for a
   * linked worktree that of its main repository, not `.git/worktrees/<name>`. The overlap check
   * keeps the output out of it, and the build reads the `description` file in it.
   */
  struct Str git_dir;

  /** Work tree, without a trailing `/`, or empty for a bare repository. */
  struct Str work_dir;
};

/**
 * Most pack files libgit2 keeps mapped at once, across every repository and worker of the process.
 * With one worker per CPU, the default, each holding one open file for its output, this keeps a
 * build within a descriptor limit of 256 up to about 120 CPUs, and at four workers per CPU, the
 * most a build runs, up to about 30.
 */
enum { REPOSITORY_MWINDOW_FILE_LIMIT = 128 };

/**
 * @brief Sets libgit2 up for the process: its allocator, its initialization and the options that
 *        every repository read relies on.
 *
 * The allocator calls `os_oom` when `malloc` fails, so libgit2 running out of memory ends in the
 * same report as an arena does. Every config search path level is empty, so neither the user's nor
 * the system's git config changes what a build reads, and owner validation is off, so a repository
 * that another user owns is read as the user's own, as the trust model in ARCHITECTURE.md decides.
 * The options are process-wide, so this runs once, on the main thread, before any worker starts.
 *
 * @param log Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the linked libgit2 is not the version the headers name,
 *         lacks thread support, or rejects an option.
 */
int repository_setup(struct Log* log) __attribute__((nonnull(1)));

/**
 * @brief Opens the repository at `path`, a work tree or a bare git directory, exactly there:
 *        libgit2 does not search the parent directories, nor read `GIT_DIR`.
 *
 * @param repository Repository to open, zeroed. Must not be `NULL`.
 * @param path       Path the config gives.
 * @param name       Name of the repo in the config.
 * @param perm       Arena that owns the names. Must not be `NULL`.
 * @param log        Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when no repository is at `path`.
 */
int repository_open(struct Repository* repository,
                    struct Str path,
                    struct Str name,
                    struct Arena* perm,
                    struct Log* log) __attribute__((nonnull(1, 4, 5)));

/**
 * @brief Closes `repository`, if it is open.
 *
 * @param repository Repository to close. Must not be `NULL`.
 */
void repository_close(struct Repository* repository) __attribute__((nonnull(1)));

/**
 * @brief Reads the blob `oid` of `repository` into `perm`.
 *
 * @param repository    Open repository. Must not be `NULL`.
 * @param oid           Hex id of a blob.
 * @param perm          Arena that owns the bytes. Must not be `NULL`.
 * @param data_out      Receives the blob's bytes. Must not be `NULL`.
 * @param is_binary_out Receives whether `git_blob_is_binary` judges the blob binary. Must not be
 *                      `NULL`.
 * @param log           Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the blob cannot be read.
 */
int repository_read_blob(const struct Repository* repository,
                         struct Str oid,
                         struct Arena* perm,
                         struct Str* data_out,
                         bool* is_binary_out,
                         struct Log* log) __attribute__((nonnull(1, 3, 4, 5, 6)));

/**
 * @brief Records the failure of a libgit2 call as
 *        `<operation>: <libgit2 message> (in repo '<repo>')`.
 *
 * Every failed libgit2 call on an open repository is recorded here, so every one reads the same way
 * and keeps libgit2's own message. `repository_setup` and `repository_open`, which have no open
 * repository, relay libgit2's message in messages of their own.
 *
 * @param repository Repository the call was about. Must not be `NULL`.
 * @param operation  Operation leading the message, such as `failed to list refs`.
 * @param log        Log that receives the error. Must not be `NULL`.
 * @return `-1` always.
 */
int repository_fail(const struct Repository* repository, struct Str operation, struct Log* log)
    __attribute__((nonnull(1, 3)));

/**
 * @brief Returns an object id as hex text.
 *
 * @param oid   Object id. Must not be `NULL`.
 * @param arena Arena that owns the text. Must not be `NULL`.
 * @return The id in hex, which no `NUL` ends.
 */
struct Str repository_oid_text(const struct git_oid* oid, struct Arena* arena)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Returns the git blob id of `text`, which the build cache records for text it compares,
 *        such as `info/refs`.
 *
 * @param text  Text to hash.
 * @param arena Arena that owns the id. Must not be `NULL`.
 * @return The id in hex.
 */
struct Str repository_text_id(struct Str text, struct Arena* arena) __attribute__((nonnull(2)));

#endif
