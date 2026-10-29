#include <acutest.h>
#include <stddef.h>
#include <string.h>

#include "build/template_context.h"
#include "core/arena.h"
#include "core/log.h"
#include "core/str.h"
#include "domain/content_entry.h"
#include "domain/site_config.h"
#include "os/os_posix.h"
#include "templates/template_set.h"
#include "testing/test_support.h"

// These tests run on the POSIX host, because they render the real templates of the golden site
// `tests/fixtures/site_file_permalink`, which only the real filesystem holds. The test runs from
// the project's source directory. `test_template_context.c` runs the rest of the data on the
// in-memory host.

/** Templates of the golden site the tests render, relative to the project's source directory. */
#define GOLDEN_TEMPLATES_DIR "tests/fixtures/site_file_permalink/templates"

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
 * @brief Loads a template of the golden site, renders it through `template_context_render`, and
 *        hands back the HTML as a C string.
 *
 * The assertions inspect the rendered text with `strstr`, so the helper hands it back terminated,
 * copied after the HTML in `perm`. Rendered output holds no `NUL` byte, so the copy loses nothing.
 *
 * @param template_name Terminated name of a template below `GOLDEN_TEMPLATES_DIR`.
 * @param context       Template context. Must not be `NULL`.
 * @param perm          Arena that owns the set, the HTML and its terminated copy. Must not be
 *                      `NULL`.
 * @param html_out      Receives the terminated HTML on success. Must not be `NULL`.
 * @return `0` on success, or `-1` when the load or the render failed.
 */
static int render(const char* template_name,
                  const struct TemplateContext* context,
                  struct Arena* perm,
                  char** html_out) {
  struct Log log = test_log();
  const struct Str name = str_from_cstr(template_name);
  struct TemplateSet* set = NULL;
  if (template_set_load(S(GOLDEN_TEMPLATES_DIR), &name, NULL, 1, os_posix(), perm, test_arena(),
                        &set, &log) != 0) {
    return -1;
  }
  struct Str html = {0};
  const int rc = template_context_render(set, name, context, name, perm, test_arena(), &html, &log);
  template_set_release(set);
  if (rc != 0) {
    return -1;
  }
  *html_out = str_cstr(html, perm);
  return 0;
}

// `{{title}}` is escaped and `{{{body_html}}}` is not in the same render, so escaping is chosen per
// tag rather than per template. This renders the real `templates/content.html`, whose output,
// including the feed `<link rel="alternate">`, is compared end to end with the expected output in
// `tests/expected/site_file_permalink` by the golden test.
static void test_renders_escaped_title_and_raw_body(void) {
  struct Arena perm = test_arena();
  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  init_test_content_entry(&entry);
  entry.title = S("Content Entry <One>");
  const struct TemplateContext context = {.site_config = &site_config,
                                          .content_entry_current = &entry};
  char* rendered_html = NULL;
  TEST_ASSERT(render("content.html", &context, &perm, &rendered_html) == 0);
  TEST_CHECK(strstr(rendered_html, "Content Entry &lt;One&gt;") != NULL);
  TEST_CHECK(strstr(rendered_html, "<p>raw</p>") != NULL);
  TEST_CHECK(strstr(rendered_html, "rel=\"alternate\"") != NULL);
}

// The feed template renders through the same name-based entry point as an HTML page. A body that
// already holds HTML is escaped again for XML. This renders the real `templates/atom.xml`, so the
// golden test compares its output end to end with `tests/expected/site_file_permalink/atom.xml`.
static void test_renders_atom_feed(void) {
  struct Arena perm = test_arena();
  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct ContentEntry entry;
  init_test_content_entry(&entry);
  entry.title = S("Content Entry <One>");
  entry.description = S("Desc & More");
  const struct ContentEntry* content_entries[] = {&entry};
  const struct TemplateContext context = {.site_config = &site_config,
                                          .content_entry_current = NULL,
                                          .content_entries = content_entries,
                                          .content_entry_count = 1,
                                          .is_feed = true};
  char* rendered_html = NULL;
  TEST_ASSERT(render("atom.xml", &context, &perm, &rendered_html) == 0);
  TEST_CHECK(strstr(rendered_html, "<title>Content Entry &lt;One&gt;</title>") != NULL);
  TEST_CHECK(strstr(rendered_html, "<content type=\"html\">&lt;p&gt;raw&lt;/p&gt;</content>") !=
             NULL);
  TEST_CHECK(strstr(rendered_html, "<updated>2026-07-01T00:00:00Z</updated>") != NULL);
}

TEST_LIST = {
    {"renders escaped title and raw body", test_renders_escaped_title_and_raw_body},
    {"renders atom feed", test_renders_atom_feed},
    {NULL, NULL},
};
