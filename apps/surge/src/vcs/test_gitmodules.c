#include <acutest.h>
#include <stdbool.h>
#include <stddef.h>

#include "core/arena.h"
#include "core/log.h"
#include "core/str.h"
#include "testing/test_support.h"
#include "vcs/gitmodules.h"
#include "vcs/repository.h"

/**
 * @brief Sets libgit2 up and parses `text` as `gitmodules_parse` does.
 *
 * @param text           Terminated text of the file. Must not be `NULL`.
 * @param arena          Arena that owns the submodules. Must not be `NULL`.
 * @param gitmodules_out Receives the submodules. Must not be `NULL`.
 * @param count_out      Receives the number of submodules. Must not be `NULL`.
 * @param reason_out     Receives the failure's reason. Must not be `NULL`.
 * @return What `gitmodules_parse` returns.
 */
static int parse(const char* text,
                 struct Arena* arena,
                 struct Gitmodule** gitmodules_out,
                 ptrdiff_t* count_out,
                 struct Str* reason_out) {
  struct Log log = test_log();
  TEST_ASSERT(repository_setup(&log) == 0);
  return gitmodules_parse(str_from_cstr(text), arena, gitmodules_out, count_out, reason_out);
}

/**
 * @brief Reports whether a submodule has the name, path and URL given.
 *
 * @param gitmodule Submodule to check. Must not be `NULL`.
 * @param name      Terminated name. Must not be `NULL`.
 * @param path      Terminated path. Must not be `NULL`.
 * @param url       Terminated URL. Must not be `NULL`.
 * @return `true` when all three match.
 */
static bool is_gitmodule(const struct Gitmodule* gitmodule,
                         const char* name,
                         const char* path,
                         const char* url) {
  return test_text_is(gitmodule->name, "%s", name) && test_text_is(gitmodule->path, "%s", path) &&
         test_text_is(gitmodule->url, "%s", url);
}

// Each submodule with a path is read, sorted by path, with git-config quoting, escapes, comments,
// continuation lines and section names in any case, and a name that holds spaces and dots.
static void test_parse_reads_submodules_sorted_by_path(void) {
  struct Arena arena = test_arena();
  struct Gitmodule* gitmodules = NULL;
  ptrdiff_t count = 0;
  struct Str reason = {0};
  TEST_ASSERT(parse("# Vendored code\n"
                    "[submodule \"three.js\"]\n"
                    "\tpath = vendor/three.js\n"
                    "\turl = https://github.com/mrdoob/three.js.git ; the upstream\n"
                    "[SubModule \"my lib v1.0\"]\n"
                    "\tPath = \"lib/my lib\"\n"
                    "\turl = https://example.org/\\\n"
                    "my-lib.git\n",
                    &arena, &gitmodules, &count, &reason) == 0);
  TEST_ASSERT(count == 2);
  TEST_CHECK(
      is_gitmodule(&gitmodules[0], "my lib v1.0", "lib/my lib", "https://example.org/my-lib.git"));
  TEST_CHECK(is_gitmodule(&gitmodules[1], "three.js", "vendor/three.js",
                          "https://github.com/mrdoob/three.js.git"));
}

// A submodule without a `url` keeps an empty one, a submodule without a `path` names no gitlink,
// and a key given twice keeps its last value.
static void test_parse_keeps_a_submodule_without_a_url(void) {
  struct Arena arena = test_arena();
  struct Gitmodule* gitmodules = NULL;
  ptrdiff_t count = 0;
  struct Str reason = {0};
  TEST_ASSERT(parse("[submodule \"bare\"]\n\tpath = old\n\tpath = bare\n"
                    "[submodule \"nowhere\"]\n\turl = https://example.org/nowhere.git\n",
                    &arena, &gitmodules, &count, &reason) == 0);
  TEST_ASSERT(count == 1);
  TEST_CHECK(is_gitmodule(&gitmodules[0], "bare", "bare", ""));
}

// Two submodules of one path are both read, next to each other, so the caller can see the path is
// ambiguous.
static void test_parse_keeps_two_submodules_of_one_path(void) {
  struct Arena arena = test_arena();
  struct Gitmodule* gitmodules = NULL;
  ptrdiff_t count = 0;
  struct Str reason = {0};
  TEST_ASSERT(parse("[submodule \"a\"]\n\tpath = vendor/x\n\turl = ../a.git\n"
                    "[submodule \"z\"]\n\tpath = lib\n"
                    "[submodule \"b\"]\n\tpath = vendor/x\n\turl = ../b.git\n",
                    &arena, &gitmodules, &count, &reason) == 0);
  TEST_ASSERT(count == 3);
  TEST_CHECK(test_text_is(gitmodules[0].path, "lib"));
  TEST_CHECK(test_text_is(gitmodules[1].path, "vendor/x") &&
             test_text_is(gitmodules[2].path, "vendor/x"));
}

// An `include.path` names a file that the parse does not read, and keys outside a submodule are
// left out.
static void test_parse_follows_no_include(void) {
  struct Arena arena = test_arena();
  struct Gitmodule* gitmodules = NULL;
  ptrdiff_t count = 0;
  struct Str reason = {0};
  TEST_ASSERT(parse("[include]\n\tpath = /nonexistent/gitmodules\n"
                    "[core]\n\tbare = true\n",
                    &arena, &gitmodules, &count, &reason) == 0);
  TEST_CHECK(count == 0);
}

// An empty file has no submodule.
static void test_parse_reads_an_empty_file(void) {
  struct Arena arena = test_arena();
  struct Gitmodule* gitmodules = NULL;
  ptrdiff_t count = -1;
  struct Str reason = {0};
  TEST_CHECK(parse("", &arena, &gitmodules, &count, &reason) == 0);
  TEST_CHECK(count == 0);
}

// A file that does not parse fails with libgit2's reason, and gives no submodule.
static void test_parse_rejects_a_file_that_does_not_parse(void) {
  struct Arena arena = test_arena();
  struct Gitmodule* gitmodules = NULL;
  ptrdiff_t count = -1;
  struct Str reason = {0};
  TEST_CHECK(parse("[submodule \"a\"]\n\tpath = a\n[submodule \"b\n", &arena, &gitmodules, &count,
                   &reason) == -1);
  TEST_CHECK(count == 0 && gitmodules == NULL);
  TEST_CHECK(reason.len > 0);
}

TEST_LIST = {
    {"parse reads submodules sorted by path", test_parse_reads_submodules_sorted_by_path},
    {"parse keeps a submodule without a url", test_parse_keeps_a_submodule_without_a_url},
    {"parse keeps two submodules of one path", test_parse_keeps_two_submodules_of_one_path},
    {"parse follows no include", test_parse_follows_no_include},
    {"parse reads an empty file", test_parse_reads_an_empty_file},
    {"parse rejects a file that does not parse", test_parse_rejects_a_file_that_does_not_parse},
    {NULL, NULL},
};
