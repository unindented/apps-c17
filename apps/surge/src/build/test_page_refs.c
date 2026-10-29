#include <acutest.h>

#include "build/page_refs.h"
#include "build/template_value.h"
#include "core/arena.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "domain/site_config.h"
#include "testing/test_support.h"

// Items sort newest first, and a tie sorts by name in byte order.
static void test_sort_newest_orders_by_time_then_name(void) {
  struct PageRefsDated items[] = {
      {.index = 0, .time = 100, .name = S("v0.2.0")},
      {.index = 1, .time = 200, .name = S("v0.3.0")},
      {.index = 2, .time = 100, .name = S("v0.1.0")},
  };
  page_refs_sort_newest(items, 3);
  TEST_CHECK(items[0].index == 1);
  TEST_CHECK(items[1].index == 2);
  TEST_CHECK(items[2].index == 0);
}

// An item without a time, such as a lightweight tag of a tree, comes last.
static void test_sort_newest_puts_no_time_last(void) {
  struct PageRefsDated items[] = {
      {.index = 0, .time = 0, .name = S("a-tree")},
      {.index = 1, .time = 5, .name = S("z-commit")},
  };
  page_refs_sort_newest(items, 2);
  TEST_CHECK(items[0].index == 1);
  TEST_CHECK(items[1].index == 0);
}

// A signature's kind comes from its opening line, as git tells them apart.
static void test_signature_kind_names_each_opening(void) {
  TEST_CHECK(
      test_text_is(page_refs_signature_kind(S("-----BEGIN PGP SIGNATURE-----\n=a\n")), "PGP"));
  TEST_CHECK(test_text_is(page_refs_signature_kind(S("-----BEGIN PGP MESSAGE-----\n")), "PGP"));
  TEST_CHECK(
      test_text_is(page_refs_signature_kind(S("-----BEGIN SIGNED MESSAGE-----\n")), "X.509"));
  TEST_CHECK(test_text_is(page_refs_signature_kind(S("-----BEGIN SSH SIGNATURE-----\n")), "SSH"));
}

// No signature, and one that opens with no line git knows, have no kind.
static void test_signature_kind_is_empty_otherwise(void) {
  TEST_CHECK(page_refs_signature_kind(S("")).len == 0);
  TEST_CHECK(page_refs_signature_kind(S("-----BEGIN OTHER-----\n")).len == 0);
}

// A commit's map holds its fields and links its page, and a summary with no commit, as a ref of a
// tree or a blob has, is `false`, so a section over it is skipped.
static void test_commit_is_false_without_a_commit(void) {
  const struct SurgeRepo repo_config = {.name = S("r")};
  const struct RepoModel repo = {.config = &repo_config};
  struct Arena arena = test_arena();
  const struct TemplateValue* none =
      page_refs_commit(&repo, (struct CommitSummary){0}, S("../"), &arena);
  TEST_CHECK(none->kind == TEMPLATE_VALUE_FALSE);
  const struct TemplateValue* commit = page_refs_commit(
      &repo, (struct CommitSummary){.oid = S("0123456789abcdef"), .subject = S("Fix")}, S("../"),
      &arena);
  TEST_ASSERT(commit->kind == TEMPLATE_VALUE_MAP);
  TEST_CHECK(test_text_is(template_value_find(commit, S("short_oid"))->text, "0123456"));
  TEST_CHECK(test_text_is(template_value_find(commit, S("url"))->text,
                          "../r/commit/0123456789abcdef/index.html"));
}

TEST_LIST = {
    {"sort newest orders by time then name", test_sort_newest_orders_by_time_then_name},
    {"sort newest puts no time last", test_sort_newest_puts_no_time_last},
    {"signature kind names each opening", test_signature_kind_names_each_opening},
    {"signature kind is empty otherwise", test_signature_kind_is_empty_otherwise},
    {"commit is false without a commit", test_commit_is_false_without_a_commit},
    {NULL, NULL},
};
