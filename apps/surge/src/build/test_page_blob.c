#include <acutest.h>
#include <stddef.h>

#include "build/page_blob.h"
#include "core/arena.h"
#include "core/str.h"
#include "testing/test_support.h"

// Image files, in any case, are shown as images.
static void test_is_image_goes_by_extension(void) {
  TEST_CHECK(page_blob_is_image(S("logo.SVG")) && page_blob_is_image(S("a.jpeg")));
  TEST_CHECK(!page_blob_is_image(S(".png")) && !page_blob_is_image(S("png")));
}

// Each line is a code line, escaped, whose number links to itself, and a final newline ends the
// last line inside it.
static void test_lines_writes_a_code_line_per_line(void) {
  struct Arena arena = test_arena();
  ptrdiff_t line_count = 0;
  const struct Str html = page_blob_lines(S("a<b\n\nc\n"), NULL, &arena, test_arena(), &line_count);
  TEST_CHECK(line_count == 3);
  TEST_CHECK(test_text_is(html,
                          "<pre class=\"surge-code\"><code><span class=\"code-line\"><span "
                          "class=\"code-line-numbers\"><a class=\"code-line-number\" id=\"L1\" "
                          "href=\"#L1\">1</a></span><span class=\"code-line-text\">a&lt;b\n"
                          "</span></span><span class=\"code-line\"><span "
                          "class=\"code-line-numbers\"><a class=\"code-line-number\" id=\"L2\" "
                          "href=\"#L2\">2</a></span><span class=\"code-line-text\">\n"
                          "</span></span><span class=\"code-line\"><span "
                          "class=\"code-line-numbers\"><a class=\"code-line-number\" id=\"L3\" "
                          "href=\"#L3\">3</a></span><span class=\"code-line-text\">c\n"
                          "</span></span></code></pre>\n"));
}

// A last line without a newline is still a code line, and a byte that is not UTF-8 stays as it is,
// for the render to repair.
static void test_lines_keeps_a_last_line_and_its_bytes(void) {
  struct Arena arena = test_arena();
  ptrdiff_t line_count = 0;
  const struct Str html = page_blob_lines(S("x\xff"), NULL, &arena, test_arena(), &line_count);
  TEST_CHECK(line_count == 1);
  TEST_CHECK(test_text_is(html,
                          "<pre class=\"surge-code\"><code><span class=\"code-line\"><span "
                          "class=\"code-line-numbers\"><a class=\"code-line-number\" id=\"L1\" "
                          "href=\"#L1\">1</a></span><span "
                          "class=\"code-line-text\">x\xff</span></span></code></pre>\n"));
}

TEST_LIST = {
    {"is image goes by extension", test_is_image_goes_by_extension},
    {"lines writes a code line per line", test_lines_writes_a_code_line_per_line},
    {"lines keeps a last line and its bytes", test_lines_keeps_a_last_line_and_its_bytes},
    {NULL, NULL},
};
