#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app/cmd_build.h"
#include "build/entry_renderer.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/exit_code.h"
#include "core/log.h"
#include "core/path_list.h"
#include "core/str.h"
#include "domain/site_config.h"
#include "formats/toml.h"
#include "os/os.h"
#include "os/os_memory.h"
#include "runtime/job.h"
#include "runtime/pool.h"
#include "testing/test_support.h"

// Every build here runs against the in-memory host, whose root is the working directory the build
// resolves the config path from, and which keeps what the build prints. The golden sites build the
// real executable on the real filesystem.

/**
 * Size of a buffer for an expected diagnostic. The log never truncates, so this only has to hold
 * the longest text a test spells out.
 */
enum { EXPECTED_SIZE = 512 };

/**
 * Bytes of the arenas `build_in_tiny_arena` gives a build, a quarter for each of the main thread's
 * and the worker's perm and scratch. Each is too small for the configuration the build loads.
 */
enum { TINY_ARENA_SIZE = 128 };

/** Config body every fixture shares. It configures no aggregate or feed templates. */
static const char* const SITE_CONFIG =
    "title = \"Site\"\n"
    "description = \"Example notes\"\n"
    "author = \"Author\"\n"
    "base_url = \"https://example.com\"\n"
    "templates.aggregates = []\n"
    "templates.feeds = []\n";

/** Minimal content template, which renders a page as its body alone. */
static const char* const CONTENT_TEMPLATE = "{{{body_html}}}\n";

/** Not-found template every fixture has, which renders its `path_to_root`. */
static const char* const NOT_FOUND_TEMPLATE = "not found {{path_to_root}}\n";

/** One published content entry whose body renders as `<p>Body</p>`. */
static const char* const HELLO_ENTRY =
    "+++\n"
    "title = \"Hello\"\n"
    "date = 2026-07-01T00:00:00Z\n"
    "+++\n"
    "Body\n";

/** Page `HELLO_ENTRY` renders to through `CONTENT_TEMPLATE`. */
static const char* const HELLO_PAGE = "<p>Body</p>\n\n";

/** Files `every_output_kind_fixture` builds to, in byte order. */
static const char* const EVERY_OUTPUT_KIND_FILES[] = {
    "public/404.html",   "public/_sosig/output-cache.tsv",
    "public/alpha.html", "public/beta.html",
    "public/feed.xml",   "public/index.html",
    "public/sosig.css",
};

/** Number of paths in `EVERY_OUTPUT_KIND_FILES`. */
enum {
  EVERY_OUTPUT_KIND_FILE_COUNT = sizeof(EVERY_OUTPUT_KIND_FILES) /
  sizeof(EVERY_OUTPUT_KIND_FILES[0])
};

/**
 * @brief Returns a host holding `sosig.toml`, the content template and the not-found template.
 *
 * @param config_extra Config lines placed before the shared body, or `NULL` for the body alone. It
 *                     must not repeat a key the body sets, because TOML rejects a duplicate key.
 * @return The host.
 */
static struct Os* site_fixture(const char* config_extra) {
  struct Os* os = os_memory();
  os_memory_write(os, "templates/content.html", CONTENT_TEMPLATE);
  os_memory_write(os, "templates/404.html", NOT_FOUND_TEMPLATE);
  struct Arena arena = test_arena();
  struct Buf config = {.arena = &arena};
  if (config_extra != NULL) {
    buf_append(&config, str_from_cstr(config_extra));
  }
  buf_append(&config, str_from_cstr(SITE_CONFIG));
  os_memory_write(os, "sosig.toml", buf_cstr(&config));
  return os;
}

/**
 * @brief Returns a host holding a site with one output of every kind: two content pages beside a
 *        draft, an aggregate, a feed, the not-found page and a static file, which build to
 *        `EVERY_OUTPUT_KIND_FILES`.
 *
 * @return The host.
 */
static struct Os* every_output_kind_fixture(void) {
  // The shared config turns off aggregates and feeds, so this fixture writes its own.
  struct Os* os = os_memory();
  os_memory_write(os, "sosig.toml",
                  "title = \"Site\"\n"
                  "description = \"Example notes\"\n"
                  "author = \"Author\"\n"
                  "base_url = \"https://example.com\"\n"
                  "templates.aggregates = [\"index.html\"]\n"
                  "templates.feeds = [\"feed.xml\"]\n");
  os_memory_write(os, "content/alpha.md",
                  "+++\ntitle = \"Alpha\"\ndate = 2026-07-02T00:00:00Z\n+++\nAlpha body\n");
  os_memory_write(os, "content/beta.md",
                  "+++\ntitle = \"Beta\"\ndate = 2026-07-01T00:00:00Z\n+++\nBeta body\n");
  os_memory_write(os, "content/draft.md",
                  "+++\ntitle = \"Draft\"\ndate = 2026-07-03T00:00:00Z\ndraft = true\n+++\n"
                  "Draft body\n");
  os_memory_write(os, "templates/content.html", CONTENT_TEMPLATE);
  os_memory_write(os, "templates/index.html", "index\n");
  os_memory_write(os, "templates/feed.xml", "feed\n");
  os_memory_write(os, "templates/404.html", NOT_FOUND_TEMPLATE);
  os_memory_write(os, "static/sosig.css", "body{}\n");
  return os;
}

/**
 * @brief Executes a build with `options` in test arenas and collects its errors.
 *
 * @param options Build options passed to `cmd_build_execute`, whose worker count `0` stands for the
 *                CPU count and whose empty config path stands for `SITE_CONFIG_PATH_DEFAULT`, as on
 *                the command line. Must not be `NULL`.
 * @param os      Host the build runs against. Must not be `NULL`.
 * @param log     Log that receives every build error. Its arena must be none of the arenas the
 *                build allocates from, as `cmd_build_execute` requires. Must not be `NULL`.
 * @return `0` on success, or `-1` on build failure.
 */
static int execute_build_with(const struct BuildOptions* options, struct Os* os, struct Log* log) {
  struct BuildOptions resolved = *options;
  if (resolved.config_path.len == 0) {
    resolved.config_path = SITE_CONFIG_PATH_DEFAULT;
  }
  if (resolved.worker_count == 0) {
    resolved.worker_count = pool_resolve_worker_count();
  }
  struct Arena perm = test_arena();
  struct JobArenas* worker_arenas = arena_new(&perm, resolved.worker_count, struct JobArenas);
  for (ptrdiff_t i = 0; i < resolved.worker_count; i++) {
    worker_arenas[i] = (struct JobArenas){.perm = test_arena(), .scratch = test_arena()};
  }
  return cmd_build_execute(&resolved, worker_arenas, os, perm, test_arena(), log);
}

/**
 * @brief Executes a build with default options and collects its errors.
 *
 * @param os  Host the build runs against. Must not be `NULL`.
 * @param log Log that receives every build error. Must not be `NULL`.
 * @return `0` on success, or `-1` on build failure.
 */
static int execute_build(struct Os* os, struct Log* log) {
  const struct BuildOptions options = {0};
  return execute_build_with(&options, os, log);
}

/**
 * @brief Executes a dry run with otherwise default options and collects its errors.
 *
 * @param os  Host the dry run runs against. Must not be `NULL`.
 * @param log Log that receives every build error. Must not be `NULL`.
 * @return `0` on success, or `-1` on build failure.
 */
static int execute_dry_run(struct Os* os, struct Log* log) {
  const struct BuildOptions options = {.is_dry_run = true};
  return execute_build_with(&options, os, log);
}

/**
 * @brief Reports whether the regular files below `dir_path` are exactly `expected`, printing them
 *        when they are not.
 *
 * @param os             Host to inspect. Must not be `NULL`.
 * @param dir_path       Terminated directory to list. Must not be `NULL`.
 * @param expected       Array of `expected_count` terminated paths in byte order. Must not be
 *                       `NULL`.
 * @param expected_count Number of expected paths.
 * @return `true` when the files match.
 */
static bool is_file_list(struct Os* os,
                         const char* dir_path,
                         const char* const* expected,
                         ptrdiff_t expected_count) {
  const struct PathList files = os_memory_files(os, dir_path);
  bool is_equal = files.count == expected_count;
  for (ptrdiff_t i = 0; is_equal && i < expected_count; i++) {
    is_equal = str_eq(files.items[i], str_from_cstr(expected[i]));
  }
  TEST_CHECK_(is_equal, "'%s' holds exactly the expected files", dir_path);
  for (ptrdiff_t i = 0; !is_equal && i < files.count; i++) {
    TEST_MSG("found: '%.*s'", STR_FMT(files.items[i]));
  }
  return is_equal;
}

/**
 * @brief Reports whether `os_print` has written `line` as a whole line to `fd`, printing the stream
 *        when it has not.
 *
 * @param os   Host to inspect. Must not be `NULL`.
 * @param fd   `1` for standard output or `2` for standard error.
 * @param line Terminated line, without its newline. Must not be `NULL`.
 * @return `true` when one of the printed lines is `line`.
 */
static bool has_printed_line(struct Os* os, int fd, const char* line) {
  const struct Str printed = os_memory_printed(os, fd);
  bool is_printed = false;
  struct StrCut cut = {.tail = printed, .is_found = true};
  while (!is_printed && cut.is_found) {
    cut = str_cut(cut.tail, '\n');
    is_printed = str_eq(cut.head, str_from_cstr(line));
  }
  TEST_CHECK_(is_printed, "'%s' is a printed line", line);
  if (!is_printed) {
    TEST_MSG("printed: '%.*s'", STR_FMT(printed));
  }
  return is_printed;
}

/**
 * @brief Reports whether the `write` lines a dry run printed name exactly the files below `public`
 *        on another host, printing the listing when they do not.
 *
 * Every line must be a `write` line, and the manifest makes the listed paths distinct, so a count
 * that matches and a path for each line that exists there make the two sets equal.
 *
 * @param dry_run_os Host the dry run printed its listing on. Must not be `NULL`.
 * @param real_os    Host a real build of the same site wrote. Must not be `NULL`.
 * @return `true` when the sets are equal.
 */
static bool is_write_set_of(struct Os* dry_run_os, struct Os* real_os) {
  const struct Str printed = os_memory_printed(dry_run_os, 1);
  const struct PathList files = os_memory_files(real_os, "public");
  struct Arena arena = test_arena();
  ptrdiff_t line_count = 0;
  bool is_equal = true;
  for (struct StrCut cut = str_cut(printed, '\n'); cut.is_found; cut = str_cut(cut.tail, '\n')) {
    const struct StrCut fields = str_cut(cut.head, '\t');
    is_equal = is_equal && str_eq(fields.head, S("write")) &&
               os_memory_has_path(real_os, str_cstr(fields.tail, &arena));
    line_count++;
  }
  is_equal = is_equal && line_count == files.count;
  TEST_CHECK_(is_equal, "the dry run lists exactly the files a real build writes");
  if (!is_equal) {
    TEST_MSG("printed: '%.*s'", STR_FMT(printed));
  }
  return is_equal;
}

/**
 * @brief Runs a dry run and returns the listing it printed, which says what a build would keep and
 *        what it would make again.
 *
 * @param os        Host the dry run runs against. Must not be `NULL`.
 * @param is_forced Whether the dry run ignores the output cache.
 * @param arena     Arena that owns the returned listing. Must not be `NULL`.
 * @return The listing, owned by `arena`.
 */
static struct Str dry_run_listing(struct Os* os, bool is_forced, struct Arena* arena) {
  const ptrdiff_t printed_len = os_memory_printed(os, 1).len;
  const struct BuildOptions options = {.is_dry_run = true, .is_forced = is_forced};
  struct Log log = test_log();
  TEST_CHECK(execute_build_with(&options, os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  return str_clone(str_tail(os_memory_printed(os, 1), printed_len), arena);
}

/**
 * @brief Reports whether `listing` is exactly `expected`, printing it when it is not.
 *
 * @param listing  Listing a dry run printed.
 * @param expected Terminated listing it must be. Must not be `NULL`.
 * @return `true` when the two are equal.
 */
static bool is_listing(struct Str listing, const char* expected) {
  const bool is_equal = str_eq(listing, str_from_cstr(expected));
  TEST_CHECK_(is_equal, "the dry run lists what is expected");
  if (!is_equal) {
    TEST_MSG("listed: '%.*s'", STR_FMT(listing));
  }
  return is_equal;
}

/**
 * @brief Returns the modification time of the file at `file_path`, which tells whether a later
 *        build wrote it.
 *
 * @param os        Host holding the file. Must not be `NULL`.
 * @param file_path Path of a file that exists.
 * @return The file's modification time in seconds, which the in-memory host advances on each write.
 */
static int64_t file_mtime(struct Os* os, struct Str file_path) {
  struct OsStat st = {0};
  TEST_CHECK_(os_stat(os, file_path, &st) == 0, "'%.*s' exists", STR_FMT(file_path));
  return st.mtime_s;
}

/** Listing a dry run of `every_output_kind_fixture` prints when it keeps nothing. */
static const char* const EVERY_OUTPUT_KIND_WRITES =
    "write\tpublic/alpha.html\n"
    "write\tpublic/beta.html\n"
    "write\tpublic/index.html\n"
    "write\tpublic/feed.xml\n"
    "write\tpublic/404.html\n"
    "write\tpublic/sosig.css\n"
    "write\tpublic/_sosig/output-cache.tsv\n";

/** Config line that accepts the frontmatter keys of denote's `markdown-toml` notes. */
static const char* const DENOTE_CONFIG =
    "frontmatter_ignored_keys = [\"identifier\", \"signature\"]\n";

/**
 * @brief Writes two notes named as denote names them, which link each other by path and by
 *        identifier: an Org note below `notes`, and a Markdown note below `other` with the
 *        frontmatter of denote's `markdown-toml` file type, which `DENOTE_CONFIG` accepts.
 *
 * @param os Host to write to. Must not be `NULL`.
 */
static void write_linked_notes(struct Os* os) {
  os_memory_write(
      os, "content/notes/20240103T000000--a.org",
      "#+title: A\n"
      "#+date: [2024-01-03 Wed]\n"
      "#+identifier: 20240103T000000\n"
      "\n"
      "[[file:../other/20240101T000000==1a--b__x.md][B]] [[denote:20240101T000000][D]]\n");
  os_memory_write(os, "content/other/20240101T000000==1a--b__x.md",
                  "+++\n"
                  "title      = \"B\"\n"
                  "date       = 2024-01-01T00:00:00+00:00\n"
                  "tags       = [\"x\"]\n"
                  "identifier = \"20240101T000000\"\n"
                  "signature  = \"1a\"\n"
                  "+++\n"
                  "[A](../notes/20240103T000000--a.org) [D](denote:20240103T000000)\n");
}

/**
 * @brief Runs a one-worker build in `TINY_ARENA_SIZE` bytes of arenas. Run through `test_is_oom`,
 *        which calls it in a child process.
 *
 * @param userdata Host the build runs against, as a `struct Os*`. Must not be `NULL`.
 */
static void build_in_tiny_arena(void* userdata) {
  static _Alignas(max_align_t) char memory[TINY_ARENA_SIZE];
  struct Arena rest = {.beg = memory, .end = memory + sizeof(memory)};
  const struct Arena perm = arena_split(&rest, sizeof(memory) / 4);
  const struct Arena scratch = arena_split(&rest, sizeof(memory) / 4);
  struct JobArenas worker_arenas[1] = {
      {.perm = arena_split(&rest, sizeof(memory) / 4), .scratch = rest}};
  struct Log log = test_log();
  const struct BuildOptions options = {.config_path = SITE_CONFIG_PATH_DEFAULT, .worker_count = 1};
  (void)cmd_build_execute(&options, worker_arenas, userdata, perm, scratch, &log);
}

// A configured content template name is used instead of the default.
static void test_honors_configured_content_template(void) {
  struct Os* os = site_fixture("templates.content = \"content-entry.html\"\n");
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  os_memory_write(os, "templates/content-entry.html", "<main>{{title}} {{{body_html}}}</main>\n");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(os, "public/hello.html", "<main>Hello <p>Body</p>\n</main>\n"));
}

// The build loads the content template only when an entry uses it, so a missing one fails no build
// whose every entry names its own template, nor one with no entry at all, and a draft's `template`
// is never loaded.
static void test_loads_the_content_template_only_when_an_entry_uses_it(void) {
  struct Os* os = site_fixture("templates.content = \"absent.html\"\n");
  os_memory_mkdir(os, "content");
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));

  os_memory_write(os, "templates/own.html", "[{{title}}]\n");
  os_memory_write(os, "content/hello.md",
                  "+++\ntitle = \"Hello\"\ndate = 2026-07-01T00:00:00Z\ntemplate = \"own.html\"\n"
                  "+++\nBody\n");
  os_memory_write(
      os, "content/draft.md",
      "+++\ntitle = \"Draft\"\ndate = 2026-07-01T00:00:00Z\ntemplate = \"missing.html\"\n"
      "draft = true\n+++\nBody\n");
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(os, "public/hello.html", "[Hello]\n"));
}

// A build writes exactly the manifest's intended outputs: content pages, aggregate and feed
// outputs, static files, and nothing for drafts.
static void test_writes_exactly_manifest_outputs(void) {
  struct Os* os = every_output_kind_fixture();

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(is_file_list(os, "public", EVERY_OUTPUT_KIND_FILES, EVERY_OUTPUT_KIND_FILE_COUNT));
}

// A trailing slash on `content_dir` does not leak the directory name into the output tree. The
// prefix strip that derives an entry's section compares `<content_dir>/`, so an unnormalized
// `"content/"` would miss and publish `public/content/post.html`.
static void test_tolerates_trailing_slash_on_content_dir(void) {
  struct Os* os = site_fixture("content_dir = \"content/\"\n");
  os_memory_write(os, "content/post.md", HELLO_ENTRY);

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  static const char* const expected[] = {"public/404.html", "public/_sosig/output-cache.tsv",
                                         "public/post.html"};
  TEST_CHECK(is_file_list(os, "public", expected, 3));
}

// A config that `-c` names in another directory reads and writes each directory next to it, its
// defaults included, so the site builds from a working directory that holds none of it, and the
// build writes nothing outside that directory.
static void test_builds_site_of_config_in_another_dir(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "sub/sosig.toml", SITE_CONFIG);
  os_memory_write(os, "sub/templates/content.html", CONTENT_TEMPLATE);
  os_memory_write(os, "sub/templates/404.html", NOT_FOUND_TEMPLATE);
  os_memory_write(os, "sub/content/hello.md", HELLO_ENTRY);
  os_memory_write(os, "sub/static/robots.txt", "User-agent: *\n");

  const struct BuildOptions options = {.config_path = S("sub/sosig.toml")};
  struct Log log = test_log();
  TEST_CHECK(execute_build_with(&options, os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  static const char* const expected[] = {"sub/public/404.html",
                                         "sub/public/_sosig/output-cache.tsv",
                                         "sub/public/hello.html", "sub/public/robots.txt"};
  TEST_CHECK(is_file_list(os, "sub/public", expected, 4));
  TEST_CHECK(os_memory_is_file(os, "sub/public/hello.html", HELLO_PAGE));
  TEST_CHECK(!os_memory_has_path(os, "public"));
}

// A site that ships no `static` directory builds. `static_dir` is optional with a default, so an
// absent one is skipped rather than reported.
static void test_builds_with_no_static_dir(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  TEST_CHECK(!os_memory_has_path(os, "static"));

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(os, "public/hello.html", HELLO_PAGE));
}

// A `static` directory is mirrored into `output_dir` as-is, so a top-level file lands at the output
// root and a nested one keeps its directories.
static void test_copies_static_files_when_present(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  os_memory_write(os, "static/robots.txt", "User-agent: *\n");
  os_memory_write(os, "static/assets/sosig.css", "body{}\n");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(os, "public/robots.txt", "User-agent: *\n"));
  TEST_CHECK(os_memory_is_file(os, "public/assets/sosig.css", "body{}\n"));
}

// Every file below `content_dir` that does not end in `.md` or `.org`, in any letter case, is an
// asset, copied to its relative path below `output_dir`. That includes an editor backup such as
// `e.org~`. `D.MD` publishes as `d.html`, and `f.ORG` renders as Org, where `*Bold*` is strong.
static void test_copies_assets_beside_content(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/a.md", HELLO_ENTRY);
  os_memory_write(os, "content/b.org", "#+title: B\n#+date: [2026-07-01 Wed]\nB body\n");
  os_memory_write(os, "content/c.png", "png");
  os_memory_write(os, "content/D.MD", HELLO_ENTRY);
  os_memory_write(os, "content/e.org~", "backup");
  os_memory_write(os, "content/f.ORG", "#+title: F\n#+date: [2026-07-01 Wed]\n*Bold*\n");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  static const char* const expected[] = {
      "public/404.html", "public/_sosig/output-cache.tsv",
      "public/a.html",   "public/b.html",
      "public/c.png",    "public/d.html",
      "public/e.org~",   "public/f.html",
  };
  TEST_CHECK(is_file_list(os, "public", expected, 8));
  TEST_CHECK(os_memory_is_file(os, "public/a.html", HELLO_PAGE));
  TEST_CHECK(os_memory_is_file(os, "public/b.html", "<p>B body</p>\n\n"));
  TEST_CHECK(os_memory_is_file(os, "public/c.png", "png"));
  TEST_CHECK(os_memory_is_file(os, "public/d.html", HELLO_PAGE));
  TEST_CHECK(os_memory_is_file(os, "public/e.org~", "backup"));
  TEST_CHECK(os_memory_is_file(os, "public/f.html", "<p><strong>Bold</strong></p>\n\n"));
}

// A not-found page below a directory, as bunny.net serves it, gets the root-relative path of
// `base_url` as its `path_to_root`, and nothing is written at the default `404.html`. The golden
// test `site_nested_sections` asserts a `base_url` with a path end to end.
static void test_renders_a_nested_not_found_page_from_the_root(void) {
  struct Os* os = site_fixture("not_found_path = \"bunnycdn_errors/404.html\"\n");
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(os, "public/bunnycdn_errors/404.html", "not found /\n"));
  TEST_CHECK(!os_memory_has_path(os, "public/404.html"));
}

// A custom pretty permalink publishes `<slug>/index.html` instead of `<slug>.html`. The golden test
// compares that output shape end to end with `tests/expected/site_index_permalink`. What this adds
// is the assertion at the command boundary.
static void test_honors_custom_permalink(void) {
  struct Os* os = site_fixture("permalink = \"/{slug}/\"\n");
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(os, "public/hello/index.html", HELLO_PAGE));
}

// Same-named sources in different directories publish to distinct, non-colliding outputs under the
// default permalink.
static void test_distinguishes_same_name_in_different_dirs(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/a/post.md", HELLO_ENTRY);
  os_memory_write(os, "content/b/post.md", HELLO_ENTRY);

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  static const char* const expected[] = {"public/404.html", "public/_sosig/output-cache.tsv",
                                         "public/a/post.html", "public/b/post.html"};
  TEST_CHECK(is_file_list(os, "public", expected, 4));
}

// An Org note and a Markdown note in two sections link each other by path and by denote identifier,
// and each `href` reaches the other page from the linking page's directory, under the default
// permalink and under one that puts each page in a directory of its own. The Markdown note has the
// frontmatter of denote's `markdown-toml` file type.
static void test_links_notes_to_each_others_pages(void) {
  static const struct {
    const char* permalink;
    const char* page_a;
    const char* page_b;
    const char* href_b;
    const char* href_a;
  } cases[] = {
      {"", "public/notes/20240103t000000-a.html", "public/other/20240101t000000-1a-b-x.html",
       "../other/20240101t000000-1a-b-x.html", "../notes/20240103t000000-a.html"},
      {"permalink = \"/{slug}/\"\n", "public/20240103t000000-a/index.html",
       "public/20240101t000000-1a-b-x/index.html", "../20240101t000000-1a-b-x/index.html",
       "../20240103t000000-a/index.html"},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    char config[256];
    char page_a[256];
    char page_b[256];
    (void)snprintf(config, sizeof(config), "%s%s", DENOTE_CONFIG, cases[i].permalink);
    (void)snprintf(page_a, sizeof(page_a), "<p><a href=\"%s\">B</a> <a href=\"%s\">D</a></p>\n\n",
                   cases[i].href_b, cases[i].href_b);
    (void)snprintf(page_b, sizeof(page_b), "<p><a href=\"%s\">A</a> <a href=\"%s\">D</a></p>\n\n",
                   cases[i].href_a, cases[i].href_a);
    struct Os* os = site_fixture(config);
    write_linked_notes(os);

    struct Log log = test_log();
    TEST_CHECK(execute_build(os, &log) == 0);
    TEST_CHECK(test_log_is_empty(&log));
    TEST_CHECK(os_memory_is_file(os, cases[i].page_a, page_a));
    TEST_CHECK(os_memory_is_file(os, cases[i].page_b, page_b));
  }
}

// A link keeps a Markdown `#fragment` and an Org `::#custom-id` as the fragment of its URL, drops
// any other Org search option, and resolves `.` and `..` segments and a suffix in any letter case.
// An Org `file` link without a description shows the URL, and a denote link without one, in Org or
// as a Markdown autolink, shows the target as typed. A link to a file that is not a source, a path
// from the root, a URL, an image and a fragment alone keep the output they have without links
// between content files.
static void test_resolves_each_form_of_link(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/notes/a.org",
                  "#+title: A\n#+date: 2024-01-01\n\n"
                  "[[file:b.org::#cid][1]] [[file:./b.org::*Heading][2]] [[file:b.org]] "
                  "[[file:../Up.MD][3]] [[denote:20240101T000000::#cid][4]] [[file:c.txt][5]] "
                  "[[file:/abs.org][6]] [[file:i.png]] [[denote:20240101T000000::*Heading]]\n");
  os_memory_write(os, "content/notes/m.md",
                  "+++\ntitle = \"M\"\ndate = 2024-01-01\n+++\n"
                  "[1](b.org#cid) [2](../notes/../Up.MD) [3](c.txt) [4](/abs.md) "
                  "[5](https://e.com/x.md) ![i](i.png) [6](#top) <denote:20240101T000000>\n");
  os_memory_write(os, "content/notes/b.org", "#+title: B\n#+date: 2024-01-01\n\nB\n");
  os_memory_write(os, "content/Up.MD", HELLO_ENTRY);
  os_memory_write(os, "content/20240101T000000--d.md", HELLO_ENTRY);

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(
      os, "public/notes/a.html",
      "<p><a href=\"b.html#cid\">1</a> <a href=\"b.html\">2</a> <a href=\"b.html\">b.html</a> "
      "<a href=\"../up.html\">3</a> <a href=\"../20240101t000000-d.html#cid\">4</a> "
      "<a href=\"c.txt\">5</a> <a href=\"/abs.html\">6</a> <img src=\"i.png\" alt=\"i.png\"> "
      "<a href=\"../20240101t000000-d.html\">denote:20240101T000000::*Heading</a></p>\n\n"));
  TEST_CHECK(os_memory_is_file(os, "public/notes/m.html",
                               "<p><a href=\"b.html#cid\">1</a> <a href=\"../up.html\">2</a> "
                               "<a href=\"c.txt\">3</a> <a href=\"/abs.md\">4</a> "
                               "<a href=\"https://e.com/x.md\">5</a> <img src=\"i.png\" alt=\"i\"> "
                               "<a href=\"#top\">6</a> <a href=\"../20240101t000000-d.html\">"
                               "denote:20240101T000000</a></p>\n\n"));
}

// A denote identifier can be the `@@` field of a name, and an asset can hold one, such as a PDF
// that denote renamed, whose link points to the asset's copy. When a source and an asset share an
// identifier, the link names the source.
static void test_links_denote_identifiers_of_each_form(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/a.md",
                  "+++\ntitle = \"A\"\ndate = 2024-01-01\n+++\n"
                  "[n](denote:n1) [p](denote:20240105T000000)\n");
  os_memory_write(os, "content/notes/x@@n1--note.org", "#+title: N\n#+date: 2024-01-01\n\nN\n");
  os_memory_write(os, "content/files/y@@n1.pdf", "pdf");
  os_memory_write(os, "content/files/20240105T000000--paper.pdf", "pdf");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(os, "public/a.html",
                               "<p><a href=\"notes/x-n1-note.html\">n</a> "
                               "<a href=\"files/20240105T000000--paper.pdf\">p</a></p>\n\n"));
}

// Two sources that share an identifier: the link goes to the first in sorted path order, and the
// build reports nothing.
static void test_links_the_first_source_of_a_shared_identifier(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/a.md",
                  "+++\ntitle = \"A\"\ndate = 2024-01-01\n+++\n[n](denote:20240101T000000)\n");
  os_memory_write(os, "content/one/20240101T000000--first.org",
                  "#+title: One\n#+date: 2024-01-01\n\nOne\n");
  os_memory_write(os, "content/two/20240101T000000--second.md",
                  "+++\ntitle = \"Two\"\ndate = 2024-01-01\n+++\nTwo\n");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(os, "public/a.html",
                               "<p><a href=\"one/20240101t000000-first.html\">n</a></p>\n\n"));
}

// A draft's body is not converted, so its links are not checked: a draft that links a missing file
// does not fail the build.
static void test_skips_links_in_drafts(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/a.md", HELLO_ENTRY);
  os_memory_write(os, "content/d.md",
                  "+++\ntitle = \"D\"\ndate = 2024-01-01\ndraft = true\n+++\n"
                  "[gone](denote:19990101T000000) [also](gone.md)\n");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(os, "public/a.html", HELLO_PAGE));
  TEST_CHECK(!os_memory_has_path(os, "public/d.html"));
}

// A second build over a finished output tree succeeds and produces the same set of outputs.
static void test_rebuild_succeeds_and_repeats_its_outputs(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  // The one content page and the output cache: the second build adds nothing and removes nothing.
  static const char* const expected[] = {"public/404.html", "public/_sosig/output-cache.tsv",
                                         "public/hello.html"};
  TEST_CHECK(is_file_list(os, "public", expected, 3));
}

// A rebuild removes what the current inputs no longer produce: the page of a deleted entry, an old
// section tree, and a file added to `output_dir` by hand. Every current output stays, and a third
// build finds nothing more to remove.
static void test_rebuild_removes_stale_outputs(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  os_memory_write(os, "content/goodbye.md", HELLO_ENTRY);
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(os_memory_has_path(os, "public/goodbye.html"));

  TEST_CHECK(os_remove(os, S("content/goodbye.md"), S("failed to remove fixture"), &log) == 0);
  os_memory_write(os, "public/notes.txt", "hand");
  os_memory_write(os, "public/old-section/deeper/page.html", "old");
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  static const char* const expected[] = {"public/404.html", "public/_sosig/output-cache.tsv",
                                         "public/hello.html"};
  TEST_CHECK(is_file_list(os, "public", expected, 3));
  TEST_CHECK(!os_memory_has_path(os, "public/old-section"));

  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(is_file_list(os, "public", expected, 3));
}

// A rebuild removes the copy of an asset that was deleted from `content_dir`.
static void test_rebuild_removes_deleted_asset(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/a.md", HELLO_ENTRY);
  os_memory_write(os, "content/c.png", "png");
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(os_memory_is_file(os, "public/c.png", "png"));

  TEST_CHECK(os_remove(os, S("content/c.png"), S("failed to remove fixture"), &log) == 0);
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  static const char* const expected[] = {"public/404.html", "public/_sosig/output-cache.tsv",
                                         "public/a.html"};
  TEST_CHECK(is_file_list(os, "public", expected, 3));
}

// An `output_dir` that a symlink inside `content_dir` reaches is left out of the content walk, so a
// rebuild does not read the first build's outputs back as sources. The aggregate here publishes a
// `.md` file, which the walk would otherwise parse as a content entry and claim as an input.
static void test_rebuild_skips_output_dir_linked_from_content_dir(void) {
  // The shared config turns off aggregates, so this fixture writes its own.
  struct Os* os = os_memory();
  os_memory_write(os, "sosig.toml",
                  "title = \"Site\"\n"
                  "description = \"Example notes\"\n"
                  "author = \"Author\"\n"
                  "base_url = \"https://example.com\"\n"
                  "templates.aggregates = [\"notes.md\"]\n"
                  "templates.feeds = []\n");
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  os_memory_write(os, "templates/content.html", CONTENT_TEMPLATE);
  os_memory_write(os, "templates/404.html", NOT_FOUND_TEMPLATE);
  os_memory_write(os, "templates/notes.md", "notes\n");
  os_memory_symlink(os, "../public", "content/published");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  // The page, the aggregate and the output cache, and no page rendered from the aggregate's own
  // output.
  static const char* const expected[] = {"public/404.html", "public/_sosig/output-cache.tsv",
                                         "public/hello.html", "public/notes.md"};
  TEST_CHECK(is_file_list(os, "public", expected, 4));
}

// A dry run of a site with every kind of output succeeds and creates nothing, not even
// `output_dir`, which shows `prepare_output_dir` was withheld. Its `write` lines name exactly the
// files a real build of the same site writes, so no site left a path pending, and they come in
// manifest order: content pages newest first, then the aggregate, the feed and the static file.
static void test_dry_run_lists_every_output_and_writes_nothing(void) {
  struct Os* real_os = every_output_kind_fixture();
  struct Os* dry_run_os = every_output_kind_fixture();

  struct Log log = test_log();
  TEST_CHECK(execute_build(real_os, &log) == 0);
  TEST_CHECK(execute_dry_run(dry_run_os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(!os_memory_has_path(dry_run_os, "public"));
  TEST_CHECK(is_write_set_of(dry_run_os, real_os));
  TEST_CHECK(os_memory_is_printed(dry_run_os, 1, EVERY_OUTPUT_KIND_WRITES));
}

// After a real build, a dry run keeps the one page, which is unchanged, and lists the output cache,
// then what the prune would remove, sorted: a deleted entry's page, a hand-added file, and a stale
// directory with its page. It leaves each of them in place. Its verbose status counts the `remove`
// lines, on `stderr`, and adds nothing to the listing.
static void test_dry_run_lists_stale_outputs_and_keeps_them(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  os_memory_write(os, "content/goodbye.md", HELLO_ENTRY);
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(os_remove(os, S("content/goodbye.md"), S("failed to remove fixture"), &log) == 0);
  os_memory_write(os, "public/notes.txt", "hand");
  os_memory_write(os, "public/old/page.html", "old");

  const struct BuildOptions options = {.is_verbose = true, .is_dry_run = true};
  TEST_CHECK(execute_build_with(&options, os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_printed(os, 1,
                                  "keep\tpublic/hello.html\n"
                                  "keep\tpublic/404.html\n"
                                  "write\tpublic/_sosig/output-cache.tsv\n"
                                  "remove\tpublic/goodbye.html\n"
                                  "remove\tpublic/notes.txt\n"
                                  "remove\tpublic/old\n"
                                  "remove\tpublic/old/page.html\n"));
  TEST_CHECK(has_printed_line(os, 2, "removed stale outputs: 4"));
  TEST_CHECK(os_memory_is_file(os, "public/goodbye.html", HELLO_PAGE));
  TEST_CHECK(os_memory_is_file(os, "public/notes.txt", "hand"));
  TEST_CHECK(os_memory_is_file(os, "public/old/page.html", "old"));
}

// A dry run over an existing tree, where every write and every removal below `output_dir` would
// fail, still succeeds, so it attempts neither.
static void test_dry_run_attempts_no_change(void) {
  struct Os* os = every_output_kind_fixture();
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  os_memory_write(os, "public/notes.txt", "hand");
  os_memory_fail(os, OS_MEMORY_OP_WRITE, "public", EACCES);
  os_memory_fail(os, OS_MEMORY_OP_REMOVE, "public", EACCES);

  TEST_CHECK(execute_dry_run(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(os, "public/notes.txt", "hand"));
}

// A dry run leaves nothing behind that a later build sees: a real build after it writes the same
// files as a real build alone.
static void test_dry_run_leaves_no_state(void) {
  struct Os* os = every_output_kind_fixture();
  struct Log log = test_log();
  TEST_CHECK(execute_dry_run(os, &log) == 0);
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(is_file_list(os, "public", EVERY_OUTPUT_KIND_FILES, EVERY_OUTPUT_KIND_FILE_COUNT));
  TEST_CHECK(os_memory_is_file(os, "public/index.html", "index\n"));
}

// A build after an unchanged build leaves every output, the output cache included, byte for byte as
// the first build wrote it, and writes none of them but the output cache, so each keeps its
// modification time.
static void test_unchanged_rebuild_changes_nothing(void) {
  struct Os* os = every_output_kind_fixture();
  struct Arena arena = test_arena();
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  const struct PathList files = os_memory_files(os, "public");
  struct Str* contents = arena_new(&arena, files.count, struct Str);
  int64_t* mtimes = arena_new(&arena, files.count, int64_t);
  for (ptrdiff_t i = 0; i < files.count; i++) {
    TEST_CHECK(os_memory_file(os, str_cstr(files.items[i], &arena), &contents[i]) == 0);
    contents[i] = str_clone(contents[i], &arena);
    mtimes[i] = file_mtime(os, files.items[i]);
  }

  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(is_file_list(os, "public", EVERY_OUTPUT_KIND_FILES, EVERY_OUTPUT_KIND_FILE_COUNT));
  for (ptrdiff_t i = 0; i < files.count; i++) {
    struct Str now = {0};
    TEST_CHECK_(
        os_memory_file(os, str_cstr(files.items[i], &arena), &now) == 0 && str_eq(now, contents[i]),
        "'%.*s' is unchanged", STR_FMT(files.items[i]));
    TEST_CHECK_(str_eq(files.items[i], S("public/_sosig/output-cache.tsv")) ||
                    file_mtime(os, files.items[i]) == mtimes[i],
                "'%.*s' is not written again", STR_FMT(files.items[i]));
  }
}

// After an unchanged build, a dry run makes every page, aggregate and feed in memory and lists each
// as `keep`, as it does the static file, whose source is unchanged, and it leaves the output cache
// as it was.
static void test_dry_run_keeps_every_page_after_an_unchanged_build(void) {
  struct Os* os = every_output_kind_fixture();
  struct Arena arena = test_arena();
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  struct Str cache = {0};
  TEST_CHECK(os_memory_file(os, "public/_sosig/output-cache.tsv", &cache) == 0);
  cache = str_clone(cache, &arena);

  TEST_CHECK(is_listing(dry_run_listing(os, false, &arena),
                        "keep\tpublic/alpha.html\n"
                        "keep\tpublic/beta.html\n"
                        "keep\tpublic/index.html\n"
                        "keep\tpublic/feed.xml\n"
                        "keep\tpublic/404.html\n"
                        "keep\tpublic/sosig.css\n"
                        "write\tpublic/_sosig/output-cache.tsv\n"));
  TEST_CHECK(os_memory_is_file(os, "public/_sosig/output-cache.tsv", str_cstr(cache, &arena)));
}

// A static file whose size or modification time changed is copied again, though its bytes did not
// change, and the others are kept.
static void test_changed_static_file_is_copied_again(void) {
  struct Os* os = every_output_kind_fixture();
  struct Arena arena = test_arena();
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  const int64_t copy_mtime = file_mtime(os, S("public/sosig.css"));
  os_memory_touch(os, "static/sosig.css");

  TEST_CHECK(is_listing(dry_run_listing(os, false, &arena),
                        "keep\tpublic/alpha.html\n"
                        "keep\tpublic/beta.html\n"
                        "keep\tpublic/index.html\n"
                        "keep\tpublic/feed.xml\n"
                        "keep\tpublic/404.html\n"
                        "write\tpublic/sosig.css\n"
                        "write\tpublic/_sosig/output-cache.tsv\n"));
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(file_mtime(os, S("public/sosig.css")) > copy_mtime);
}

// A changed source makes every page again, and a build writes only the outputs whose bytes change:
// here the source's own page. The other page, and the aggregate and the feed, whose templates show
// no entry, are left as they were.
static void test_changed_source_rewrites_only_its_page(void) {
  struct Os* os = every_output_kind_fixture();
  struct Arena arena = test_arena();
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  static const char* const kept_paths[] = {"public/beta.html", "public/index.html",
                                           "public/feed.xml", "public/sosig.css"};
  enum { KEPT_COUNT = sizeof(kept_paths) / sizeof(kept_paths[0]) };
  int64_t kept_mtimes[KEPT_COUNT];
  for (ptrdiff_t i = 0; i < KEPT_COUNT; i++) {
    kept_mtimes[i] = file_mtime(os, str_from_cstr(kept_paths[i]));
  }
  const int64_t page_mtime = file_mtime(os, S("public/alpha.html"));
  os_memory_write(os, "content/alpha.md",
                  "+++\ntitle = \"Alpha\"\ndate = 2026-07-02T00:00:00Z\n+++\nNew body\n");

  TEST_CHECK(is_listing(dry_run_listing(os, false, &arena),
                        "write\tpublic/alpha.html\n"
                        "keep\tpublic/beta.html\n"
                        "keep\tpublic/index.html\n"
                        "keep\tpublic/feed.xml\n"
                        "keep\tpublic/404.html\n"
                        "keep\tpublic/sosig.css\n"
                        "write\tpublic/_sosig/output-cache.tsv\n"));
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(os, "public/alpha.html", "<p>New body</p>\n\n"));
  TEST_CHECK(file_mtime(os, S("public/alpha.html")) > page_mtime);
  for (ptrdiff_t i = 0; i < KEPT_COUNT; i++) {
    TEST_CHECK_(file_mtime(os, str_from_cstr(kept_paths[i])) == kept_mtimes[i],
                "'%s' is not written again", kept_paths[i]);
  }
}

// A page whose template lists `content_entries` is written again when another entry's title
// changes, while a page whose template reads only its own entry is kept.
static void test_page_reading_the_entry_list_follows_every_entry(void) {
  struct Os* os = site_fixture("templates.content = \"listing.html\"\n");
  struct Arena arena = test_arena();
  os_memory_write(os, "templates/listing.html",
                  "{{{body_html}}}{{#content_entries}}{{title}};{{/content_entries}}\n");
  os_memory_write(os, "content/a.md", HELLO_ENTRY);
  os_memory_write(os, "content/b.md", HELLO_ENTRY);
  os_memory_write(os, "content/c.md",
                  "+++\ntitle = \"C\"\ndate = 2026-07-01T00:00:00Z\ntemplate = \"content.html\"\n"
                  "+++\nBody\n");
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(is_listing(dry_run_listing(os, false, &arena),
                        "keep\tpublic/a.html\n"
                        "keep\tpublic/b.html\n"
                        "keep\tpublic/c.html\n"
                        "keep\tpublic/404.html\n"
                        "write\tpublic/_sosig/output-cache.tsv\n"));
  os_memory_write(os, "content/b.md",
                  "+++\ntitle = \"Renamed\"\ndate = 2026-07-01T00:00:00Z\n+++\nBody\n");

  TEST_CHECK(is_listing(dry_run_listing(os, false, &arena),
                        "write\tpublic/a.html\n"
                        "write\tpublic/b.html\n"
                        "keep\tpublic/c.html\n"
                        "keep\tpublic/404.html\n"
                        "write\tpublic/_sosig/output-cache.tsv\n"));
}

// A page that links another entry is written again when that entry moves, here when a new `#+slug:`
// moves the other page, and its link follows the move. The next build fails when the other entry
// becomes a draft.
static void test_page_linking_an_entry_follows_it(void) {
  struct Os* os = site_fixture(NULL);
  struct Arena arena = test_arena();
  os_memory_write(os, "content/a.md", "+++\ntitle = \"A\"\ndate = 2024-01-01\n+++\n[B](b.org)\n");
  os_memory_write(os, "content/b.org", "#+title: B\n#+date: 2024-01-02\n\nB\n");
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(os_memory_is_file(os, "public/a.html", "<p><a href=\"b.html\">B</a></p>\n\n"));
  os_memory_write(os, "content/b.org", "#+title: B\n#+date: 2024-01-02\n#+slug: bee\n\nB\n");

  TEST_CHECK(is_listing(dry_run_listing(os, false, &arena),
                        "write\tpublic/bee.html\n"
                        "write\tpublic/a.html\n"
                        "keep\tpublic/404.html\n"
                        "write\tpublic/_sosig/output-cache.tsv\n"
                        "remove\tpublic/b.html\n"));
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(os, "public/a.html", "<p><a href=\"bee.html\">B</a></p>\n\n"));
  os_memory_write(os, "content/b.org",
                  "#+title: B\n#+date: 2024-01-02\n#+slug: bee\n#+draft: true\n\nB\n");

  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log, "link 'b.org' names draft 'content/b.org' (in 'content/a.md')\n"));
}

// A page whose denote link names an asset is kept while the asset is unchanged, and written again
// when the asset is renamed under the same identifier, with its link following the new name. The
// new copy is written and the old one removed, and every other output is kept.
static void test_page_linking_an_asset_follows_its_rename(void) {
  struct Os* os = every_output_kind_fixture();
  struct Arena arena = test_arena();
  os_memory_write(os, "content/gamma.md",
                  "+++\ntitle = \"Gamma\"\ndate = 2026-06-30T00:00:00Z\n+++\n"
                  "[p](denote:20240105T000000)\n");
  os_memory_write(os, "content/20240105T000000--paper.pdf", "pdf");
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(os, "public/gamma.html",
                               "<p><a href=\"20240105T000000--paper.pdf\">p</a></p>\n\n"));
  TEST_CHECK(is_listing(dry_run_listing(os, false, &arena),
                        "keep\tpublic/alpha.html\n"
                        "keep\tpublic/beta.html\n"
                        "keep\tpublic/gamma.html\n"
                        "keep\tpublic/index.html\n"
                        "keep\tpublic/feed.xml\n"
                        "keep\tpublic/404.html\n"
                        "keep\tpublic/sosig.css\n"
                        "keep\tpublic/20240105T000000--paper.pdf\n"
                        "write\tpublic/_sosig/output-cache.tsv\n"));
  TEST_CHECK(os_remove(os, S("content/20240105T000000--paper.pdf"), S("failed to remove fixture"),
                       &log) == 0);
  os_memory_write(os, "content/20240105T000000--report.pdf", "pdf");

  TEST_CHECK(is_listing(dry_run_listing(os, false, &arena),
                        "keep\tpublic/alpha.html\n"
                        "keep\tpublic/beta.html\n"
                        "write\tpublic/gamma.html\n"
                        "keep\tpublic/index.html\n"
                        "keep\tpublic/feed.xml\n"
                        "keep\tpublic/404.html\n"
                        "keep\tpublic/sosig.css\n"
                        "write\tpublic/20240105T000000--report.pdf\n"
                        "write\tpublic/_sosig/output-cache.tsv\n"
                        "remove\tpublic/20240105T000000--paper.pdf\n"));
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(os, "public/gamma.html",
                               "<p><a href=\"20240105T000000--report.pdf\">p</a></p>\n\n"));
}

// A change below `templates_dir`, here to a partial, writes every page whose bytes it changes.
static void test_changed_partial_makes_every_page(void) {
  struct Os* os = site_fixture(NULL);
  struct Arena arena = test_arena();
  os_memory_write(os, "templates/content.html", "{{{body_html}}}{{>footer}}\n");
  os_memory_write(os, "templates/partials/footer.html", "footer");
  os_memory_write(os, "content/a.md", HELLO_ENTRY);
  os_memory_write(os, "content/b.md", HELLO_ENTRY);
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  os_memory_write(os, "templates/partials/footer.html", "new footer");

  TEST_CHECK(is_listing(dry_run_listing(os, false, &arena),
                        "write\tpublic/a.html\n"
                        "write\tpublic/b.html\n"
                        "keep\tpublic/404.html\n"
                        "write\tpublic/_sosig/output-cache.tsv\n"));
}

// `--force` keeps nothing: a dry run lists every output as `write`, and a build writes every file
// again, though none changed.
static void test_force_makes_every_output(void) {
  struct Os* os = every_output_kind_fixture();
  struct Arena arena = test_arena();
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  const struct PathList files = os_memory_files(os, "public");
  int64_t* mtimes = arena_new(&arena, files.count, int64_t);
  for (ptrdiff_t i = 0; i < files.count; i++) {
    mtimes[i] = file_mtime(os, files.items[i]);
  }

  TEST_CHECK(is_listing(dry_run_listing(os, true, &arena), EVERY_OUTPUT_KIND_WRITES));
  const struct BuildOptions options = {.is_forced = true};
  TEST_CHECK(execute_build_with(&options, os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(is_file_list(os, "public", EVERY_OUTPUT_KIND_FILES, EVERY_OUTPUT_KIND_FILE_COUNT));
  for (ptrdiff_t i = 0; i < files.count; i++) {
    TEST_CHECK_(file_mtime(os, files.items[i]) > mtimes[i], "'%.*s' is written again",
                STR_FMT(files.items[i]));
  }
}

// A build that fails after it copied a static file still writes the output cache, with that copy's
// row, so the next build keeps the copy. The copy that failed, an asset whose new bytes could not
// be written over its old copy, gets no row, so the next build copies it again.
static void test_failed_build_keeps_the_copies_it_made(void) {
  struct Os* os = every_output_kind_fixture();
  struct Arena arena = test_arena();
  os_memory_write(os, "content/paper.pdf", "pdf");
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  os_memory_write(os, "static/sosig.css", "body{color:red}\n");
  os_memory_write(os, "content/paper.pdf", "pdf, second edition");
  os_memory_fail(os, OS_MEMORY_OP_WRITE, "public/paper.pdf", EACCES);
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(
      test_log_is(&log, "failed to copy static file: %s ('public/paper.pdf')\n", strerror(EACCES)));
  TEST_CHECK(os_memory_is_file(os, "public/sosig.css", "body{color:red}\n"));
  TEST_CHECK(os_memory_is_file(os, "public/paper.pdf", "pdf"));
  struct Buf cache = {.arena = &arena};
  buf_printf(&cache,
             "output cache v1\noutput\tsosig.css\tsrc=sosig.css len=16 mtime=%lld.000000000\t16\n",
             (long long)file_mtime(os, S("static/sosig.css")));
  TEST_CHECK(os_memory_is_file(os, "public/_sosig/output-cache.tsv", buf_cstr(&cache)));
  os_memory_fail(os, OS_MEMORY_OP_WRITE, "public/paper.pdf", 0);

  TEST_CHECK(is_listing(dry_run_listing(os, false, &arena),
                        "keep\tpublic/alpha.html\n"
                        "keep\tpublic/beta.html\n"
                        "keep\tpublic/index.html\n"
                        "keep\tpublic/feed.xml\n"
                        "keep\tpublic/404.html\n"
                        "keep\tpublic/sosig.css\n"
                        "write\tpublic/paper.pdf\n"
                        "write\tpublic/_sosig/output-cache.tsv\n"));
}

// An output cache that is empty, as a build that did not finish leaves it, or that does not parse,
// keeps nothing, and is never an error: the verbose log says why, every file is copied again, and
// every page whose bytes are unchanged is still kept.
static void test_ignores_output_cache_it_cannot_use(void) {
  static const struct {
    const char* text;
    const char* reason;
  } cases[] = {
      {"", "output cache is empty, as a build that did not finish leaves it"},
      {"not an output cache\n", "output cache is not in the current format"},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct Os* os = every_output_kind_fixture();
    struct Arena arena = test_arena();
    struct Log log = test_log();
    TEST_CHECK(execute_build(os, &log) == 0);
    os_memory_write(os, "public/_sosig/output-cache.tsv", cases[i].text);

    const struct BuildOptions options = {.is_verbose = true, .is_dry_run = true};
    TEST_CHECK(execute_build_with(&options, os, &log) == 0);
    TEST_CHECK(test_log_is_empty(&log));
    TEST_CHECK(os_memory_is_printed(os, 1,
                                    "keep\tpublic/alpha.html\n"
                                    "keep\tpublic/beta.html\n"
                                    "keep\tpublic/index.html\n"
                                    "keep\tpublic/feed.xml\n"
                                    "keep\tpublic/404.html\n"
                                    "write\tpublic/sosig.css\n"
                                    "write\tpublic/_sosig/output-cache.tsv\n"));
    struct Buf line = {.arena = &arena};
    buf_printf(&line, "output cache not used, copying every file: %s", cases[i].reason);
    TEST_CHECK(has_printed_line(os, 2, buf_cstr(&line)));
    TEST_MSG("case %td", i);
  }
}

// The build cache of the format before the shared one has another name, so a build never reads it,
// and the stale-output prune removes it, keeping the current one.
static void test_prunes_the_old_build_cache(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  os_memory_write(os, "public/_sosig/v1-build-cache.tsv", "an older format\n");
  struct Log log = test_log();

  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(!os_memory_has_path(os, "public/_sosig/v1-build-cache.tsv"));
  TEST_CHECK(os_memory_has_path(os, "public/_sosig/output-cache.tsv"));
}

// A caller-supplied `worker_count` reaches every pool instead of being replaced by the detected
// default. A build with it produces the same bytes as the default build. The resolved count is not
// observable from the return value, so it is read off the verbose phase lines, which are the only
// place the module reports it. Workers finish their jobs in any order, so the progress lines are
// not compared.
static void test_honors_requested_worker_count(void) {
  // One worker, requested explicitly, must be honored rather than read as unset. A guard spelled
  // `worker_count != 1` instead of `!= 0` swallows that request and falls back to the detected core
  // count, which asking for `2` cannot see. Asking for one is also the only way to ask for a serial
  // build. A request the build dropped would show the detected count. Both cases assume the host
  // reports at least three cores.
  static const ptrdiff_t worker_counts[] = {2, 1};
  static const char* const source_paths[] = {"content/a.md", "content/b.md", "content/c.md"};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(worker_counts) / sizeof(worker_counts[0])); i++) {
    struct Os* os = site_fixture(NULL);
    for (ptrdiff_t j = 0; j < 3; j++) {
      os_memory_write(os, source_paths[j], HELLO_ENTRY);
    }
    const struct BuildOptions options = {
        .config_path = SITE_CONFIG_PATH_DEFAULT,
        .worker_count = worker_counts[i],
        .is_verbose = true,
    };
    TEST_CHECK(cmd_build_run(&options, os) == EXIT_CODE_OK);
    TEST_CHECK(os_memory_is_printed(os, 1, ""));
    struct Arena arena = test_arena();
    struct Buf parse_line = {.arena = &arena};
    struct Buf render_line = {.arena = &arena};
    buf_printf(&parse_line, "parsing content, workers: %td", worker_counts[i]);
    buf_printf(&render_line, "rendering content, workers: %td", worker_counts[i]);
    TEST_CHECK(has_printed_line(os, 2, buf_cstr(&parse_line)));
    TEST_CHECK(has_printed_line(os, 2, buf_cstr(&render_line)));
    // The requested count changes only how the work is scheduled, never the output.
    TEST_CHECK(os_memory_is_file(os, "public/a.html", HELLO_PAGE));
    TEST_CHECK(os_memory_is_file(os, "public/c.html", HELLO_PAGE));
  }
}

// Under an address-space limit that refuses even one worker's full arenas, the build runs on the
// one worker that `build_run` kept, though it asked for four, and the verbose phase lines name that
// count, where a build that kept the requested count would name four. The limit is far below one
// worker's arenas, and above the smallest size that halving them reaches.
static void test_uses_the_worker_count_the_memory_allows(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/a.md", HELLO_ENTRY);
  const struct BuildOptions options = {
      .config_path = SITE_CONFIG_PATH_DEFAULT,
      .worker_count = 4,
      .is_verbose = true,
  };
  arena_reserve_len_max = (ptrdiff_t)1 << 30;
  const enum ExitCode code = cmd_build_run(&options, os);
  arena_reserve_len_max = PTRDIFF_MAX;
  TEST_CHECK(code == EXIT_CODE_OK);
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
  TEST_CHECK(has_printed_line(os, 2, "parsing content, workers: 1"));
  TEST_CHECK(has_printed_line(os, 2, "rendering content, workers: 1"));
  TEST_CHECK(os_memory_is_file(os, "public/a.html", HELLO_PAGE));
}

// A site with no content sources skips the parse, conversion and render phases, so the verbose log
// carries no status or progress line for any of them. A phase with no jobs returns before it
// prints, so an empty site does not report a worker count for work it never starts.
static void test_skips_empty_phases_in_verbose_output(void) {
  struct Os* os = site_fixture(NULL);
  // A file that is not a source keeps `content_dir` present while the site holds no sources.
  os_memory_write(os, "content/notes.txt", "not a source\n");
  const struct BuildOptions options = {
      .config_path = SITE_CONFIG_PATH_DEFAULT,
      .worker_count = 3,
      .is_verbose = true,
  };

  TEST_CHECK(cmd_build_run(&options, os) == EXIT_CODE_OK);
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
  TEST_CHECK(os_memory_is_printed(os, 2,
                                  "loading config\n"
                                  "discovering content\n"
                                  "discovering templates and static files\n"
                                  "collecting content entries\n"
                                  "compiling templates\n"
                                  "building output manifest\n"
                                  "loading output cache\n"
                                  "no output cache, copying every file\n"
                                  "rendering aggregate templates\n"
                                  "rendering feed templates\n"
                                  "rendering not-found page\n"
                                  "copying static files\n"
                                  "writing output cache\n"
                                  "removing stale outputs\n"
                                  "removed stale outputs: 0\n"
                                  "build complete\n"));
}

// A build that succeeds leaves its log empty, which is the other half of `cmd_build_execute`'s
// `log` contract. Without this a phase that logged on the success path would go unnoticed, since
// every other test that inspects the log is a failure test.
static void test_leaves_log_empty_on_success(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
}

// `cmd_build_run` reports failure as `EXIT_CODE_FAILURE` and prints the collected diagnostic to
// standard error exactly once, which no other test asserts. Every other test routes failures
// through `cmd_build_execute` to inspect the message, so without this the boundary itself is
// unasserted: the exit code and the single print.
static void test_run_prints_diagnostic_once_and_fails(void) {
  // No `sosig.toml`, the earliest failure, so the diagnostic is a single known line.
  struct Os* os = os_memory();
  const struct BuildOptions options = {.config_path = SITE_CONFIG_PATH_DEFAULT};

  TEST_CHECK(cmd_build_run(&options, os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
  // Compared whole rather than by substring: "exactly once" is the claim, and only a whole-buffer
  // comparison can tell one print from two.
  struct Arena arena = test_arena();
  struct Buf expected = {.arena = &arena};
  buf_printf(&expected, "failed to read config: %s ('sosig.toml')\n", strerror(ENOENT));
  TEST_CHECK(os_memory_is_printed(os, 2, buf_cstr(&expected)));
}

// A build whose arena is too small for its data ends in `os_oom` rather than failing with a
// diagnostic or overrunning the arena. Under the test build's `os_oom` that is the process exiting
// with `TEST_OOM_EXIT_STATUS`. `main.c`'s `os_oom` exits with `EXIT_CODE_FAILURE` instead, so this
// test does not pin the production exit status.
static void test_tiny_arena_ends_in_oom(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  TEST_CHECK(test_is_oom(build_in_tiny_arena, os));
}

// A missing `sosig.toml` is logged with its cause, and nothing prints it before the command
// boundary. The cause is part of the claim: a missing config must not read like an unreadable one.
static void test_reports_missing_config(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(os_memory_is_printed(os, 2, ""));
  // Exact, not by substring: a substring check would also pass for this message with something
  // appended to it.
  TEST_CHECK(test_log_is(&log, "failed to read config: %s ('sosig.toml')\n", strerror(ENOENT)));
}

// `content_dir` is required, so an absent one is a failure, reported as a resolution failure
// carrying the system cause. `ENOENT` and `EACCES` need different fixes and nothing later would
// report either, so the cause has to appear here. Compared exactly, so a caller that appended the
// path a second time would fail.
static void test_reports_absent_content_dir(void) {
  struct Os* os = site_fixture(NULL);
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to resolve config directory 'content_dir': %s ('content')\n",
                         strerror(ENOENT)));
}

// `templates_dir` is required the same way, and reports the key the user has to fix rather than the
// one that happened to be checked first. A root that cannot be inspected reports its own cause.
static void test_reports_absent_templates_dir(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "sosig.toml", SITE_CONFIG);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to resolve config directory 'templates_dir': %s ('templates')\n",
                         strerror(ENOENT)));

  os_memory_fail(os, OS_MEMORY_OP_STAT, "content", EACCES);
  log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to resolve config directory 'content_dir': %s ('content')\nfailed "
                         "to resolve config directory 'templates_dir': %s ('templates')\n",
                         strerror(EACCES), strerror(ENOENT)));
}

// A `templates_dir` whose subdirectory cannot be listed stops the build while it lists the inputs,
// because a partial the walk could not see could be neither compiled nor protected. The reason
// names the directory that failed, so the configured root is not repeated ahead of it.
static void test_reports_an_unlistable_templates_subdirectory(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  os_memory_write(os, "templates/partials/card.html", "card");
  os_memory_fail(os, OS_MEMORY_OP_LIST, "templates/partials", EACCES);
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(
      &log, "failed to list templates: cannot open directory: %s ('templates/partials')\n",
      strerror(EACCES)));
  TEST_CHECK(!os_memory_has_path(os, "public"));
}

// A root that fails its check stops the build before the overlap check, so a missing `content_dir`
// is the one error even with an `output_dir` inside `templates_dir`.
static void test_stops_at_a_bad_root_before_the_overlap_check(void) {
  struct Os* os = site_fixture("output_dir = \"templates/site\"\n");
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to resolve config directory 'content_dir': %s ('content')\n",
                         strerror(ENOENT)));
  TEST_CHECK(!os_memory_has_path(os, "templates/site"));
}

// A directory key the config load rejects stops the build with that one error, before any directory
// is checked. A `/` is rejected as empty once its trailing `/` is trimmed, the same way.
static void test_stops_at_a_rejected_directory_key(void) {
  static const char* const cases[][4] = {
      {"content_dir = 5\n", "posts/hello.md", "templates/content.html",
       "config key 'content_dir' must be a string"},
      {"content_dir = \"/\"\n", "posts/hello.md", "templates/content.html",
       "config key 'content_dir' must not be empty"},
      {"templates_dir = [\"layouts\"]\n", "content/hello.md", "layouts/content.html",
       "config key 'templates_dir' must be a string"},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    TEST_CASE(cases[i][0]);
    struct Os* os = os_memory();
    struct Arena config_arena = test_arena();
    struct Buf config = {.arena = &config_arena};
    buf_append(&config, str_from_cstr(cases[i][0]));
    buf_append(&config, str_from_cstr(SITE_CONFIG));
    os_memory_write(os, "sosig.toml", buf_cstr(&config));
    os_memory_write(os, cases[i][1], HELLO_ENTRY);
    os_memory_write(os, cases[i][2], CONTENT_TEMPLATE);
    struct Log log = test_log();
    TEST_CHECK(execute_build(os, &log) == -1);
    TEST_CHECK(test_log_is_one(&log, cases[i][3]));
  }
  TEST_CASE_(NULL);
}

// A rejected `static_dir` is the one error too, even when its default names something that would
// fail on its own: here a regular file, which the root check would report as not a directory.
static void test_stops_at_a_rejected_static_dir(void) {
  struct Os* os = site_fixture("static_dir = \"\"\n");
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  os_memory_write(os, "static", "not a directory\n");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is_one(&log, "config key 'static_dir' must not be empty"));
}

// A `templates_dir` that exists but is a regular file is reported as the wrong type, which is its
// own cause rather than an `errno`, so the message carries no system text.
static void test_reports_templates_dir_that_is_a_file(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "sosig.toml", SITE_CONFIG);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  os_memory_write(os, "templates", "not a directory\n");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to resolve config directory 'templates_dir': not a directory "
                         "('templates')\n"));
}

// A configured `output_dir` that already exists as a regular file is reported once the manifest has
// passed, where the build creates `output_dir`, before any page is written. The reason names the
// component that failed and the caller does not repeat the configured root, so the message is
// compared whole.
static void test_reports_unusable_output_dir(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  // `public` is the default `output_dir`, so a file there is what makes `os_mkdirs` fail.
  os_memory_write(os, "public", "not a directory");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(
      &log, "failed to prepare output directory: exists and is not a directory ('public')\n"));
}

// An `output_dir` that is a dangling symlink is reported once the manifest has passed, where the
// build creates `output_dir`, before any page is written. A dangling symlink has no identity, so
// the manifest sees nothing there, and `os_mkdirs` then fails the `stat` that follows `mkdir`'s
// `EEXIST`. The message is compared whole.
static void test_reports_dangling_output_dir_symlink(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  os_memory_symlink(os, "missing", "public");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(
      &log, "failed to prepare output directory: cannot inspect directory: %s ('public')\n",
      strerror(ENOENT)));
}

// An `output_dir` below `content_dir` is refused before content is discovered, so no earlier
// build's output can be read back as a source, and it is refused although it does not exist yet.
// The verbose phase lines show that the build stopped before the content walk, and the refused
// build creates none of the nested directories.
static void test_rejects_output_dir_inside_content_dir(void) {
  struct Os* os = site_fixture("output_dir = \"content/generated/site\"\n");
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  const struct BuildOptions options = {.config_path = SITE_CONFIG_PATH_DEFAULT, .is_verbose = true};

  TEST_CHECK(cmd_build_run(&options, os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
  // The status line comes first and the log once the build has stopped.
  TEST_CHECK(os_memory_is_printed(os, 2,
                                  "loading config\n"
                                  "output directory would write inside 'content_dir': "
                                  "'content/generated/site'\n"));
  TEST_CHECK(!os_memory_has_path(os, "content/generated"));
}

// An `output_dir` that holds `content_dir`, such as `output_dir = "."`, is refused before content
// is discovered, so the project's own files never share a tree with its outputs, and the refused
// build writes nothing beside them.
static void test_rejects_output_dir_holding_content_dir(void) {
  struct Os* os = site_fixture("output_dir = \".\"\n");
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  const struct BuildOptions options = {.config_path = SITE_CONFIG_PATH_DEFAULT, .is_verbose = true};

  TEST_CHECK(cmd_build_run(&options, os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
  TEST_CHECK(os_memory_is_printed(os, 2,
                                  "loading config\n"
                                  "output directory would hold 'content_dir': '.'\n"));
  // The config, the templates, and the source.
  static const char* const expected[] = {"./content/hello.md", "./sosig.toml",
                                         "./templates/404.html", "./templates/content.html"};
  TEST_CHECK(is_file_list(os, ".", expected, 4));
}

// The config that `-c` names is the one the build protects: an `output_dir` that holds its
// directory is refused, while every input root lies outside it.
static void test_rejects_output_dir_holding_config_in_another_dir(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "site/sosig.toml",
                  "content_dir = \"../content\"\n"
                  "templates_dir = \"../templates\"\n"
                  "output_dir = \".\"\n"
                  "title = \"Site\"\n"
                  "description = \"Example notes\"\n"
                  "author = \"Author\"\n"
                  "base_url = \"https://example.com\"\n");
  os_memory_write(os, "templates/content.html", CONTENT_TEMPLATE);
  os_memory_write(os, "templates/404.html", NOT_FOUND_TEMPLATE);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);

  const struct BuildOptions options = {.config_path = S("site/sosig.toml")};
  struct Log log = test_log();
  TEST_CHECK(execute_build_with(&options, os, &log) == -1);
  TEST_CHECK(test_log_is_one(&log, "output directory would hold the config: 'site'"));
}

// Slugging is lossy, so two different source names can claim one output path. The manifest reports
// the collision and names both sources instead of letting the second write win. A non-ASCII byte
// folds to hex, so `caf\xC3\xA9` produces the slug a literal `cafc3a9` also produces. The refused
// build creates no `output_dir`, because nothing is written before the manifest passes.
static void test_rejects_colliding_source_names(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/caf\xC3\xA9.md", HELLO_ENTRY);
  os_memory_write(os, "content/cafc3a9.md", HELLO_ENTRY);

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  // Exact, not by substring: a substring check would also pass for this message with something
  // appended to it.
  TEST_CHECK(test_log_is(&log,
                         "duplicate output path for 'content/cafc3a9.md' and "
                         "'content/caf\xC3\xA9.md': 'public/cafc3a9.html'\n"));
  TEST_CHECK(!os_memory_has_path(os, "public"));
}

// A static file copied to the output root shares its paths with the generated pages, so one named
// after a content page is rejected as a duplicate of it rather than overwriting it or being
// overwritten. The refused build creates no `output_dir`.
static void test_rejects_static_file_colliding_with_content_page(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  os_memory_write(os, "static/hello.html", "static\n");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "duplicate output path for 'content/hello.md' and "
                         "'static/hello.html': 'public/hello.html'\n"));
  TEST_CHECK(!os_memory_has_path(os, "public"));
}

// A content template that fails to parse fails the build when the templates load, before anything
// is written. The diagnostic carries the parser's cause, its position and the template name.
static void test_reports_bad_template(void) {
  struct Os* os = site_fixture("templates.content = \"broken.html\"\n");
  // The section never closes, so the template is read but fails to compile.
  os_memory_write(os, "templates/broken.html", "{{#title}}{{title}}\n");
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  // Exact, not by substring: a substring check would also pass for this message with something
  // appended to it.
  TEST_CHECK(test_log_is(
      &log, "section-opening tag has no closer at line 1, column 1 (in 'broken.html')\n"));
  TEST_CHECK(!os_memory_has_path(os, "public"));
}

// A dry run loads every template, so a content template that does not compile fails it with the
// diagnostic a real build gives, and no listing is printed.
static void test_dry_run_reports_bad_template(void) {
  struct Os* os = site_fixture("templates.content = \"broken.html\"\n");
  os_memory_write(os, "templates/broken.html", "{{#title}}{{title}}\n");
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);

  struct Log log = test_log();
  TEST_CHECK(execute_dry_run(os, &log) == -1);
  TEST_CHECK(test_log_is(
      &log, "section-opening tag has no closer at line 1, column 1 (in 'broken.html')\n"));
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
}

// A dry run of two entries on one output path fails with the duplicate-path diagnostic a real build
// gives, and no listing is printed.
static void test_dry_run_rejects_duplicate_output(void) {
  struct Os* os = site_fixture("permalink = \"/{slug}.html\"\n");
  os_memory_write(os, "content/a/post.md", HELLO_ENTRY);
  os_memory_write(os, "content/b/post.md", HELLO_ENTRY);

  struct Log log = test_log();
  TEST_CHECK(execute_dry_run(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "duplicate output path for 'content/a/post.md' and "
                         "'content/b/post.md': 'public/post.html'\n"));
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
}

// A build that fails removes nothing, so a stale file from an earlier build is still there after a
// render failure.
static void test_failed_build_removes_no_stale_output(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "templates/content.html", "{{#title}}{{title}}\n");
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  os_memory_write(os, "public/notes.txt", "hand");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(log.errors == 1);
  TEST_CHECK(os_memory_is_file(os, "public/notes.txt", "hand"));
}

// A stale entry that cannot be removed fails the build after every output is written, and the
// failure names the entry.
static void test_reports_stale_output_that_cannot_be_removed(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  os_memory_write(os, "public/notes.txt", "hand");
  os_memory_fail(os, OS_MEMORY_OP_REMOVE, "public/notes.txt", EACCES);

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to remove stale output: %s ('public/notes.txt')\n",
                         strerror(EACCES)));
  TEST_CHECK(os_memory_has_path(os, "public/hello.html"));
  TEST_CHECK(os_memory_has_path(os, "public/notes.txt"));
}

// Two content entries that fail to render through one template fail for one cause. The report
// prints the first entry's line whole and counts the second, and both count as errors.
// `test_reports_unwritable_output` covers two entries that fail for different causes.
static void test_reports_a_shared_render_failure_once(void) {
  // A triple mustache of `title` fails the page render of both entries.
  struct Os* os = site_fixture("templates.content = \"raw.html\"\n");
  os_memory_write(os, "templates/raw.html", "{{{title}}}\n");
  os_memory_write(os, "content/a.md", HELLO_ENTRY);
  os_memory_write(os, "content/b.md",
                  "+++\ntitle = \"Second\"\ndate = 2026-07-02T00:00:00Z\n+++\nBody\n");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  // The page phase reports in source order, and sources are walked in sorted order, so `a.html` is
  // the one printed.
  TEST_CHECK(
      test_log_is(&log,
                  "unescaped Mustache interpolation is supported only for a key that ends "
                  "in '_html' (in 'raw.html') (while rendering 'a.html')\n\xE2\x80\xA6 and 1 "
                  "more failure\n"));
  TEST_CHECK(log.errors == 2);
}

// A template that only entries name, and that is missing, fails the build once, when the templates
// load, naming the first of those entries in source order.
static void test_reports_a_missing_entry_template_once(void) {
  struct Os* os = site_fixture(NULL);
  static const char* const sources[] = {"content/b.md", "content/a.md"};
  for (ptrdiff_t i = 0; i < 2; i++) {
    os_memory_write(os, sources[i],
                    "+++\ntitle = \"T\"\ndate = 2026-07-01T00:00:00Z\ntemplate = \"missing.html\"\n"
                    "+++\nBody\n");
  }

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to read template: %s ('templates/missing.html') (for "
                         "'content/a.md')\n",
                         strerror(ENOENT)));
  TEST_CHECK(!os_memory_has_path(os, "public"));
}

// A missing template that a `[templates]` key names fails the build when the templates load, naming
// the key, and a template that two keys name names the first in table order.
static void test_reports_a_missing_configured_template_with_its_key(void) {
  static const struct {
    const char* templates;
    const char* missing;
    const char* key;
  } cases[] = {
      {"content = \"missing.html\"\naggregates = []\nfeeds = []\n", "missing.html",
       "templates.content"},
      {"content = \"index.html\"\naggregates = [\"index.html\"]\nfeeds = []\n", "index.html",
       "templates.content"},
      {"aggregates = [\"index.html\"]\nfeeds = []\n", "index.html", "templates.aggregates"},
      {"aggregates = []\nfeeds = [\"atom.xml\"]\n", "atom.xml", "templates.feeds"},
      {"aggregates = []\nfeeds = []\nnot_found = \"missing.html\"\n", "missing.html",
       "templates.not_found"},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    TEST_CASE(cases[i].templates);
    struct Os* os = os_memory();
    struct Arena arena = test_arena();
    struct Buf config = {.arena = &arena};
    buf_printf(&config,
               "title = \"Site\"\n"
               "description = \"Example notes\"\n"
               "author = \"Author\"\n"
               "base_url = \"https://example.com\"\n"
               "[templates]\n"
               "%s",
               cases[i].templates);
    os_memory_write(os, "sosig.toml", buf_cstr(&config));
    os_memory_write(os, "templates/content.html", CONTENT_TEMPLATE);
    os_memory_write(os, "templates/404.html", NOT_FOUND_TEMPLATE);
    os_memory_write(os, "content/hello.md", HELLO_ENTRY);

    struct Log log = test_log();
    TEST_CHECK(execute_build(os, &log) == -1);
    TEST_CHECK(test_log_is(&log, "failed to read template: %s ('templates/%s') (for '%s')\n",
                           strerror(ENOENT), cases[i].missing, cases[i].key));
    TEST_CHECK(!os_memory_has_path(os, "public"));
  }
}

// An output path that cannot be written fails the build at the page phase, where each job writes
// the page it rendered. The failure is that entry's own diagnostic, so the other entries' pages are
// still written. A write failure reported as success would otherwise pass the suite with the page
// silently missing. One page path is an existing directory, and the other fails with an injected
// `EACCES`, as an unwritable directory would refuse it.
static void test_reports_unwritable_output(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/a.md", HELLO_ENTRY);
  os_memory_write(os, "content/b.md", HELLO_ENTRY);
  os_memory_write(os, "content/c.md", HELLO_ENTRY);
  os_memory_mkdir(os, "public/a.html");
  os_memory_fail(os, OS_MEMORY_OP_WRITE, "public/c.html", EACCES);

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to write page for 'content/a.md': %s ('public/a.html')\nfailed "
                         "to write page for 'content/c.md': %s ('public/c.html')\n",
                         strerror(EISDIR), strerror(EACCES)));
  TEST_CHECK(os_memory_is_file(os, "public/b.html", HELLO_PAGE));
}

// A permalink that would expand to a path escaping the output directory is rejected at config load,
// so the build reports it once rather than once per content entry.
static void test_rejects_unsafe_permalink(void) {
  struct Os* os = site_fixture("permalink = \"/{slug}/../evil.html\"\n");
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  // The load stops at the permalink, so the entries are not checked. Exact, not by substring: a
  // substring check would also pass for this message with something appended to it.
  TEST_CHECK(test_log_is(&log,
                         "config key 'permalink' must expand to a safe relative path using only "
                         "letters, digits, '_', '-', '.', '/' and the '{slug}'/'{section}' tokens, "
                         "with no empty, '.' or '..' path segment: '/{slug}/../evil.html'\n"));
  TEST_CHECK(log.errors == 1);
}

// A config error stops the build after the config load, with that error alone and nothing written.
// The load stops at its first error, so the second case reports the permalink and not the
// `templates.feeds` that the load checks later.
static void test_stops_after_config_error(void) {
  static const struct {
    const char* config;
    const char* expected;
  } cases[] = {
      {"templates.aggregates = [5]\n",
       "config key 'templates.aggregates' must contain only strings\n"},
      {"templates.feeds = [\"../x\"]\npermalink = \"/{slug}.htm#l\"\n",
       "config key 'permalink' must expand to a safe relative path using only letters, digits, "
       "'_', '-', '.', '/' and the '{slug}'/'{section}' tokens, with no empty, '.' or '..' path "
       "segment: '/{slug}.htm#l'\n"},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    TEST_CASE(cases[i].config);
    struct Os* os = os_memory();
    char config[512];
    const int config_len = snprintf(config, sizeof(config),
                                    "title = \"Site\"\n"
                                    "description = \"Example notes\"\n"
                                    "author = \"Author\"\n"
                                    "base_url = \"https://example.com\"\n"
                                    "%s",
                                    cases[i].config);
    TEST_ASSERT(config_len > 0 && config_len < (int)sizeof(config));
    os_memory_write(os, "sosig.toml", config);
    os_memory_write(os, "content/index.md", HELLO_ENTRY);
    os_memory_write(os, "templates/content.html", CONTENT_TEMPLATE);
    os_memory_write(os, "templates/404.html", NOT_FOUND_TEMPLATE);
    os_memory_write(os, "templates/index.html", "index\n");

    struct Log log = test_log();
    TEST_CHECK(execute_build(os, &log) == -1);
    TEST_CHECK(test_log_is(&log, "%s", cases[i].expected));
    TEST_CHECK(!os_memory_has_path(os, "public"));
  }
}

// A content entry that cannot be parsed fails the build at the parse phase and its diagnostic
// reaches the command boundary. This is a different wiring from
// `test_reports_a_shared_render_failure_once`, which fails at the *page* phase: the two failures
// travel through separate entry points, `entry_renderer_parse_entries` and
// `page_renderer_render_pages`, so neither test covers the other's propagation. Without this, no
// test shows a parse-phase error reaching the log that stops the build before its first write. An
// unreadable source fails the same phase.
static void test_reports_unparsable_content(void) {
  struct Os* os = site_fixture(NULL);
  // No closing fence, so `frontmatter_split` rejects the file before any key is read.
  os_memory_write(os, "content/a.md", "+++\ntitle = \"X\"\ndate = 2026-07-01T00:00:00Z\nBody\n");
  os_memory_write(os, "content/b.md", HELLO_ENTRY);
  os_memory_fail(os, OS_MEMORY_OP_READ, "content/b.md", EACCES);

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "missing closing '+++' frontmatter fence (in 'content/a.md')\nfailed to "
                         "read content: %s ('content/b.md')\n",
                         strerror(EACCES)));
}

// A link to a missing file and a link to a draft, by path and by denote identifier, each fail the
// build with their own message, reported in one run with a parse error in another note, and the
// build writes nothing. A dry run reports the same errors. A link from a draft is not checked.
static void test_reports_every_broken_link(void) {
  static const char* const expected =
      "missing closing '+++' frontmatter fence (in 'content/bad.md')\n"
      "link 'other.md' names no content file (in 'content/notes/a.org')\n"
      "link '20240101T000000--b.org' names draft 'content/notes/20240101T000000--b.org' (in "
      "'content/notes/a.org')\n"
      "denote link '19990101T000000' names no file (in 'content/notes/a.org')\n"
      "denote link '20240101T000000' names draft 'content/notes/20240101T000000--b.org' (in "
      "'content/notes/a.org')\n";
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/bad.md", "+++\ntitle = \"Bad\"\n");
  os_memory_write(os, "content/notes/a.org",
                  "#+title: A\n#+date: 2024-01-03\n\n"
                  "[[file:other.md][O]] [[file:20240101T000000--b.org][B]] "
                  "[[denote:19990101T000000][G]] [[denote:20240101T000000][D]]\n");
  os_memory_write(os, "content/notes/20240101T000000--b.org",
                  "#+title: B\n#+date: 2024-01-01\n#+draft: true\n\n[[file:missing.org][M]]\n");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log, "%s", expected));
  TEST_CHECK(log.errors == 5);
  TEST_CHECK(!os_memory_has_path(os, "public"));

  struct Log dry_run_log = test_log();
  TEST_CHECK(execute_dry_run(os, &dry_run_log) == -1);
  TEST_CHECK(test_log_is(&dry_run_log, "%s", expected));
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
}

// A link to a note whose parse failed reports only that note's own error, which already fails the
// build.
static void test_reports_only_the_parse_error_of_a_linked_note(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/a.md", "+++\ntitle = \"A\"\ndate = 2024-01-01\n+++\n[B](b.md)\n");
  os_memory_write(os, "content/b.md", "+++\ntitle = \"B\"\n");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log, "missing closing '+++' frontmatter fence (in 'content/b.md')\n"));
}

// A source over the content limit is rejected by its read. The file is extended as `truncate`
// extends a sparse file, so the fixture costs no memory.
static void test_rejects_oversize_source_at_read(void) {
  enum { BIG_SOURCE_LEN = 1024 * 1024 * 1024 };
  char expected[EXPECTED_SIZE];
  const int expected_len = snprintf(
      expected, sizeof(expected),
      "failed to read content: exceeds max file size (%d bytes) at %d bytes ('content/big.md')",
      CONTENT_FILE_LEN_MAX, BIG_SOURCE_LEN);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/big.md", HELLO_ENTRY);
  os_memory_truncate(os, "content/big.md", BIG_SOURCE_LEN);
  const struct BuildOptions options = {.worker_count = 1};

  struct Log log = test_log();
  TEST_CHECK(execute_build_with(&options, os, &log) == -1);
  TEST_CHECK(test_log_is(&log, "%s\n", expected));
}

// A content page at the not-found page's path is refused before anything is written, and the
// message names `not_found_path`.
static void test_rejects_content_page_at_not_found_path(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/404.md", HELLO_ENTRY);

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "duplicate output path for 'content/404.md' and 'not_found_path': "
                         "'public/404.html'\n"));
  TEST_CHECK(!os_memory_has_path(os, "public"));
}

// A configured aggregate output that collides with a content entry output is rejected, with the
// collision described in the returned diagnostic. The refused build creates no `output_dir`,
// because nothing is written before the manifest passes.
static void test_rejects_duplicate_output(void) {
  // The shared config turns off aggregates, so this fixture writes its own.
  struct Os* os = os_memory();
  os_memory_write(os, "sosig.toml",
                  "title = \"Site\"\n"
                  "description = \"Example notes\"\n"
                  "author = \"Author\"\n"
                  "base_url = \"https://example.com\"\n"
                  "templates.aggregates = [\"index.html\"]\n"
                  "templates.feeds = []\n");
  os_memory_write(os, "content/index.md", HELLO_ENTRY);
  os_memory_write(os, "templates/content.html", CONTENT_TEMPLATE);
  os_memory_write(os, "templates/404.html", NOT_FOUND_TEMPLATE);
  os_memory_write(os, "templates/index.html", "index\n");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  // Exact, not by substring: a substring check would also pass for this message with something
  // appended to it.
  TEST_CHECK(test_log_is(&log,
                         "duplicate output path for 'content/index.md' and "
                         "'templates.aggregates[0]': 'public/index.html'\n"));
  TEST_CHECK(!os_memory_has_path(os, "public"));
}

// Several independent mistakes in one site are all reported by one build, and the build writes
// nothing: two sources whose frontmatter fails in different ways, two sources that claim one output
// path, and a static file that claims a content page. Each kind comes from a different phase, so
// the order is the phase order, and within a phase it is the sorted walk order the slots follow.
static void test_reports_every_independent_error(void) {
  const char same_slug[] =
      "+++\n"
      "title = \"Same\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "slug = \"same\"\n"
      "+++\n"
      "Body\n";
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/a.md", same_slug);
  os_memory_write(os, "content/b.md", same_slug);
  os_memory_write(os, "content/c.md", "+++\ntitle = \"C\"\n");
  os_memory_write(os, "content/d.md",
                  "+++\ntitle = \"Draft\"\ndate = 2026-07-01T00:00:00Z\ndraft = \"yes\"\n+++\n"
                  "Body\n");
  os_memory_write(os, "static/same.html", "static\n");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "missing closing '+++' frontmatter fence (in 'content/c.md')\n"
                         "frontmatter key 'draft' must be a boolean (in 'content/d.md')\n"
                         "duplicate output path for 'content/a.md' and 'content/b.md': "
                         "'public/same.html'\n"
                         "duplicate output path for 'content/a.md' and 'static/same.html': "
                         "'public/same.html'\n"));
  TEST_CHECK(log.errors == 4);
  TEST_CHECK(!os_memory_has_path(os, "public"));
}

// A `static_dir` that exists but is a regular file is reported. Skipping an absent optional root
// must not also skip a misconfigured one, which is why the skip asks whether anything is there
// rather than whether a directory is there: the wrong type survives the skip and reaches the type
// check, which names it.
static void test_reports_static_dir_that_is_a_file(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  os_memory_write(os, "static", "not a directory\n");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to resolve config directory 'static_dir': not a directory "
                         "('static')\n"));
}

// A `static_dir` that cannot be inspected is reported with its cause rather than skipped as absent,
// because it may well be there. A symlink that resolves to itself stands in for any such cause.
static void test_reports_static_dir_that_cannot_be_inspected(void) {
  struct Os* os = site_fixture(NULL);
  os_memory_write(os, "content/hello.md", HELLO_ENTRY);
  os_memory_symlink(os, "static", "static");

  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to resolve config directory 'static_dir': %s ('static')\n",
                         strerror(ELOOP)));
}

TEST_LIST = {
    {"honors configured content template", test_honors_configured_content_template},
    {"loads the content template only when an entry uses it",
     test_loads_the_content_template_only_when_an_entry_uses_it},
    {"writes exactly manifest outputs", test_writes_exactly_manifest_outputs},
    {"tolerates trailing slash on content_dir", test_tolerates_trailing_slash_on_content_dir},
    {"builds site of config in another dir", test_builds_site_of_config_in_another_dir},
    {"builds with no static dir", test_builds_with_no_static_dir},
    {"copies static files when present", test_copies_static_files_when_present},
    {"copies assets beside content", test_copies_assets_beside_content},
    {"renders a nested not found page from the root",
     test_renders_a_nested_not_found_page_from_the_root},
    {"honors custom permalink", test_honors_custom_permalink},
    {"distinguishes same name in different dirs", test_distinguishes_same_name_in_different_dirs},
    {"links notes to each others pages", test_links_notes_to_each_others_pages},
    {"resolves each form of link", test_resolves_each_form_of_link},
    {"links denote identifiers of each form", test_links_denote_identifiers_of_each_form},
    {"links the first source of a shared identifier",
     test_links_the_first_source_of_a_shared_identifier},
    {"skips links in drafts", test_skips_links_in_drafts},
    {"rebuild succeeds and repeats its outputs", test_rebuild_succeeds_and_repeats_its_outputs},
    {"rebuild removes stale outputs", test_rebuild_removes_stale_outputs},
    {"rebuild removes deleted asset", test_rebuild_removes_deleted_asset},
    {"rebuild skips output dir linked from content dir",
     test_rebuild_skips_output_dir_linked_from_content_dir},
    {"dry run lists every output and writes nothing",
     test_dry_run_lists_every_output_and_writes_nothing},
    {"dry run lists stale outputs and keeps them", test_dry_run_lists_stale_outputs_and_keeps_them},
    {"dry run attempts no change", test_dry_run_attempts_no_change},
    {"dry run leaves no state", test_dry_run_leaves_no_state},
    {"unchanged rebuild changes nothing", test_unchanged_rebuild_changes_nothing},
    {"dry run keeps every page after an unchanged build",
     test_dry_run_keeps_every_page_after_an_unchanged_build},
    {"changed static file is copied again", test_changed_static_file_is_copied_again},
    {"changed source rewrites only its page", test_changed_source_rewrites_only_its_page},
    {"page reading the entry list follows every entry",
     test_page_reading_the_entry_list_follows_every_entry},
    {"page linking an entry follows it", test_page_linking_an_entry_follows_it},
    {"page linking an asset follows its rename", test_page_linking_an_asset_follows_its_rename},
    {"changed partial makes every page", test_changed_partial_makes_every_page},
    {"force makes every output", test_force_makes_every_output},
    {"failed build keeps the copies it made", test_failed_build_keeps_the_copies_it_made},
    {"ignores output cache it cannot use", test_ignores_output_cache_it_cannot_use},
    {"prunes the old build cache", test_prunes_the_old_build_cache},
    {"honors requested worker count", test_honors_requested_worker_count},
    {"uses the worker count the memory allows", test_uses_the_worker_count_the_memory_allows},
    {"skips empty phases in verbose output", test_skips_empty_phases_in_verbose_output},
    {"leaves log empty on success", test_leaves_log_empty_on_success},
    {"run prints diagnostic once and fails", test_run_prints_diagnostic_once_and_fails},
    {"tiny arena ends in oom", test_tiny_arena_ends_in_oom},
    {"reports missing config", test_reports_missing_config},
    {"reports absent content dir", test_reports_absent_content_dir},
    {"reports absent templates dir", test_reports_absent_templates_dir},
    {"reports an unlistable templates subdirectory",
     test_reports_an_unlistable_templates_subdirectory},
    {"stops at a bad root before the overlap check",
     test_stops_at_a_bad_root_before_the_overlap_check},
    {"stops at a rejected directory key", test_stops_at_a_rejected_directory_key},
    {"stops at a rejected static dir", test_stops_at_a_rejected_static_dir},
    {"reports templates dir that is a file", test_reports_templates_dir_that_is_a_file},
    {"reports unusable output dir", test_reports_unusable_output_dir},
    {"reports dangling output dir symlink", test_reports_dangling_output_dir_symlink},
    {"rejects output dir inside content dir", test_rejects_output_dir_inside_content_dir},
    {"rejects output dir holding content dir", test_rejects_output_dir_holding_content_dir},
    {"rejects output dir holding config in another dir",
     test_rejects_output_dir_holding_config_in_another_dir},
    {"rejects colliding source names", test_rejects_colliding_source_names},
    {"rejects static file colliding with content page",
     test_rejects_static_file_colliding_with_content_page},
    {"reports bad template", test_reports_bad_template},
    {"dry run reports bad template", test_dry_run_reports_bad_template},
    {"dry run rejects duplicate output", test_dry_run_rejects_duplicate_output},
    {"failed build removes no stale output", test_failed_build_removes_no_stale_output},
    {"reports stale output that cannot be removed",
     test_reports_stale_output_that_cannot_be_removed},
    {"reports a shared render failure once", test_reports_a_shared_render_failure_once},
    {"reports a missing entry template once", test_reports_a_missing_entry_template_once},
    {"reports a missing configured template with its key",
     test_reports_a_missing_configured_template_with_its_key},
    {"reports unwritable output", test_reports_unwritable_output},
    {"rejects unsafe permalink", test_rejects_unsafe_permalink},
    {"stops after config error", test_stops_after_config_error},
    {"reports unparsable content", test_reports_unparsable_content},
    {"reports every broken link", test_reports_every_broken_link},
    {"reports only the parse error of a linked note",
     test_reports_only_the_parse_error_of_a_linked_note},
    {"rejects oversize source at read", test_rejects_oversize_source_at_read},
    {"rejects content page at not found path", test_rejects_content_page_at_not_found_path},
    {"rejects duplicate output", test_rejects_duplicate_output},
    {"reports every independent error", test_reports_every_independent_error},
    {"reports static dir that is a file", test_reports_static_dir_that_is_a_file},
    {"reports static dir that cannot be inspected",
     test_reports_static_dir_that_cannot_be_inspected},
    {NULL, NULL},
};
