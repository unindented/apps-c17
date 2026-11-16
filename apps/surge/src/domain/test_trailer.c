#include <acutest.h>

#include "core/arena.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "domain/trailer.h"
#include "testing/test_support.h"

/** Number of entries in a fixed array. */
#define COUNT(array) ((ptrdiff_t)(sizeof(array) / sizeof((array)[0])))

/** The default `llm_trailer_keys`, in the spelling of the README. */
static const struct Str DEFAULT_KEYS[] = {
    STR_INIT("Co-authored-by"),
    STR_INIT("Assisted-by"),
    STR_INIT("Generated-by"),
};

/**
 * @brief Lists the LLM tools of `trailers` for the default keys and `values`.
 *
 * @param trailers      The trailers. Must not be `NULL` unless `trailer_count` is `0`.
 * @param trailer_count Number of entries in `trailers`.
 * @param values        Texts to look for. Must not be `NULL` unless `value_count` is `0`.
 * @param value_count   Number of entries in `values`.
 * @param tools_out     Receives the tools, in a reservation of their own. Must not be `NULL`.
 * @return The number of tools.
 */
static ptrdiff_t list_tools(const struct Trailer* trailers,
                            ptrdiff_t trailer_count,
                            const struct Str* values,
                            ptrdiff_t value_count,
                            struct Str** tools_out) {
  const struct TrailerMatch match = {
      .keys = DEFAULT_KEYS,
      .key_count = COUNT(DEFAULT_KEYS),
      .values = values,
      .value_count = value_count,
  };
  struct Arena arena = test_arena();
  ptrdiff_t count = -1;
  trailer_llm_tools(trailers, trailer_count, &match, &arena, tools_out, &count);
  return count;
}

// A value that ends in an address keeps the name before it, without the space between them.
static void test_name_drops_the_address(void) {
  TEST_CHECK(test_text_is(trailer_name(S("Grace Hopper <grace@example.org>")), "Grace Hopper"));
  TEST_CHECK(
      test_text_is(trailer_name(S("Claude Opus 5.5  <noreply@anthropic.com>")), "Claude Opus 5.5"));
}

// A value with no `<…>` part at its end is the name as it is.
static void test_name_keeps_a_value_without_an_address(void) {
  TEST_CHECK(test_text_is(trailer_name(S("Claude:claude-opus-5-5 clang-format")),
                          "Claude:claude-opus-5-5 clang-format"));
  TEST_CHECK(test_text_is(trailer_name(S("a <b> c")), "a <b> c"));
  TEST_CHECK(test_text_is(trailer_name(S("x>")), "x>"));
  TEST_CHECK(test_text_is(trailer_name(S("")), ""));
}

// A bare address has an empty name.
static void test_name_of_a_bare_address_is_empty(void) {
  TEST_CHECK(trailer_name(S("<ada@example.org>")).len == 0);
}

// The co-authors are the names of the `Co-authored-by` trailers in message order, their keys
// matched in any letter case, and no other trailer's.
static void test_co_authors_lists_the_names_in_order(void) {
  const struct Trailer trailers[] = {
      {S("Signed-off-by"), S("Ada Lovelace <ada@example.org>")},
      {S("Co-Authored-By"), S("Claude Opus 5.5 <noreply@anthropic.com>")},
      {S("CO-AUTHORED-BY"), S("Grace Hopper <grace@example.org>")},
      {S("co-authored-by"), S("Alan Turing")},
      {S("Co-developed-by"), S("Edsger Dijkstra <edsger@example.org>")},
  };
  struct Arena arena = test_arena();
  struct Str* names = NULL;
  ptrdiff_t count = 0;
  trailer_co_authors(trailers, COUNT(trailers), &arena, &names, &count);
  TEST_ASSERT(count == 3);
  TEST_CHECK(test_text_is(names[0], "Claude Opus 5.5"));
  TEST_CHECK(test_text_is(names[1], "Grace Hopper"));
  TEST_CHECK(test_text_is(names[2], "Alan Turing"));
}

// A value that repeats an earlier one byte for byte adds no name, while one that differs in its
// address or its letter case does, and a bare address adds none.
static void test_co_authors_drops_repeats_and_bare_addresses(void) {
  const struct Trailer trailers[] = {
      {S("Co-authored-by"), S("Grace Hopper <grace@example.org>")},
      {S("Co-Authored-By"), S("Grace Hopper <grace@example.org>")},
      {S("Co-authored-by"), S("Grace Hopper <hopper@example.org>")},
      {S("Co-authored-by"), S("grace hopper <grace@example.org>")},
      {S("Co-authored-by"), S("<ada@example.org>")},
  };
  struct Arena arena = test_arena();
  struct Str* names = NULL;
  ptrdiff_t count = 0;
  trailer_co_authors(trailers, COUNT(trailers), &arena, &names, &count);
  TEST_ASSERT(count == 3);
  TEST_CHECK(test_text_is(names[0], "Grace Hopper"));
  TEST_CHECK(test_text_is(names[1], "Grace Hopper"));
  TEST_CHECK(test_text_is(names[2], "grace hopper"));
}

// A commit without trailers has no co-authors.
static void test_co_authors_of_no_trailers_is_empty(void) {
  struct Arena arena = test_arena();
  struct Str* names = NULL;
  ptrdiff_t count = -1;
  trailer_co_authors(NULL, 0, &arena, &names, &count);
  TEST_CHECK(count == 0);
}

// A value under each default key marks the commit, its tool the value without its address.
static void test_llm_tools_match_under_each_default_key(void) {
  const struct Str values[] = {S("Claude")};
  const char* const keys[] = {"Co-authored-by", "Assisted-by", "Generated-by"};
  for (ptrdiff_t i = 0; i < COUNT(keys); i++) {
    const struct Trailer trailers[] = {
        {str_from_cstr(keys[i]), S("Claude <noreply@anthropic.com>")}};
    struct Str* tools = NULL;
    TEST_ASSERT(list_tools(trailers, COUNT(trailers), values, COUNT(values), &tools) == 1);
    TEST_CHECK(test_text_is(tools[0], "Claude"));
    TEST_MSG("key: %s", keys[i]);
  }
}

// A key matches in any ASCII letter case, as git compares trailer keys, and so does a value.
static void test_llm_tools_fold_the_case_of_keys_and_values(void) {
  const struct Str values[] = {S("claude")};
  const char* const keys[] = {"Co-authored-by", "Co-Authored-By", "CO-AUTHORED-BY"};
  for (ptrdiff_t i = 0; i < COUNT(keys); i++) {
    const struct Trailer trailers[] = {{str_from_cstr(keys[i]), S("CLAUDE Opus 5.5")}};
    struct Str* tools = NULL;
    TEST_ASSERT(list_tools(trailers, COUNT(trailers), values, COUNT(values), &tools) == 1);
    TEST_CHECK(test_text_is(tools[0], "CLAUDE Opus 5.5"));
    TEST_MSG("key: %s", keys[i]);
  }
}

// A value matches in the name, in the address, or in a value with no address at all.
static void test_llm_tools_match_a_name_an_address_or_a_plain_value(void) {
  const struct Trailer trailers[] = {
      {S("Co-authored-by"), S("Claude Opus 5.5 <noreply@anthropic.com>")},
      {S("Co-authored-by"), S("Some Bot <noreply@anthropic.com>")},
      {S("Assisted-by"), S("Cursor:gpt-5 grep")},
  };
  const struct Str by_name[] = {S("Opus")};
  struct Str* tools = NULL;
  TEST_ASSERT(list_tools(trailers, COUNT(trailers), by_name, COUNT(by_name), &tools) == 1);
  TEST_CHECK(test_text_is(tools[0], "Claude Opus 5.5"));
  const struct Str by_address[] = {S("noreply@anthropic.com")};
  TEST_ASSERT(list_tools(trailers, COUNT(trailers), by_address, COUNT(by_address), &tools) == 2);
  TEST_CHECK(test_text_is(tools[0], "Claude Opus 5.5"));
  TEST_CHECK(test_text_is(tools[1], "Some Bot"));
  const struct Str plain[] = {S("cursor")};
  TEST_ASSERT(list_tools(trailers, COUNT(trailers), plain, COUNT(plain), &tools) == 1);
  TEST_CHECK(test_text_is(tools[0], "Cursor:gpt-5 grep"));
}

// A commit with two tools lists each once, in message order, without its address, and a value that
// is only an address is its own tool.
static void test_llm_tools_list_each_tool_once(void) {
  const struct Trailer trailers[] = {
      {S("Co-Authored-By"), S("Claude Opus 5.5 <noreply@anthropic.com>")},
      {S("Assisted-by"), S("aider:gpt-5")},
      {S("Co-authored-by"), S("Claude Opus 5.5 <noreply@anthropic.com>")},
      {S("Generated-by"), S("<aider@example.org>")},
  };
  const struct Str values[] = {S("Claude"), S("aider")};
  struct Str* tools = NULL;
  TEST_ASSERT(list_tools(trailers, COUNT(trailers), values, COUNT(values), &tools) == 3);
  TEST_CHECK(test_text_is(tools[0], "Claude Opus 5.5"));
  TEST_CHECK(test_text_is(tools[1], "aider:gpt-5"));
  TEST_CHECK(test_text_is(tools[2], "<aider@example.org>"));
}

// A human co-author, and a listed value under a key that is not listed, mark nothing.
static void test_llm_tools_skip_humans_and_unlisted_keys(void) {
  const struct Trailer trailers[] = {
      {S("Co-authored-by"), S("Jane Doe <jane@example.com>")},
      {S("Signed-off-by"), S("Claude Smith <claude@example.com>")},
  };
  const struct Str values[] = {S("Claude")};
  struct Str* tools = NULL;
  TEST_CHECK(list_tools(trailers, COUNT(trailers), values, COUNT(values), &tools) == 0);
}

// With no values, as by default, no trailer marks a commit.
static void test_llm_tools_of_no_values_is_empty(void) {
  const struct Trailer trailers[] = {
      {S("Co-Authored-By"), S("Claude Opus 5.5 <noreply@anthropic.com>")},
  };
  struct Str* tools = NULL;
  TEST_CHECK(list_tools(trailers, COUNT(trailers), NULL, 0, &tools) == 0);
}

TEST_LIST = {
    {"name drops the address", test_name_drops_the_address},
    {"name keeps a value without an address", test_name_keeps_a_value_without_an_address},
    {"name of a bare address is empty", test_name_of_a_bare_address_is_empty},
    {"co authors lists the names in order", test_co_authors_lists_the_names_in_order},
    {"co authors drops repeats and bare addresses",
     test_co_authors_drops_repeats_and_bare_addresses},
    {"co authors of no trailers is empty", test_co_authors_of_no_trailers_is_empty},
    {"llm tools match under each default key", test_llm_tools_match_under_each_default_key},
    {"llm tools fold the case of keys and values", test_llm_tools_fold_the_case_of_keys_and_values},
    {"llm tools match a name an address or a plain value",
     test_llm_tools_match_a_name_an_address_or_a_plain_value},
    {"llm tools list each tool once", test_llm_tools_list_each_tool_once},
    {"llm tools skip humans and unlisted keys", test_llm_tools_skip_humans_and_unlisted_keys},
    {"llm tools of no values is empty", test_llm_tools_of_no_values_is_empty},
    {NULL, NULL},
};
