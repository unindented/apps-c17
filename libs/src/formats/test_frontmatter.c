#include <acutest.h>

#include "core/str.h"
#include "formats/frontmatter.h"
#include "testing/test_support.h"

// A `+++` block is TOML, and the text holds its lines, line ends included, with the body after it.
static void test_finds_toml_front_matter(void) {
  struct Frontmatter found = {0};
  TEST_CHECK(frontmatter_find(S("+++\ntitle = \"A\"\n+++\nBody.\n"), &found) == FRONTMATTER_FOUND);
  TEST_CHECK(found.kind == FRONTMATTER_KIND_TOML);
  TEST_CHECK(str_eq(found.text, S("title = \"A\"\n")));
  TEST_CHECK(str_eq(found.body, S("Body.\n")));
}

// A `---` block is YAML, which only the fences mark, and a `+++` line inside it closes nothing.
static void test_finds_yaml_front_matter(void) {
  struct Frontmatter found = {0};
  TEST_CHECK(frontmatter_find(S("---\ntitle: A\n+++\n---\nBody.\n"), &found) == FRONTMATTER_FOUND);
  TEST_CHECK(found.kind == FRONTMATTER_KIND_YAML);
  TEST_CHECK(str_eq(found.text, S("title: A\n+++\n")));
  TEST_CHECK(str_eq(found.body, S("Body.\n")));
}

// A BOM before the opening fence and `\r\n` line ends are allowed, and an empty block has no text.
static void test_finds_front_matter_after_a_bom_with_crlf(void) {
  struct Frontmatter found = {0};
  TEST_CHECK(frontmatter_find(S("\xEF\xBB\xBF+++\r\n+++\r\nBody.\r\n"), &found) ==
             FRONTMATTER_FOUND);
  TEST_CHECK(found.kind == FRONTMATTER_KIND_TOML);
  TEST_CHECK(found.text.len == 0);
  TEST_CHECK(str_eq(found.body, S("Body.\r\n")));
}

// The kinds name their fences and their code block languages.
static void test_names_fences_and_languages(void) {
  TEST_CHECK(str_eq(frontmatter_fence(FRONTMATTER_KIND_TOML), S("+++")));
  TEST_CHECK(str_eq(frontmatter_fence(FRONTMATTER_KIND_YAML), S("---")));
  TEST_CHECK(str_eq(frontmatter_language(FRONTMATTER_KIND_TOML), S("toml")));
  TEST_CHECK(str_eq(frontmatter_language(FRONTMATTER_KIND_YAML), S("yaml")));
}

// A file whose first line is no fence, such as one that starts with text or with `++++`, has no
// front matter, and its body is the whole file without its BOM.
static void test_reports_no_front_matter(void) {
  struct Frontmatter found = {0};
  TEST_CHECK(frontmatter_find(S("\xEF\xBB\xBF# Title\n+++\n"), &found) == FRONTMATTER_ABSENT);
  TEST_CHECK(found.kind == FRONTMATTER_KIND_NONE);
  TEST_CHECK(str_eq(found.body, S("# Title\n+++\n")));
  TEST_CHECK(frontmatter_find(S("++++\n+++\n"), &found) == FRONTMATTER_ABSENT);
  TEST_CHECK(frontmatter_find(S(""), &found) == FRONTMATTER_ABSENT);
}

// An opening fence that no line of the same fence closes is unclosed, with the opening's kind.
static void test_reports_an_unclosed_block(void) {
  struct Frontmatter found = {0};
  TEST_CHECK(frontmatter_find(S("+++\ntitle = \"A\"\n---\n"), &found) == FRONTMATTER_UNCLOSED);
  TEST_CHECK(found.kind == FRONTMATTER_KIND_TOML);
  TEST_CHECK(str_eq(found.body, S("+++\ntitle = \"A\"\n---\n")));
}

TEST_LIST = {
    {"finds toml front matter", test_finds_toml_front_matter},
    {"finds yaml front matter", test_finds_yaml_front_matter},
    {"finds front matter after a bom with crlf", test_finds_front_matter_after_a_bom_with_crlf},
    {"names fences and languages", test_names_fences_and_languages},
    {"reports no front matter", test_reports_no_front_matter},
    {"reports an unclosed block", test_reports_an_unclosed_block},
    {NULL, NULL},
};
