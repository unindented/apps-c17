#include <acutest.h>

#include "build/page_paths.h"
#include "core/arena.h"
#include "core/str.h"
#include "testing/test_support.h"

// A per-ref page nests the ref's name and the path as directories, with an index page below.
static void test_path_nests_the_ref_and_the_path(void) {
  struct Arena arena = test_arena();
  TEST_CHECK(test_text_is(page_path(PAGE_PATH_TREE, S("r"), S("feature/x"), S("a/b"), &arena),
                          "r/tree/feature/x/a/b/index.html"));
  TEST_CHECK(test_text_is(page_path(PAGE_PATH_BLOB, S("r"), S("main"), S("a.c"), &arena),
                          "r/blob/main/a.c/index.html"));
  TEST_CHECK(test_text_is(page_path(PAGE_PATH_TREE, S("r"), S("main"), (struct Str){0}, &arena),
                          "r/tree/main/index.html"));
}

// A raw copy is the file itself, with no index page.
static void test_path_names_a_raw_copy_by_its_path(void) {
  struct Arena arena = test_arena();
  TEST_CHECK(test_text_is(page_path(PAGE_PATH_RAW, S("r"), S("main"), S("media/a b.png"), &arena),
                          "r/raw/main/media/a b.png"));
}

// A link encodes each segment of the ref's name and of the path, `+` included, and keeps the `/`
// between them.
static void test_link_encodes_each_segment(void) {
  struct Arena arena = test_arena();
  TEST_CHECK(test_text_is(
      page_path_link(S("../"), PAGE_PATH_BLOB, S("r"), S("f/ñ"), S("a b/c+d#e.txt"), &arena),
      "../r/blob/f/%%C3%%B1/a%%20b/c%%2Bd%%23e.txt/index.html"));
  TEST_CHECK(test_text_is(page_path_link((struct Str){0}, PAGE_PATH_COMMITS, S("r"), S("main"),
                                         (struct Str){0}, &arena),
                          "r/commits/main/index.html"));
}

TEST_LIST = {
    {"path nests the ref and the path", test_path_nests_the_ref_and_the_path},
    {"path names a raw copy by its path", test_path_names_a_raw_copy_by_its_path},
    {"link encodes each segment", test_link_encodes_each_segment},
    {NULL, NULL},
};
