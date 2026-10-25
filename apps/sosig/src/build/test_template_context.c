#include <acutest.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "build/template_context.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/str.h"
#include "domain/content_entry.h"
#include "domain/site_config.h"
#include "domain/sosig_version.h"
#include "os/os_memory.h"
#include "templates/template_set.h"
#include "testing/test_support.h"

// Every test here runs against the in-memory host, which holds the templates and partials a render
// reads. `test_template_context_posix.c` renders the templates of a golden site, which only the
// real filesystem holds. `libs/src/templates/test_template_set.c` covers the engine itself.

/** Directory the templates are written to and rendered from, with partials below `partials/`. */
#define TEMPLATES_DIR "templates"

/**
 * Failure of an unescaped interpolation of a key that does not end in `_html`, which a test follows
 * with the `(in '<template>')` of the template being rendered.
 */
#define UNESCAPED_FAILURE \
  "unescaped Mustache interpolation is supported only for a key that ends in '_html'"

/**
 * @brief Initializes shared site metadata for template tests.
 *
 * The assigned fields are string literals.
 *
 * @param site_config Configuration to initialize. Must not be `NULL`.
 */
static void init_test_site_config(struct SiteConfig* site_config) {
  site_config_init(site_config);
  site_config->title = S("Site");
  site_config->description = S("Notes & more");
  site_config->author = S("Author");
  site_config->base_url = S("https://example.com");
}

/**
 * @brief Initializes shared content-entry metadata for template tests.
 *
 * The assigned fields are string literals.
 *
 * @param entry Content entry to initialize. Must not be `NULL`.
 */
static void init_test_content_entry(struct ContentEntry* entry) {
  *entry = (struct ContentEntry){0};
  entry->title = S("Content Entry");
  entry->date = S("2026-07-01T00:00:00Z");
  entry->description = S("Desc");
  entry->slug = S("content-entry");
  entry->url_path = S("/content-entry.html");
  entry->body_html = S("<p>raw</p>");
}

/**
 * @brief Writes a template or a partial below `TEMPLATES_DIR`, creating its parent directories.
 *
 * @param os   Host to write to. Must not be `NULL`.
 * @param name Terminated path below `TEMPLATES_DIR`, such as `partials/card.html`. Must not be
 *             `NULL`.
 * @param text Terminated template text. Must not be `NULL`.
 */
static void write_template(struct Os* os, const char* name, const char* text) {
  struct Arena arena = test_arena();
  struct Buf path = {.arena = &arena};
  buf_printf(&path, "%s/%s", TEMPLATES_DIR, name);
  os_memory_write(os, buf_cstr(&path), text);
}

/**
 * @brief Returns a fresh host holding one template, `index.html`.
 *
 * @param text Terminated template text. Must not be `NULL`.
 * @return The host.
 */
static struct Os* template_host(const char* text) {
  struct Os* os = os_memory();
  write_template(os, "index.html", text);
  return os;
}

/**
 * @brief Reports whether a log holds exactly one render failure, of the output `output_name`.
 *
 * A render attributes its failure to the output it was for, so the expected line is the failure
 * followed by `(while rendering '<output>')`.
 *
 * @param log         Log the render wrote. Must not be `NULL`.
 * @param output_name Terminated name of the output the render was for. Must not be `NULL`.
 * @param failure     Terminated failure the log must hold, without the attribution. Must not be
 *                    `NULL`.
 * @return `true` when the log holds that one failure.
 */
static bool is_log_one_failure(const struct Log* log,
                               const char* output_name,
                               const char* failure) {
  return test_log_is(log, "%s (while rendering '%s')\n", failure, output_name) &&
         TEST_CHECK(log->errors == 1);
}

/**
 * @brief Loads a set whose one page template is `template_name`, renders it through
 *        `template_context_render` for the output of the same name, and hands back the HTML as a C
 *        string.
 *
 * This is a deliberate test-only bridge back to C strings. Every assertion below inspects rendered
 * text with C string functions such as `strcmp` and `strstr`, so the helper hands it back
 * terminated, copied after the HTML in `perm`. Rendered output holds no `NUL` byte, so the copy
 * loses nothing.
 *
 * @param template_name          Terminated name of a template below `TEMPLATES_DIR`.
 * @param context                Template context. Must not be `NULL`.
 * @param os                     Host holding the templates. Must not be `NULL`.
 * @param perm                   Arena that owns the set, the HTML and its terminated copy. Must not
 *                               be `NULL`.
 * @param html_out               Receives the terminated HTML on success, or `NULL` when the test
 *                               asserts only the result.
 * @param is_entry_list_read_out Receives whether the render read the entry list, or `NULL`.
 * @param log                    Log that receives the failure, or `NULL` when the test asserts only
 *                               the result.
 * @return `0` on success, or `-1` when the load or the render failed.
 */
static int render_reading(const char* template_name,
                          const struct TemplateContext* context,
                          struct Os* os,
                          struct Arena* perm,
                          char** html_out,
                          bool* is_entry_list_read_out,
                          struct Log* log) {
  struct Log discarded = {0};
  if (log == NULL) {
    discarded = test_log();
    log = &discarded;
  }
  const struct Str name = str_from_cstr(template_name);
  struct TemplateSet* set = NULL;
  if (template_set_load(S(TEMPLATES_DIR), &name, NULL, 1, os, perm, test_arena(), &set, log) != 0) {
    return -1;
  }
  struct Str html = {0};
  const int rc = template_context_render(set, name, context, name, perm, test_arena(), &html,
                                         is_entry_list_read_out, log);
  template_set_release(set);
  if (rc != 0) {
    return -1;
  }
  if (html_out != NULL) {
    *html_out = str_cstr(html, perm);
  }
  return 0;
}

/**
 * @brief Renders as `render_reading` does, without asking whether the render read the entry list.
 *
 * @param template_name Terminated name of a template below `TEMPLATES_DIR`.
 * @param context       Template context. Must not be `NULL`.
 * @param os            Host holding the templates. Must not be `NULL`.
 * @param perm          Arena that owns the set, the HTML and its terminated copy. Must not be
 *                      `NULL`.
 * @param html_out      Receives the terminated HTML on success, or `NULL`.
 * @param log           Log that receives the failure, or `NULL`.
 * @return `0` on success, or `-1` when the load or the render failed.
 */
static int render(const char* template_name,
                  const struct TemplateContext* context,
                  struct Os* os,
                  struct Arena* perm,
                  char** html_out,
                  struct Log* log) {
  return render_reading(template_name, context, os, perm, html_out, NULL, log);
}

// A render reports whether it read the entry list: `content_entries` depends on every entry,
// directly or through a partial, and the `site.*` fields and the current entry's own fields do not.
static void test_reports_whether_it_read_the_entry_list(void) {
  static const struct {
    const char* text;
    bool is_entry_list_read;
  } cases[] = {
      {"{{title}} {{site.title}} {{generator}}", false},
      {"{{#content_entries}}{{title}}{{/content_entries}}", true},
      {"{{^content_entries}}none{{/content_entries}}", true},
      {"{{>card}}", true},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct Arena perm = test_arena();
    struct Os* os = template_host(cases[i].text);
    write_template(os, "partials/card.html", "{{#content_entries}}{{/content_entries}}");
    struct SiteConfig site_config;
    init_test_site_config(&site_config);
    struct ContentEntry entry;
    init_test_content_entry(&entry);
    const struct TemplateContext context = {.site_config = &site_config,
                                            .content_entry_current = &entry};
    bool is_entry_list_read = !cases[i].is_entry_list_read;
    struct Log log = test_log();
    TEST_CHECK(render_reading("index.html", &context, os, &perm, NULL, &is_entry_list_read, &log) ==
               0);
    TEST_CHECK_(is_entry_list_read == cases[i].is_entry_list_read, "'%s'", cases[i].text);
  }
}

// Content entries and their tag lists iterate with escaping.
static void test_iterates_content_entries_and_tags(void) {
  struct Arena perm = test_arena();
  struct Os* os = template_host(
      "{{#content_entries}}{{title}}: {{#tags}}[{{.}}]{{/tags}}\n"
      "{{/content_entries}}");

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry first;
  struct ContentEntry second;
  init_test_content_entry(&first);
  init_test_content_entry(&second);
  const struct ContentEntry* content_entries[] = {&first, &second};
  const struct TemplateContext context = {
      .site_config = &site_config, .content_entries = content_entries, .content_entry_count = 2};
  char* rendered_html = NULL;
  static struct Str first_tags[] = {STR_INIT("c"), STR_INIT("x&y")};
  static struct Str second_tags[] = {STR_INIT("z")};
  first.title = S("One <A>");
  first.tags = first_tags;
  first.tag_count = 2;
  second.title = S("Two");
  second.tags = second_tags;
  second.tag_count = 1;
  if (!TEST_CHECK(render("index.html", &context, os, &perm, &rendered_html, NULL) == 0)) {
    return;
  }
  TEST_CHECK(strcmp(rendered_html, "One &lt;A&gt;: [c][x&amp;y]\nTwo: [z]\n") == 0);
}

// An empty tag renders as an empty item and does not end the iteration. `NULL` from the child
// callback means end-of-list to mustache4c, so a tag list must yield a node for every tag, even an
// empty one, or an empty tag would silently drop every tag after it. Frontmatter accepts
// `tags = ["", "x"]`, so this is reachable from a real site.
static void test_empty_tag_does_not_truncate_tag_list(void) {
  struct Arena perm = test_arena();
  struct Os* os =
      template_host("{{#content_entries}}{{#tags}}[{{.}}]{{/tags}}{{/content_entries}}");

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct ContentEntry* content_entries[] = {&entry};
  const struct TemplateContext context = {
      .site_config = &site_config, .content_entries = content_entries, .content_entry_count = 1};
  char* rendered_html = NULL;
  init_test_content_entry(&entry);
  static struct Str tags[] = {STR_INIT("a"), STR_INIT(""), STR_INIT("b")};
  entry.tags = tags;
  entry.tag_count = 3;
  if (!TEST_CHECK(render("index.html", &context, os, &perm, &rendered_html, NULL) == 0)) {
    return;
  }
  TEST_CHECK(strcmp(rendered_html, "[a][][b]") == 0);
}

// Nested sections re-iterate content entries in an inner context.
static void test_nested_section_reiterates_content_entries(void) {
  struct Arena perm = test_arena();
  struct Os* os = template_host(
      "{{#content_entries}}"
      "["
      "{{title}}:"
      "{{#content_entries}}"
      "{{title}},"
      "{{/content_entries}}"
      "]"
      "{{/content_entries}}");

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry first;
  struct ContentEntry second;
  init_test_content_entry(&first);
  init_test_content_entry(&second);
  const struct ContentEntry* content_entries[] = {&first, &second};
  const struct TemplateContext context = {
      .site_config = &site_config, .content_entries = content_entries, .content_entry_count = 2};
  char* rendered_html = NULL;
  first.title = S("One");
  second.title = S("Two");
  if (!TEST_CHECK(render("index.html", &context, os, &perm, &rendered_html, NULL) == 0)) {
    return;
  }
  TEST_CHECK(strcmp(rendered_html, "[One:One,Two,][Two:One,Two,]") == 0);
}

// Double-brace variables are escaped, and a key that ends in `_html` stays raw in both a
// triple-brace and an ampersand tag. An empty one writes nothing and does not fail.
static void test_escapes_double_brace_and_leaves_html_key_raw(void) {
  struct Arena perm = test_arena();
  struct Os* os = template_host("{{title}} [{{{body_html}}}|{{& body_html}}]\n");

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct TemplateContext context = {.site_config = &site_config,
                                          .content_entry_current = &entry};
  char* rendered_html = NULL;
  init_test_content_entry(&entry);
  entry.title = S("Content Entry <One>");
  if (!TEST_CHECK(render("index.html", &context, os, &perm, &rendered_html, NULL) == 0)) {
    return;
  }
  TEST_CHECK(strcmp(rendered_html, "Content Entry &lt;One&gt; [<p>raw</p>|<p>raw</p>]\n") == 0);

  entry.body_html = S("");
  if (!TEST_CHECK(render("index.html", &context, os, &perm, &rendered_html, NULL) == 0)) {
    return;
  }
  TEST_CHECK(strcmp(rendered_html, "Content Entry &lt;One&gt; [|]\n") == 0);
}

// `generator` names the tool and its version, at the root and inside a section.
static void test_renders_generator(void) {
  struct Arena perm = test_arena();
  struct Os* os =
      template_host("{{generator}}|{{#content_entries}}{{generator}}{{/content_entries}}");

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct ContentEntry* content_entries[] = {&entry};
  const struct TemplateContext context = {
      .site_config = &site_config, .content_entries = content_entries, .content_entry_count = 1};
  const struct Str version = sosig_version_string();
  char* rendered_html = NULL;
  char expected[64];
  int expected_len = 0;
  init_test_content_entry(&entry);
  if (!TEST_CHECK(render("index.html", &context, os, &perm, &rendered_html, NULL) == 0)) {
    return;
  }
  expected_len = snprintf(expected, sizeof(expected), "sosig %.*s|sosig %.*s", STR_FMT(version),
                          STR_FMT(version));
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(rendered_html, expected) == 0);
  TEST_MSG("rendered: '%s'", rendered_html);
}

// Every `site.*` field from the configuration renders, escaped, at the root and inside a section,
// in a template of any kind.
static void test_renders_site_fields(void) {
  struct Arena perm = test_arena();
  struct Os* os = template_host(
      "{{site.title}}|{{site.description}}|{{site.author}}|{{site.base_url}}"
      "{{#content_entries}}|{{site.description}}{{/content_entries}}");

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  init_test_content_entry(&entry);
  const struct ContentEntry* content_entries[] = {&entry};
  const struct TemplateContext context = {
      .site_config = &site_config, .content_entries = content_entries, .content_entry_count = 1};
  char* rendered_html = NULL;
  if (!TEST_CHECK(render("index.html", &context, os, &perm, &rendered_html, NULL) == 0)) {
    return;
  }
  TEST_CHECK(strcmp(rendered_html,
                    "Site|Notes &amp; more|Author|https://example.com|Notes &amp; more") == 0);
  TEST_MSG("rendered: '%s'", rendered_html);
}

// `path_to_root` renders the prefix the caller set for the page, at the root and inside
// `{{#content_entries}}`, where a listed entry has none of its own. It is empty when the caller
// sets none. `test_page_renderer.c` and `test_site_writer.c` check the prefix each kind of page
// gets.
static void test_renders_path_to_root(void) {
  struct Arena perm = test_arena();
  struct Os* os =
      template_host("[{{path_to_root}}]{{#content_entries}}({{path_to_root}}){{/content_entries}}");

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  init_test_content_entry(&entry);
  const struct ContentEntry* content_entries[] = {&entry};
  const struct TemplateContext nested_context = {.site_config = &site_config,
                                                 .content_entries = content_entries,
                                                 .content_entry_count = 1,
                                                 .content_entry_current = &entry,
                                                 .path_to_root = S("../../")};
  const struct TemplateContext top_context = {
      .site_config = &site_config, .content_entries = content_entries, .content_entry_count = 1};
  char* nested_html = NULL;
  char* top_html = NULL;
  if (!TEST_CHECK(render("index.html", &nested_context, os, &perm, &nested_html, NULL) == 0)) {
    return;
  }
  TEST_CHECK(strcmp(nested_html, "[../../](../../)") == 0);
  TEST_MSG("rendered: '%s'", nested_html);
  if (!TEST_CHECK(render("index.html", &top_context, os, &perm, &top_html, NULL) == 0)) {
    return;
  }
  TEST_CHECK(strcmp(top_html, "[]()") == 0);
  TEST_MSG("rendered: '%s'", top_html);
}

// An unknown variable renders as empty text per the Mustache spec, at the root and in a subtree.
static void test_unknown_variable_renders_empty(void) {
  struct Arena perm = test_arena();
  struct Os* os = template_host("[{{typo}}|{{site.typo}}]\n");

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct TemplateContext context = {.site_config = &site_config,
                                          .content_entry_current = &entry};
  char* rendered_html = NULL;
  init_test_content_entry(&entry);
  if (!TEST_CHECK(render("index.html", &context, os, &perm, &rendered_html, NULL) == 0)) {
    return;
  }
  TEST_CHECK(strcmp(rendered_html, "[|]\n") == 0);
}

// An inverted section renders only when the list is empty.
static void test_inverted_section_renders_when_list_empty(void) {
  struct Arena perm = test_arena();
  struct Os* os = template_host(
      "{{#content_entries}}x{{/content_entries}}{{^content_entries}}none{{/"
      "content_entries}}");

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct TemplateContext context_empty = {.site_config = &site_config};
  const struct ContentEntry* content_entries[] = {&entry};
  const struct TemplateContext context_full = {
      .site_config = &site_config, .content_entries = content_entries, .content_entry_count = 1};
  char* rendered_html = NULL;
  init_test_content_entry(&entry);

  if (!TEST_CHECK(render("index.html", &context_empty, os, &perm, &rendered_html, NULL) == 0)) {
    return;
  }
  TEST_CHECK(strcmp(rendered_html, "none") == 0);

  if (!TEST_CHECK(render("index.html", &context_full, os, &perm, &rendered_html, NULL) == 0)) {
    return;
  }
  TEST_CHECK(strcmp(rendered_html, "x") == 0);
}

// An empty field value is falsey as a section, so an unset optional field reads as absent. Were an
// empty scalar truthy, `{{#description}}` would render for every entry and `{{^description}}` for
// none, so both branches are asserted here. The list renders a second time inside a content page
// whose entry has a description and tags, and a listed entry without them still reads as empty,
// rather than showing the page entry's, which Mustache's lookup in the enclosing context would.
static void test_empty_field_is_falsey_as_section(void) {
  struct Arena perm = test_arena();
  struct Os* os = template_host(
      "{{#content_entries}}[{{#description}}D:{{description}}{{/description}}"
      "{{^description}}NONE{{/description}}{{^tags}}|NOTAGS{{/tags}}]{{/content_entries}}");

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  // An empty `description` is the shape a content file that omits the key leaves on its zeroed
  // entry.
  struct ContentEntry first;
  struct ContentEntry second;
  init_test_content_entry(&first);
  init_test_content_entry(&second);
  const struct ContentEntry* content_entries[] = {&first, &second};
  const struct TemplateContext context = {
      .site_config = &site_config, .content_entries = content_entries, .content_entry_count = 2};
  struct ContentEntry current;
  init_test_content_entry(&current);
  current.description = S("Page");
  struct Str current_tags[] = {S("tag")};
  current.tags = current_tags;
  current.tag_count = 1;
  const struct TemplateContext page_context = {.site_config = &site_config,
                                               .content_entry_current = &current,
                                               .content_entries = content_entries,
                                               .content_entry_count = 2};
  char* rendered_html = NULL;
  first.description = S("");
  second.description = S("Desc");
  if (!TEST_CHECK(render("index.html", &context, os, &perm, &rendered_html, NULL) == 0)) {
    return;
  }
  TEST_CHECK(strcmp(rendered_html, "[NONE|NOTAGS][D:Desc|NOTAGS]") == 0);

  if (!TEST_CHECK(render("index.html", &page_context, os, &perm, &rendered_html, NULL) == 0)) {
    return;
  }
  TEST_CHECK(strcmp(rendered_html, "[NONE|NOTAGS][D:Desc|NOTAGS]") == 0);
}

// `{{modified}}` renders the current entry's modified date and each listed entry's, and
// `{{#modified}}` skips an entry without one, so a feed can fall back to `{{date}}` there. The list
// renders with a current entry that has a modified date, as a content template does, and a listed
// entry without one still falls back to its own date rather than showing the current entry's.
static void test_renders_modified_with_date_fallback(void) {
  struct Arena perm = test_arena();
  struct Os* os = template_host(
      "{{#content_entries}}[{{#modified}}{{modified}}{{/modified}}"
      "{{^modified}}{{date}}{{/modified}}]{{/content_entries}}");
  write_template(os, "content.html", "[{{modified}}]");

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry first;
  struct ContentEntry second;
  init_test_content_entry(&first);
  init_test_content_entry(&second);
  first.modified = S("2026-07-03T00:00:00Z");
  const struct ContentEntry* content_entries[] = {&first, &second};
  const struct TemplateContext list_context = {.site_config = &site_config,
                                               .content_entry_current = &first,
                                               .content_entries = content_entries,
                                               .content_entry_count = 2};
  const struct TemplateContext first_context = {.site_config = &site_config,
                                                .content_entry_current = &first};
  const struct TemplateContext second_context = {.site_config = &site_config,
                                                 .content_entry_current = &second};
  char* list_html = NULL;
  char* first_html = NULL;
  char* second_html = NULL;
  if (!TEST_CHECK(render("index.html", &list_context, os, &perm, &list_html, NULL) == 0) ||
      !TEST_CHECK(render("content.html", &first_context, os, &perm, &first_html, NULL) == 0) ||
      !TEST_CHECK(render("content.html", &second_context, os, &perm, &second_html, NULL) == 0)) {
    return;
  }
  TEST_CHECK(strcmp(list_html, "[2026-07-03T00:00:00Z][2026-07-01T00:00:00Z]") == 0);
  TEST_CHECK(strcmp(first_html, "[2026-07-03T00:00:00Z]") == 0);
  TEST_CHECK(strcmp(second_html, "[]") == 0);
}

// Triple braces and the ampersand tag fail for any key that does not end in `_html`, whatever kind
// of value it holds: text, an empty field, the `site` subtree or the `content_entries` list. A
// partial cannot bypass the rule either, and its failure is attributed to the template being
// rendered.
static void test_rejects_unescaped_interpolation_of_other_keys(void) {
  struct Arena perm = test_arena();
  struct Os* os = template_host("{{{title}}}");

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  const struct TemplateContext context = {.site_config = &site_config,
                                          .content_entry_current = &entry};
  init_test_content_entry(&entry);
  entry.description = S("");
  static const struct {
    const char* template_name;
    const char* text;
    const char* expected;
  } cases[] = {
      {"index.html", "{{{title}}}", UNESCAPED_FAILURE " (in 'index.html')"},
      {"ampersand.html", "{{& title}}", UNESCAPED_FAILURE " (in 'ampersand.html')"},
      {"empty.html", "{{{description}}}", UNESCAPED_FAILURE " (in 'empty.html')"},
      {"site.html", "{{{site}}}", UNESCAPED_FAILURE " (in 'site.html')"},
      {"list.html", "{{{content_entries}}}", UNESCAPED_FAILURE " (in 'list.html')"},
      {"outer.html", "{{>raw}}", UNESCAPED_FAILURE " (in 'outer.html')"},
  };
  write_template(os, "partials/raw.html", "{{{site.title}}}");
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    write_template(os, cases[i].template_name, cases[i].text);
    struct Log log = test_log();
    TEST_CHECK(render(cases[i].template_name, &context, os, &perm, NULL, &log) == -1);
    TEST_CHECK(is_log_one_failure(&log, cases[i].template_name, cases[i].expected));
    TEST_MSG("template: %s", cases[i].template_name);
  }
}

TEST_LIST = {
    {"reports whether it read the entry list", test_reports_whether_it_read_the_entry_list},
    {"iterates content entries and tags", test_iterates_content_entries_and_tags},
    {"empty tag does not truncate tag list", test_empty_tag_does_not_truncate_tag_list},
    {"nested section reiterates content entries", test_nested_section_reiterates_content_entries},
    {"escapes double brace and leaves html key raw",
     test_escapes_double_brace_and_leaves_html_key_raw},
    {"renders generator", test_renders_generator},
    {"renders site fields", test_renders_site_fields},
    {"renders path to root", test_renders_path_to_root},
    {"unknown variable renders empty", test_unknown_variable_renders_empty},
    {"inverted section renders when list empty", test_inverted_section_renders_when_list_empty},
    {"empty field is falsey as section", test_empty_field_is_falsey_as_section},
    {"renders modified with date fallback", test_renders_modified_with_date_fallback},
    {"rejects unescaped interpolation of other keys",
     test_rejects_unescaped_interpolation_of_other_keys},
    {NULL, NULL},
};
