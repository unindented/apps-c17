#include <acutest.h>
#include <errno.h>
#include <string.h>

#include "build/site_writer.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/str.h"
#include "domain/content_entry.h"
#include "domain/site_config.h"
#include "os/os_memory.h"
#include "output/manifest.h"
#include "output/output_cache.h"
#include "output/output_writer.h"
#include "templates/template_set.h"
#include "testing/test_support.h"

// Every test here runs against the in-memory host: the writers write their outputs only through
// `os/os.h`, from templates that each test loads first.

/** Output cache of no earlier build, which keeps nothing. */
static const struct OutputCache EMPTY_CACHE = {0};

/**
 * @brief Initializes a configuration rooted at `templates` and `public`, with no outputs.
 *
 * @param site_config Configuration to initialize. Must not be `NULL`.
 */
static void init_config(struct SiteConfig* site_config) {
  site_config_init(site_config);
  site_config->output_dir = S("public");
  site_config->templates_dir = S("templates");
}

/**
 * @brief Initializes a writer on `os` whose manifest names `output_paths`, as the build's would.
 *
 * @param writer            Writer to initialize. Must not be `NULL`.
 * @param manifest          Empty manifest that receives the paths. Must not be `NULL`.
 * @param output_paths      Array of `output_path_count` terminated paths. Must not be `NULL`.
 * @param output_path_count Number of paths in `output_paths`.
 * @param os                Host the writer writes to. Must not be `NULL`.
 * @param arena             Arena that owns the manifest's entries. Must not be `NULL`.
 */
static void init_writer(struct OutputWriter* writer,
                        struct Manifest* manifest,
                        const char* const* output_paths,
                        ptrdiff_t output_path_count,
                        struct Os* os,
                        struct Arena* arena) {
  for (ptrdiff_t i = 0; i < output_path_count; i++) {
    TEST_CHECK(manifest_add(manifest, str_from_cstr(output_paths[i]), S("test"), arena, NULL) == 0);
  }
  *writer = (struct OutputWriter){.manifest = manifest, .os = os};
}

/**
 * @brief Loads the templates `names` below `templates/` of `os` as a set, which the test releases.
 *
 * @param os    Host holding the templates. Must not be `NULL`.
 * @param names Array of `count` template names. Must not be `NULL`.
 * @param count Number of entries in `names`.
 * @return The set, in an arena of its own.
 */
static struct TemplateSet* load_templates(struct Os* os, const struct Str* names, ptrdiff_t count) {
  struct TemplateSet* set = NULL;
  struct Arena perm = test_arena();
  struct Log log = test_log();
  TEST_ASSERT(template_set_load(S("templates"), names, NULL, count, os, &perm, test_arena(), &set,
                                &log) == 0);
  return set;
}

// Templates, nested ones included, land at their planned destinations, and the templates see the
// entries passed, with page-relative links in an aggregate and absolute ones in a feed.
static void test_writes_complete_output_layer(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "templates/index.html",
                  "<h1>{{site.title}}</h1>{{#content_entries}}{{title}};{{/content_entries}}");
  os_memory_write(os, "templates/nested/links.html",
                  "{{#content_entries}}{{url}}|{{absolute_url}};{{/content_entries}}");
  os_memory_write(os, "templates/feeds/atom.xml",
                  "<updated>{{feed.modified}}</updated>{{#content_entries}}{{title}}={{url}};"
                  "{{/content_entries}}");
  const struct ContentEntry newer = {
      .title = S("Newer"), .url_path = S("/newer.html"), .date = S("2026-07-01T00:00:00Z")};
  const struct ContentEntry older = {.title = S("Older"), .url_path = S("/older.html")};
  const struct ContentEntry* const entries[] = {&newer, &older};
  static const struct Str aggregates[] = {STR_INIT("index.html"), STR_INIT("nested/links.html")};
  static const struct Str feeds[] = {STR_INIT("feeds/atom.xml")};
  struct SiteConfig site_config;
  init_config(&site_config);
  site_config.title = S("Site & Co");
  site_config.description = S("Notes");
  site_config.author = S("Ada");
  site_config.base_url = S("https://example.com");
  struct Arena paths = test_arena();
  static const char* const output_paths[] = {"public/index.html", "public/nested/links.html",
                                             "public/feeds/atom.xml"};
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, output_paths, 3, os, &paths);

  struct Arena output = test_arena();
  struct Log log = test_log();
  struct TemplateSet* aggregates_set = load_templates(os, aggregates, 2);
  struct TemplateSet* feeds_set = load_templates(os, feeds, 1);
  TEST_CHECK(site_writer_write_templates(&writer, &EMPTY_CACHE, &site_config, aggregates_set,
                                         aggregates, 2, entries, 2, false, output, test_arena(),
                                         &log) == 0);
  TEST_CHECK(site_writer_write_templates(&writer, &EMPTY_CACHE, &site_config, feeds_set, feeds, 1,
                                         entries, 1, true, output, test_arena(), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(os, "public/index.html", "<h1>Site &amp; Co</h1>Newer;Older;"));
  TEST_CHECK(os_memory_is_file(os, "public/nested/links.html",
                               "../newer.html|https://example.com/newer.html;"
                               "../older.html|https://example.com/older.html;"));
  TEST_CHECK(os_memory_is_file(os, "public/feeds/atom.xml",
                               "<updated>2026-07-01T00:00:00Z</updated>"
                               "Newer=https://example.com/newer.html;"));
  template_set_release(aggregates_set);
  template_set_release(feeds_set);
}

// A feed template gets `feed`: its own absolute URL as `id` and `url`, the site's home as
// `html_url`, and the newest change among its own entries as `modified`, so a later change to an
// entry outside the feed does not move it. In a feed, an entry without `modified` gives its `date`.
// An aggregate gets no `feed`, and an entry's `modified` stays empty there.
static void test_write_templates_gives_feed_data(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "templates/index.html",
                  "[{{feed.url}}]{{#content_entries}}{{modified}};{{/content_entries}}");
  os_memory_write(os, "templates/feeds/atom.xml",
                  "{{feed.id}}|{{feed.url}}|{{feed.html_url}}|{{feed.modified}}|"
                  "{{#content_entries}}{{modified}}={{absolute_url}};{{/content_entries}}");
  const struct ContentEntry newer = {.title = S("Newer"),
                                     .url_path = S("/newer.html"),
                                     .date = S("2026-07-01T00:00:00Z"),
                                     .date_epoch = 1782864000,
                                     .modified_epoch = 1782864000};
  const struct ContentEntry older = {.title = S("Older"),
                                     .url_path = S("/older.html"),
                                     .date = S("2026-06-01T00:00:00Z"),
                                     .date_epoch = 1780272000,
                                     .modified = S("2026-06-15T00:00:00Z"),
                                     .modified_epoch = 1781481600};
  const struct ContentEntry oldest = {.title = S("Oldest"),
                                      .url_path = S("/oldest.html"),
                                      .date = S("2026-05-01T00:00:00Z"),
                                      .date_epoch = 1777593600,
                                      .modified = S("2026-08-01T00:00:00Z"),
                                      .modified_epoch = 1785542400};
  const struct ContentEntry* const entries[] = {&newer, &older, &oldest};
  static const struct Str aggregates[] = {STR_INIT("index.html")};
  static const struct Str feeds[] = {STR_INIT("feeds/atom.xml")};
  struct SiteConfig site_config;
  init_config(&site_config);
  site_config.base_url = S("https://example.com");
  static const char* const output_paths[] = {"public/index.html", "public/feeds/atom.xml"};
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, output_paths, 2, os, &arena);

  struct Arena output = test_arena();
  struct Log log = test_log();
  struct TemplateSet* aggregates_set = load_templates(os, aggregates, 1);
  struct TemplateSet* feeds_set = load_templates(os, feeds, 1);
  TEST_CHECK(site_writer_write_templates(&writer, &EMPTY_CACHE, &site_config, aggregates_set,
                                         aggregates, 1, entries, 3, false, output, test_arena(),
                                         &log) == 0);
  TEST_CHECK(site_writer_write_templates(&writer, &EMPTY_CACHE, &site_config, feeds_set, feeds, 1,
                                         entries, 2, true, output, test_arena(), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(
      os_memory_is_file(os, "public/index.html", "[];2026-06-15T00:00:00Z;2026-08-01T00:00:00Z;"));
  TEST_CHECK(
      os_memory_is_file(os, "public/feeds/atom.xml",
                        "https://example.com/feeds/atom.xml|https://example.com/feeds/atom.xml|"
                        "https://example.com/|2026-07-01T00:00:00Z|"
                        "2026-07-01T00:00:00Z=https://example.com/newer.html;"
                        "2026-06-15T00:00:00Z=https://example.com/older.html;"));
  template_set_release(aggregates_set);
  template_set_release(feeds_set);
}

// An aggregate or a feed resolves `path_to_root` from its own name, which is also its output path
// below `output_dir`: empty at the top, and one `../` for each directory a nested name is below.
// The golden test `site_nested_sections` asserts the nested aggregate end to end.
static void test_write_templates_renders_path_to_root_per_template(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "templates/index.html", "[{{path_to_root}}]");
  os_memory_write(os, "templates/field-notes/index.html", "[{{path_to_root}}]");
  os_memory_write(os, "templates/a/b/c.html", "[{{path_to_root}}]");
  os_memory_write(os, "templates/feeds/atom.xml", "[{{path_to_root}}]");
  static const struct Str aggregates[] = {
      STR_INIT("index.html"), STR_INIT("field-notes/index.html"), STR_INIT("a/b/c.html")};
  static const struct Str feeds[] = {STR_INIT("feeds/atom.xml")};
  struct SiteConfig site_config;
  init_config(&site_config);
  static const char* const output_paths[] = {"public/index.html", "public/field-notes/index.html",
                                             "public/a/b/c.html", "public/feeds/atom.xml"};
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, output_paths, 4, os, &arena);

  struct Arena output = test_arena();
  struct Log log = test_log();
  struct TemplateSet* aggregates_set = load_templates(os, aggregates, 3);
  struct TemplateSet* feeds_set = load_templates(os, feeds, 1);
  TEST_CHECK(site_writer_write_templates(&writer, &EMPTY_CACHE, &site_config, aggregates_set,
                                         aggregates, 3, NULL, 0, false, output, test_arena(),
                                         &log) == 0);
  TEST_CHECK(site_writer_write_templates(&writer, &EMPTY_CACHE, &site_config, feeds_set, feeds, 1,
                                         NULL, 0, true, output, test_arena(), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(os, "public/index.html", "[]"));
  TEST_CHECK(os_memory_is_file(os, "public/field-notes/index.html", "[../]"));
  TEST_CHECK(os_memory_is_file(os, "public/a/b/c.html", "[../../]"));
  TEST_CHECK(os_memory_is_file(os, "public/feeds/atom.xml", "[../]"));
  template_set_release(aggregates_set);
  template_set_release(feeds_set);
}

// An output whose file already holds the rendered bytes is kept, left as it was, and one whose file
// holds other bytes is written.
static void test_write_templates_keeps_an_unchanged_output(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "templates/index.html", "<h1>{{site.title}}</h1>");
  os_memory_write(os, "templates/about.html", "<p>{{site.title}}</p>");
  os_memory_write(os, "public/index.html", "<h1>Site</h1>");
  os_memory_write(os, "public/about.html", "<p>Old</p>");
  struct OsStat before;
  TEST_ASSERT(os_stat(os, S("public/index.html"), &before) == 0);
  static const struct Str aggregates[] = {STR_INIT("index.html"), STR_INIT("about.html")};
  struct SiteConfig site_config;
  init_config(&site_config);
  site_config.title = S("Site");
  static const char* const output_paths[] = {"public/index.html", "public/about.html"};
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, output_paths, 2, os, &arena);

  struct Log log = test_log();
  struct TemplateSet* aggregates_set = load_templates(os, aggregates, 2);
  TEST_CHECK(site_writer_write_templates(&writer, &EMPTY_CACHE, &site_config, aggregates_set,
                                         aggregates, 2, NULL, 0, false, test_arena(), test_arena(),
                                         &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(manifest_find_mark(&manifest, S("public/index.html")) == MANIFEST_MARK_KEPT);
  TEST_CHECK(manifest_find_mark(&manifest, S("public/about.html")) == MANIFEST_MARK_WRITTEN);
  struct OsStat after;
  TEST_CHECK(os_stat(os, S("public/index.html"), &after) == 0);
  TEST_CHECK(after.mtime_s == before.mtime_s && after.mtime_ns == before.mtime_ns);
  TEST_CHECK(os_memory_is_file(os, "public/about.html", "<p>Site</p>"));
  template_set_release(aggregates_set);
}

// A failed write names the operation and the cause, then the destination. The destination is an
// existing directory, so opening it for writing fails.
static void test_write_templates_reports_write_failure(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "templates/index.html", "<h1></h1>");
  os_memory_mkdir(os, "public/index.html");
  static const struct Str aggregates[] = {STR_INIT("index.html")};
  struct SiteConfig site_config;
  init_config(&site_config);
  static const char* const output_paths[] = {"public/index.html"};
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, output_paths, 1, os, &arena);

  struct Arena output = test_arena();
  struct Log log = test_log();
  struct TemplateSet* aggregates_set = load_templates(os, aggregates, 1);
  TEST_CHECK(site_writer_write_templates(&writer, &EMPTY_CACHE, &site_config, aggregates_set,
                                         aggregates, 1, NULL, 0, false, output, test_arena(),
                                         &log) == -1);
  TEST_CHECK(
      test_log_is(&log, "failed to write page: %s ('public/index.html')\n", strerror(EISDIR)));
  template_set_release(aggregates_set);
}

TEST_LIST = {
    {"writes complete output layer", test_writes_complete_output_layer},
    {"write templates gives feed data", test_write_templates_gives_feed_data},
    {"write templates renders path to root per template",
     test_write_templates_renders_path_to_root_per_template},
    {"write templates keeps an unchanged output", test_write_templates_keeps_an_unchanged_output},
    {"write templates reports write failure", test_write_templates_reports_write_failure},
    {NULL, NULL},
};
