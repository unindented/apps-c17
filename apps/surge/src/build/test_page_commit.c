#include <acutest.h>

#include "build/page_commit.h"
#include "core/arena.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "testing/test_support.h"

// A diff is a code line for the hunk and one for each line, whose numbers link the hunk's `L` and
// `R` anchors.
static void test_diff_html_anchors_each_line_by_side(void) {
  struct DiffLine lines[] = {
      {.origin = ' ', .old_line = 1, .new_line = 1, .text = S("a")},
      {.origin = '-', .old_line = 2, .text = S("<b>")},
      {.origin = '+', .new_line = 2, .text = S("c")},
      {.origin = '\\', .text = S("No newline at end of file")},
  };
  struct DiffHunk hunk = {.header = S("@@ -1,2 +1,2 @@"), .line_count = 4};
  const struct ChangedFile file = {
      .is_shown = true, .hunks = &hunk, .hunk_count = 1, .lines = lines, .line_count = 4};
  struct Arena arena = test_arena();
  TEST_CHECK(test_text_is(
      page_commit_diff_html(&file, NULL, S("diff-3"), &arena, test_arena()),
      "<pre class=\"surge-diff\"><code><span class=\"code-line surge-hunk\"><span "
      "class=\"code-line-numbers\"><span class=\"code-line-number\"></span><span "
      "class=\"code-line-number\"></span></span><span class=\"code-line-text\">@@ -1,2 +1,2 @@\n"
      "</span></span><span class=\"code-line surge-context\"><span class=\"code-line-numbers\"><a "
      "class=\"code-line-number\" id=\"diff-3-hunk-0-L1\" href=\"#diff-3-hunk-0-L1\">1</a><a "
      "class=\"code-line-number\" id=\"diff-3-hunk-0-R1\" "
      "href=\"#diff-3-hunk-0-R1\">1</a></span><span class=\"code-line-text\"> a\n"
      "</span></span><span class=\"code-line surge-del\"><span class=\"code-line-numbers\"><a "
      "class=\"code-line-number\" id=\"diff-3-hunk-0-L2\" href=\"#diff-3-hunk-0-L2\">2</a><span "
      "class=\"code-line-number\"></span></span><span class=\"code-line-text\">-&lt;b&gt;\n"
      "</span></span><span class=\"code-line surge-add\"><span class=\"code-line-numbers\"><span "
      "class=\"code-line-number\"></span><a class=\"code-line-number\" id=\"diff-3-hunk-0-R2\" "
      "href=\"#diff-3-hunk-0-R2\">2</a></span><span class=\"code-line-text\">+c\n"
      "</span></span><span class=\"code-line surge-no-newline\"><span "
      "class=\"code-line-numbers\"><span class=\"code-line-number\"></span><span "
      "class=\"code-line-number\"></span></span><span class=\"code-line-text\"> No newline at end "
      "of file\n"
      "</span></span></code></pre>\n"));
}

// A line with a fragment shows it as it is, already escaped, in place of its own text.
static void test_diff_html_shows_the_fragment_of_each_line(void) {
  struct DiffLine lines[] = {
      {.origin = '-', .old_line = 2, .text = S("old")},
      {.origin = '+', .new_line = 1, .text = S("new")},
  };
  struct DiffHunk hunk = {.header = S("@@ -2 +1 @@"), .line_count = 2};
  const struct ChangedFile file = {
      .is_shown = true, .hunks = &hunk, .hunk_count = 1, .lines = lines, .line_count = 2};
  const struct Str fragments[] = {S("<b>o2</b>"), S("<u>n1</u>")};
  struct Arena arena = test_arena();
  const struct Str html =
      page_commit_diff_html(&file, fragments, S("diff-0"), &arena, test_arena());
  TEST_CHECK(test_text_is(
      html,
      "<pre class=\"surge-diff\"><code><span class=\"code-line surge-hunk\"><span "
      "class=\"code-line-numbers\"><span class=\"code-line-number\"></span><span "
      "class=\"code-line-number\"></span></span><span class=\"code-line-text\">@@ -2 +1 @@\n"
      "</span></span><span class=\"code-line surge-del\"><span class=\"code-line-numbers\"><a "
      "class=\"code-line-number\" id=\"diff-0-hunk-0-L2\" href=\"#diff-0-hunk-0-L2\">2</a><span "
      "class=\"code-line-number\"></span></span><span class=\"code-line-text\">-<b>o2</b>\n"
      "</span></span><span class=\"code-line surge-add\"><span class=\"code-line-numbers\"><span "
      "class=\"code-line-number\"></span><a class=\"code-line-number\" id=\"diff-0-hunk-0-R1\" "
      "href=\"#diff-0-hunk-0-R1\">1</a></span><span class=\"code-line-text\">+<u>n1</u>\n"
      "</span></span></code></pre>\n"));
}

// Each hunk shows its own lines, from its first line in the file's list, and anchors them with its
// own index.
static void test_diff_html_numbers_each_hunk(void) {
  struct DiffLine lines[] = {
      {.origin = '-', .old_line = 1, .text = S("one")},
      {.origin = ' ', .old_line = 20, .new_line = 19, .text = S("twenty")},
      {.origin = '+', .new_line = 20, .text = S("added")},
  };
  struct DiffHunk hunks[] = {
      {.header = S("@@ -1 +0,0 @@"), .line_index = 0, .line_count = 1},
      {.header = S("@@ -20 +19,2 @@"), .line_index = 1, .line_count = 2},
  };
  const struct ChangedFile file = {
      .is_shown = true, .hunks = hunks, .hunk_count = 2, .lines = lines, .line_count = 3};
  struct Arena arena = test_arena();
  TEST_CHECK(test_text_is(
      page_commit_diff_html(&file, NULL, S("diff-1"), &arena, test_arena()),
      "<pre class=\"surge-diff\"><code><span class=\"code-line surge-hunk\"><span "
      "class=\"code-line-numbers\"><span class=\"code-line-number\"></span><span "
      "class=\"code-line-number\"></span></span><span class=\"code-line-text\">@@ -1 +0,0 @@\n"
      "</span></span><span class=\"code-line surge-del\"><span class=\"code-line-numbers\"><a "
      "class=\"code-line-number\" id=\"diff-1-hunk-0-L1\" href=\"#diff-1-hunk-0-L1\">1</a><span "
      "class=\"code-line-number\"></span></span><span class=\"code-line-text\">-one\n"
      "</span></span><span class=\"code-line surge-hunk\"><span class=\"code-line-numbers\"><span "
      "class=\"code-line-number\"></span><span class=\"code-line-number\"></span></span><span "
      "class=\"code-line-text\">@@ -20 +19,2 @@\n"
      "</span></span><span class=\"code-line surge-context\"><span class=\"code-line-numbers\"><a "
      "class=\"code-line-number\" id=\"diff-1-hunk-1-L20\" href=\"#diff-1-hunk-1-L20\">20</a><a "
      "class=\"code-line-number\" id=\"diff-1-hunk-1-R19\" "
      "href=\"#diff-1-hunk-1-R19\">19</a></span><span class=\"code-line-text\"> twenty\n"
      "</span></span><span class=\"code-line surge-add\"><span class=\"code-line-numbers\"><span "
      "class=\"code-line-number\"></span><a class=\"code-line-number\" id=\"diff-1-hunk-1-R20\" "
      "href=\"#diff-1-hunk-1-R20\">20</a></span><span class=\"code-line-text\">+added\n"
      "</span></span></code></pre>\n"));
}

// A file whose diff is not shown, or has no lines, has no code lines.
static void test_diff_html_is_empty_without_lines(void) {
  struct Arena arena = test_arena();
  const struct ChangedFile hidden = {0};
  const struct ChangedFile renamed = {.is_shown = true};
  TEST_CHECK(page_commit_diff_html(&hidden, NULL, S("diff-0"), &arena, test_arena()).len == 0);
  TEST_CHECK(page_commit_diff_html(&renamed, NULL, S("diff-0"), &arena, test_arena()).len == 0);
}

TEST_LIST = {
    {"diff html anchors each line by side", test_diff_html_anchors_each_line_by_side},
    {"diff html shows the fragment of each line", test_diff_html_shows_the_fragment_of_each_line},
    {"diff html numbers each hunk", test_diff_html_numbers_each_hunk},
    {"diff html is empty without lines", test_diff_html_is_empty_without_lines},
    {NULL, NULL},
};
