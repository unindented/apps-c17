#ifndef SURGE_REVISION_H
#define SURGE_REVISION_H

#include <stddef.h>

#include "core/str.h"

struct Arena;
struct CommitDetail;
struct CommitSummary;
struct Log;
struct Ref;
struct Refset;
struct Repository;
struct TagNote;
struct Trailer;
struct TrailerMatch;

/**
 * @brief Resolves the default branch of a repository: `configured` when it is not empty, else the
 *        branch that `HEAD` names.
 *
 * A repository with no branch at all is empty, and its default branch is the one `HEAD` names,
 * which does not exist yet, so that its clone still names one. Otherwise a `HEAD` that is detached,
 * or names no branch, needs `configured`, and that is an error for the repository.
 *
 * @param repository Open repository. Must not be `NULL`.
 * @param refset     Refs loaded from `repository`. Must not be `NULL`.
 * @param configured Branch name the config gives, or empty.
 * @param key        Config key that gives it, such as `repo[0].default_branch`, which a failure
 *                   names.
 * @param perm       Arena that owns the name. Must not be `NULL`.
 * @param name_out   Receives the branch name below `refs/heads/`. Must not be `NULL`.
 * @param branch_out Receives the branch, or `NULL` for an empty repository. Must not be `NULL`.
 * @param log        Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when no default branch can be resolved.
 */
int revision_resolve_default_branch(const struct Repository* repository,
                                    const struct Refset* refset,
                                    struct Str configured,
                                    struct Str key,
                                    struct Arena* perm,
                                    struct Str* name_out,
                                    const struct Ref** branch_out,
                                    struct Log* log) __attribute__((nonnull(1, 2, 5, 6, 7, 8)));

/**
 * @brief Reads what the pages show of the commit `commit_oid`: its subject, its author's name, its
 *        committer date, its signature and the LLM tools its trailers name.
 *
 * @param repository   Open repository. Must not be `NULL`.
 * @param commit_oid   Hex id of a commit, which the summary's `oid` keeps.
 * @param llm_trailers Which trailers mark the commit LLM-assisted. Must not be `NULL`.
 * @param perm         Arena that owns the texts. Must not be `NULL`.
 * @param summary_out  Receives the summary. Must not be `NULL`.
 * @param log          Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the commit cannot be read.
 */
int revision_commit_summary(const struct Repository* repository,
                            struct Str commit_oid,
                            const struct TrailerMatch* llm_trailers,
                            struct Arena* perm,
                            struct CommitSummary* summary_out,
                            struct Log* log) __attribute__((nonnull(1, 3, 4, 5, 6)));

/**
 * @brief Reads what a commit page shows of the commit `commit_oid`: its summary, the rest of its
 *        message, its trailers, its author's date, its committer and its parents.
 *
 * @param repository   Open repository. Must not be `NULL`.
 * @param commit_oid   Hex id of a commit, which the summary's `oid` keeps.
 * @param llm_trailers Which trailers mark the commit LLM-assisted. Must not be `NULL`.
 * @param perm         Arena that owns the texts. Must not be `NULL`.
 * @param detail_out   Receives the commit. Must not be `NULL`.
 * @param log          Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the commit cannot be read.
 */
int revision_commit_detail(const struct Repository* repository,
                           struct Str commit_oid,
                           const struct TrailerMatch* llm_trailers,
                           struct Arena* perm,
                           struct CommitDetail* detail_out,
                           struct Log* log) __attribute__((nonnull(1, 3, 4, 5, 6)));

/**
 * @brief Parses the trailers of a commit message with git's rules, which `git_message_trailers`
 *        follows: the `Key: value` lines of its last paragraph, which cannot be its first.
 *
 * Each value joins its continuation lines with one space and drops the white space around it.
 *
 * @param message           The whole message, subject included.
 * @param perm              Arena that owns the trailers. Must not be `NULL`.
 * @param scratch           Arena for a terminated copy of the message.
 * @param trailers_out      Receives the trailers, in message order. Must not be `NULL`.
 * @param trailer_count_out Receives the number of trailers. Must not be `NULL`.
 */
void revision_message_trailers(struct Str message,
                               struct Arena* perm,
                               struct Arena scratch,
                               struct Trailer** trailers_out,
                               ptrdiff_t* trailer_count_out) __attribute__((nonnull(2, 4, 5)));

/**
 * @brief Reads what a release page and the tags feed show of the tag `tag`: an annotated tag's
 *        message and its tagger's date, or for a lightweight tag the date of its commit.
 *
 * A tag of a tag keeps the message and the date of the outer tag, which the ref names.
 *
 * @param repository Open repository. Must not be `NULL`.
 * @param tag        A tag of the repository. Must not be `NULL`.
 * @param perm       Arena that owns the texts. Must not be `NULL`.
 * @param note_out   Receives the note. Must not be `NULL`.
 * @param log        Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the tag or its commit cannot be read.
 */
int revision_tag_note(const struct Repository* repository,
                      const struct Ref* tag,
                      struct Arena* perm,
                      struct TagNote* note_out,
                      struct Log* log) __attribute__((nonnull(1, 2, 3, 4, 5)));

/**
 * @brief Lists the commits that `branch` reaches, newest first in `git log --date-order` order: by
 *        committer time, with no commit before one of its children.
 *
 * @param repository  Open repository. Must not be `NULL`.
 * @param branch      Branch whose log it is, which names a commit. Must not be `NULL`.
 * @param perm        Arena that owns the ids. Must not be `NULL`.
 * @param commits_out Receives the hex ids. Must not be `NULL`.
 * @param count_out   Receives the number of commits. Must not be `NULL`.
 * @param log         Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the history cannot be walked.
 */
int revision_walk_log(const struct Repository* repository,
                      const struct Ref* branch,
                      struct Arena* perm,
                      struct Str** commits_out,
                      ptrdiff_t* count_out,
                      struct Log* log) __attribute__((nonnull(1, 2, 3, 4, 5, 6)));

/**
 * @brief Lists every commit that a ref of `refset` reaches, each once, sorted bytewise.
 *
 * @param repository  Open repository. Must not be `NULL`.
 * @param refset      Refs loaded from `repository`. Must not be `NULL`.
 * @param perm        Arena that owns the ids. Must not be `NULL`.
 * @param commits_out Receives the hex ids. Must not be `NULL`.
 * @param count_out   Receives the number of commits. Must not be `NULL`.
 * @param log         Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the history cannot be walked.
 */
int revision_list_commits(const struct Repository* repository,
                          const struct Refset* refset,
                          struct Arena* perm,
                          struct Str** commits_out,
                          ptrdiff_t* count_out,
                          struct Log* log) __attribute__((nonnull(1, 2, 3, 4, 5, 6)));

#endif
