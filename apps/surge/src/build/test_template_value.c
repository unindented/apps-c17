#include <acutest.h>

#include "build/template_value.h"
#include "core/arena.h"
#include "core/log.h"
#include "core/str.h"
#include "os/os_memory.h"
#include "templates/template_set.h"
#include "testing/test_support.h"

/** The page template every render test renders. */
static const struct Str PAGE_NAMES[] = {STR_INIT("page.html")};

/**
 * @brief Loads a set whose only template, `page.html`, is `page`, renders it over `root`, and
 *        reports whether it rendered `expected`.
 *
 * @param page     Terminated text of `templates/page.html`. Must not be `NULL`.
 * @param root     Tree the names resolve against. Must not be `NULL`.
 * @param expected Terminated text the render must produce. Must not be `NULL`.
 * @return `true` when the load and the render succeeded, and the render produced exactly
 *         `expected`.
 */
static bool renders(const char* page, const struct TemplateValue* root, const char* expected) {
  struct Os* os = os_memory();
  os_memory_write(os, "templates/page.html", page);
  struct Arena perm = test_arena();
  struct Log log = test_log();
  struct TemplateSet* set = NULL;
  if (template_set_load(S("templates"), PAGE_NAMES, NULL, 1, os, &perm, test_arena(), &set, &log) !=
      0) {
    return false;
  }
  struct Str html = {0};
  const bool is_rendered = template_value_render(set, S("page.html"), root, S("page.html"), &perm,
                                                 test_arena(), &html, &log) == 0;
  template_set_release(set);
  return is_rendered && test_log_is_empty(&log) && test_text_is(html, "%s", expected);
}

// A map finds each field it holds by name, and nothing else.
static void test_map_finds_fields(void) {
  struct Arena arena = test_arena();
  struct TemplateValue* map = template_value_map(&arena);
  template_value_set_text(map, S("name"), S("surge"), &arena);
  template_value_set_bool(map, S("is_new"), true, &arena);
  const struct TemplateValue* name = template_value_find(map, S("name"));
  TEST_ASSERT(name != NULL);
  TEST_CHECK(name->kind == TEMPLATE_VALUE_TEXT && str_eq(name->text, S("surge")));
  TEST_CHECK(template_value_find(map, S("is_new"))->kind == TEMPLATE_VALUE_TRUE);
  TEST_CHECK(template_value_find(map, S("missing")) == NULL);
}

// A map grows past its first allocation and keeps every field.
static void test_map_grows(void) {
  struct Arena arena = test_arena();
  struct TemplateValue* map = template_value_map(&arena);
  const struct Str names[] = {S("a"), S("b"), S("c"), S("d"), S("e"),
                              S("f"), S("g"), S("h"), S("i"), S("j")};
  for (ptrdiff_t i = 0; i < 10; i++) {
    template_value_set_text(map, names[i], names[i], &arena);
  }
  TEST_CHECK(map->field_count == 10);
  TEST_CHECK(str_eq(template_value_find(map, S("j"))->text, S("j")));
}

// Text set under a key that ends in `_html` is HTML that a template may write unescaped. Text under
// any other key is not, nor is a text value that `template_value_set` adds under such a key, since
// only `template_value_set_text` sees the key and the text together.
static void test_set_text_marks_only_html_keys(void) {
  struct Arena arena = test_arena();
  struct TemplateValue* map = template_value_map(&arena);
  template_value_set_text(map, S("rendered_html"), S("<p>"), &arena);
  template_value_set_text(map, S("html"), S("<p>"), &arena);
  template_value_set_text(map, S("html_url"), S("<p>"), &arena);
  template_value_set(map, S("added_html"), template_value_text(S("<p>"), &arena), &arena);
  TEST_CHECK(template_value_find(map, S("rendered_html"))->is_html);
  TEST_CHECK(!template_value_find(map, S("html"))->is_html);
  TEST_CHECK(!template_value_find(map, S("html_url"))->is_html);
  TEST_CHECK(!template_value_find(map, S("added_html"))->is_html);
}

// A field of a value that is not a map is never found.
static void test_find_in_non_map_finds_nothing(void) {
  struct Arena arena = test_arena();
  TEST_CHECK(template_value_find(template_value_text(S("x"), &arena), S("x")) == NULL);
}

// A map, true, non-empty text and a non-empty list are truthy, and false, empty text and an empty
// list are not.
static void test_is_truthy_only_for_non_empty_values(void) {
  struct Arena arena = test_arena();
  struct TemplateValue* list = template_value_list(1, &arena);
  list->items[0] = template_value_bool(true, &arena);
  TEST_CHECK(template_value_is_truthy(template_value_map(&arena)));
  TEST_CHECK(template_value_is_truthy(template_value_bool(true, &arena)));
  TEST_CHECK(template_value_is_truthy(template_value_text(S("x"), &arena)));
  TEST_CHECK(template_value_is_truthy(list));
  TEST_CHECK(!template_value_is_truthy(template_value_bool(false, &arena)));
  TEST_CHECK(!template_value_is_truthy(template_value_text((struct Str){0}, &arena)));
  TEST_CHECK(!template_value_is_truthy(template_value_list(0, &arena)));
}

// A section enters a list once per item, a true value once, and a false value or empty text not at
// all, and an inverted section does the opposite. True writes `true`, and false writes nothing.
static void test_render_enters_sections_by_truthiness(void) {
  struct Arena arena = test_arena();
  struct TemplateValue* root = template_value_map(&arena);
  struct TemplateValue* items = template_value_list(2, &arena);
  const struct Str names[] = {S("a"), S("b")};
  for (ptrdiff_t i = 0; i < 2; i++) {
    struct TemplateValue* item = template_value_map(&arena);
    template_value_set_text(item, S("name"), names[i], &arena);
    items->items[i] = item;
  }
  template_value_set(root, S("items"), items, &arena);
  template_value_set_bool(root, S("yes"), true, &arena);
  template_value_set_bool(root, S("no"), false, &arena);
  template_value_set_text(root, S("empty"), (struct Str){0}, &arena);
  TEST_CHECK(
      renders("{{#items}}[{{name}}]{{/items}}{{#yes}}Y{{/yes}}{{#no}}N{{/no}}"
              "{{^no}}n{{/no}}{{#empty}}E{{/empty}}{{^empty}}e{{/empty}}[{{yes}}{{no}}]",
              root, "[a][b]Yne[true]"));
}

// A key a section's map holds, even as empty text or false, hides the same key of an outer scope,
// which Mustache would look up when the map left it out.
static void test_render_present_key_shadows_outer_scope(void) {
  struct Arena arena = test_arena();
  struct TemplateValue* root = template_value_map(&arena);
  template_value_set_text(root, S("name"), S("outer"), &arena);
  template_value_set_bool(root, S("flag"), true, &arena);
  struct TemplateValue* inner = template_value_map(&arena);
  template_value_set_text(inner, S("name"), (struct Str){0}, &arena);
  template_value_set_bool(inner, S("flag"), false, &arena);
  template_value_set(root, S("inner"), inner, &arena);
  TEST_CHECK(renders("{{#inner}}[{{name}}]{{#flag}}F{{/flag}}{{/inner}}", root, "[]"));
}

// Text under a key that ends in `_html` is written as it is by a triple mustache, and other text is
// escaped.
static void test_render_writes_html_keys_unescaped(void) {
  struct Arena arena = test_arena();
  struct TemplateValue* root = template_value_map(&arena);
  template_value_set_text(root, S("x"), S("<a>"), &arena);
  template_value_set_text(root, S("x_html"), S("<b>"), &arena);
  TEST_CHECK(renders("{{x}}|{{{x_html}}}", root, "&lt;a&gt;|<b>"));
}

TEST_LIST = {
    {"map finds fields", test_map_finds_fields},
    {"map grows", test_map_grows},
    {"set text marks only html keys", test_set_text_marks_only_html_keys},
    {"find in non map finds nothing", test_find_in_non_map_finds_nothing},
    {"is truthy only for non-empty values", test_is_truthy_only_for_non_empty_values},
    {"render enters sections by truthiness", test_render_enters_sections_by_truthiness},
    {"render present key shadows outer scope", test_render_present_key_shadows_outer_scope},
    {"render writes html keys unescaped", test_render_writes_html_keys_unescaped},
    {NULL, NULL},
};
