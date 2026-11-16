#include "vcs/revision.h"

#include <git2.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "core/arena.h"
#include "core/ascii.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "domain/timestamp.h"
#include "domain/trailer.h"
#include "vcs/refset.h"
#include "vcs/repository.h"

#define REVISION_BRANCH_PREFIX S("refs/heads/")

/**
 * @brief Looks the commit `commit_oid` up.
 *
 * @param repository Open repository. Must not be `NULL`.
 * @param commit_oid Hex id of a commit.
 * @param commit_out Receives the commit, which the caller frees. Must not be `NULL`.
 * @param log        Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the commit cannot be read.
 */
static int lookup_commit(const struct Repository* repository,
                         struct Str commit_oid,
                         git_commit** commit_out,
                         struct Log* log) __attribute__((nonnull(1, 3, 4)));

/**
 * @brief Reads what the pages show of a commit: its subject, its author's name, its date, its
 *        signature and the LLM tools its trailers name.
 *
 * @param repository    Open repository, which names a failure. Must not be `NULL`.
 * @param commit        The commit, which `lookup_commit` found. It carries no `nonnull`, which the
 *                      analyzer cannot prove through `lookup_commit`'s out parameter.
 * @param commit_oid    Its hex id, which the summary keeps.
 * @param trailers      Its trailers, from `read_trailers`. Must not be `NULL` unless
 *                      `trailer_count` is `0`.
 * @param trailer_count Number of entries in `trailers`.
 * @param llm_trailers  Which trailers mark the commit LLM-assisted. Must not be `NULL`.
 * @param perm          Arena that owns the texts. Must not be `NULL`.
 * @param summary_out   Receives the summary. Must not be `NULL`.
 * @param log           Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the signature header cannot be read.
 */
static int summarize(const struct Repository* repository,
                     const git_commit* commit,
                     struct Str commit_oid,
                     const struct Trailer* trailers,
                     ptrdiff_t trailer_count,
                     const struct TrailerMatch* llm_trailers,
                     struct Arena* perm,
                     struct CommitSummary* summary_out,
                     struct Log* log) __attribute__((nonnull(1, 6, 7, 8, 9)));

/**
 * @brief Sets a tag note's date to the committer date of the commit `commit_oid`.
 *
 * @param note       Note that receives `date` and `time`. Must not be `NULL`.
 * @param repository Open repository. Must not be `NULL`.
 * @param commit_oid Hex id of a commit.
 * @param perm       Arena that owns the date. Must not be `NULL`.
 * @param log        Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the commit cannot be read.
 */
static int read_commit_date(struct TagNote* note,
                            const struct Repository* repository,
                            struct Str commit_oid,
                            struct Arena* perm,
                            struct Log* log) __attribute__((nonnull(1, 2, 4, 5)));

/**
 * @brief Reads the trailers of a terminated commit message, as `revision_message_trailers` does.
 *
 * @param message           The whole message, terminated. Must not be `NULL`.
 * @param perm              Arena that owns the trailers. Must not be `NULL`.
 * @param trailers_out      Receives the trailers. Must not be `NULL`.
 * @param trailer_count_out Receives the number of trailers. Must not be `NULL`.
 */
static void read_trailers(const char* message,
                          struct Arena* perm,
                          struct Trailer** trailers_out,
                          ptrdiff_t* trailer_count_out) __attribute__((nonnull(1, 2, 3, 4)));

/**
 * @brief Returns a trailer value as libgit2 gives it with each continuation line joined to the line
 *        before with one space, and the white space at its end dropped.
 *
 * @param value The value, whose continuation lines start after a newline.
 * @param arena Arena that owns the result. Must not be `NULL`.
 * @return The value.
 */
static struct Str unfold_value(struct Str value, struct Arena* arena) __attribute__((nonnull(2)));

/**
 * @brief Returns where a tag's message ends and its signature starts, as git finds it: at the start
 *        of the last line that opens a PGP, X.509 or SSH signature, or at the end of the message
 *        when no line does.
 *
 * @param message The tag's whole message.
 * @return The length of the message without its signature.
 */
static ptrdiff_t find_signature(struct Str message);

/**
 * @brief Walks the history from every commit in `starts` into a list of hex ids, in walk order.
 *
 * @param repository  Open repository. Must not be `NULL`.
 * @param starts      Hex ids of the commits to start from, or empty strings to skip. Must not be
 *                    `NULL` unless `start_count` is `0`.
 * @param start_count Number of entries in `starts`.
 * @param perm        Arena that owns the ids. Must not be `NULL`.
 * @param commits_out Receives the hex ids. Must not be `NULL`.
 * @param count_out   Receives the number of commits. Must not be `NULL`.
 * @param operation   Operation leading the failure message, which names the walk.
 * @param log         Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the history cannot be walked.
 */
static ptrdiff_t find_signature(struct Str message) {
  static const struct Str openings[] = {
      STR_INIT("-----BEGIN PGP SIGNATURE-----"),
      STR_INIT("-----BEGIN PGP MESSAGE-----"),
      STR_INIT("-----BEGIN SIGNED MESSAGE-----"),
      STR_INIT("-----BEGIN SSH SIGNATURE-----"),
  };
  ptrdiff_t found = message.len;
  struct Str rest = message;
  while (rest.len > 0) {
    for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(openings) / sizeof(openings[0])); i++) {
      if (str_startswith(rest, openings[i])) {
        found = rest.data - message.data;
      }
    }
    rest = str_cut(rest, '\n').tail;
  }
  return found;
}

static int walk_history(const struct Repository* repository,
                        const struct Str* starts,
                        ptrdiff_t start_count,
                        struct Arena* perm,
                        struct Str** commits_out,
                        ptrdiff_t* count_out,
                        struct Str operation,
                        struct Log* log) __attribute__((nonnull(1, 4, 5, 6, 8)));

/**
 * @brief Orders two hex ids bytewise, for `qsort`.
 *
 * @param a First `struct Str`. Must not be `NULL`.
 * @param b Second `struct Str`. Must not be `NULL`.
 * @return Negative, zero or positive as `a` sorts before, with, or after `b`.
 */
static int compare_oids(const void* a, const void* b) __attribute__((nonnull(1, 2)));

int revision_resolve_default_branch(const struct Repository* repository,
                                    const struct Refset* refset,
                                    struct Str configured,
                                    struct Str key,
                                    struct Arena* perm,
                                    struct Str* name_out,
                                    const struct Ref** branch_out,
                                    struct Log* log) {
  if (configured.len > 0) {
    const struct Ref* branch = refset_find_branch(refset, configured);
    if (branch == NULL) {
      return log_error(log,
                       "config key '%.*s' names no branch of the repo: '%.*s' (in repo '%.*s')",
                       STR_FMT(key), STR_FMT(configured), STR_FMT(repository->name));
    }
    *name_out = branch->short_name;
    *branch_out = branch;
    return 0;
  }
  git_reference* head = NULL;
  if (git_reference_lookup(&head, repository->git, "HEAD") != 0) {
    return repository_fail(repository, S("failed to read HEAD"), log);
  }
  struct Str target = {0};
  if (git_reference_type(head) == GIT_REFERENCE_SYMBOLIC) {
    target = str_clone(str_from_cstr(git_reference_symbolic_target(head)), perm);
  }
  git_reference_free(head);
  if (!str_startswith(target, REVISION_BRANCH_PREFIX)) {
    return log_error(log,
                     "HEAD names no branch, so the repo needs config key '%.*s' (in repo '%.*s')",
                     STR_FMT(key), STR_FMT(repository->name));
  }
  const struct Str name = str_tail(target, REVISION_BRANCH_PREFIX.len);
  const struct Ref* branch = refset_find_branch(refset, name);
  bool has_branch = false;
  for (ptrdiff_t i = 0; i < refset->count; i++) {
    has_branch = has_branch || refset->refs[i].kind == REF_KIND_BRANCH;
  }
  if (branch == NULL && has_branch) {
    return log_error(log,
                     "HEAD names a branch that does not exist, so the repo needs config key "
                     "'%.*s': '%.*s' (in repo '%.*s')",
                     STR_FMT(key), STR_FMT(name), STR_FMT(repository->name));
  }
  *name_out = name;
  *branch_out = branch;
  return 0;
}

int revision_commit_summary(const struct Repository* repository,
                            struct Str commit_oid,
                            const struct TrailerMatch* llm_trailers,
                            struct Arena* perm,
                            struct CommitSummary* summary_out,
                            struct Log* log) {
  git_commit* commit = NULL;
  if (lookup_commit(repository, commit_oid, &commit, log) != 0) {
    return -1;
  }
  const char* message = git_commit_message(commit);
  struct Trailer* trailers = NULL;
  ptrdiff_t trailer_count = 0;
  read_trailers(message != NULL ? message : "", perm, &trailers, &trailer_count);
  const int rc = summarize(repository, commit, commit_oid, trailers, trailer_count, llm_trailers,
                           perm, summary_out, log);
  git_commit_free(commit);
  return rc;
}

int revision_commit_detail(const struct Repository* repository,
                           struct Str commit_oid,
                           const struct TrailerMatch* llm_trailers,
                           struct Arena* perm,
                           struct CommitDetail* detail_out,
                           struct Log* log) {
  *detail_out = (struct CommitDetail){0};
  git_commit* commit = NULL;
  if (lookup_commit(repository, commit_oid, &commit, log) != 0) {
    return -1;
  }
  const char* message = git_commit_message(commit);
  read_trailers(message != NULL ? message : "", perm, &detail_out->trailers,
                &detail_out->trailer_count);
  if (summarize(repository, commit, commit_oid, detail_out->trailers, detail_out->trailer_count,
                llm_trailers, perm, &detail_out->summary, log) != 0) {
    git_commit_free(commit);
    return -1;
  }
  struct Str body = str_cut(str_from_cstr(message != NULL ? message : ""), '\n').tail;
  while (body.len > 0 && (body.data[0] == '\n' || body.data[0] == '\r')) {
    body = str_tail(body, 1);
  }
  while (body.len > 0 && (body.data[body.len - 1] == '\n' || body.data[body.len - 1] == '\r')) {
    body = str_head(body, body.len - 1);
  }
  const git_signature* author = git_commit_author(commit);
  const git_signature* committer = git_commit_committer(commit);
  detail_out->body = str_clone(body, perm);
  detail_out->author_date =
      author != NULL ? timestamp_format_rfc3339(author->when.time, author->when.offset, perm)
                     : (struct Str){0};
  detail_out->committer = str_clone(str_from_cstr(committer != NULL ? committer->name : ""), perm);
  const unsigned int parent_count = git_commit_parentcount(commit);
  detail_out->parents = arena_new(perm, (ptrdiff_t)parent_count, struct Str);
  for (unsigned int i = 0; i < parent_count; i++) {
    detail_out->parents[detail_out->parent_count++] =
        repository_oid_text(git_commit_parent_id(commit, i), perm);
  }
  git_commit_free(commit);
  return 0;
}

void revision_message_trailers(struct Str message,
                               struct Arena* perm,
                               struct Arena scratch,
                               struct Trailer** trailers_out,
                               ptrdiff_t* trailer_count_out) {
  char* text = arena_new(&scratch, message.len + 1, char);
  if (message.len > 0) {
    memcpy(text, message.data, (size_t)message.len);
  }
  text[message.len] = '\0';
  read_trailers(text, perm, trailers_out, trailer_count_out);
}

int revision_tag_note(const struct Repository* repository,
                      const struct Ref* tag,
                      struct Arena* perm,
                      struct TagNote* note_out,
                      struct Log* log) {
  *note_out = (struct TagNote){0};
  // A ref with a peeled id points at a tag object, and one without straight at its target.
  if (tag->peeled_oid.len == 0) {
    return tag->commit_oid.len > 0
               ? read_commit_date(note_out, repository, tag->commit_oid, perm, log)
               : 0;
  }
  git_oid oid;
  git_tag* tag_object = NULL;
  if (git_oid_fromstrn(&oid, tag->oid.data, (size_t)tag->oid.len) != 0 ||
      git_tag_lookup(&tag_object, repository->git, &oid) != 0) {
    struct Buf operation = {.arena = perm};
    buf_printf(&operation, "failed to read tag '%.*s'", STR_FMT(tag->short_name));
    return repository_fail(repository, buf_str(&operation), log);
  }
  const char* message_text = git_tag_message(tag_object);
  const struct Str message = str_from_cstr(message_text != NULL ? message_text : "");
  const ptrdiff_t message_len = find_signature(message);
  note_out->message = str_clone(str_head(message, message_len), perm);
  note_out->signature = str_clone(str_tail(message, message_len), perm);
  const git_signature* tagger = git_tag_tagger(tag_object);
  if (tagger != NULL) {
    note_out->date = timestamp_format_rfc3339(tagger->when.time, tagger->when.offset, perm);
    note_out->time = tagger->when.time;
    note_out->tagger = str_clone(str_from_cstr(tagger->name != NULL ? tagger->name : ""), perm);
  }
  git_tag_free(tag_object);
  if (tagger == NULL && tag->commit_oid.len > 0) {
    return read_commit_date(note_out, repository, tag->commit_oid, perm, log);
  }
  return 0;
}

int revision_walk_log(const struct Repository* repository,
                      const struct Ref* branch,
                      struct Arena* perm,
                      struct Str** commits_out,
                      ptrdiff_t* count_out,
                      struct Log* log) {
  struct Buf operation = {.arena = perm};
  buf_printf(&operation, "failed to walk history of branch '%.*s'", STR_FMT(branch->short_name));
  return walk_history(repository, &branch->commit_oid, 1, perm, commits_out, count_out,
                      buf_str(&operation), log);
}

int revision_list_commits(const struct Repository* repository,
                          const struct Refset* refset,
                          struct Arena* perm,
                          struct Str** commits_out,
                          ptrdiff_t* count_out,
                          struct Log* log) {
  struct Str* starts = arena_new(perm, refset->count, struct Str);
  for (ptrdiff_t i = 0; i < refset->count; i++) {
    starts[i] = refset->refs[i].commit_oid;
  }
  if (walk_history(repository, starts, refset->count, perm, commits_out, count_out,
                   S("failed to list commits of refs"), log) != 0) {
    return -1;
  }
  if (*count_out > 0) {
    qsort(*commits_out, (size_t)*count_out, sizeof(**commits_out), compare_oids);
  }
  return 0;
}

static int lookup_commit(const struct Repository* repository,
                         struct Str commit_oid,
                         git_commit** commit_out,
                         struct Log* log) {
  git_oid oid;
  if (git_oid_fromstrn(&oid, commit_oid.data, (size_t)commit_oid.len) != 0 ||
      git_commit_lookup(commit_out, repository->git, &oid) != 0) {
    return repository_fail(repository, S("failed to read commit"), log);
  }
  return 0;
}

static int summarize(const struct Repository* repository,
                     const git_commit* commit,
                     struct Str commit_oid,
                     const struct Trailer* trailers,
                     ptrdiff_t trailer_count,
                     const struct TrailerMatch* llm_trailers,
                     struct Arena* perm,
                     struct CommitSummary* summary_out,
                     struct Log* log) {
  const char* message = git_commit_message(commit);
  struct Str subject = str_cut(str_from_cstr(message != NULL ? message : ""), '\n').head;
  if (str_endswith(subject, S("\r"))) {
    subject = str_head(subject, subject.len - 1);
  }
  const git_signature* author = git_commit_author(commit);
  *summary_out = (struct CommitSummary){
      .oid = commit_oid,
      .subject = str_clone(subject, perm),
      .author = str_clone(str_from_cstr(author != NULL ? author->name : ""), perm),
      .date =
          timestamp_format_rfc3339(git_commit_time(commit), git_commit_time_offset(commit), perm),
  };
  trailer_llm_tools(trailers, trailer_count, llm_trailers, perm, &summary_out->llm_tools,
                    &summary_out->llm_tool_count);
  // libgit2 joins the header's continuation lines without their leading space. A SHA-1 commit keeps
  // its signature in `gpgsig`, whichever kind it is.
  git_buf signature = GIT_BUF_INIT;
  const int error = git_commit_header_field(&signature, commit, "gpgsig");
  if (error == 0) {
    summary_out->signature =
        str_clone((struct Str){signature.ptr, (ptrdiff_t)signature.size}, perm);
  }
  git_buf_dispose(&signature);
  if (error != 0 && error != GIT_ENOTFOUND) {
    return repository_fail(repository, S("failed to read commit signature"), log);
  }
  return 0;
}

static void read_trailers(const char* message,
                          struct Arena* perm,
                          struct Trailer** trailers_out,
                          ptrdiff_t* trailer_count_out) {
  git_message_trailer_array parsed = {0};
  // libgit2 fails here only when an allocation fails, and `repository_setup` routes that to
  // `os_oom`.
  if (git_message_trailers(&parsed, message) != 0) {
    os_oom();
  }
  struct Trailer* trailers = arena_new(perm, (ptrdiff_t)parsed.count, struct Trailer);
  for (size_t i = 0; i < parsed.count; i++) {
    trailers[i].key = str_clone(str_from_cstr(parsed.trailers[i].key), perm);
    trailers[i].value = unfold_value(str_from_cstr(parsed.trailers[i].value), perm);
  }
  *trailers_out = trailers;
  *trailer_count_out = (ptrdiff_t)parsed.count;
  git_message_trailer_array_free(&parsed);
}

static struct Str unfold_value(struct Str value, struct Arena* arena) {
  struct Buf text = {.arena = arena};
  for (ptrdiff_t i = 0; i < value.len; i++) {
    const char c = value.data[i];
    if (c != '\n') {
      buf_append_char(&text, c);
      continue;
    }
    while (text.len > 0 && ascii_is_space((unsigned char)text.data[text.len - 1])) {
      text.len--;
    }
    while (i + 1 < value.len && ascii_is_space((unsigned char)value.data[i + 1])) {
      i++;
    }
    buf_append_char(&text, ' ');
  }
  while (text.len > 0 && ascii_is_space((unsigned char)text.data[text.len - 1])) {
    text.len--;
  }
  return buf_str(&text);
}

static int read_commit_date(struct TagNote* note,
                            const struct Repository* repository,
                            struct Str commit_oid,
                            struct Arena* perm,
                            struct Log* log) {
  git_commit* commit = NULL;
  if (lookup_commit(repository, commit_oid, &commit, log) != 0) {
    return -1;
  }
  note->date =
      timestamp_format_rfc3339(git_commit_time(commit), git_commit_time_offset(commit), perm);
  note->time = git_commit_time(commit);
  git_commit_free(commit);
  return 0;
}

static int walk_history(const struct Repository* repository,
                        const struct Str* starts,
                        ptrdiff_t start_count,
                        struct Arena* perm,
                        struct Str** commits_out,
                        ptrdiff_t* count_out,
                        struct Str operation,
                        struct Log* log) {
  *commits_out = NULL;
  *count_out = 0;
  git_revwalk* walk = NULL;
  if (git_revwalk_new(&walk, repository->git) != 0 ||
      git_revwalk_sorting(walk, GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME) != 0) {
    git_revwalk_free(walk);
    return repository_fail(repository, operation, log);
  }
  int rc = 0;
  for (ptrdiff_t i = 0; rc == 0 && i < start_count; i++) {
    git_oid oid;
    if (starts[i].len > 0 && (git_oid_fromstrn(&oid, starts[i].data, (size_t)starts[i].len) != 0 ||
                              git_revwalk_push(walk, &oid) != 0)) {
      rc = repository_fail(repository, operation, log);
    }
  }
  // The binary ids are the only allocation while the walk runs, so their array grows in place, and
  // their text goes past them once the walk is done.
  git_oid* oids = NULL;
  ptrdiff_t count = 0;
  ptrdiff_t capacity = 0;
  git_oid oid = {0};
  int next = rc == 0 ? git_revwalk_next(&oid, walk) : GIT_ITEROVER;
  while (next == 0) {
    if (count == capacity) {
      oids = arena_grow(perm, oids, &capacity, (ptrdiff_t)sizeof(git_oid),
                        (ptrdiff_t)_Alignof(git_oid));
    }
    oids[count++] = oid;
    next = git_revwalk_next(&oid, walk);
  }
  git_revwalk_free(walk);
  if (rc != 0) {
    return -1;
  }
  if (next != GIT_ITEROVER) {
    return repository_fail(repository, operation, log);
  }
  struct Str* commits = arena_new(perm, count, struct Str);
  for (ptrdiff_t i = 0; i < count; i++) {
    commits[i] = repository_oid_text(&oids[i], perm);
  }
  *commits_out = commits;
  *count_out = count;
  return 0;
}

static int compare_oids(const void* a, const void* b) {
  return str_compare(*(const struct Str*)a, *(const struct Str*)b);
}
