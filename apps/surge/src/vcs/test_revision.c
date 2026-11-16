#include <acutest.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "core/arena.h"
#include "core/log.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "domain/trailer.h"
#include "testing/test_support.h"
#include "vcs/refset.h"
#include "vcs/repository.h"
#include "vcs/revision.h"
#include "vcs/test_vcs_support.h"

/**
 * @brief Orders two C strings bytewise, for `qsort`.
 *
 * @param a First `const char*`. Must not be `NULL`.
 * @param b Second `const char*`. Must not be `NULL`.
 * @return Negative, zero or positive as `a` sorts before, with, or after `b`.
 */
static int compare_cstrings(const void* a, const void* b) {
  return strcmp(*(const char* const*)a, *(const char* const*)b);
}

/** A match that marks no commit LLM-assisted, for the tests whose subject is elsewhere. */
static const struct TrailerMatch NO_LLM_TRAILERS = {0};

/** Config key the tests give for the default branch, which a failure names. */
#define KEY S("repo[0].default_branch")

/**
 * @brief Opens a fixture and loads its refs.
 *
 * @param repository Repository to open, zeroed. Must not be `NULL`.
 * @param refset     Refset that receives the refs, zeroed. Must not be `NULL`.
 * @param name       Terminated name of the fixture. Must not be `NULL`.
 * @param arena      Arena that owns the refs. Must not be `NULL`.
 * @return `0` when both succeeded, or `-1` otherwise.
 */
static int open_with_refs(struct Repository* repository,
                          struct Refset* refset,
                          const char* name,
                          struct Arena* arena) {
  struct Log log = test_log();
  return open_fixture(repository, name, arena) == 0 &&
                 refset_load(refset, repository, arena, &log) == 0
             ? 0
             : -1;
}

/**
 * @brief Finds the tag `short_name` of `refset`.
 *
 * @param refset     Refs to search. Must not be `NULL`.
 * @param short_name Name of the tag below `refs/tags/`.
 * @return The tag, or `NULL` when `refset` has none of that name.
 */
static const struct Ref* find_tag(const struct Refset* refset, struct Str short_name) {
  for (ptrdiff_t i = 0; i < refset->count; i++) {
    const struct Ref* ref = &refset->refs[i];
    if (ref->kind == REF_KIND_TAG && str_eq(ref->short_name, short_name)) {
      return ref;
    }
  }
  return NULL;
}

/**
 * @brief Parses the trailers of `message` with `revision_message_trailers`.
 *
 * @param message   Terminated message. Must not be `NULL`.
 * @param arena     Arena that owns the trailers. Must not be `NULL`.
 * @param count_out Receives the number of trailers. Must not be `NULL`.
 * @return The trailers.
 */
static struct Trailer* parse_trailers(const char* message,
                                      struct Arena* arena,
                                      ptrdiff_t* count_out) {
  struct Log log = test_log();
  TEST_ASSERT(repository_setup(&log) == 0);
  struct Trailer* trailers = NULL;
  revision_message_trailers(str_from_cstr(message), arena, test_arena(), &trailers, count_out);
  return trailers;
}

// Without a configured branch, the default branch is the one `HEAD` names.
static void test_resolve_uses_head(void) {
  struct Arena arena = test_arena();
  struct Repository repository = {0};
  struct Refset refset = {0};
  TEST_ASSERT(open_with_refs(&repository, &refset, "showcase", &arena) == 0);
  struct Str name = {0};
  const struct Ref* branch = NULL;
  struct Log log = test_log();
  TEST_CHECK(revision_resolve_default_branch(&repository, &refset, (struct Str){0}, KEY, &arena,
                                             &name, &branch, &log) == 0);
  TEST_CHECK(str_eq(name, S("main")));
  TEST_CHECK(branch == refset_find_branch(&refset, S("main")));
  repository_close(&repository);
}

// A configured branch wins over `HEAD`.
static void test_resolve_uses_configured_branch(void) {
  struct Arena arena = test_arena();
  struct Repository repository = {0};
  struct Refset refset = {0};
  TEST_ASSERT(open_with_refs(&repository, &refset, "showcase", &arena) == 0);
  struct Str name = {0};
  const struct Ref* branch = NULL;
  struct Log log = test_log();
  TEST_CHECK(revision_resolve_default_branch(&repository, &refset, S("feature/x"), KEY, &arena,
                                             &name, &branch, &log) == 0);
  TEST_CHECK(str_eq(name, S("feature/x")) && branch != NULL);
  repository_close(&repository);
}

// An empty repository's default branch is the one `HEAD` names, which does not exist yet.
static void test_resolve_names_the_unborn_branch_of_an_empty_repository(void) {
  struct Arena arena = test_arena();
  struct Repository repository = {0};
  struct Refset refset = {0};
  TEST_ASSERT(open_with_refs(&repository, &refset, "empty", &arena) == 0);
  struct Str name = {0};
  const struct Ref* branch = NULL;
  struct Log log = test_log();
  TEST_CHECK(revision_resolve_default_branch(&repository, &refset, (struct Str){0}, KEY, &arena,
                                             &name, &branch, &log) == 0);
  TEST_CHECK(str_eq(name, S("main")) && branch == NULL);
  repository_close(&repository);
}

// A configured branch the repository does not have is an error that names it.
static void test_resolve_rejects_a_missing_configured_branch(void) {
  struct Arena arena = test_arena();
  struct Repository repository = {0};
  struct Refset refset = {0};
  TEST_ASSERT(open_with_refs(&repository, &refset, "showcase", &arena) == 0);
  struct Str name = {0};
  const struct Ref* branch = NULL;
  struct Log log = test_log();
  TEST_CHECK(revision_resolve_default_branch(&repository, &refset, S("trunk"), KEY, &arena, &name,
                                             &branch, &log) == -1);
  TEST_CHECK(
      test_log_is(&log,
                  "config key 'repo[0].default_branch' names no branch of the repo: 'trunk' (in "
                  "repo 'showcase')\n"));
  repository_close(&repository);
}

// A commit's summary is its subject, its author's name and its committer date.
static void test_commit_summary_reads_subject_author_and_date(void) {
  struct Arena arena = test_arena();
  struct Repository repository = {0};
  struct Refset refset = {0};
  TEST_ASSERT(open_with_refs(&repository, &refset, "showcase", &arena) == 0);
  const struct Ref* main_branch = refset_find_branch(&refset, S("main"));
  TEST_ASSERT(main_branch != NULL);
  struct CommitSummary summary = {0};
  struct Log log = test_log();
  TEST_CHECK(revision_commit_summary(&repository, main_branch->commit_oid, &NO_LLM_TRAILERS, &arena,
                                     &summary, &log) == 0);
  TEST_CHECK(str_eq(summary.oid, main_branch->commit_oid));
  TEST_CHECK(test_text_is(summary.subject, "Sign off the greeting"));
  TEST_CHECK(test_text_is(summary.author, "Ada Lovelace"));
  TEST_CHECK(test_text_is(summary.date, "2026-01-15T12:00:00+01:00"));
  repository_close(&repository);
}

// A commit's detail keeps its author's date apart from its committer's, names the committer, and
// holds the body after the subject and its parents in order.
static void test_commit_detail_reads_the_committer_body_and_parents(void) {
  struct Arena arena = test_arena();
  struct Repository repository = {0};
  struct Refset refset = {0};
  TEST_ASSERT(open_with_refs(&repository, &refset, "hostile", &arena) == 0);
  const struct Ref* main_branch = refset_find_branch(&refset, S("main"));
  TEST_ASSERT(main_branch != NULL);
  struct CommitDetail detail = {0};
  struct Log log = test_log();
  TEST_CHECK(revision_commit_detail(&repository, main_branch->commit_oid, &NO_LLM_TRAILERS, &arena,
                                    &detail, &log) == 0);
  TEST_CHECK(test_text_is(detail.summary.author, "Ada Lovelace"));
  TEST_CHECK(test_text_is(detail.author_date, "2026-01-09T12:00:00+01:00"));
  TEST_CHECK(test_text_is(detail.committer, "Grace Hopper"));
  TEST_CHECK(test_text_is(detail.summary.date, "2026-01-09T23:00:00Z"));
  TEST_CHECK(test_text_is(detail.body, "Each of them is a blob next to the stream's entries."));
  TEST_ASSERT(detail.parent_count == 1);
  TEST_CHECK(test_text_is(detail.parents[0], "c2914601eb8581519ca31d8a4672eda4d420f5e1"));
  repository_close(&repository);
}

// A commit's summary lists the LLM tools its trailers name, for the match it is given.
static void test_commit_summary_reads_the_llm_tools(void) {
  struct Arena arena = test_arena();
  struct Repository repository = {0};
  TEST_ASSERT(open_fixture(&repository, "showcase", &arena) == 0);
  const struct Str keys[] = {S("Co-authored-by"), S("Assisted-by")};
  const struct Str values[] = {S("claude")};
  const struct TrailerMatch match = {
      .keys = keys, .key_count = 2, .values = values, .value_count = 1};
  struct CommitSummary summary = {0};
  struct Log log = test_log();
  TEST_CHECK(revision_commit_summary(&repository, S("7040256583a8bd2313ea4c99c9c5a3c460627b56"),
                                     &match, &arena, &summary, &log) == 0);
  TEST_ASSERT(summary.llm_tool_count == 1);
  TEST_CHECK(test_text_is(summary.llm_tools[0], "Claude Opus 5.5"));
  TEST_CHECK(revision_commit_summary(&repository, S("0f9370311ea61a7719d764e70e44635013f92283"),
                                     &match, &arena, &summary, &log) == 0);
  TEST_ASSERT(summary.llm_tool_count == 1);
  TEST_CHECK(test_text_is(summary.llm_tools[0], "Claude:claude-opus-5-5 clang-format"));
  TEST_CHECK(revision_commit_summary(&repository, S("41fbd903fd4c50ffac2e0946a938d07f8c8e1457"),
                                     &match, &arena, &summary, &log) == 0);
  TEST_CHECK(summary.llm_tool_count == 0);
  TEST_CHECK(test_log_is_empty(&log));
  repository_close(&repository);
}

// A commit's detail holds the trailers of its message, keys as written and values with their
// addresses, while its body keeps them too.
static void test_commit_detail_reads_the_trailers(void) {
  struct Arena arena = test_arena();
  struct Repository repository = {0};
  struct Refset refset = {0};
  TEST_ASSERT(open_with_refs(&repository, &refset, "showcase", &arena) == 0);
  const struct Ref* main_branch = refset_find_branch(&refset, S("main"));
  TEST_ASSERT(main_branch != NULL);
  struct CommitDetail detail = {0};
  struct Log log = test_log();
  TEST_CHECK(revision_commit_detail(&repository, main_branch->commit_oid, &NO_LLM_TRAILERS, &arena,
                                    &detail, &log) == 0);
  TEST_CHECK(test_text_is(detail.body,
                          "Co-authored-by: Grace Hopper <grace@example.org>\n"
                          "Signed-off-by: Ada Lovelace <ada@example.org>"));
  TEST_ASSERT(detail.trailer_count == 2);
  TEST_CHECK(test_text_is(detail.trailers[0].key, "Co-authored-by"));
  TEST_CHECK(test_text_is(detail.trailers[0].value, "Grace Hopper <grace@example.org>"));
  TEST_CHECK(test_text_is(detail.trailers[1].key, "Signed-off-by"));
  TEST_CHECK(test_text_is(detail.trailers[1].value, "Ada Lovelace <ada@example.org>"));
  TEST_CHECK(test_log_is_empty(&log));
  repository_close(&repository);
}

// An annotated tag's note is its message, its tagger and the tagger's date and time, and a
// lightweight tag's the date and time of its commit, with no message or tagger.
static void test_tag_note_reads_the_message_and_the_date(void) {
  struct Arena arena = test_arena();
  struct Repository repository = {0};
  struct Refset refset = {0};
  TEST_ASSERT(open_with_refs(&repository, &refset, "monorepo", &arena) == 0);
  const struct Ref* annotated = find_tag(&refset, S("alpha-v0.1.0"));
  const struct Ref* lightweight = find_tag(&refset, S("alpha-v0.10.0-rc.1"));
  TEST_ASSERT(annotated != NULL && lightweight != NULL);
  struct TagNote note = {0};
  struct Log log = test_log();
  TEST_CHECK(revision_tag_note(&repository, annotated, &arena, &note, &log) == 0);
  TEST_CHECK(test_text_is(note.message, "alpha 0.1.0\n\nThe first release.\n"));
  TEST_CHECK(test_text_is(note.date, "2026-01-01T13:00:00+01:00"));
  TEST_CHECK(note.time == 1767268800);
  TEST_CHECK(test_text_is(note.tagger, "Ada Lovelace"));
  TEST_CHECK(revision_tag_note(&repository, lightweight, &arena, &note, &log) == 0);
  TEST_CHECK(note.message.len == 0);
  TEST_CHECK(test_text_is(note.date, "2026-01-02T12:00:00+01:00"));
  TEST_CHECK(note.time == 1767351600);
  TEST_CHECK(note.tagger.len == 0);
  TEST_CHECK(test_log_is_empty(&log));
  repository_close(&repository);
}

// A signed tag's message is cut before the line that opens its signature, which the note keeps
// apart, and an unsigned tag's note has no signature.
static void test_tag_note_keeps_the_signature_apart(void) {
  struct Arena arena = test_arena();
  struct Repository repository = {0};
  struct Refset refset = {0};
  TEST_ASSERT(open_with_refs(&repository, &refset, "monorepo", &arena) == 0);
  const struct Ref* signed_tag = find_tag(&refset, S("alpha-v0.2.0"));
  const struct Ref* unsigned_tag = find_tag(&refset, S("alpha-v0.1.0"));
  TEST_ASSERT(signed_tag != NULL && unsigned_tag != NULL);
  struct TagNote note = {0};
  struct Log log = test_log();
  TEST_CHECK(revision_tag_note(&repository, signed_tag, &arena, &note, &log) == 0);
  TEST_CHECK(test_text_is(note.message, "alpha 0.2.0\n\nNow returns `2`.\n"));
  TEST_CHECK(test_text_is(note.signature,
                          "-----BEGIN PGP SIGNATURE-----\n"
                          "\n"
                          "iHUEABYKAB0WIQQ+c3VyZ2UgZml4dHVyZSBzaWduYXR1cmUFAmlXdkAACgkQc3Vy\n"
                          "Z2UgZml4dHVyZQ==\n"
                          "=c3Vy\n"
                          "-----END PGP SIGNATURE-----\n"));
  TEST_CHECK(revision_tag_note(&repository, unsigned_tag, &arena, &note, &log) == 0);
  TEST_CHECK(note.signature.len == 0);
  TEST_CHECK(test_log_is_empty(&log));
  repository_close(&repository);
}

/** The showcase fixture's commits on `main`, in `git log --date-order` order. */
static const char* const SHOWCASE_MAIN_LOG[] = {
    "41fbd903fd4c50ffac2e0946a938d07f8c8e1457", "0f9370311ea61a7719d764e70e44635013f92283",
    "7040256583a8bd2313ea4c99c9c5a3c460627b56", "63168a25b70d4f363faa8db3ffb0bd71d71af2c6",
    "9610f333d1531ef544206dc183d05fa2e612aa84", "aa02d29453a240bab99ef9357b50480a5ada392e",
    "730686c20ef19f0b7e4fccd7f6ccde465f692e0b", "f39d4f8eed9b346a0f6e4b51fa0ae919559bc871",
    "119edc43de569e78adf5af0a0608e005771ac3ca", "f6dce922b2cca31b54d59ab58e19c2d71428e1b5",
    "f5ee58f15ba34190e53faee44824e024b57bf1c4", "8d028c5c6a8323cf091b48aaf86542b633a9986f",
    "cb93aa52c5ad382bd8f4c2ad81d0356edf251e79", "7ed6f9f821dad694bd213cde09fcf1558e505dee",
};

/** Number of entries in `SHOWCASE_MAIN_LOG`. */
enum {
  SHOWCASE_MAIN_LOG_COUNT = (int)(sizeof(SHOWCASE_MAIN_LOG) / sizeof(SHOWCASE_MAIN_LOG[0])),
};

// A signed commit's summary holds its `gpgsig` header without each line's leading space, and an
// unsigned commit's holds no signature.
static void test_commit_summary_reads_the_signature(void) {
  struct Arena arena = test_arena();
  struct Repository repository = {0};
  struct Refset refset = {0};
  TEST_ASSERT(open_with_refs(&repository, &refset, "hostile", &arena) == 0);
  const struct Ref* signed_tag = find_tag(&refset, S("signed"));
  TEST_ASSERT(signed_tag != NULL);
  struct CommitSummary summary = {0};
  struct Log log = test_log();
  TEST_CHECK(revision_commit_summary(&repository, signed_tag->commit_oid, &NO_LLM_TRAILERS, &arena,
                                     &summary, &log) == 0);
  TEST_CHECK(test_text_is(summary.subject, "Sign the names that git refuses"));
  TEST_CHECK(test_text_is(summary.signature,
                          "-----BEGIN SSH SIGNATURE-----\n"
                          "U1NIU0lHAAAAAQAAADMAAAALc3NoLWVkMjU1MTkAAAAgc3VyZ2UgZml4dHVyZQ==\n"
                          "-----END SSH SIGNATURE-----"));
  TEST_CHECK(revision_commit_summary(&repository, S("6d61f5cac1694bb8c8beb3179e620b7f10c9ac2d"),
                                     &NO_LLM_TRAILERS, &arena, &summary, &log) == 0);
  TEST_CHECK(summary.signature.len == 0);
  TEST_CHECK(test_log_is_empty(&log));
  repository_close(&repository);
}

// A branch's log lists every commit it reaches, newest first, with no commit before its children,
// as `git log --date-order` lists them.
static void test_walk_log_lists_the_branch_newest_first(void) {
  struct Arena arena = test_arena();
  struct Repository repository = {0};
  struct Refset refset = {0};
  TEST_ASSERT(open_with_refs(&repository, &refset, "showcase", &arena) == 0);
  const struct Ref* main_branch = refset_find_branch(&refset, S("main"));
  TEST_ASSERT(main_branch != NULL);
  struct Str* commits = NULL;
  ptrdiff_t count = 0;
  struct Log log = test_log();
  TEST_CHECK(revision_walk_log(&repository, main_branch, &arena, &commits, &count, &log) == 0);
  TEST_ASSERT(count == SHOWCASE_MAIN_LOG_COUNT);
  for (ptrdiff_t i = 0; i < count; i++) {
    TEST_CHECK(test_text_is(commits[i], "%s", SHOWCASE_MAIN_LOG[i]));
  }
  repository_close(&repository);
}

// The commits every ref reaches are listed once each, in byte order: every branch's and tag's
// commit is on `main`, and a tag of a tree adds none.
static void test_list_commits_lists_each_commit_once(void) {
  struct Arena arena = test_arena();
  struct Repository repository = {0};
  struct Refset refset = {0};
  TEST_ASSERT(open_with_refs(&repository, &refset, "showcase", &arena) == 0);
  struct Str* commits = NULL;
  ptrdiff_t count = 0;
  struct Log log = test_log();
  TEST_CHECK(revision_list_commits(&repository, &refset, &arena, &commits, &count, &log) == 0);
  TEST_ASSERT(count == SHOWCASE_MAIN_LOG_COUNT);
  const char* sorted[SHOWCASE_MAIN_LOG_COUNT];
  for (ptrdiff_t i = 0; i < SHOWCASE_MAIN_LOG_COUNT; i++) {
    sorted[i] = SHOWCASE_MAIN_LOG[i];
  }
  qsort(sorted, SHOWCASE_MAIN_LOG_COUNT, sizeof(sorted[0]), compare_cstrings);
  for (ptrdiff_t i = 0; i < count; i++) {
    TEST_CHECK(test_text_is(commits[i], "%s", sorted[i]));
  }
  repository_close(&repository);
}

// A message without a trailer block has no trailers.
static void test_message_trailers_of_a_message_without_any(void) {
  struct Arena arena = test_arena();
  ptrdiff_t count = -1;
  parse_trailers("Fix the parser\n\nIt read one byte too many.\n", &arena, &count);
  TEST_CHECK(count == 0);
}

// The trailer block is the last paragraph, after a blank line, each key as written and each value
// with its address, and a `Key: value` line in an earlier paragraph is prose.
static void test_message_trailers_reads_the_last_paragraph(void) {
  struct Arena arena = test_arena();
  ptrdiff_t count = 0;
  const struct Trailer* trailers = parse_trailers(
      "Fix the parser\n\nNote: it read one byte too many.\nNow it stops.\n\n"
      "CO-AUTHORED-BY: Grace Hopper <grace@example.org>\n"
      "Signed-off-by: Ada Lovelace <ada@example.org>\n",
      &arena, &count);
  TEST_ASSERT(count == 2);
  TEST_CHECK(test_text_is(trailers[0].key, "CO-AUTHORED-BY"));
  TEST_CHECK(test_text_is(trailers[0].value, "Grace Hopper <grace@example.org>"));
  TEST_CHECK(test_text_is(trailers[1].key, "Signed-off-by"));
  TEST_CHECK(test_text_is(trailers[1].value, "Ada Lovelace <ada@example.org>"));
}

// A continuation line joins its trailer's value with one space, and the white space at the end of a
// value goes.
static void test_message_trailers_joins_continuation_lines(void) {
  struct Arena arena = test_arena();
  ptrdiff_t count = 0;
  const struct Trailer* trailers = parse_trailers(
      "Tidy\n\nAssisted-by: Claude:claude-opus-5-5  \n   clang-format\n"
      "Link: https://example.org/1 \r\n",
      &arena, &count);
  TEST_ASSERT(count == 2);
  TEST_CHECK(test_text_is(trailers[0].value, "Claude:claude-opus-5-5 clang-format"));
  TEST_CHECK(test_text_is(trailers[1].key, "Link"));
  TEST_CHECK(test_text_is(trailers[1].value, "https://example.org/1"));
}

// A tool that only the subject and the body name marks nothing, since only trailers are read, and
// the same tool in a trailer does.
static void test_message_trailers_mark_only_trailers(void) {
  struct Arena arena = test_arena();
  const struct Str values[] = {S("Claude")};
  const struct Str keys[] = {S("Co-authored-by")};
  const struct TrailerMatch match = {
      .keys = keys, .key_count = 1, .values = values, .value_count = 1};
  ptrdiff_t count = 0;
  const struct Trailer* trailers = parse_trailers(
      "Fix the Claude API client\n\nClaude said so.\n\nSigned-off-by: Ada\n", &arena, &count);
  struct Str* tools = NULL;
  ptrdiff_t tool_count = -1;
  trailer_llm_tools(trailers, count, &match, &arena, &tools, &tool_count);
  TEST_CHECK(tool_count == 0);
  trailers = parse_trailers("Fix the client\n\nCo-authored-by: Claude <noreply@anthropic.com>\n",
                            &arena, &count);
  trailer_llm_tools(trailers, count, &match, &arena, &tools, &tool_count);
  TEST_ASSERT(tool_count == 1);
  TEST_CHECK(test_text_is(tools[0], "Claude"));
}

// A message of trailers alone has none, because its first paragraph is its subject.
static void test_message_trailers_of_a_message_of_only_trailers(void) {
  struct Arena arena = test_arena();
  ptrdiff_t count = -1;
  parse_trailers("Signed-off-by: Ada Lovelace <ada@example.org>\n", &arena, &count);
  TEST_CHECK(count == 0);
}

TEST_LIST = {
    {"resolve uses head", test_resolve_uses_head},
    {"resolve uses configured branch", test_resolve_uses_configured_branch},
    {"resolve names the unborn branch of an empty repository",
     test_resolve_names_the_unborn_branch_of_an_empty_repository},
    {"resolve rejects a missing configured branch",
     test_resolve_rejects_a_missing_configured_branch},
    {"commit summary reads subject author and date",
     test_commit_summary_reads_subject_author_and_date},
    {"commit detail reads the committer body and parents",
     test_commit_detail_reads_the_committer_body_and_parents},
    {"commit summary reads the llm tools", test_commit_summary_reads_the_llm_tools},
    {"commit detail reads the trailers", test_commit_detail_reads_the_trailers},
    {"tag note reads the message and the date", test_tag_note_reads_the_message_and_the_date},
    {"tag note keeps the signature apart", test_tag_note_keeps_the_signature_apart},
    {"commit summary reads the signature", test_commit_summary_reads_the_signature},
    {"walk log lists the branch newest first", test_walk_log_lists_the_branch_newest_first},
    {"list commits lists each commit once", test_list_commits_lists_each_commit_once},
    {"message trailers of a message without any", test_message_trailers_of_a_message_without_any},
    {"message trailers reads the last paragraph", test_message_trailers_reads_the_last_paragraph},
    {"message trailers joins continuation lines", test_message_trailers_joins_continuation_lines},
    {"message trailers mark only trailers", test_message_trailers_mark_only_trailers},
    {"message trailers of a message of only trailers",
     test_message_trailers_of_a_message_of_only_trailers},
    {NULL, NULL},
};
