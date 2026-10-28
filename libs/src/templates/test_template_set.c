#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/str.h"
#include "os/os_memory.h"
#include "templates/template_set.h"
#include "testing/test_support.h"

/** Directory of every test's templates. */
#define TEMPLATES_DIR "templates"

/** U+FFFD REPLACEMENT CHARACTER in UTF-8. */
#define FFFD "\xEF\xBF\xBD"

/** Failure of an unescaped interpolation of a node whose text is not HTML. */
#define UNESCAPED_FAILURE \
  "unescaped Mustache interpolation is supported only for a key that ends in '_html'"

/** The page template every test renders. */
static const struct Str PAGE_NAMES[] = {STR_INIT("page.html")};

/** Kind of a node of the tests' data. */
enum TestNodeKind {
  /** Text, which a tag writes. */
  TEST_NODE_TEXT,

  /** A map, whose children resolve by name. */
  TEST_NODE_MAP,

  /** A list, whose children resolve by index. */
  TEST_NODE_LIST,
};

/** A node of the tests' data: a small tree, as an app gives one through `struct TemplateData`. */
struct TestNode {
  /** Kind of the node. */
  enum TestNodeKind kind;

  /** Name the node has in its map. */
  struct Str name;

  /** Text of a `TEST_NODE_TEXT`. */
  struct Str text;

  /** Whether the text is HTML. */
  bool is_html;

  /** Children of a map or a list. */
  const struct TestNode* children;

  /** Number of entries in `children`. */
  ptrdiff_t child_count;
};

/** A text node named `n` that holds `t`. */
#define TEXT(n, t) {.kind = TEST_NODE_TEXT, .name = STR_INIT(n), .text = STR_INIT(t)}

/** A text node named `n` that holds the HTML `t`. */
#define HTML(n, t) \
  {.kind = TEST_NODE_TEXT, .name = STR_INIT(n), .text = STR_INIT(t), .is_html = true}

/** A map or list node named `n` over the array `c`. */
#define PARENT(k, n, c) \
  {.kind = (k), .name = STR_INIT(n), .children = (c), .child_count = sizeof(c) / sizeof((c)[0])}

/**
 * @brief Returns the root of a render's data, the node that `userdata` points at.
 *
 * @param arena    Arena of the render. Unused.
 * @param userdata The root `struct TestNode`.
 * @return The root.
 */
static const void* test_root(struct Arena* arena, void* userdata) {
  (void)arena;
  return userdata;
}

/**
 * @brief Returns the child of a map named `name`.
 *
 * @param node     Parent `struct TestNode`.
 * @param name     Child name.
 * @param arena    Arena of the render. Unused.
 * @param userdata Unused.
 * @return The child, or `NULL`.
 */
static const void* test_child_by_name(const void* node,
                                      struct Str name,
                                      struct Arena* arena,
                                      void* userdata) {
  (void)arena;
  (void)userdata;
  const struct TestNode* parent = node;
  if (parent->kind != TEST_NODE_MAP) {
    return NULL;
  }
  for (ptrdiff_t i = 0; i < parent->child_count; i++) {
    if (str_eq(parent->children[i].name, name)) {
      return &parent->children[i];
    }
  }
  return NULL;
}

/**
 * @brief Returns a list's item at `index`, or a true node itself at index `0`.
 *
 * @param node     Parent `struct TestNode`.
 * @param index    Zero-based index.
 * @param arena    Arena of the render. Unused.
 * @param userdata Unused.
 * @return The item, or `NULL`.
 */
static const void* test_child_by_index(const void* node,
                                       ptrdiff_t index,
                                       struct Arena* arena,
                                       void* userdata) {
  (void)arena;
  (void)userdata;
  const struct TestNode* parent = node;
  if (parent->kind == TEST_NODE_LIST) {
    return index < parent->child_count ? &parent->children[index] : NULL;
  }
  const bool is_true = parent->kind == TEST_NODE_MAP || parent->text.len > 0;
  return index == 0 && is_true ? node : NULL;
}

/**
 * @brief Returns what a tag of a node writes: a text node's text, and nothing for another node.
 *
 * @param node     The `struct TestNode`.
 * @param userdata Unused.
 * @return The node's text.
 */
static struct TemplateText test_text(const void* node, void* userdata) {
  (void)userdata;
  const struct TestNode* test_node = node;
  if (test_node->kind != TEST_NODE_TEXT) {
    return (struct TemplateText){0};
  }
  return (struct TemplateText){.text = test_node->text, .is_html = test_node->is_html};
}

/**
 * @brief Returns the data of a render over `root`.
 *
 * @param root Root of the data. Must not be `NULL`.
 * @return The data.
 */
static struct TemplateData test_data(const struct TestNode* root) {
  return (struct TemplateData){
      .root = test_root,
      .child_by_name = test_child_by_name,
      .child_by_index = test_child_by_index,
      .text = test_text,
      .userdata = (void*)(uintptr_t)root,
  };
}

/**
 * @brief Loads a set whose `page.html` is `page`, on a host that also holds any partials.
 *
 * @param os      Host holding `templates/partials/...` already. Must not be `NULL`.
 * @param page    Terminated text of `templates/page.html`. Must not be `NULL`.
 * @param set_out Receives the set on success. Must not be `NULL`.
 * @param log     Log that receives the failures. Must not be `NULL`.
 * @return The result of `template_set_load`.
 */
static int load_page(struct Os* os,
                     const char* page,
                     struct TemplateSet** set_out,
                     struct Log* log) {
  os_memory_write(os, TEMPLATES_DIR "/page.html", page);
  struct Arena perm = test_arena();
  return template_set_load(S(TEMPLATES_DIR), PAGE_NAMES, NULL, 1, os, &perm, test_arena(), set_out,
                           log);
}

/**
 * @brief Renders `page.html` of `set` over `root`, under `output_len_max`, for the output
 *        `out.html`.
 *
 * @param set            Loaded set. Must not be `NULL`.
 * @param root           Root of the data. Must not be `NULL`.
 * @param output_len_max Largest output accepted, in bytes.
 * @param output_out     Receives the output on success. Must not be `NULL`.
 * @param log            Log that receives the failure. Must not be `NULL`.
 * @return The result of `template_set_render`.
 */
static int render_limited(const struct TemplateSet* set,
                          const struct TestNode* root,
                          ptrdiff_t output_len_max,
                          struct Str* output_out,
                          struct Log* log) {
  const struct TemplateData data = test_data(root);
  struct Arena perm = test_arena();
  return template_set_render(set, S("page.html"), &data, S("out.html"), output_len_max, &perm,
                             test_arena(), output_out, log);
}

/**
 * @brief Renders `page.html` of `set` over `root`, and reports whether it rendered `expected`.
 *
 * @param set      Loaded set. Must not be `NULL`.
 * @param root     Root of the data. Must not be `NULL`.
 * @param expected Terminated text the render must produce. Must not be `NULL`.
 * @return `true` when the render succeeded with exactly `expected` and logged nothing.
 */
static bool renders(const struct TemplateSet* set,
                    const struct TestNode* root,
                    const char* expected) {
  struct Log log = test_log();
  struct Str output = {0};
  const bool is_rendered =
      render_limited(set, root, TEMPLATE_RENDER_OUTPUT_LEN_MAX, &output, &log) == 0;
  return is_rendered && test_log_is_empty(&log) && test_text_is(output, "%s", expected);
}

/**
 * @brief Renders `page.html` of `set` over `root`, and reports whether it failed with exactly
 *        `expected` in its log.
 *
 * @param set      Loaded set. Must not be `NULL`.
 * @param root     Root of the data. Must not be `NULL`.
 * @param expected Terminated text of the log. Must not be `NULL`.
 * @return `true` when the render failed and logged exactly `expected`.
 */
static bool fails(const struct TemplateSet* set,
                  const struct TestNode* root,
                  const char* expected) {
  struct Log log = test_log();
  struct Str output = {0};
  const bool is_failed =
      render_limited(set, root, TEMPLATE_RENDER_OUTPUT_LEN_MAX, &output, &log) == -1;
  return is_failed && test_log_is(&log, "%s", expected);
}

/**
 * @brief Loads a set over one partial more than `TEMPLATE_PARTIAL_COUNT_MAX`, `p00.html` to
 *        `p64.html`, written in ascending or descending order of name, and reports whether the load
 *        refused the last one in name order.
 *
 * @param is_descending Whether the partials are written from the last name to the first.
 * @return `true` when the load failed and named exactly `templates/partials/p64.html`.
 */
static bool is_extra_partial_refused(bool is_descending) {
  struct Os* os = os_memory();
  for (ptrdiff_t i = 0; i <= TEMPLATE_PARTIAL_COUNT_MAX; i++) {
    const ptrdiff_t index = is_descending ? TEMPLATE_PARTIAL_COUNT_MAX - i : i;
    char partial_path[64];
    (void)snprintf(partial_path, sizeof(partial_path), TEMPLATES_DIR "/partials/p%02td.html",
                   index);
    os_memory_write(os, partial_path, "x");
  }
  struct TemplateSet* set = NULL;
  struct Log log = test_log();
  const bool is_refused = load_page(os, "page", &set, &log) == -1;
  return is_refused && test_log_is(&log,
                                   "templates exceed max partial count (%d): "
                                   "'" TEMPLATES_DIR "/partials/p%02d.html'\n",
                                   TEMPLATE_PARTIAL_COUNT_MAX, TEMPLATE_PARTIAL_COUNT_MAX);
}

/** The data of most tests: a map with text, HTML, an empty value, a list and a nested map. */
static const struct TestNode ITEMS[] = {TEXT("", "a"), TEXT("", "b")};
static const struct TestNode SITE[] = {TEXT("author", "Ada")};
static const struct TestNode FIELDS[] = {
    TEXT("x", "<a&b>"), HTML("x_html", "<b>a&amp;b</b>"),       HTML("empty_html", ""),
    TEXT("empty", ""),  PARENT(TEST_NODE_LIST, "items", ITEMS), PARENT(TEST_NODE_MAP, "site", SITE),
};
static const struct TestNode ROOT = PARENT(TEST_NODE_MAP, "", FIELDS);

// Text is escaped by a double mustache, and HTML is written as it is by a triple mustache and by an
// ampersand tag. Empty HTML writes nothing and does not fail.
static void test_render_escapes_text(void) {
  struct Os* os = os_memory();
  struct TemplateSet* set = NULL;
  struct Log log = test_log();
  TEST_ASSERT(load_page(os, "{{x}}|{{{x_html}}}|{{& x_html}}|{{{empty_html}}}", &set, &log) == 0);
  TEST_CHECK(renders(set, &ROOT, "&lt;a&amp;b&gt;|<b>a&amp;b</b>|<b>a&amp;b</b>|"));
  template_set_release(set);
}

// A section enters a list once per item and a true value once, and skips empty text, and an
// inverted section does the opposite. A nested name resolves through its map.
static void test_render_enters_sections_by_truthiness(void) {
  struct Os* os = os_memory();
  struct TemplateSet* set = NULL;
  struct Log log = test_log();
  TEST_ASSERT(load_page(os,
                        "{{#items}}[{{.}}]{{/items}}{{#x}}x{{/x}}{{#empty}}e{{/empty}}"
                        "{{^empty}}!e{{/empty}}{{^items}}!i{{/items}}|{{site.author}}",
                        &set, &log) == 0);
  TEST_CHECK(renders(set, &ROOT, "[a][b]x!e|Ada"));
  template_set_release(set);
}

// Every text a render writes is valid UTF-8 without what XML 1.0 forbids, whatever the node: an
// invalid byte becomes U+FFFD and a control character is dropped, in an escaped value, a nested
// one, HTML, and the literal text of the template.
static void test_render_repairs_every_text(void) {
  static const struct TestNode site[] = {TEXT("author",
                                              "A\x0c"
                                              "b")};
  static const struct TestNode fields[] = {
      TEXT("x", "caf\xe9\x01"),
      HTML("x_html", "<b>\xff\xef\xbf\xbf</b>"),
      PARENT(TEST_NODE_MAP, "site", site),
  };
  static const struct TestNode root = PARENT(TEST_NODE_MAP, "", fields);
  struct Os* os = os_memory();
  struct TemplateSet* set = NULL;
  struct Log log = test_log();
  TEST_ASSERT(load_page(os, "{{x}}|{{site.author}}|{{{x_html}}}|a\x0c", &set, &log) == 0);
  TEST_CHECK(renders(set, &root, "caf" FFFD "|Ab|<b>" FFFD "</b>|a"));
  template_set_release(set);
}

// An unescaped tag of a name the data does not hold writes nothing and does not fail, whatever the
// name ends in, as an escaped tag of it does.
static void test_render_unescaped_unknown_name_renders_empty(void) {
  struct Os* os = os_memory();
  struct TemplateSet* set = NULL;
  struct Log log = test_log();
  TEST_ASSERT(load_page(os, "[{{{typo_html}}}|{{& typo}}]", &set, &log) == 0);
  TEST_CHECK(renders(set, &ROOT, "[|]"));
  template_set_release(set);
}

// A partial resolves by its file name without `.html`, also from inside another partial.
static void test_render_resolves_partials(void) {
  struct Os* os = os_memory();
  os_memory_write(os, TEMPLATES_DIR "/partials/outer.html", "(<{{>inner}}>)");
  os_memory_write(os, TEMPLATES_DIR "/partials/inner.html", "{{x}}");
  struct TemplateSet* set = NULL;
  struct Log log = test_log();
  TEST_ASSERT(load_page(os, "{{>outer}}", &set, &log) == 0);
  TEST_CHECK(renders(set, &ROOT, "(<&lt;a&amp;b&gt;>)"));
  template_set_release(set);
}

// A standalone partial indents each line it writes by the tag's indentation, inside a nested
// partial by both. A standalone section or comment tag writes nothing, its indentation included, so
// the line after it is indented once, as any other.
static void test_render_indents_each_line_of_a_partial_once(void) {
  struct Os* os = os_memory();
  os_memory_write(
      os, TEMPLATES_DIR "/partials/items.html",
      "<li>a</li>\n{{#x}}\n<li>x</li>\n{{/x}}\n{{! a note }}\n<li>b</li>\n  {{> deep}}\n");
  os_memory_write(os, TEMPLATES_DIR "/partials/deep.html", "{{#x}}\n<i>d</i>\n{{/x}}\n<i>e</i>\n");
  struct TemplateSet* set = NULL;
  struct Log log = test_log();
  TEST_ASSERT(load_page(os, "<ul>\n  {{> items}}\n</ul>\n", &set, &log) == 0);
  TEST_CHECK(renders(set, &ROOT,
                     "<ul>\n  <li>a</li>\n  <li>x</li>\n  <li>b</li>\n    <i>d</i>\n    <i>e</i>\n"
                     "</ul>\n"));
  template_set_release(set);
}

// A file of `partials/` whose name is not an identifier, a file that does not end in `.html`, and a
// subdirectory are not partials, so the load ignores a broken one, and a reference to one fails.
static void test_load_ignores_files_that_are_not_partials(void) {
  struct Os* os = os_memory();
  os_memory_write(os, TEMPLATES_DIR "/partials/a.b.html", "{{#x}}");
  os_memory_write(os, TEMPLATES_DIR "/partials/notes.txt", "{{#x}}");
  os_memory_write(os, TEMPLATES_DIR "/partials/sub/card.html", "{{#x}}");
  struct TemplateSet* set = NULL;
  struct Log log = test_log();
  TEST_ASSERT(load_page(os, "{{>notes}}", &set, &log) == 0);
  TEST_CHECK(fails(set, &ROOT,
                   "partial 'notes' is not in the templates ('partials/notes.html') (in "
                   "'page.html') (while rendering 'out.html')\n"));
  template_set_release(set);
}

// A partial included in a section renders once per item, and two partials that include each other
// stop at the expansion limit, as a partial that includes itself does.
static void test_render_repeats_partials_and_stops_a_cycle(void) {
  struct Os* os = os_memory();
  os_memory_write(os, TEMPLATES_DIR "/partials/item.html", "<{{.}}>");
  os_memory_write(os, TEMPLATES_DIR "/partials/a.html", "{{>b}}");
  os_memory_write(os, TEMPLATES_DIR "/partials/b.html", "{{>a}}");
  struct TemplateSet* set = NULL;
  struct Log log = test_log();
  TEST_ASSERT(load_page(os, "{{#items}}{{>item}}{{/items}}", &set, &log) == 0);
  TEST_CHECK(renders(set, &ROOT, "<a><b>"));
  template_set_release(set);
  TEST_ASSERT(load_page(os, "{{>a}}", &set, &log) == 0);
  char expected[256];
  (void)snprintf(expected, sizeof(expected),
                 "render exceeds max partial expansions (%d); check for a partial that includes "
                 "itself (in 'page.html') (while rendering 'out.html')\n",
                 TEMPLATE_RENDER_EXPANSION_COUNT_MAX);
  TEST_CHECK(fails(set, &ROOT, expected));
  template_set_release(set);
}

// An empty template renders nothing, and a comment writes nothing.
static void test_render_writes_nothing_for_an_empty_template_or_a_comment(void) {
  struct Os* os = os_memory();
  struct TemplateSet* set = NULL;
  struct Log log = test_log();
  TEST_ASSERT(load_page(os, "", &set, &log) == 0);
  TEST_CHECK(renders(set, &ROOT, ""));
  template_set_release(set);
  TEST_ASSERT(load_page(os, "a{{! note }}b", &set, &log) == 0);
  TEST_CHECK(renders(set, &ROOT, "ab"));
  template_set_release(set);
}

// A failure ends with the output that the render is for, and with no attribution when it names
// none.
static void test_render_names_its_output(void) {
  struct Os* os = os_memory();
  struct TemplateSet* set = NULL;
  struct Log log = test_log();
  TEST_ASSERT(load_page(os, "{{{x}}}", &set, &log) == 0);
  TEST_CHECK(
      fails(set, &ROOT, UNESCAPED_FAILURE " (in 'page.html') (while rendering 'out.html')\n"));
  const struct TemplateData data = test_data(&ROOT);
  struct Arena perm = test_arena();
  struct Str output = {0};
  TEST_CHECK(template_set_render(set, S("page.html"), &data, (struct Str){0},
                                 TEMPLATE_RENDER_OUTPUT_LEN_MAX, &perm, test_arena(), &output,
                                 &log) == -1);
  TEST_CHECK(test_log_is(&log, UNESCAPED_FAILURE " (in 'page.html')\n"));
  template_set_release(set);
}

// An unescaped tag of any node whose text is not HTML fails the render, text, empty text and a list
// alike, so a value the app did not make cannot add markup.
static void test_render_rejects_unescaped_interpolation_of_other_nodes(void) {
  static const char* const pages[] = {"{{{x}}}", "{{& empty}}", "{{{items}}}"};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(pages) / sizeof(pages[0])); i++) {
    struct Os* os = os_memory();
    struct TemplateSet* set = NULL;
    struct Log log = test_log();
    TEST_ASSERT(load_page(os, pages[i], &set, &log) == 0);
    TEST_CHECK(
        fails(set, &ROOT, UNESCAPED_FAILURE " (in 'page.html') (while rendering 'out.html')\n"));
    TEST_MSG("page: '%s'", pages[i]);
    template_set_release(set);
  }
}

// A reference to a partial that the set does not hold fails, naming the template that holds the
// reference: the page, or the partial that refers to it.
static void test_render_reports_unknown_partial(void) {
  struct Os* os = os_memory();
  os_memory_write(os, TEMPLATES_DIR "/partials/card.html", "{{>missing}}");
  struct TemplateSet* set = NULL;
  struct Log log = test_log();
  TEST_ASSERT(load_page(os, "{{>missing}}", &set, &log) == 0);
  TEST_CHECK(fails(set, &ROOT,
                   "partial 'missing' is not in the templates ('partials/missing.html') (in "
                   "'page.html') (while rendering 'out.html')\n"));
  template_set_release(set);
  TEST_ASSERT(load_page(os, "a{{>card}}", &set, &log) == 0);
  TEST_CHECK(fails(set, &ROOT,
                   "partial 'missing' is not in the templates ('partials/missing.html') (in "
                   "'partials/card.html') (while rendering 'out.html')\n"));
  template_set_release(set);
}

// A partial name that is not an identifier fails as one, naming the template that holds it.
static void test_render_reports_invalid_partial_name(void) {
  struct Os* os = os_memory();
  os_memory_write(os, TEMPLATES_DIR "/partials/card.html", "{{>a/b}}");
  struct TemplateSet* set = NULL;
  struct Log log = test_log();
  TEST_ASSERT(load_page(os, "{{>card}}", &set, &log) == 0);
  TEST_CHECK(fails(set, &ROOT,
                   "partial name must contain only letters, digits, '_' and '-': 'a/b' (in "
                   "'partials/card.html') (while rendering 'out.html')\n"));
  template_set_release(set);
}

// A partial that includes itself stops at the expansion limit.
static void test_render_stops_a_partial_that_includes_itself(void) {
  struct Os* os = os_memory();
  os_memory_write(os, TEMPLATES_DIR "/partials/loop.html", "{{>loop}}");
  struct TemplateSet* set = NULL;
  struct Log log = test_log();
  TEST_ASSERT(load_page(os, "{{>loop}}", &set, &log) == 0);
  char expected[256];
  (void)snprintf(expected, sizeof(expected),
                 "render exceeds max partial expansions (%d); check for a partial that includes "
                 "itself (in 'page.html') (while rendering 'out.html')\n",
                 TEMPLATE_RENDER_EXPANSION_COUNT_MAX);
  TEST_CHECK(fails(set, &ROOT, expected));
  template_set_release(set);
}

// A render whose output passes its bound fails, naming the bound and the length it reached, and a
// repair that grows the output counts: one invalid byte is the three bytes of U+FFFD.
static void test_render_limits_output(void) {
  static const struct TestNode fields[] = {TEXT("x", "\xff\xff")};
  static const struct TestNode root = PARENT(TEST_NODE_MAP, "", fields);
  struct Os* os = os_memory();
  struct TemplateSet* set = NULL;
  struct Log log = test_log();
  TEST_ASSERT(load_page(os, "0123456789", &set, &log) == 0);
  struct Str output = {0};
  TEST_CHECK(render_limited(set, &ROOT, 4, &output, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "render exceeds max rendered output (4 bytes) at 10 bytes (in "
                         "'page.html') (while rendering 'out.html')\n"));
  template_set_release(set);
  TEST_ASSERT(load_page(os, "{{x}}", &set, &log) == 0);
  TEST_CHECK(render_limited(set, &root, 6, &output, &log) == 0);
  TEST_CHECK(test_text_is(output, FFFD FFFD));
  log = test_log();
  TEST_CHECK(render_limited(set, &root, 5, &output, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "render exceeds max rendered output (5 bytes) at 6 bytes (in "
                         "'page.html') (while rendering 'out.html')\n"));
  template_set_release(set);
}

// A load reports every broken template in one run: a missing page template, and a syntax error, at
// its line and column, in each file that has one.
static void test_load_reports_every_broken_template(void) {
  struct Os* os = os_memory();
  os_memory_write(os, TEMPLATES_DIR "/partials/a.html", "ok\n{{#x}}");
  os_memory_write(os, TEMPLATES_DIR "/partials/b.html", "{{/y}}");
  static const struct Str page_names[] = {STR_INIT("missing.html"), STR_INIT("page.html")};
  os_memory_write(os, TEMPLATES_DIR "/page.html", "x\n\n{{/y}}");
  struct Arena perm = test_arena();
  struct TemplateSet* set = NULL;
  struct Log log = test_log();
  TEST_CHECK(template_set_load(S(TEMPLATES_DIR), page_names, NULL, 2, os, &perm, test_arena(), &set,
                               &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to read template: No such file or directory ('" TEMPLATES_DIR
                         "/missing.html')\n"
                         "%s at line 3, column 1 (in 'page.html')\n"
                         "%s at line 2, column 1 (in 'partials/a.html')\n"
                         "%s at line 1, column 1 (in 'partials/b.html')\n",
                         "section-closing tag has no opener", "section-opening tag has no closer",
                         "section-closing tag has no opener"));
  TEST_MSG("log: '%s'", buf_cstr(&log.buf));
}

// A tag with no name, or a section closed by another name, fails the load with mustache4c's cause,
// at its line and column.
static void test_load_reports_malformed_tags(void) {
  static const struct {
    const char* page;
    const char* expected;
  } cases[] = {
      {"{{}}", "tag has no name at line 1, column 1 (in 'page.html')\n"},
      {"{{#a}}{{/b}}",
       "name of section-closing tag does not match corresponding section-opening tag at line 1, "
       "column 7 (in 'page.html')\n"},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct Os* os = os_memory();
    struct TemplateSet* set = NULL;
    struct Log log = test_log();
    TEST_CHECK(load_page(os, cases[i].page, &set, &log) == -1);
    TEST_CHECK(test_log_is(&log, "%s", cases[i].expected));
    TEST_MSG("page: '%s'", cases[i].page);
  }
}

// A partial that cannot be inspected fails the load, rather than a later reference to it as a
// partial that is missing.
static void test_load_reports_a_partial_that_cannot_be_inspected(void) {
  struct Os* os = os_memory();
  os_memory_write(os, TEMPLATES_DIR "/partials/card.html", "x");
  os_memory_fail(os, OS_MEMORY_OP_STAT, TEMPLATES_DIR "/partials/card.html", EACCES);
  struct TemplateSet* set = NULL;
  struct Log log = test_log();
  TEST_CHECK(load_page(os, "page", &set, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to list partials: cannot inspect entry: %s ('" TEMPLATES_DIR
                         "/partials/card.html')\n",
                         strerror(EACCES)));
}

// A page template that a source names, and that cannot be read, names that source.
static void test_load_names_the_owner_of_a_missing_page(void) {
  struct Os* os = os_memory();
  static const struct Str page_names[] = {STR_INIT("a.html"), STR_INIT("b.html")};
  static const struct Str page_owners[] = {STR_INIT("content/a.md"), STR_INIT("")};
  struct Arena perm = test_arena();
  struct TemplateSet* set = NULL;
  struct Log log = test_log();
  TEST_CHECK(template_set_load(S(TEMPLATES_DIR), page_names, page_owners, 2, os, &perm,
                               test_arena(), &set, &log) == -1);
  TEST_CHECK(test_log_is(
      &log, "failed to read template: No such file or directory ('" TEMPLATES_DIR "/a.html') (for "
            "'content/a.md')\n"
            "failed to read template: No such file or directory ('" TEMPLATES_DIR "/b.html')\n"));
}

// A template or a partial over the file limit, or with a `NUL` byte, fails its read.
static void test_load_rejects_oversize_and_nul_templates(void) {
  struct Os* os = os_memory();
  const ptrdiff_t file_len = (ptrdiff_t)TEMPLATE_FILE_LEN_MAX + 1;
  os_memory_write(os, TEMPLATES_DIR "/partials/big.html", "");
  os_memory_truncate(os, TEMPLATES_DIR "/partials/big.html", file_len);
  static const char bytes[] = {'a', '\0', 'b'};
  os_memory_write_bytes(os, S(TEMPLATES_DIR "/page.html"), (struct Str){bytes, sizeof(bytes)});
  struct Arena perm = test_arena();
  struct TemplateSet* set = NULL;
  struct Log log = test_log();
  TEST_CHECK(template_set_load(S(TEMPLATES_DIR), PAGE_NAMES, NULL, 1, os, &perm, test_arena(), &set,
                               &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to read template: contains an embedded NUL byte ('" TEMPLATES_DIR
                         "/page.html')\n"
                         "failed to read partial: exceeds max file size (%d bytes) at %td bytes "
                         "('" TEMPLATES_DIR "/partials/big.html')\n",
                         TEMPLATE_FILE_LEN_MAX, file_len));
}

// A set holds at most `TEMPLATE_PARTIAL_COUNT_MAX` partials, and the one it refuses is the first
// past the limit in name order, whatever order the files were written in.
static void test_load_refuses_partials_past_the_limit_in_name_order(void) {
  TEST_CHECK(is_extra_partial_refused(false));
  TEST_CHECK(is_extra_partial_refused(true));
}

TEST_LIST = {
    {"render escapes text", test_render_escapes_text},
    {"render enters sections by truthiness", test_render_enters_sections_by_truthiness},
    {"render repairs every text", test_render_repairs_every_text},
    {"render unescaped unknown name renders empty",
     test_render_unescaped_unknown_name_renders_empty},
    {"render resolves partials", test_render_resolves_partials},
    {"render indents each line of a partial once", test_render_indents_each_line_of_a_partial_once},
    {"render repeats partials and stops a cycle", test_render_repeats_partials_and_stops_a_cycle},
    {"render writes nothing for an empty template or a comment",
     test_render_writes_nothing_for_an_empty_template_or_a_comment},
    {"load ignores files that are not partials", test_load_ignores_files_that_are_not_partials},
    {"render names its output", test_render_names_its_output},
    {"render rejects unescaped interpolation of other nodes",
     test_render_rejects_unescaped_interpolation_of_other_nodes},
    {"render reports unknown partial", test_render_reports_unknown_partial},
    {"render reports invalid partial name", test_render_reports_invalid_partial_name},
    {"render stops a partial that includes itself",
     test_render_stops_a_partial_that_includes_itself},
    {"render limits output", test_render_limits_output},
    {"load reports every broken template", test_load_reports_every_broken_template},
    {"load reports malformed tags", test_load_reports_malformed_tags},
    {"load reports a partial that cannot be inspected",
     test_load_reports_a_partial_that_cannot_be_inspected},
    {"load names the owner of a missing page", test_load_names_the_owner_of_a_missing_page},
    {"load rejects oversize and nul templates", test_load_rejects_oversize_and_nul_templates},
    {"load refuses partials past the limit in name order",
     test_load_refuses_partials_past_the_limit_in_name_order},
    {NULL, NULL},
};
