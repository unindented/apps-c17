#include <acutest.h>

#include "build/page_log.h"
#include "core/arena.h"
#include "core/str.h"
#include "testing/test_support.h"

// A branch's commits fill pages of the page size, and a branch with none still has one page.
static void test_count_rounds_up_to_whole_pages(void) {
  TEST_CHECK(page_log_count(9, 2) == 5);
  TEST_CHECK(page_log_count(10, 2) == 5);
  TEST_CHECK(page_log_count(1, 100) == 1);
  TEST_CHECK(page_log_count(0, 100) == 1);
}

// The first page is the log's index, and the others `page-<n>.html` beside it, from 2.
static void test_path_names_the_first_page_index(void) {
  struct Arena arena = test_arena();
  TEST_CHECK(test_text_is(page_log_path(S("r"), S("feature/x"), 0, &arena),
                          "r/commits/feature/x/index.html"));
  TEST_CHECK(
      test_text_is(page_log_path(S("r"), S("main"), 2, &arena), "r/commits/main/page-3.html"));
}

TEST_LIST = {
    {"count rounds up to whole pages", test_count_rounds_up_to_whole_pages},
    {"path names the first page index", test_path_names_the_first_page_index},
    {NULL, NULL},
};
