#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "build/entry_renderer.h"
#include "build/page_renderer.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/error.h"
#include "core/log.h"
#include "core/path.h"
#include "core/path_list.h"
#include "core/str.h"
#include "domain/content_entry.h"
#include "domain/site_config.h"
#include "os/os_memory.h"
#include "output/manifest.h"
#include "output/output_writer.h"
#include "runtime/job.h"
#include "testing/test_support.h"

// Every test here runs against the in-memory host, whose root is the working directory the phases
// resolve the config and the sources from, and which keeps the pages they write and the progress
// they print.

/**
 * Size of a buffer for an expected diagnostic. The log never truncates, so this only has to hold
 * the longest message a test spells out.
 */
enum { EXPECTED_SIZE = 1024 };

/** Config most fixtures share. It turns off the aggregate and feed phases. */
static const char* const SITE_CONFIG =
    "title = \"Site\"\n"
    "description = \"Example notes\"\n"
    "author = \"Author\"\n"
    "base_url = \"https://example.com\"\n"
    "aggregate_templates = []\n"
    "feed_templates = []\n";

/**
 * @brief Allocates one `JobArenas` per worker, each region a fresh `test_arena` reservation.
 *
 * @param worker_count Number of workers to allocate arenas for.
 * @param perm         Arena that owns the array. Must not be `NULL`.
 * @return The array of `worker_count` arenas, none overlapping another or `perm`.
 */
static struct JobArenas* test_job_arenas(ptrdiff_t worker_count, struct Arena* perm) {
  struct JobArenas* arenas = arena_new(perm, worker_count, struct JobArenas);
  for (ptrdiff_t i = 0; i < worker_count; i++) {
    arenas[i].perm = test_arena();
    arenas[i].scratch = test_arena();
  }
  return arenas;
}

/**
 * @brief Returns a fresh host holding `SITE_CONFIG` as `sosig.toml`, one source, and a content
 *        template.
 *
 * @param source_path      Terminated path of the source. Must not be `NULL`.
 * @param source           Terminated text of the source. Must not be `NULL`.
 * @param content_template Terminated text of `templates/content.html`. Must not be `NULL`.
 * @return The host.
 */
static struct Os* site_host(const char* source_path,
                            const char* source,
                            const char* content_template) {
  struct Os* os = os_memory();
  os_memory_write(os, "sosig.toml", SITE_CONFIG);
  os_memory_write(os, source_path, source);
  os_memory_write(os, "templates/content.html", content_template);
  return os;
}

/**
 * @brief Runs the page phase over entry slots the parse phase filled.
 *
 * Collects, sorts and dates the published entries the way the build does before its page phase.
 *
 * @param site_config        Configuration used for rendering. Must not be `NULL`.
 * @param source_entries     Entry slots filled by the parse phase. Must not be `NULL`.
 * @param source_entry_count Number of slots in `source_entries`.
 * @param worker_count       Worker threads the page phase runs on.
 * @param worker_arenas      One `JobArenas` per worker, holding the parsed entries. Must not be
 *                           `NULL`.
 * @param os                 Host holding the templates, which receives the pages. Must not be
 *                           `NULL`.
 * @param log                Log that receives every error. Its arena also holds the collected entry
 *                           array. Must not be `NULL`.
 * @return `0` on success, or `-1` on render failure.
 */
static int render_pages(const struct SiteConfig* site_config,
                        struct ContentEntry* const* source_entries,
                        ptrdiff_t source_entry_count,
                        ptrdiff_t worker_count,
                        struct JobArenas* worker_arenas,
                        struct Os* os,
                        struct Log* log) {
  struct ContentEntry** entries =
      arena_new(log->buf.arena, source_entry_count, struct ContentEntry*);
  // The manifest names every page, as the build's would.
  struct Arena manifest_arena = test_arena();
  struct Manifest manifest = {0};
  ptrdiff_t entry_count = 0;
  for (ptrdiff_t i = 0; i < source_entry_count; i++) {
    if (source_entries[i] != NULL) {
      entries[entry_count++] = source_entries[i];
      (void)manifest_add(&manifest, source_entries[i]->output_path, source_entries[i]->source_path,
                         &manifest_arena, NULL);
    }
  }
  content_entry_sort(entries, entry_count);
  const struct OutputWriter writer = {.manifest = &manifest, .os = os};
  bool* entry_list_flags = arena_new(log->buf.arena, source_entry_count, bool);
  return page_renderer_render_pages(
      &writer, site_config, (const struct ContentEntry* const*)source_entries, source_entry_count,
      (const struct ContentEntry* const*)entries, entry_count, NULL, worker_count, worker_arenas,
      false, os, test_arena(), entry_list_flags, log);
}

/**
 * @brief Renders named sources through the three parallel phases on one worker.
 *
 * Loads `sosig.toml` from the host's working directory. The configuration, the path list, and the
 * entry arrays live in `log`'s arena, and the entries in worker arenas of their own, so all of them
 * outlive the call.
 *
 * @param relative_paths      Source paths relative to the working directory. Must not be `NULL`.
 * @param relative_path_count Number of entries in `relative_paths`.
 * @param os                  Host holding the config, the sources and the templates, which receives
 *                            the pages. Must not be `NULL`.
 * @param site_config         Configuration populated from `sosig.toml`. Must not be `NULL`.
 * @param source_paths        Path list populated with the sources. Must not be `NULL`.
 * @param source_entries      One entry slot per path in `relative_paths`, each `NULL`-initialized.
 *                            Must not be `NULL`.
 * @param log                 Log that receives every error. Its arena also holds the configuration
 *                            and the path list, which only ever reach it by pointer. Must not be
 *                            `NULL`.
 * @return `0` on success, `-1` on render failure, or `TEST_PLUMBING_FAILED` when the config does
 *         not load.
 */
static int render_sources(const char* const* relative_paths,
                          ptrdiff_t relative_path_count,
                          struct Os* os,
                          struct SiteConfig* site_config,
                          struct PathList* source_paths,
                          struct ContentEntry** source_entries,
                          struct Log* log) {
  const bool is_config_loaded =
      site_config_load(site_config, S("sosig.toml"), os, log->buf.arena, test_arena(), log) == 0 &&
      log->errors == 0;
  const struct Str config_errors = buf_str(&log->buf);
  TEST_CHECK(is_config_loaded);
  TEST_MSG("config: %.*s", STR_FMT(config_errors));
  if (!is_config_loaded) {
    return TEST_PLUMBING_FAILED;
  }
  for (ptrdiff_t i = 0; i < relative_path_count; i++) {
    path_list_push(source_paths, str_from_cstr(relative_paths[i]), log->buf.arena);
  }
  struct JobArenas* worker_arenas = test_job_arenas(1, log->buf.arena);
  bool* draft_flags = arena_new(log->buf.arena, relative_path_count, bool);
  const struct PathList asset_paths = {0};
  int rc = entry_renderer_parse_entries(site_config, source_paths, CONTENT_FILE_LEN_MAX, 1,
                                        worker_arenas, false, os, test_arena(), source_entries,
                                        draft_flags, log);
  if (rc == 0) {
    rc =
        entry_renderer_convert_bodies(source_entries, draft_flags, site_config, source_paths,
                                      &asset_paths, 1, worker_arenas, false, os, test_arena(), log);
  }
  if (rc == 0) {
    rc = render_pages(site_config, source_entries, relative_path_count, 1, worker_arenas, os, log);
  }
  return rc;
}

/**
 * @brief Renders one source through the three parallel phases on one worker.
 *
 * Equivalent to `render_sources` with a single path.
 *
 * @param source_relative_path Source path relative to the working directory. Must not be `NULL`.
 * @param os                   Host holding the site, as for `render_sources`. Must not be `NULL`.
 * @param site_config          Configuration populated from `sosig.toml`. Must not be `NULL`.
 * @param source_paths         Path list populated with the source. Must not be `NULL`.
 * @param source_entries       One `NULL`-initialized entry slot. Must not be `NULL`.
 * @param log                  Log that receives every error, as for `render_sources`. Must not be
 *                             `NULL`.
 * @return `0` on success, `-1` on render failure, or `TEST_PLUMBING_FAILED` when the config does
 *         not load.
 */
static int render_single_source(const char* source_relative_path,
                                struct Os* os,
                                struct SiteConfig* site_config,
                                struct PathList* source_paths,
                                struct ContentEntry** source_entries,
                                struct Log* log) {
  return render_sources(&source_relative_path, 1, os, site_config, source_paths, source_entries,
                        log);
}

/**
 * @brief Runs the parse phase over one source on one worker.
 *
 * Uses the default configuration, whose `content_dir` is `content`.
 *
 * @param relative_path  Source path relative to the working directory. Must not be `NULL`.
 * @param source_len_max Largest source the job reads, in bytes.
 * @param worker_arenas  Arenas of the one worker. Must not be `NULL`.
 * @param os             Host holding the source. Must not be `NULL`.
 * @param entry_out      Receives the entry slot the job fills. Must not be `NULL`.
 * @param log            Log that receives the parse errors. Must not be `NULL`.
 * @return The renderer result.
 */
static int parse_source(const char* relative_path,
                        ptrdiff_t source_len_max,
                        struct JobArenas* worker_arenas,
                        struct Os* os,
                        struct ContentEntry** entry_out,
                        struct Log* log) {
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths = {0};
  path_list_push(&source_paths, str_from_cstr(relative_path), log->buf.arena);
  *entry_out = NULL;
  bool is_draft = false;
  return entry_renderer_parse_entries(&site_config, &source_paths, source_len_max, 1, worker_arenas,
                                      false, os, test_arena(), entry_out, &is_draft, log);
}

// A single source renders its frontmatter metadata, body HTML, page output, and output paths.
static void test_renders_entry_metadata_and_html(void) {
  const char content[] =
      "+++\n"
      "title = \"Hello\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "+++\n"
      "Body\n";
  struct Os* os =
      site_host("content/hello.md", content, "<main>{{title}} {{{body_html}}}</main>\n");
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths = {0};
  struct ContentEntry* source_entries[1] = {NULL};
  struct Arena perm = test_arena();
  struct Log log = {.buf = {.arena = &perm}};

  TEST_CHECK(render_single_source("content/hello.md", os, &site_config, &source_paths,
                                  source_entries, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  const struct ContentEntry* entry = source_entries[0];
  TEST_ASSERT(entry != NULL);
  TEST_CHECK(str_eq(entry->title, S("Hello")));
  TEST_CHECK(str_eq(entry->slug, S("hello")));
  TEST_CHECK(strstr(str_cstr(entry->body_html, &perm), "<p>Body</p>") != NULL);
  TEST_CHECK(str_eq(entry->url_path, S("/hello.html")));
  TEST_CHECK(str_eq(entry->output_path, S("public/hello.html")));

  TEST_CHECK(os_memory_is_file(os, "public/hello.html", "<main>Hello <p>Body</p>\n</main>\n"));
}

// An Org source renders its keyword metadata and its whole file as the body, through the Org
// converter, with the same slug, paths and page as a Markdown source of that name.
static void test_renders_org_entry_metadata_and_html(void) {
  const char content[] =
      ":PROPERTIES:\n"
      ":ID: 1234\n"
      ":END:\n"
      "#+title: Hello\n"
      "#+date: [2026-07-01 Wed]\n"
      "/Body/\n";
  struct Os* os =
      site_host("content/hello.org", content, "<main>{{title}} {{{body_html}}}</main>\n");
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths = {0};
  struct ContentEntry* source_entries[1] = {NULL};
  struct Arena perm = test_arena();
  struct Log log = {.buf = {.arena = &perm}};

  TEST_CHECK(render_single_source("content/hello.org", os, &site_config, &source_paths,
                                  source_entries, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  const struct ContentEntry* entry = source_entries[0];
  TEST_ASSERT(entry != NULL);
  TEST_CHECK(str_eq(entry->title, S("Hello")));
  TEST_CHECK(str_eq(entry->slug, S("hello")));
  TEST_CHECK(str_eq(entry->body_html, S("<p><em>Body</em></p>\n")));
  TEST_CHECK(str_eq(entry->url_path, S("/hello.html")));
  TEST_CHECK(str_eq(entry->output_path, S("public/hello.html")));

  TEST_CHECK(
      os_memory_is_file(os, "public/hello.html", "<main>Hello <p><em>Body</em></p>\n</main>\n"));
}

// A draft source is parsed but leaves its entry slot `NULL` and writes no page.
static void test_skips_draft_entry(void) {
  const char content[] =
      "+++\n"
      "title = \"Hello\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "draft = true\n"
      "+++\n"
      "Body\n";
  struct Os* os = site_host("content/hello.md", content, "{{{body_html}}}\n");
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths = {0};
  struct ContentEntry* source_entries[1] = {NULL};
  struct Arena perm = test_arena();
  struct Log log = {.buf = {.arena = &perm}};

  TEST_CHECK(render_single_source("content/hello.md", os, &site_config, &source_paths,
                                  source_entries, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(source_entries[0] == NULL);

  TEST_CHECK(!os_memory_has_path(os, "public/hello.html"));
}

// An Org draft is parsed but leaves its entry slot `NULL` and writes no page, as a Markdown one
// does.
static void test_skips_org_draft_entry(void) {
  const char content[] =
      "#+title: Hello\n"
      "#+date: 2026-07-01\n"
      "#+draft: true\n"
      "Body\n";
  struct Os* os = site_host("content/hello.org", content, "{{{body_html}}}\n");
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths = {0};
  struct ContentEntry* source_entries[1] = {NULL};
  struct Arena perm = test_arena();
  struct Log log = {.buf = {.arena = &perm}};

  TEST_CHECK(render_single_source("content/hello.org", os, &site_config, &source_paths,
                                  source_entries, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(source_entries[0] == NULL);

  TEST_CHECK(!os_memory_has_path(os, "public/hello.html"));
}

// A job advances its worker's `perm` only when it publishes an entry. A draft and a source that
// fails to parse leave it where it was, so neither keeps memory for the rest of the build.
static void test_keeps_perm_only_for_a_published_entry(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "content/draft.md",
                  "+++\ntitle = \"Draft\"\ndate = 2026-07-01\ndraft = true\n+++\nBody\n");
  os_memory_write(os, "content/broken.md", "no fence\n");
  os_memory_write(os, "content/hello.md", "+++\ntitle = \"Hello\"\ndate = 2026-07-01\n+++\nBody\n");
  struct Arena perm = test_arena();
  struct Log log = {.buf = {.arena = &perm}};
  struct JobArenas* worker_arenas = test_job_arenas(1, &perm);
  struct ContentEntry* entry = NULL;

  char* perm_beg = worker_arenas[0].perm.beg;
  TEST_CHECK(
      parse_source("content/draft.md", CONTENT_FILE_LEN_MAX, worker_arenas, os, &entry, &log) == 0);
  TEST_CHECK(entry == NULL && worker_arenas[0].perm.beg == perm_beg);

  TEST_CHECK(parse_source("content/broken.md", CONTENT_FILE_LEN_MAX, worker_arenas, os, &entry,
                          &log) == -1);
  TEST_CHECK(entry == NULL && worker_arenas[0].perm.beg == perm_beg);
  TEST_CHECK(
      test_log_is_one(&log, "missing opening '+++' frontmatter fence (in 'content/broken.md')"));

  TEST_CHECK(
      parse_source("content/hello.md", CONTENT_FILE_LEN_MAX, worker_arenas, os, &entry, &log) == 0);
  TEST_CHECK(entry != NULL && worker_arenas[0].perm.beg > perm_beg);
}

// A verbose phase prints one progress line per finished job, drafts included, then closes the line.
// The lines are printed from worker threads under the lock `job.c` keeps for the progress line.
// This drives it with more workers than one so the lock is contended, and the `tsan` test preset
// runs the same path under ThreadSanitizer.
static void test_verbose_prints_one_progress_line_per_job(void) {
  const char page[] =
      "+++\n"
      "title = \"Hello\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "+++\n"
      "Body\n";
  const char draft[] =
      "+++\n"
      "title = \"Draft\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "draft = true\n"
      "+++\n"
      "Body\n";
  enum { WORKER_COUNT = 4 };
  struct Os* os = os_memory();
  os_memory_write(os, "sosig.toml", SITE_CONFIG);
  os_memory_write(os, "content/hello.md", page);
  os_memory_write(os, "content/second.md", page);
  os_memory_write(os, "content/draft.md", draft);
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths = {0};
  struct ContentEntry* source_entries[3] = {NULL};
  struct Arena perm = test_arena();
  struct Log log = {.buf = {.arena = &perm}};
  path_list_push(&source_paths, S("content/hello.md"), &perm);
  path_list_push(&source_paths, S("content/second.md"), &perm);
  path_list_push(&source_paths, S("content/draft.md"), &perm);
  TEST_ASSERT(site_config_load(&site_config, S("sosig.toml"), os, &perm, test_arena(), &log) == 0 &&
              log.errors == 0);

  bool* draft_flags = arena_new(&perm, source_paths.count, bool);
  TEST_CHECK(entry_renderer_parse_entries(&site_config, &source_paths, CONTENT_FILE_LEN_MAX,
                                          WORKER_COUNT, test_job_arenas(WORKER_COUNT, &perm), true,
                                          os, test_arena(), source_entries, draft_flags,
                                          &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  // Three jobs, so three progress lines even though one entry is a draft that produces no output,
  // and one trailing newline closing the line before any later status message. Workers finish in
  // any order, so each count is asserted present once rather than the whole text in sequence.
  const char* stderr_out = str_cstr(os_memory_printed(os, 2), &perm);
  static const char* const progress_lines[] = {"\rparsing content 1/3", "\rparsing content 2/3",
                                               "\rparsing content 3/3"};
  size_t progress_len = 0;
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(progress_lines) / sizeof(progress_lines[0])); i++) {
    const char* found = strstr(stderr_out, progress_lines[i]);
    TEST_CHECK(found != NULL && strstr(found + 1, progress_lines[i]) == NULL);
    progress_len += strlen(progress_lines[i]);
  }
  TEST_CHECK(strlen(stderr_out) == progress_len + 1);
  TEST_CHECK(stderr_out[progress_len] == '\n');
  TEST_MSG("stderr: '%s'", stderr_out);
}

// A source nested two directories deep publishes under both slugified section segments, joined by a
// single `/`. One level of nesting cannot show this. The join loop runs once, so a test with one
// level cannot observe the separator insertion or the per-segment slugification of anything beyond
// the first segment. Both directory names here need slugifying, so a join that skipped the
// separator would publish `my-sectionsub-dir` as one directory.
static void test_nests_output_under_slugified_sections(void) {
  const char content[] =
      "+++\n"
      "title = \"Hello\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "+++\n"
      "Body\n";
  const char* const source_relative_path = "content/My Section/Sub Dir/hello.md";
  struct Os* os = site_host(source_relative_path, content, "{{{body_html}}}\n");
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths = {0};
  struct ContentEntry* source_entries[1] = {NULL};
  struct Arena perm = test_arena();
  struct Log log = {.buf = {.arena = &perm}};

  TEST_CHECK(render_single_source(source_relative_path, os, &site_config, &source_paths,
                                  source_entries, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  const struct ContentEntry* entry = source_entries[0];
  TEST_ASSERT(entry != NULL);
  TEST_CHECK(str_eq(entry->url_path, S("/my-section/sub-dir/hello.html")));
  TEST_CHECK(str_eq(entry->output_path, S("public/my-section/sub-dir/hello.html")));
}

// The parse and conversion phases accept an empty site with `NULL` slot arrays, which their
// contracts allow for a count of 0, and run no job. `test_accepts_empty_entry_set` in
// `test_page_renderer.c` pins the same for the page phase.
static void test_accepts_empty_site(void) {
  struct Os* os = os_memory();
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths = {0};
  struct Arena perm = test_arena();
  struct Log log = {.buf = {.arena = &perm}};

  TEST_CHECK(entry_renderer_parse_entries(&site_config, &source_paths, CONTENT_FILE_LEN_MAX, 1,
                                          test_job_arenas(1, &perm), false, os, test_arena(), NULL,
                                          NULL, &log) == 0);
  TEST_CHECK(entry_renderer_convert_bodies(NULL, NULL, &site_config, &source_paths, &source_paths,
                                           1, test_job_arenas(1, &perm), false, os, test_arena(),
                                           &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
}

// A source that exists but cannot be read is reported with the system cause and the path it
// happened on, rather than as a generic parse failure. The host fails the read with `EACCES`, as a
// source without read permission does.
static void test_reports_unreadable_source(void) {
  char reason[ERROR_SYSTEM_MESSAGE_SIZE];
  char expected[EXPECTED_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "failed to read content: %s ('content/hello.md')",
               error_system_message(reason, sizeof(reason), EACCES));
  TEST_ASSERT(expected_len > 0 && expected_len < (int)sizeof(expected));

  const char content[] =
      "+++\n"
      "title = \"Hello\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "+++\n"
      "Body\n";
  struct Os* os = site_host("content/hello.md", content, "{{{body_html}}}\n");
  os_memory_fail(os, OS_MEMORY_OP_READ, "content/hello.md", EACCES);
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths = {0};
  struct ContentEntry* source_entries[1] = {NULL};
  struct Arena perm = test_arena();
  struct Log log = {.buf = {.arena = &perm}};

  TEST_CHECK(render_single_source("content/hello.md", os, &site_config, &source_paths,
                                  source_entries, &log) == -1);
  // Exact, not by substring: a substring check would also pass for this message with something
  // appended to it.
  TEST_CHECK(test_log_is(&log, "%s\n", expected));
}

// A source larger than `CONTENT_FILE_LEN_MAX` is rejected at the read, from its size, naming the
// limit and the size, rather than read whole and turned away by the converter. The host stores only
// the size of the extension, so the fixture costs no memory and a read that loaded it would show
// only as the wrong diagnostic.
static void test_rejects_oversize_source(void) {
  const ptrdiff_t source_len = (ptrdiff_t)CONTENT_FILE_LEN_MAX + 1;
  char expected[EXPECTED_SIZE];
  const int expected_len = snprintf(expected, sizeof(expected),
                                    "failed to read content: exceeds max file size (%d bytes) at "
                                    "%td bytes ('content/hello.md')",
                                    CONTENT_FILE_LEN_MAX, source_len);
  TEST_ASSERT(expected_len > 0 && expected_len < (int)sizeof(expected));

  struct Os* os =
      site_host("content/hello.md", "+++\ntitle = \"Hello\"\n+++\n", "{{{body_html}}}\n");
  os_memory_truncate(os, "content/hello.md", source_len);
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths = {0};
  struct ContentEntry* source_entries[1] = {NULL};
  struct Arena perm = test_arena();
  struct Log log = {.buf = {.arena = &perm}};

  TEST_CHECK(render_single_source("content/hello.md", os, &site_config, &source_paths,
                                  source_entries, &log) == -1);
  TEST_CHECK(test_log_is(&log, "%s\n", expected));
}

// A source larger than the limit the build passes, such as one that grew since the walk sized the
// worker's memory, fails its own job at the read and leaves the worker's `perm` untouched.
static void test_rejects_source_over_job_limit(void) {
  enum { SOURCE_LEN_MAX = 8 };
  static const char content[] = "+++\ntitle = \"Hello\"\n+++\nBody\n";
  char expected[EXPECTED_SIZE];
  const int expected_len = snprintf(expected, sizeof(expected),
                                    "failed to read content: exceeds max file size (%d bytes) at "
                                    "%zu bytes ('content/hello.md')",
                                    SOURCE_LEN_MAX, sizeof(content) - 1);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  struct Os* os = os_memory();
  os_memory_write(os, "content/hello.md", content);
  struct Arena perm = test_arena();
  struct Log log = {.buf = {.arena = &perm}};
  struct JobArenas* worker_arenas = test_job_arenas(1, &perm);
  char* perm_beg = worker_arenas[0].perm.beg;
  struct ContentEntry* entry = NULL;

  TEST_CHECK(parse_source("content/hello.md", SOURCE_LEN_MAX, worker_arenas, os, &entry, &log) ==
             -1);
  TEST_CHECK(test_log_is(&log, "%s\n", expected));
  TEST_CHECK(entry == NULL && worker_arenas[0].perm.beg == perm_beg);
}

// Two failing sources each append one line ending in a newline, with nothing before or between
// them. `entry_renderer.h` states this as errors in source order, as `job_run` collects them, and a
// single-source failure cannot observe it at all. `test_reports_write_failure_per_entry` in
// `src/build/test_page_renderer.c` pins the same for failures in the page phase, which runs only
// after this phase passes. This one pins it at the module boundary, where the contract is
// documented.
static void test_appends_one_error_line_per_failing_source(void) {
  // Both sources omit the required `title`, so each fails in the parse phase with its own message.
  const char first[] =
      "+++\n"
      "date = 2026-07-01T00:00:00Z\n"
      "+++\n"
      "Body\n";
  const char second[] =
      "+++\n"
      "date = 2026-07-02T00:00:00Z\n"
      "+++\n"
      "Body\n";
  static const char* const sources[] = {"content/a.md", "content/b.md"};
  struct Os* os = site_host("content/a.md", first, "{{{body_html}}}\n");
  os_memory_write(os, "content/b.md", second);
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths = {0};
  struct ContentEntry* source_entries[2] = {NULL};
  struct Arena perm = test_arena();
  struct Log log = {.buf = {.arena = &perm}};

  TEST_CHECK(render_sources(sources, sizeof(sources) / sizeof(sources[0]), os, &site_config,
                            &source_paths, source_entries, &log) == -1);
  // Compared whole, with the separator in the middle: a dropped separator, a lost line, or a
  // leading or trailing newline each change these bytes.
  TEST_CHECK(test_log_is(&log,
                         "missing required frontmatter key 'title' (in 'content/a.md')\n"
                         "missing required frontmatter key 'title' (in 'content/b.md')\n"));
}

// The whole expanded path is capped, not only each segment, so a pattern that is legal segment by
// segment still fails here. Config load does not measure a pattern, so this per-entry check is the
// one limit on the expanded path.
static void test_rejects_oversize_output_path(void) {
  // A long but URL-safe literal segment clears `path_is_safe_relative` and reaches the length cap.
  char pattern[OUTPUT_PATH_RELATIVE_LEN_MAX + 64];
  memset(pattern, 'a', sizeof(pattern) - 1);
  pattern[sizeof(pattern) - 1] = '\0';
  pattern[0] = '/';
  memcpy(pattern + sizeof(pattern) - sizeof("/{slug}.html"), "/{slug}.html",
         sizeof("/{slug}.html"));
  // The relative output path is the pattern without its leading `/`, with the slug of
  // `content/hello.md` in place of `{slug}`.
  char expected_path[sizeof(pattern)];
  const int expected_path_len =
      snprintf(expected_path, sizeof(expected_path), "%.*s/hello.html",
               (int)(sizeof(pattern) - sizeof("/{slug}.html") - 1), pattern + 1);
  TEST_ASSERT(expected_path_len > 0 && expected_path_len < (int)sizeof(expected_path));
  // Config load accepts the pattern, because only an expanded path is measured.
  char config[OUTPUT_PATH_RELATIVE_LEN_MAX + 256];
  const int config_len =
      snprintf(config, sizeof(config), "%spermalink = \"%s\"\n", SITE_CONFIG, pattern);
  TEST_ASSERT(config_len > 0 && config_len < (int)sizeof(config));
  const char content[] =
      "+++\n"
      "title = \"Hello\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "+++\n"
      "Body\n";
  struct Os* os = site_host("content/hello.md", content, "{{{body_html}}}\n");
  os_memory_write(os, "sosig.toml", config);
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths = {0};
  struct ContentEntry* source_entries[1] = {NULL};
  struct Arena perm = test_arena();
  struct Log log = {.buf = {.arena = &perm}};

  TEST_CHECK(render_single_source("content/hello.md", os, &site_config, &source_paths,
                                  source_entries, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "output path for 'content/hello.md' exceeds max output path length (%d "
                         "bytes) at %d bytes: '%s'\n",
                         OUTPUT_PATH_RELATIVE_LEN_MAX, expected_path_len, expected_path));
  TEST_CHECK(log.errors == 1);
}

// A segment that overflows once a slug replaces its token is rejected per entry, with a diagnostic
// naming the limit and the segment, even though the whole path is under the output-path cap.
static void test_rejects_oversize_path_segment(void) {
  // A maximum-length slug plus the pattern's literal suffix, which together exceed
  // `FILENAME_LEN_MAX` while the total path stays inside `OUTPUT_PATH_RELATIVE_LEN_MAX`, so only
  // the per-segment check can reject this.
  static const char suffix[] = "-overflow.html";
  enum { SLUG_LEN = FILENAME_LEN_MAX - (int)(sizeof(".html") - 1) };
  char slug[SLUG_LEN + 1];
  memset(slug, 'a', sizeof(slug) - 1);
  slug[sizeof(slug) - 1] = '\0';
  char segment[SLUG_LEN + sizeof(suffix)];
  const int segment_len = snprintf(segment, sizeof(segment), "%s%s", slug, suffix);
  TEST_ASSERT(segment_len > 0 && segment_len < (int)sizeof(segment));
  _Static_assert(SLUG_LEN + sizeof(suffix) - 1 > FILENAME_LEN_MAX,
                 "the slug plus the pattern suffix must exceed the filename limit");
  _Static_assert(SLUG_LEN + sizeof(suffix) - 1 < OUTPUT_PATH_RELATIVE_LEN_MAX,
                 "the oversize segment must stay within the total output path limit");
  char content[SLUG_LEN + 128];
  const int content_len = snprintf(content, sizeof(content),
                                   "+++\n"
                                   "title = \"Hello\"\n"
                                   "date = 2026-07-01T00:00:00Z\n"
                                   "slug = \"%s\"\n"
                                   "+++\n"
                                   "Body\n",
                                   slug);
  TEST_ASSERT(content_len > 0 && content_len < (int)sizeof(content));
  const char config[] =
      "title = \"Site\"\n"
      "description = \"Example notes\"\n"
      "author = \"Author\"\n"
      "base_url = \"https://example.com\"\n"
      "permalink = \"/{slug}-overflow.html\"\n"
      "aggregate_templates = []\n"
      "feed_templates = []\n";
  struct Os* os = site_host("content/hello.md", content, "{{{body_html}}}\n");
  os_memory_write(os, "sosig.toml", config);
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths = {0};
  struct ContentEntry* source_entries[1] = {NULL};
  struct Arena perm = test_arena();
  struct Log log = {.buf = {.arena = &perm}};

  TEST_CHECK(render_single_source("content/hello.md", os, &site_config, &source_paths,
                                  source_entries, &log) == -1);
  // It is asserted in full, including the offending segment it names: a length alone would leave
  // the user no way to find which segment of the path was too long.
  TEST_CHECK(test_log_is(&log,
                         "output path segment for 'content/hello.md' exceeds max filename length "
                         "(%d bytes) at %zu bytes: '%s'\n",
                         FILENAME_LEN_MAX, strlen(segment), segment));
}

// A source with no frontmatter fence fails in `frontmatter_split`, which names the source after the
// cause, like the `frontmatter_parse` errors.
static void test_reports_missing_frontmatter_fence(void) {
  struct Os* os = site_host("content/hello.md", "no fence here\n", "<main>{{title}}</main>\n");
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths = {0};
  struct ContentEntry* source_entries[1] = {NULL};
  struct Arena perm = test_arena();
  struct Log log = {.buf = {.arena = &perm}};

  TEST_CHECK(render_single_source("content/hello.md", os, &site_config, &source_paths,
                                  source_entries, &log) == -1);
  TEST_CHECK(
      test_log_is(&log, "missing opening '+++' frontmatter fence (in 'content/hello.md')\n"));
}

// An Org source without its required keywords fails in `frontmatter_parse_org`, which names the
// source after each cause, as a Markdown source without a fence fails in `frontmatter_split`.
static void test_reports_missing_org_keywords(void) {
  struct Os* os =
      site_host("content/hello.org", "* No keywords here\n", "<main>{{title}}</main>\n");
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct PathList source_paths = {0};
  struct ContentEntry* source_entries[1] = {NULL};
  struct Arena perm = test_arena();
  struct Log log = {.buf = {.arena = &perm}};

  TEST_CHECK(render_single_source("content/hello.org", os, &site_config, &source_paths,
                                  source_entries, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "missing required Org keyword 'title' (in 'content/hello.org')\n"
                         "missing required Org keyword 'date' (in 'content/hello.org')\n"));
  TEST_CHECK(source_entries[0] == NULL);
}

TEST_LIST = {
    {"renders entry metadata and html", test_renders_entry_metadata_and_html},
    {"renders org entry metadata and html", test_renders_org_entry_metadata_and_html},
    {"skips draft entry", test_skips_draft_entry},
    {"skips org draft entry", test_skips_org_draft_entry},
    {"keeps perm only for a published entry", test_keeps_perm_only_for_a_published_entry},
    {"verbose prints one progress line per job", test_verbose_prints_one_progress_line_per_job},
    {"nests output under slugified sections", test_nests_output_under_slugified_sections},
    {"accepts empty site", test_accepts_empty_site},
    {"reports unreadable source", test_reports_unreadable_source},
    {"rejects oversize source", test_rejects_oversize_source},
    {"rejects source over job limit", test_rejects_source_over_job_limit},
    {"appends one error line per failing source", test_appends_one_error_line_per_failing_source},
    {"rejects oversize output path", test_rejects_oversize_output_path},
    {"rejects oversize path segment", test_rejects_oversize_path_segment},
    {"reports missing frontmatter fence", test_reports_missing_frontmatter_fence},
    {"reports missing org keywords", test_reports_missing_org_keywords},
    {NULL, NULL},
};
