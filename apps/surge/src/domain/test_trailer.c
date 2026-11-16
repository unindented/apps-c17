#include <acutest.h>

#include "core/arena.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "domain/trailer.h"
#include "testing/test_support.h"

/** Number of entries in a fixed array. */
#define COUNT(array) ((ptrdiff_t)(sizeof(array) / sizeof((array)[0])))

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

TEST_LIST = {
    {"name drops the address", test_name_drops_the_address},
    {"name keeps a value without an address", test_name_keeps_a_value_without_an_address},
    {"name of a bare address is empty", test_name_of_a_bare_address_is_empty},
    {"co authors lists the names in order", test_co_authors_lists_the_names_in_order},
    {"co authors drops repeats and bare addresses",
     test_co_authors_drops_repeats_and_bare_addresses},
    {"co authors of no trailers is empty", test_co_authors_of_no_trailers_is_empty},
    {NULL, NULL},
};
