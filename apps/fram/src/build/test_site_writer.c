#include <acutest.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "build/page_renderer.h"
#include "build/site_writer.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/str.h"
#include "domain/album.h"
#include "domain/site_config.h"
#include "os/os_memory.h"
#include "output/manifest.h"
#include "output/output_cache.h"
#include "output/output_writer.h"
#include "templates/template_set.h"
#include "testing/test_support.h"

// Every test here runs against the in-memory host: the writers write their outputs only through
// `os/os.h`, from templates that each test loads first.

/** An output cache with no rows, which a page never needs, as it declares no attributes. */
static const struct OutputCache NO_OUTPUT_CACHE = {0};

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
  *writer = (struct OutputWriter){.manifest = manifest, .os = os, .output_dir = S("public")};
}

/**
 * @brief Loads the templates `names` below `templates_dir` of `os` as a set, which the test
 *        releases.
 *
 * @param templates_dir Directory of the templates.
 * @param names         Array of `count` template names. Must not be `NULL`.
 * @param count         Number of entries in `names`.
 * @param os            Host holding the templates. Must not be `NULL`.
 * @return The set, in an arena of its own.
 */
static struct TemplateSet* load_templates(struct Str templates_dir,
                                          const struct Str* names,
                                          ptrdiff_t count,
                                          struct Os* os) {
  struct TemplateSet* set = NULL;
  struct Arena perm = test_arena();
  struct Log log = test_log();
  TEST_ASSERT(template_set_load(templates_dir, names, NULL, count, os, &perm, test_arena(), &set,
                                &log) == 0);
  return set;
}

// Album pages and aggregate templates land at their planned destinations.
static void test_writes_complete_output_layer(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "templates/map.xml", "<g>{{site.title}}</g>");
  os_memory_write(os, "templates/nested/links.html", "{{album.url}}");
  struct Arena paths = test_arena();
  const struct Arena output = test_arena();
  const struct Arena scratch = test_arena();
  static const struct Str aggregates[] = {STR_INIT("map.xml"), STR_INIT("nested/links.html")};
  struct SiteConfig site_config;
  site_config_init(&site_config);
  site_config.title = S("Gallery & Co");
  site_config.description = S("Description");
  site_config.author = S("Ada");
  site_config.output_dir = S("public");
  site_config.templates_dir = S("templates");
  site_config.static_dir = S("static");
  site_config.templates.aggregates = aggregates;
  site_config.templates.aggregate_template_count = 2;
  struct Album root = {.source_dir = S(""),
                       .title = S("Root"),
                       .url_path = S("index.html"),
                       .output_path = S("public/index.html"),
                       .path_to_root = S("")};
  const struct Album* albums[] = {&root};
  const struct RenderedPage rendered[] = {{.html = S("<h1>Root</h1>")}};
  struct Log log = test_log();
  static const char* const output_paths[] = {"public/index.html", "public/map.xml",
                                             "public/nested/links.html"};
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, output_paths, 3, os, &paths);

  TEST_CHECK(site_writer_write_album_pages(&writer, &NO_OUTPUT_CACHE, albums, 1, rendered, scratch,
                                           &log) == 0);
  struct TemplateSet* template_set = load_templates(site_config.templates_dir, aggregates, 2, os);
  TEST_CHECK(site_writer_write_aggregates(&writer, &NO_OUTPUT_CACHE, &site_config, template_set,
                                          albums[0], output, scratch, &log) == 0);
  template_set_release(template_set);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(os, "public/index.html", "<h1>Root</h1>"));
  TEST_CHECK(os_memory_is_file(os, "public/map.xml", "<g>Gallery &amp; Co</g>"));
  TEST_CHECK(os_memory_is_file(os, "public/nested/links.html", "../index.html"));
}

// The not-found page renders `templates.not_found` to `not_found_path`, with the root-relative path
// of `base_url` as `path_to_root` however deep the page is, and `/` for a gallery without one. It
// sees no album.
static void test_write_not_found_renders_from_the_root(void) {
  static const struct {
    const char* base_url;
    const char* expected;
  } cases[] = {
      {"https://example.com/gallery", "Gallery|/gallery/style.css|none"},
      {"", "Gallery|/style.css|none"},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct Os* os = os_memory();
    os_memory_write(os, "templates/errors/missing.html",
                    "{{site.title}}|{{path_to_root}}style.css|{{album.title}}"
                    "{{^sub_albums}}none{{/sub_albums}}");
    static const struct Str names[] = {STR_INIT("errors/missing.html")};
    struct SiteConfig site_config;
    site_config_init(&site_config);
    site_config.title = S("Gallery");
    site_config.base_url = str_from_cstr(cases[i].base_url);
    site_config.output_dir = S("public");
    site_config.templates_dir = S("templates");
    site_config.not_found_path = S("bunnycdn_errors/404.html");
    site_config.templates.not_found = names[0];
    struct Arena paths = test_arena();
    static const char* const output_paths[] = {"public/bunnycdn_errors/404.html"};
    struct Manifest manifest = {0};
    struct OutputWriter writer;
    init_writer(&writer, &manifest, output_paths, 1, os, &paths);

    struct Log log = test_log();
    struct TemplateSet* template_set = load_templates(site_config.templates_dir, names, 1, os);
    TEST_CHECK(site_writer_write_not_found(&writer, &NO_OUTPUT_CACHE, &site_config, template_set,
                                           test_arena(), test_arena(), &log) == 0);
    template_set_release(template_set);
    TEST_CHECK(test_log_is_empty(&log));
    TEST_CHECK(os_memory_is_file(os, "public/bunnycdn_errors/404.html", cases[i].expected));
    TEST_MSG("base_url '%s'", cases[i].base_url);
  }
}

// A page or an aggregate whose bytes did not change is kept and left untouched, while one whose
// bytes changed is written.
static void test_keeps_unchanged_pages_untouched(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "templates/map.xml", "<g>{{site.title}}</g>");
  os_memory_write(os, "public/index.html", "<h1>Root</h1>");
  os_memory_write(os, "public/about/index.html", "<h1>Old</h1>");
  os_memory_write(os, "public/map.xml", "<g>Gallery</g>");
  struct Arena arena = test_arena();
  static const struct Str aggregates[] = {STR_INIT("map.xml")};
  struct SiteConfig site_config;
  site_config_init(&site_config);
  site_config.title = S("Gallery");
  site_config.output_dir = S("public");
  site_config.templates_dir = S("templates");
  site_config.templates.aggregates = aggregates;
  site_config.templates.aggregate_template_count = 1;
  struct Album root = {.source_dir = S(""),
                       .title = S("Root"),
                       .output_path = S("public/index.html"),
                       .path_to_root = S("")};
  struct Album about = {
      .source_dir = S("about"), .title = S("About"), .output_path = S("public/about/index.html")};
  const struct Album* albums[] = {&root, &about};
  const struct RenderedPage rendered[] = {{.html = S("<h1>Root</h1>")},
                                          {.html = S("<h1>About</h1>")}};
  static const char* const output_paths[] = {"public/index.html", "public/about/index.html",
                                             "public/map.xml"};
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, output_paths, 3, os, &arena);
  struct OsStat root_before;
  struct OsStat map_before;
  TEST_ASSERT(os_stat(os, S("public/index.html"), &root_before) == 0);
  TEST_ASSERT(os_stat(os, S("public/map.xml"), &map_before) == 0);
  struct Log log = test_log();

  TEST_CHECK(site_writer_write_album_pages(&writer, &NO_OUTPUT_CACHE, albums, 2, rendered,
                                           test_arena(), &log) == 0);
  struct TemplateSet* template_set = load_templates(site_config.templates_dir, aggregates, 1, os);
  TEST_CHECK(site_writer_write_aggregates(&writer, &NO_OUTPUT_CACHE, &site_config, template_set,
                                          &root, test_arena(), test_arena(), &log) == 0);
  template_set_release(template_set);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(manifest_find_mark(&manifest, S("public/index.html")) == MANIFEST_MARK_KEPT);
  TEST_CHECK(manifest_find_mark(&manifest, S("public/about/index.html")) == MANIFEST_MARK_WRITTEN);
  TEST_CHECK(manifest_find_mark(&manifest, S("public/map.xml")) == MANIFEST_MARK_KEPT);
  TEST_CHECK(os_memory_is_file(os, "public/about/index.html", "<h1>About</h1>"));
  struct OsStat after;
  TEST_CHECK(os_stat(os, S("public/index.html"), &after) == 0 &&
             after.mtime_s == root_before.mtime_s && after.mtime_ns == root_before.mtime_ns);
  TEST_CHECK(os_stat(os, S("public/map.xml"), &after) == 0 && after.mtime_s == map_before.mtime_s &&
             after.mtime_ns == map_before.mtime_ns);
}

// A failed page write names the root album in words rather than quoting its empty source directory,
// then the cause and the destination. The destination is an existing directory, so opening it for
// writing fails.
static void test_write_album_pages_names_root_album_in_words(void) {
  struct Os* os = os_memory();
  os_memory_mkdir(os, "public");
  struct Album root = {.source_dir = S(""), .title = S("Root"), .output_path = S("public")};
  const struct Album* albums[] = {&root};
  const struct RenderedPage rendered[] = {{.html = S("<h1>Root</h1>")}};
  static const char* const output_paths[] = {"public"};
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, output_paths, 1, os, &arena);
  struct Log log = test_log();

  TEST_CHECK(site_writer_write_album_pages(&writer, &NO_OUTPUT_CACHE, albums, 1, rendered,
                                           test_arena(), &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to write page for the root album: %s ('public')\n",
                         strerror(EISDIR)));
}

// A failed sub-album page write quotes the album's source directory, even one literally named
// `root album`, so it never reads the same as the root's diagnostic.
static void test_write_album_pages_quotes_sub_album_source_dir(void) {
  struct Os* os = os_memory();
  os_memory_mkdir(os, "public");
  struct Album album = {
      .source_dir = S("root album"), .title = S("root album"), .output_path = S("public")};
  const struct Album* albums[] = {&album};
  const struct RenderedPage rendered[] = {{.html = S("<h1>root album</h1>")}};
  static const char* const output_paths[] = {"public"};
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, output_paths, 1, os, &arena);
  struct Log log = test_log();

  TEST_CHECK(site_writer_write_album_pages(&writer, &NO_OUTPUT_CACHE, albums, 1, rendered,
                                           test_arena(), &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to write page for 'root album': %s ('public')\n",
                         strerror(EISDIR)));
}

// A failed aggregate write names the operation and the cause, then the destination. The destination
// is an existing directory, so opening it for writing fails.
static void test_write_aggregates_reports_write_failure(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "templates/map.xml", "<g></g>");
  os_memory_mkdir(os, "public/map.xml");
  static const struct Str aggregates[] = {STR_INIT("map.xml")};
  struct SiteConfig site_config;
  site_config_init(&site_config);
  site_config.output_dir = S("public");
  site_config.templates_dir = S("templates");
  site_config.templates.aggregates = aggregates;
  site_config.templates.aggregate_template_count = 1;
  struct Album root = {.source_dir = S(""), .title = S("Root"), .path_to_root = S("")};
  static const char* const output_paths[] = {"public/map.xml"};
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, output_paths, 1, os, &arena);
  struct Log log = test_log();

  struct TemplateSet* template_set = load_templates(site_config.templates_dir, aggregates, 1, os);
  TEST_CHECK(site_writer_write_aggregates(&writer, &NO_OUTPUT_CACHE, &site_config, template_set,
                                          &root, test_arena(), test_arena(), &log) == -1);
  template_set_release(template_set);
  TEST_CHECK(test_log_is(&log, "failed to write page: %s ('public/map.xml')\n", strerror(EISDIR)));
}

TEST_LIST = {
    {"writes complete output layer", test_writes_complete_output_layer},
    {"write not found renders from the root", test_write_not_found_renders_from_the_root},
    {"keeps unchanged pages untouched", test_keeps_unchanged_pages_untouched},
    {"write album pages names root album in words",
     test_write_album_pages_names_root_album_in_words},
    {"write album pages quotes sub album source dir",
     test_write_album_pages_quotes_sub_album_source_dir},
    {"write aggregates reports write failure", test_write_aggregates_reports_write_failure},
    {NULL, NULL},
};
