#include <acutest.h>
#include <stddef.h>

#include "build/page_renderer.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/str.h"
#include "domain/album.h"
#include "domain/site_config.h"
#include "os/os_memory.h"
#include "runtime/job.h"
#include "templates/template_set.h"
#include "testing/test_support.h"

// Every test here runs against the in-memory host, which holds the templates that each test loads
// before the page phase renders them.

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

// Parallel jobs render each album into its corresponding output slot with independent context.
static void test_renders_pages_by_index(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "templates/album.html", "{{album.title}}:{{path_to_root}}");
  struct SiteConfig site_config;
  site_config_init(&site_config);
  site_config.title = S("Gallery");
  site_config.description = S("Description");
  site_config.author = S("Ada");
  struct Album root = {.source_dir = S(""), .title = S("Root"), .path_to_root = S("")};
  struct Album child = {.source_dir = S("trip"), .title = S("Trip"), .path_to_root = S("../")};
  const struct Album* albums[] = {&root, &child};
  struct RenderedPage rendered[2] = {0};
  struct Arena arena = test_arena();
  struct Log log = {.buf = {.arena = &arena}};
  // Every worker gets arenas of its own, as `cmd_build_execute` carves them, so no two concurrent
  // jobs share one.
  struct JobArenas worker_arenas[] = {{.perm = test_arena(), .scratch = test_arena()},
                                      {.perm = test_arena(), .scratch = test_arena()}};

  struct TemplateSet* template_set =
      load_templates(site_config.templates_dir, &site_config.album_template, 1, os);
  TEST_CHECK(page_renderer_render_pages(&site_config, template_set, albums, 2, 2, worker_arenas,
                                        false, os, test_arena(), rendered, &log) == 0);
  template_set_release(template_set);
  TEST_CHECK(str_eq(rendered[0].html, S("Root:")));
  TEST_CHECK(str_eq(rendered[1].html, S("Trip:../")));
  TEST_CHECK(test_log_is_empty(&log));
}

// A render failure is the failing album's error in the collected log, and names the template it is
// in and the page it was for. The two albums fail alike, so the log prints the first and counts the
// second.
static void test_reports_render_failure_per_album(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "templates/album.html", "{{{album.title}}}");
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct Album root = {.source_dir = S(""), .title = S("Root"), .url_path = S("index.html")};
  struct Album trip = {.source_dir = S("trip"),
                       .title = S("Trip"),
                       .url_path = S("trip/index.html"),
                       .path_to_root = S("../")};
  const struct Album* albums[] = {&root, &trip};
  struct RenderedPage rendered[2] = {0};
  struct Arena arena = test_arena();
  struct Log log = {.buf = {.arena = &arena}};
  struct JobArenas worker_arenas = {.perm = test_arena(), .scratch = test_arena()};
  struct TemplateSet* template_set =
      load_templates(site_config.templates_dir, &site_config.album_template, 1, os);

  TEST_CHECK(page_renderer_render_pages(&site_config, template_set, albums, 2, 1, &worker_arenas,
                                        false, os, test_arena(), rendered, &log) == -1);
  template_set_release(template_set);
  TEST_CHECK(rendered[0].html.len == 0 && rendered[1].html.len == 0);
  // Exact, not by substring: a substring check would also pass for this message with something
  // appended to it.
  TEST_CHECK(test_log_is(&log,
                         "unescaped Mustache interpolation is supported only for a key that ends "
                         "in '_html' (in 'album.html') (while rendering 'index.html')\n"
                         "\xE2\x80\xA6 and 1 more failures\n"));
}

TEST_LIST = {
    {"renders pages by index", test_renders_pages_by_index},
    {"reports render failure per album", test_reports_render_failure_per_album},
    {NULL, NULL},
};
