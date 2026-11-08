#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "app/cmd_build.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/exit_code.h"
#include "core/log.h"
#include "core/path.h"
#include "core/path_list.h"
#include "core/str.h"
#include "domain/fram_version.h"
#include "domain/site_config.h"
#include "media/image.h"
#include "os/oom.h"
#include "os/os.h"
#include "os/os_memory.h"
#include "runtime/job.h"
#include "runtime/pool.h"
#include "test_jpeg_fixture.h"
#include "testing/test_support.h"

// Every build here runs against the in-memory host, whose root is the working directory the build
// resolves `fram.toml` from, and which keeps what the build prints. No fixture holds a video, so no
// build here runs `ffprobe` or `ffmpeg`. The build that does is in `test_cmd_build_video.c`, on the
// POSIX host. The golden galleries build the real executable on the real filesystem.

/**
 * Bytes of the arenas `build_in_tiny_arena` gives a build, a quarter for each of the main thread's
 * and the worker's perm and scratch. Each is too small for the configuration the build loads.
 */
enum { TINY_ARENA_SIZE = 128 };

/** Config body every fixture shares. Derivatives stay small so encoding stays cheap. */
static const char* const SITE_CONFIG =
    "title = \"Test Gallery\"\n"
    "description = \"Test description\"\n"
    "author = \"Tests\"\n"
    "media_dir = \"photos\"\n"
    "[derivatives.s]\nwidth = 8\nheight = 8\nquality = 70\ncrop = true\n"
    "[derivatives.m]\nwidth = 16\nheight = 16\nquality = 75\n";

/** Minimal album template. It renders without naming any derivative, so sizes stay free. */
static const char* const ALBUM_TEMPLATE =
    "<!doctype html><title>{{album.title}} - {{site.title}}</title>"
    "{{#has_media}}<ul>{{#media}}<li>{{title}}</li>{{/media}}</ul>{{/has_media}}";

/**
 * Album template for the large album, which lists each item the way a real gallery's template does:
 * its links, its derivatives' URLs and dimensions, and its title.
 */
static const char* const LARGE_ALBUM_TEMPLATE =
    "<!doctype html><title>{{album.title}} - {{site.title}}</title><main>"
    "{{#media}}<a class=\"item{{#is_image}} item--image{{/is_image}}"
    "{{#is_video}} item--video{{/is_video}}\" href=\"{{url}}\" download=\"{{title}}\" "
    "data-small-src=\"{{s.url}}\" data-small-width=\"{{s.width}}\" "
    "data-small-height=\"{{s.height}}\"><img src=\"{{m.url}}\" width=\"{{m.width}}\" "
    "height=\"{{m.height}}\" alt=\"{{title}}\"><span>{{title}}</span></a>{{/media}}</main>";

/**
 * @brief Writes one JPEG, creating its parent directories, as a fixture.
 *
 * The fixture encodes its own source rather than committing a binary file, so no test needs a
 * checked-in image and none needs `ffmpeg`.
 *
 * @param os        Host to write to. Must not be `NULL`.
 * @param file_path Terminated path of the JPEG. Must not be `NULL`.
 */
static void write_jpeg(struct Os* os, const char* file_path) {
  struct Arena arena = test_arena();
  struct Str jpeg = {0};
  if (TEST_CHECK(test_jpeg_encode(24, 12, &arena, &jpeg) == 0)) {
    os_memory_write_bytes(os, str_from_cstr(file_path), jpeg);
  }
}

/**
 * @brief Writes `fram.toml` and the album template.
 *
 * @param os           Host to write to. Must not be `NULL`.
 * @param config_extra Config lines placed before the shared body, or `NULL` for the body alone. It
 *                     must not repeat a key the body sets, because TOML rejects a duplicate key.
 */
static void write_gallery_fixture(struct Os* os, const char* config_extra) {
  os_memory_write(os, "templates/album.html", ALBUM_TEMPLATE);
  struct Arena arena = test_arena();
  struct Buf config = {.arena = &arena};
  if (config_extra != NULL) {
    buf_append(&config, str_from_cstr(config_extra));
  }
  buf_append(&config, str_from_cstr(SITE_CONFIG));
  os_memory_write(os, "fram.toml", buf_cstr(&config));
}

/** Files `every_output_kind_fixture` builds to, in byte order. */
static const char* const EVERY_OUTPUT_KIND_FILES[] = {
    "public/_fram/originals/album/one-jpg.jpg",
    "public/_fram/output-cache.tsv",
    "public/_fram/v2-s-8x8c-q70-m-16x16-q75-f1000/album/one-jpg-m.webp",
    "public/_fram/v2-s-8x8c-q70-m-16x16-q75-f1000/album/one-jpg-s.webp",
    "public/album/index.html",
    "public/fram.css",
    "public/index.html",
    "public/sitemap.xml",
};

/** Number of paths in `EVERY_OUTPUT_KIND_FILES`. */
enum {
  EVERY_OUTPUT_KIND_FILE_COUNT = sizeof(EVERY_OUTPUT_KIND_FILES) /
  sizeof(EVERY_OUTPUT_KIND_FILES[0])
};

/**
 * @brief Returns a host holding a gallery with one output of every kind: album pages, derivatives,
 *        an original, an aggregate and a static file, beside a file in `media_dir` that is not
 *        media, which build to `EVERY_OUTPUT_KIND_FILES`.
 *
 * @return The host.
 */
static struct Os* every_output_kind_fixture(void) {
  struct Os* os = os_memory();
  write_gallery_fixture(os, "templates.aggregates = [\"sitemap.xml\"]\n");
  os_memory_write(os, "templates/sitemap.xml", "sitemap\n");
  write_jpeg(os, "photos/album/one.jpg");
  os_memory_write(os, "photos/album/notes.txt", "not media\n");
  os_memory_write(os, "static/fram.css", "body{}\n");
  return os;
}

/**
 * @brief Executes a build with the given options in test arenas and collects its errors.
 *
 * @param options Build options passed to `cmd_build_execute`, whose worker count `0` stands for the
 *                CPU count, as on the command line. Must not be `NULL`.
 * @param os      Host the build runs against. Must not be `NULL`.
 * @param log     Log that receives every build error. Its arena must be none of the arenas the
 *                build allocates from, as `cmd_build_execute` requires. Must not be `NULL`.
 * @return `0` on success, or `-1` on build failure.
 */
static int execute_build_with_options(const struct BuildOptions* options,
                                      struct Os* os,
                                      struct Log* log) {
  struct BuildOptions resolved = *options;
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
  const struct BuildOptions options = {.config_path = SITE_CONFIG_PATH_DEFAULT};
  return execute_build_with_options(&options, os, log);
}

/**
 * @brief Executes a dry run with otherwise default options and collects its errors.
 *
 * @param os  Host the dry run runs against. Must not be `NULL`.
 * @param log Log that receives every build error. Must not be `NULL`.
 * @return `0` on success, or `-1` on build failure.
 */
static int execute_dry_run(struct Os* os, struct Log* log) {
  const struct BuildOptions options = {.config_path = SITE_CONFIG_PATH_DEFAULT, .is_dry_run = true};
  return execute_build_with_options(&options, os, log);
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
 * @param real_os    Host a real build of the same gallery wrote. Must not be `NULL`.
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

/** Path of the output cache below `public`. */
static const char* const OUTPUT_CACHE_FILE = "public/_fram/output-cache.tsv";

/** Directory below `public` of the derivatives that `SITE_CONFIG` configures. */
#define DERIVATIVE_DIR "_fram/v2-s-8x8c-q70-m-16x16-q75-f1000"

/**
 * @brief Appends the output cache's row of `public/<output_name>`, made from
 *        `<source_root>/<source_name>`, with the sizes and the time that `os_stat` reports now.
 *
 * A derivative's attributes also name the codecs' versions, which a copied original's do not.
 *
 * @param text          Text that receives the row. Must not be `NULL`.
 * @param os            Host to inspect. Must not be `NULL`.
 * @param output_name   Terminated path of the output below `public`. Must not be `NULL`.
 * @param source_root   Terminated root the source is below, such as `photos`. Must not be `NULL`.
 * @param source_name   Terminated path of the source below `source_root`. Must not be `NULL`.
 * @param is_derivative Whether the output is a derivative rather than a copied original.
 */
static void append_cache_row(struct Buf* text,
                             struct Os* os,
                             const char* output_name,
                             const char* source_root,
                             const char* source_name,
                             bool is_derivative) {
  struct Arena arena = test_arena();
  struct OsStat source = {0};
  struct OsStat output = {0};
  TEST_CHECK(os_stat(os, path_join(str_from_cstr(source_root), str_from_cstr(source_name), &arena),
                     &source) == 0);
  TEST_CHECK(os_stat(os, path_join(S("public"), str_from_cstr(output_name), &arena), &output) == 0);
  const struct Str versions =
      is_derivative ? str_concat(S(" "), image_codec_versions(&arena), &arena) : (struct Str){0};
  buf_printf(text, "output\t%s\tsrc=%s len=%td mtime=%lld.%09lld%.*s\t%td\n", output_name,
             source_name, source.len, (long long)source.mtime_s, (long long)source.mtime_ns,
             STR_FMT(versions), output.len);
}

/**
 * @brief Appends the output cache's probe fact of `photos/<source_name>`, whose probe read `probe`,
 *        with the size and the time that `os_stat` reports now.
 *
 * @param text        Text that receives the fact. Must not be `NULL`.
 * @param os          Host to inspect. Must not be `NULL`.
 * @param source_name Terminated path of the source below `photos`. Must not be `NULL`.
 * @param probe       Terminated dimensions, orientation and duration, as the fact spells them. Must
 *                    not be `NULL`.
 */
static void append_cache_fact(struct Buf* text,
                              struct Os* os,
                              const char* source_name,
                              const char* probe) {
  struct Arena arena = test_arena();
  struct OsStat source = {0};
  TEST_CHECK(os_stat(os, path_join(S("photos"), str_from_cstr(source_name), &arena), &source) == 0);
  buf_printf(text, "fact\t%s\tversion=%.*s len=%td mtime=%lld.%09lld %s\n", source_name,
             STR_FMT(fram_version_string()), source.len, (long long)source.mtime_s,
             (long long)source.mtime_ns, probe);
}

/**
 * @brief Appends the output cache's rows of the original and the derivatives of
 *        `photos/album/<stem>.jpg`, in the cache's order.
 *
 * @param text Text that receives the rows. Must not be `NULL`.
 * @param os   Host to inspect. Must not be `NULL`.
 * @param stem Terminated name of the photo without `.jpg`. Must not be `NULL`.
 */
static void append_cache_photo_rows(struct Buf* text, struct Os* os, const char* stem) {
  struct Arena arena = test_arena();
  struct Buf name = {.arena = &arena};
  buf_printf(&name, "album/%s.jpg", stem);
  struct Buf original = {.arena = &arena};
  buf_printf(&original, "_fram/originals/album/%s-jpg.jpg", stem);
  struct Buf medium = {.arena = &arena};
  buf_printf(&medium, DERIVATIVE_DIR "/album/%s-jpg-m.webp", stem);
  struct Buf small = {.arena = &arena};
  buf_printf(&small, DERIVATIVE_DIR "/album/%s-jpg-s.webp", stem);
  const char* source_name = buf_cstr(&name);
  append_cache_row(text, os, buf_cstr(&original), "photos", source_name, false);
  append_cache_row(text, os, buf_cstr(&medium), "photos", source_name, true);
  append_cache_row(text, os, buf_cstr(&small), "photos", source_name, true);
}

/**
 * @brief Reports whether two files of `os` carry the same modification time.
 *
 * @param os     Host to inspect. Must not be `NULL`.
 * @param path   Terminated path of the file now. Must not be `NULL`.
 * @param before What `os_stat` reported for it earlier. Must not be `NULL`.
 * @return `true` when the file exists and the seconds and the nanoseconds agree.
 */
static bool is_untouched(struct Os* os, const char* path, const struct OsStat* before) {
  struct OsStat now;
  return os_stat(os, str_from_cstr(path), &now) == 0 && now.mtime_s == before->mtime_s &&
         now.mtime_ns == before->mtime_ns;
}

/**
 * @brief Returns the contents of the regular file at `file_path`, copied into `arena`, or an empty
 *        string when there is none.
 *
 * @param os        Host to read. Must not be `NULL`.
 * @param file_path Terminated path of the file. Must not be `NULL`.
 * @param arena     Arena that owns the copy. Must not be `NULL`.
 * @return The contents.
 */
static struct Str file_contents(struct Os* os, const char* file_path, struct Arena* arena) {
  struct Str data = {0};
  if (os_memory_file(os, file_path, &data) != 0) {
    return (struct Str){0};
  }
  return str_clone(data, arena);
}

/** Every file below one directory and its contents, as `snapshot_files` took them. */
struct FileSnapshot {
  /** Sorted paths of the files. */
  struct PathList paths;

  /** Contents of each file, parallel to `paths`. */
  struct Str* contents;
};

/**
 * @brief Records every file below `dir_path` and its contents.
 *
 * @param os       Host to read. Must not be `NULL`.
 * @param dir_path Terminated directory to read. Must not be `NULL`.
 * @param arena    Arena that owns the copies. Must not be `NULL`.
 * @return The snapshot, owned by `arena`.
 */
static struct FileSnapshot snapshot_files(struct Os* os,
                                          const char* dir_path,
                                          struct Arena* arena) {
  struct FileSnapshot snapshot = {.paths = os_memory_files(os, dir_path)};
  snapshot.contents = arena_new(arena, snapshot.paths.count, struct Str);
  for (ptrdiff_t i = 0; i < snapshot.paths.count; i++) {
    snapshot.contents[i] = file_contents(os, str_cstr(snapshot.paths.items[i], arena), arena);
  }
  return snapshot;
}

/**
 * @brief Reports whether the files below `dir_path` are exactly those of `snapshot`, byte for byte,
 *        naming the first that differs when they are not.
 *
 * @param os       Host to read. Must not be `NULL`.
 * @param dir_path Terminated directory to read. Must not be `NULL`.
 * @param snapshot Files the directory must hold. Must not be `NULL`.
 * @return `true` when every path and every byte match.
 */
static bool is_snapshot(struct Os* os, const char* dir_path, const struct FileSnapshot* snapshot) {
  struct Arena arena = test_arena();
  const struct FileSnapshot now = snapshot_files(os, dir_path, &arena);
  bool is_equal = now.paths.count == snapshot->paths.count;
  for (ptrdiff_t i = 0; is_equal && i < now.paths.count; i++) {
    is_equal = str_eq(now.paths.items[i], snapshot->paths.items[i]) &&
               str_eq(now.contents[i], snapshot->contents[i]);
    if (!is_equal) {
      TEST_MSG("differs: '%.*s'", STR_FMT(now.paths.items[i]));
    }
  }
  TEST_CHECK_(is_equal, "'%s' holds the same files with the same bytes", dir_path);
  return is_equal;
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

// A configured album template name is used instead of the default.
static void test_honors_configured_album_template(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, "templates.album = \"album-page.html\"\n");
  write_jpeg(os, "photos/one.jpg");
  os_memory_write(os, "templates/album-page.html", "<main>{{album.title}}</main>\n");

  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));

  TEST_CHECK(os_memory_is_file(os, "public/index.html", "<main>Test Gallery</main>\n"));
}

// A build writes exactly the manifest's intended outputs: album pages, derivatives, originals,
// aggregate outputs and static files, and nothing for a file in `media_dir` that is not media.
static void test_writes_exactly_manifest_outputs(void) {
  struct Os* os = every_output_kind_fixture();
  struct Log log = test_log();

  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  // Exactly the expected set, so neither an extra output nor the non-media file passes.
  TEST_CHECK(is_file_list(os, "public", EVERY_OUTPUT_KIND_FILES, EVERY_OUTPUT_KIND_FILE_COUNT));
}

// A trailing slash on `media_dir` does not leak the directory name into the output tree. The prefix
// strip that places each media item in its album compares `<media_dir>/`, so an unnormalized
// `"photos/"` would miss and publish `public/photos/album/index.html`.
static void test_tolerates_trailing_slash_on_media_dir(void) {
  struct Os* os = os_memory();
  // The shared config sets `media_dir`, so this fixture writes its own.
  os_memory_write(os, "fram.toml",
                  "title = \"Test Gallery\"\n"
                  "description = \"Test description\"\n"
                  "author = \"Tests\"\n"
                  "media_dir = \"photos/\"\n"
                  "[derivatives.s]\nwidth = 8\nheight = 8\nquality = 70\ncrop = true\n"
                  "[derivatives.m]\nwidth = 16\nheight = 16\nquality = 75\n");
  os_memory_write(os, "templates/album.html", ALBUM_TEMPLATE);
  write_jpeg(os, "photos/album/one.jpg");
  struct Log log = test_log();

  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  // No output, page, derivative or original, carries the name of `media_dir`.
  static const char* const expected[] = {
      "public/_fram/originals/album/one-jpg.jpg",
      "public/_fram/output-cache.tsv",
      "public/_fram/v2-s-8x8c-q70-m-16x16-q75-f1000/album/one-jpg-m.webp",
      "public/_fram/v2-s-8x8c-q70-m-16x16-q75-f1000/album/one-jpg-s.webp",
      "public/album/index.html",
      "public/index.html",
  };
  TEST_CHECK(is_file_list(os, "public", expected, sizeof(expected) / sizeof(expected[0])));
}

// A config in a subdirectory resolves every directory against that subdirectory, the defaults
// included, so `sub/fram.toml` reads `sub/photos`, `sub/templates` and `sub/static`, and writes
// `sub/public`, and nothing lands beside `sub`.
static void test_builds_from_config_in_a_subdirectory(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "sub/fram.toml", SITE_CONFIG);
  os_memory_write(os, "sub/templates/album.html", ALBUM_TEMPLATE);
  os_memory_write(os, "sub/static/fram.css", "body{}\n");
  write_jpeg(os, "sub/photos/album/one.jpg");
  struct Log log = test_log();
  const struct BuildOptions options = {.config_path = S("sub/fram.toml")};

  TEST_CHECK(execute_build_with_options(&options, os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  static const char* const expected[] = {
      "sub/public/_fram/originals/album/one-jpg.jpg",
      "sub/public/_fram/output-cache.tsv",
      "sub/public/_fram/v2-s-8x8c-q70-m-16x16-q75-f1000/album/one-jpg-m.webp",
      "sub/public/_fram/v2-s-8x8c-q70-m-16x16-q75-f1000/album/one-jpg-s.webp",
      "sub/public/album/index.html",
      "sub/public/fram.css",
      "sub/public/index.html",
  };
  TEST_CHECK(is_file_list(os, "sub/public", expected, sizeof(expected) / sizeof(expected[0])));
  TEST_CHECK(!os_memory_has_path(os, "public"));
}

// A gallery that ships no `static` directory builds. `static_dir` is optional with a default, so an
// absent one is skipped rather than reported.
static void test_builds_with_no_static_dir(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  TEST_CHECK(!os_memory_has_path(os, "static"));

  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_has_path(os, "public/index.html"));
  TEST_CHECK(os_memory_has_path(os, "public/album/index.html"));
  TEST_CHECK(os_memory_has_path(os, "public/_fram/originals/album/one-jpg.jpg"));
}

// A `static` directory is mirrored into `output_dir` as-is, so a top-level file lands at the output
// root and a nested one keeps its directories.
static void test_copies_static_files_when_present(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  os_memory_write(os, "static/robots.txt", "User-agent: *\n");
  os_memory_write(os, "static/assets/fram.css", "body{}\n");

  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_has_path(os, "public/robots.txt"));
  TEST_CHECK(os_memory_has_path(os, "public/assets/fram.css"));
}

// Same-named sources in different directories publish to distinct, non-colliding outputs. One file
// name repeated in three nested albums is not a collision, because the album slug is part of every
// output path. Each directory level still gets its own album page.
static void test_distinguishes_same_name_in_different_dirs(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/one.jpg");
  write_jpeg(os, "photos/trip/one.jpg");
  write_jpeg(os, "photos/trip/deep/one.jpg");

  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_has_path(os, "public/trip/index.html"));
  TEST_CHECK(os_memory_has_path(os, "public/trip/deep/index.html"));
  TEST_CHECK(os_memory_has_path(os, "public/_fram/originals/one-jpg.jpg"));
  TEST_CHECK(os_memory_has_path(os, "public/_fram/originals/trip/one-jpg.jpg"));
  TEST_CHECK(os_memory_has_path(os, "public/_fram/originals/trip/deep/one-jpg.jpg"));
}

// An album holding only sub-directories has no media of its own. Its own page is still written, and
// the zero-length media and sub-album arrays it allocates are valid rather than a failure.
static void test_builds_an_album_with_no_media_of_its_own(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/container/inner/one.jpg");

  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_has_path(os, "public/index.html"));
  TEST_CHECK(os_memory_has_path(os, "public/container/index.html"));
  TEST_CHECK(os_memory_has_path(os, "public/container/inner/index.html"));
}

// The configured sort orders reach the rendered pages. With both descending, the root page lists
// its media and its sub-albums last name first, and each sub-album's cover is its last file by
// name, the first it lists.
static void test_renders_configured_sort_orders(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os,
                        "templates.album = \"sorted.html\"\n"
                        "sort = { albums = \"desc\", media = \"desc\" }\n");
  write_jpeg(os, "photos/a.jpg");
  write_jpeg(os, "photos/b.jpg");
  write_jpeg(os, "photos/x/1.jpg");
  write_jpeg(os, "photos/x/2.jpg");
  write_jpeg(os, "photos/y/1.jpg");
  write_jpeg(os, "photos/y/2.jpg");
  os_memory_write(os, "templates/sorted.html",
                  "{{#media}}{{title}} {{/media}}|"
                  "{{#sub_albums}} {{title}}:{{cover.title}}{{/sub_albums}}\n");

  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));

  TEST_CHECK(os_memory_is_file(os, "public/index.html", "b.jpg a.jpg | y:2.jpg x:2.jpg\n"));
  TEST_CHECK(os_memory_is_file(os, "public/x/index.html", "2.jpg 1.jpg |\n"));
}

// A second build over a finished output tree succeeds and produces the same set of outputs.
static void test_rebuild_succeeds_and_repeats_its_outputs(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");

  TEST_CHECK(execute_build(os, &log) == 0);
  const ptrdiff_t public_count_built = os_memory_files(os, "public").count;
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  // The album page, the root page, both derivatives, the copied original and the output cache: the
  // second build adds nothing and removes nothing.
  TEST_CHECK(public_count_built == 6);
  TEST_CHECK(os_memory_files(os, "public").count == public_count_built);
}

// A rebuild removes what the current inputs no longer produce: the outputs of a deleted source, an
// old recipe directory, and a file added to `output_dir` by hand. Every current output stays, and a
// third build finds nothing more to remove.
static void test_rebuild_removes_stale_outputs(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  write_jpeg(os, "photos/album/two.jpg");
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(os_memory_has_path(os, "public/_fram/originals/album/two-jpg.jpg"));

  TEST_CHECK(os_remove(os, S("photos/album/two.jpg"), S("failed to remove fixture"), &log) == 0);
  os_memory_write(os, "public/notes.txt", "hand");
  os_memory_write(os, "public/_fram/v1-old/album/one-jpg-s.jpg", "old");
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  // The album page, the root page, both derivatives and the copied original of the one source left,
  // and the output cache.
  TEST_CHECK(os_memory_files(os, "public").count == 6);
  TEST_CHECK(os_memory_has_path(os, "public/_fram/originals/album/one-jpg.jpg"));
  TEST_CHECK(!os_memory_has_path(os, "public/_fram/originals/album/two-jpg.jpg"));
  TEST_CHECK(!os_memory_has_path(os, "public/notes.txt"));
  TEST_CHECK(!os_memory_has_path(os, "public/_fram/v1-old"));

  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_files(os, "public").count == 6);
}

// An `output_dir` that a symlink inside `media_dir` reaches is left out of the media walk, so a
// rebuild does not read the first build's outputs back as sources. The derivatives here are JPEG
// files, which the walk would otherwise discover as media.
static void test_rebuild_skips_output_dir_linked_from_media_dir(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  os_memory_symlink(os, "../public", "photos/published");

  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  // The album page, the root page, both derivatives, the copied original and the output cache, and
  // no album for the linked output tree.
  TEST_CHECK(os_memory_files(os, "public").count == 6);
  TEST_CHECK(!os_memory_has_path(os, "public/published"));
}

// A dry run of a gallery with every kind of output succeeds and creates nothing, not even
// `output_dir`, which shows `prepare_output_dir` was withheld. Its `write` lines name exactly the
// files a real build of the same gallery writes, so no site left a path pending.
static void test_dry_run_lists_every_output_and_writes_nothing(void) {
  struct Os* real_os = every_output_kind_fixture();
  struct Os* dry_run_os = every_output_kind_fixture();
  struct Log log = test_log();

  TEST_CHECK(execute_build(real_os, &log) == 0);
  TEST_CHECK(execute_dry_run(dry_run_os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(!os_memory_has_path(dry_run_os, "public"));
  TEST_CHECK(is_write_set_of(dry_run_os, real_os));
}

// After a build, a dry run lists every output as `keep`, in manifest order: a derivative and the
// original because the cache vouches for them, and a page because its bytes are unchanged. Only the
// output cache, which every build writes, is listed as `write`.
static void test_dry_run_keeps_unchanged_outputs(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  TEST_CHECK(execute_build(os, &log) == 0);

  TEST_CHECK(execute_dry_run(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_printed(
      os, 1,
      "keep\tpublic/index.html\n"
      "keep\tpublic/album/index.html\n"
      "keep\tpublic/_fram/v2-s-8x8c-q70-m-16x16-q75-f1000/album/one-jpg-s.webp\n"
      "keep\tpublic/_fram/v2-s-8x8c-q70-m-16x16-q75-f1000/album/one-jpg-m.webp\n"
      "keep\tpublic/_fram/originals/album/one-jpg.jpg\n"
      "write\tpublic/_fram/output-cache.tsv\n"));
}

// After a build, a changed `[derivatives.*]` table makes a dry run list the new derivatives as
// `write` and the old recipe directory as `remove`, sorted, with everything in it. The old
// directory is still there afterward.
static void test_dry_run_lists_stale_recipe_and_keeps_it(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  TEST_CHECK(execute_build(os, &log) == 0);
  os_memory_write(os, "fram.toml",
                  "title = \"Test Gallery\"\n"
                  "description = \"Test description\"\n"
                  "author = \"Tests\"\n"
                  "media_dir = \"photos\"\n"
                  "[derivatives.s]\nwidth = 4\nheight = 4\nquality = 70\ncrop = true\n"
                  "[derivatives.m]\nwidth = 16\nheight = 16\nquality = 75\n");

  TEST_CHECK(execute_dry_run(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_printed(
      os, 1,
      "keep\tpublic/index.html\n"
      "keep\tpublic/album/index.html\n"
      "write\tpublic/_fram/v2-s-4x4c-q70-m-16x16-q75-f1000/album/one-jpg-s.webp\n"
      "write\tpublic/_fram/v2-s-4x4c-q70-m-16x16-q75-f1000/album/one-jpg-m.webp\n"
      "keep\tpublic/_fram/originals/album/one-jpg.jpg\n"
      "write\tpublic/_fram/output-cache.tsv\n"
      "remove\tpublic/_fram/v2-s-8x8c-q70-m-16x16-q75-f1000\n"
      "remove\tpublic/_fram/v2-s-8x8c-q70-m-16x16-q75-f1000/album\n"
      "remove\tpublic/_fram/v2-s-8x8c-q70-m-16x16-q75-f1000/album/one-jpg-m.webp\n"
      "remove\tpublic/_fram/v2-s-8x8c-q70-m-16x16-q75-f1000/album/one-jpg-s.webp\n"));
  TEST_CHECK(
      os_memory_has_path(os, "public/_fram/v2-s-8x8c-q70-m-16x16-q75-f1000/album/one-jpg-s.webp"));
  TEST_CHECK(!os_memory_has_path(os, "public/_fram/v2-s-4x4c-q70-m-16x16-q75-f1000"));
}

// After a build, a dry run keeps a static file whose source is unchanged, and writes one whose size
// or modification time changed, so a rebuild rewrites only what changed.
static void test_dry_run_keeps_unchanged_static_files(void) {
  struct Os* os = every_output_kind_fixture();
  os_memory_write(os, "static/robots.txt", "User-agent: *\n");
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  os_memory_touch(os, "static/robots.txt");

  TEST_CHECK(execute_dry_run(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_printed(
      os, 1,
      "keep\tpublic/index.html\n"
      "keep\tpublic/album/index.html\n"
      "keep\tpublic/_fram/v2-s-8x8c-q70-m-16x16-q75-f1000/album/one-jpg-s.webp\n"
      "keep\tpublic/_fram/v2-s-8x8c-q70-m-16x16-q75-f1000/album/one-jpg-m.webp\n"
      "keep\tpublic/_fram/originals/album/one-jpg.jpg\n"
      "keep\tpublic/sitemap.xml\n"
      "keep\tpublic/fram.css\n"
      "write\tpublic/robots.txt\n"
      "write\tpublic/_fram/output-cache.tsv\n"));
}

// A build that fails after it copied the static files still records each copy it made. A source
// edited to the same size is copied before the render fails, so the next build keeps a copy that
// matches its source rather than one the cache vouches for but the build never made.
static void test_failed_render_keeps_the_static_copies_it_made(void) {
  struct Os* os = every_output_kind_fixture();
  os_memory_write(os, "static/robots.txt", "User-agent: *\n");
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  os_memory_write(os, "static/robots.txt", "User-agent: a\n");
  os_memory_write(os, "templates/album.html", "{{{album.title}}}");
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(
      test_log_is(&log,
                  "unescaped Mustache interpolation is supported only for a key that ends "
                  "in '_html' (in 'album.html') (while rendering 'index.html')\n\xE2\x80\xA6 "
                  "and 1 more failure\n"));

  os_memory_write(os, "templates/album.html", ALBUM_TEMPLATE);
  log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(os_memory_is_file(os, "public/robots.txt", "User-agent: a\n"));
}

// A build that fails after the probe still writes the output cache, here where a page fails to
// render after every derivative and original was kept. Every probe fact and every row stay, so the
// next build neither probes nor reads a media source again.
static void test_failed_build_keeps_the_probe_facts(void) {
  struct Os* os = os_memory();
  struct Arena arena = test_arena();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  TEST_CHECK(execute_build(os, &log) == 0);
  os_memory_write(os, "templates/album.html", "{{{album.title}}}");
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(
      test_log_is(&log,
                  "unescaped Mustache interpolation is supported only for a key that ends "
                  "in '_html' (in 'album.html') (while rendering 'index.html')\n\xE2\x80\xA6 "
                  "and 1 more failure\n"));
  struct Buf cache = {.arena = &arena};
  buf_append(&cache, S("output cache v1\n"));
  append_cache_photo_rows(&cache, os, "one");
  append_cache_fact(&cache, os, "album/one.jpg", "width=24 height=12 orientation=1 duration_ms=0");
  TEST_CHECK(os_memory_is_file(os, OUTPUT_CACHE_FILE, buf_cstr(&cache)));

  os_memory_write(os, "templates/album.html", ALBUM_TEMPLATE);
  os_memory_fail(os, OS_MEMORY_OP_READ, "photos", EACCES);
  log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
}

// A copy whose write failed gets no row in the output cache, so the next build copies it again,
// while the derivatives the failed build made keep theirs.
static void test_failed_copy_gets_no_row(void) {
  struct Os* os = os_memory();
  struct Arena arena = test_arena();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  TEST_CHECK(execute_build(os, &log) == 0);
  struct Str jpeg = {0};
  TEST_ASSERT(test_jpeg_encode(12, 24, &arena, &jpeg) == 0);
  os_memory_write_bytes(os, S("photos/album/one.jpg"), jpeg);
  os_memory_fail(os, OS_MEMORY_OP_WRITE_DATA, "public/_fram/originals/album/one-jpg.jpg", ENOSPC);

  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(
      &log, "failed to copy original: %s ('public/_fram/originals/album/one-jpg.jpg')\n",
      strerror(ENOSPC)));
  struct Buf cache = {.arena = &arena};
  buf_append(&cache, S("output cache v1\n"));
  append_cache_row(&cache, os, DERIVATIVE_DIR "/album/one-jpg-m.webp", "photos", "album/one.jpg",
                   true);
  append_cache_row(&cache, os, DERIVATIVE_DIR "/album/one-jpg-s.webp", "photos", "album/one.jpg",
                   true);
  append_cache_fact(&cache, os, "album/one.jpg", "width=12 height=24 orientation=1 duration_ms=0");
  TEST_CHECK(os_memory_is_file(os, OUTPUT_CACHE_FILE, buf_cstr(&cache)));

  os_memory_fail(os, OS_MEMORY_OP_WRITE_DATA, "public/_fram/originals/album/one-jpg.jpg", 0);
  log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(str_eq(file_contents(os, "public/_fram/originals/album/one-jpg.jpg", &arena), jpeg));
}

// A static copy whose write failed gets no row, so the next build copies it again, and the probe
// facts stay.
static void test_failed_static_copy_gets_no_row(void) {
  struct Os* os = every_output_kind_fixture();
  struct Arena arena = test_arena();
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  os_memory_write(os, "static/fram.css", "body{x}\n");
  os_memory_fail(os, OS_MEMORY_OP_READ, "static/fram.css", EACCES);

  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(
      test_log_is(&log, "failed to copy static file: %s ('static/fram.css')\n", strerror(EACCES)));
  // The static files are copied first, so the failure stops the build before any derivative.
  struct Buf cache = {.arena = &arena};
  buf_append(&cache, S("output cache v1\n"));
  append_cache_fact(&cache, os, "album/one.jpg", "width=24 height=12 orientation=1 duration_ms=0");
  TEST_CHECK(os_memory_is_file(os, OUTPUT_CACHE_FILE, buf_cstr(&cache)));

  os_memory_fail(os, OS_MEMORY_OP_READ, "static/fram.css", 0);
  log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(os, "public/fram.css", "body{x}\n"));
}

// An interrupted build leaves the output cache of the build before it, here beside a static copy
// that it cut short. The next build takes every probe fact from that cache and keeps every
// derivative and original, so it reads no media source, and copies the cut file again, which leaves
// every output, the output cache included, byte for byte as the first build wrote it.
static void test_interrupted_build_keeps_the_probe_facts(void) {
  struct Os* os = every_output_kind_fixture();
  struct Arena arena = test_arena();
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  const struct FileSnapshot built = snapshot_files(os, "public", &arena);
  os_memory_write(os, "public/fram.css", "bod");
  os_memory_fail(os, OS_MEMORY_OP_READ, "photos", EACCES);

  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(is_snapshot(os, "public", &built));
}

// A dry run over an existing tree, with a new photo to write and a stale file to remove, where
// every write and every removal below `output_dir` would fail, still succeeds, so it attempts
// neither.
static void test_dry_run_attempts_no_change(void) {
  struct Os* os = every_output_kind_fixture();
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  os_memory_write(os, "public/notes.txt", "hand");
  write_jpeg(os, "photos/album/two.jpg");
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
  TEST_CHECK(os_memory_is_file(os, "public/sitemap.xml", "sitemap\n"));
}

// A build after an unchanged build takes every source's probe facts from the output cache and keeps
// every derivative and original, so it reads no source, here where every read of one would fail,
// and it leaves every output, the output cache included, byte for byte as the first build wrote it.
static void test_warm_build_reads_no_source_and_repeats_its_bytes(void) {
  struct Os* os = os_memory();
  struct Arena arena = test_arena();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  write_jpeg(os, "photos/two.jpg");
  TEST_CHECK(execute_build(os, &log) == 0);
  const struct FileSnapshot built = snapshot_files(os, "public", &arena);
  os_memory_fail(os, OS_MEMORY_OP_READ, "photos", EACCES);

  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(is_snapshot(os, "public", &built));
}

// `--force` ignores the output cache for one build, so it probes every source, here where the read
// of the one source fails.
static void test_force_probes_every_source(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  TEST_CHECK(execute_build(os, &log) == 0);
  os_memory_fail(os, OS_MEMORY_OP_READ, "photos", EACCES);

  const struct BuildOptions options = {.config_path = SITE_CONFIG_PATH_DEFAULT, .is_forced = true};
  TEST_CHECK(execute_build_with_options(&options, os, &log) == -1);
  TEST_CHECK(
      test_log_is(&log, "failed to probe media: %s ('photos/album/one.jpg')\n", strerror(EACCES)));
}

// `--force` keeps nothing, so a forced dry run after a build lists every output as `write`, and a
// forced build writes every output again, an unchanged page included.
static void test_force_writes_every_output(void) {
  struct Os* os = every_output_kind_fixture();
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == 0);
  struct OsStat page_before;
  struct OsStat derivative_before;
  TEST_ASSERT(os_stat(os, S("public/index.html"), &page_before) == 0);
  TEST_ASSERT(os_stat(os, S("public/_fram/v2-s-8x8c-q70-m-16x16-q75-f1000/album/one-jpg-s.webp"),
                      &derivative_before) == 0);

  const struct BuildOptions dry_run = {
      .config_path = SITE_CONFIG_PATH_DEFAULT, .is_dry_run = true, .is_forced = true};
  TEST_CHECK(execute_build_with_options(&dry_run, os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(is_write_set_of(os, os));
  const struct BuildOptions forced = {.config_path = SITE_CONFIG_PATH_DEFAULT, .is_forced = true};
  TEST_CHECK(execute_build_with_options(&forced, os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(!is_untouched(os, "public/index.html", &page_before));
  TEST_CHECK(!is_untouched(os, "public/_fram/v2-s-8x8c-q70-m-16x16-q75-f1000/album/one-jpg-s.webp",
                           &derivative_before));
}

// A second build leaves every output untouched: a derivative and the original because the cache
// vouches for them, and a page because the bytes it renders are those already there. A page whose
// bytes change is written.
static void test_rebuild_leaves_unchanged_outputs_untouched(void) {
  struct Os* os = every_output_kind_fixture();
  struct Log log = test_log();
  write_jpeg(os, "photos/two.jpg");
  TEST_CHECK(execute_build(os, &log) == 0);
  static const char* const paths[] = {
      "public/index.html",
      "public/album/index.html",
      "public/sitemap.xml",
      "public/fram.css",
      "public/_fram/originals/album/one-jpg.jpg",
      "public/_fram/v2-s-8x8c-q70-m-16x16-q75-f1000/album/one-jpg-s.webp",
  };
  enum { PATH_COUNT = sizeof(paths) / sizeof(paths[0]) };
  struct OsStat before[PATH_COUNT];
  for (ptrdiff_t i = 0; i < PATH_COUNT; i++) {
    TEST_ASSERT(os_stat(os, str_from_cstr(paths[i]), &before[i]) == 0);
  }

  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  for (ptrdiff_t i = 0; i < PATH_COUNT; i++) {
    TEST_CHECK_(is_untouched(os, paths[i], &before[i]), "'%s' is untouched", paths[i]);
  }
  // The root page lists its own media, so a renamed title changes it, and no other page.
  TEST_CHECK(os_remove(os, S("photos/two.jpg"), S("failed to remove fixture"), &log) == 0);
  write_jpeg(os, "photos/three.jpg");
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(!is_untouched(os, "public/index.html", &before[0]));
  TEST_CHECK(is_untouched(os, "public/album/index.html", &before[1]));
}

// A source that changed makes its derivatives and its copy again, from the new bytes, and probes it
// again, so its probe fact names the new size. A source that did not change keeps its outputs.
static void test_changed_source_remakes_its_outputs(void) {
  struct Os* os = os_memory();
  struct Arena arena = test_arena();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  write_jpeg(os, "photos/album/two.jpg");
  TEST_CHECK(execute_build(os, &log) == 0);
  const char* const one_s = "public/_fram/v2-s-8x8c-q70-m-16x16-q75-f1000/album/one-jpg-s.webp";
  const char* const two_s = "public/_fram/v2-s-8x8c-q70-m-16x16-q75-f1000/album/two-jpg-s.webp";
  const struct Str one_before = file_contents(os, one_s, &arena);
  struct OsStat two_before;
  TEST_ASSERT(os_stat(os, str_from_cstr(two_s), &two_before) == 0);
  struct Str jpeg = {0};
  TEST_ASSERT(test_jpeg_encode(12, 24, &arena, &jpeg) == 0);
  os_memory_write_bytes(os, S("photos/album/one.jpg"), jpeg);

  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(!str_eq(file_contents(os, one_s, &arena), one_before));
  TEST_CHECK(str_eq(file_contents(os, "public/_fram/originals/album/one-jpg.jpg", &arena), jpeg));
  TEST_CHECK(is_untouched(os, two_s, &two_before));
  struct Buf cache = {.arena = &arena};
  buf_append(&cache, S("output cache v1\n"));
  append_cache_row(&cache, os, "_fram/originals/album/one-jpg.jpg", "photos", "album/one.jpg",
                   false);
  append_cache_row(&cache, os, "_fram/originals/album/two-jpg.jpg", "photos", "album/two.jpg",
                   false);
  append_cache_row(&cache, os, DERIVATIVE_DIR "/album/one-jpg-m.webp", "photos", "album/one.jpg",
                   true);
  append_cache_row(&cache, os, DERIVATIVE_DIR "/album/one-jpg-s.webp", "photos", "album/one.jpg",
                   true);
  append_cache_row(&cache, os, DERIVATIVE_DIR "/album/two-jpg-m.webp", "photos", "album/two.jpg",
                   true);
  append_cache_row(&cache, os, DERIVATIVE_DIR "/album/two-jpg-s.webp", "photos", "album/two.jpg",
                   true);
  append_cache_fact(&cache, os, "album/one.jpg", "width=12 height=24 orientation=1 duration_ms=0");
  append_cache_fact(&cache, os, "album/two.jpg", "width=24 height=12 orientation=1 duration_ms=0");
  TEST_CHECK(os_memory_is_file(os, OUTPUT_CACHE_FILE, buf_cstr(&cache)));
}

// A derivative deleted from the output is made again, which a dry run lists as `write`, while the
// other outputs are kept.
static void test_deleted_derivative_is_made_again(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(os_remove(os, S("public/_fram/v2-s-8x8c-q70-m-16x16-q75-f1000/album/one-jpg-m.webp"),
                       S("failed to remove fixture"), &log) == 0);

  TEST_CHECK(execute_dry_run(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_printed(
      os, 1,
      "keep\tpublic/index.html\n"
      "keep\tpublic/album/index.html\n"
      "keep\tpublic/_fram/v2-s-8x8c-q70-m-16x16-q75-f1000/album/one-jpg-s.webp\n"
      "write\tpublic/_fram/v2-s-8x8c-q70-m-16x16-q75-f1000/album/one-jpg-m.webp\n"
      "keep\tpublic/_fram/originals/album/one-jpg.jpg\n"
      "write\tpublic/_fram/output-cache.tsv\n"));
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(
      os_memory_has_path(os, "public/_fram/v2-s-8x8c-q70-m-16x16-q75-f1000/album/one-jpg-m.webp"));
}

// A build writes the output cache with a probe fact for each current source, sorted by path, and
// none for a source deleted since.
static void test_rewrites_probe_facts_without_deleted_sources(void) {
  struct Os* os = os_memory();
  struct Arena arena = test_arena();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/c.jpg");
  write_jpeg(os, "photos/album/b.jpg");
  write_jpeg(os, "photos/album/a.jpg");
  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(os_remove(os, S("photos/album/b.jpg"), S("failed to remove fixture"), &log) == 0);

  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  struct Buf cache = {.arena = &arena};
  buf_append(&cache, S("output cache v1\n"));
  append_cache_row(&cache, os, "_fram/originals/album/a-jpg.jpg", "photos", "album/a.jpg", false);
  append_cache_row(&cache, os, "_fram/originals/album/c-jpg.jpg", "photos", "album/c.jpg", false);
  append_cache_row(&cache, os, DERIVATIVE_DIR "/album/a-jpg-m.webp", "photos", "album/a.jpg", true);
  append_cache_row(&cache, os, DERIVATIVE_DIR "/album/a-jpg-s.webp", "photos", "album/a.jpg", true);
  append_cache_row(&cache, os, DERIVATIVE_DIR "/album/c-jpg-m.webp", "photos", "album/c.jpg", true);
  append_cache_row(&cache, os, DERIVATIVE_DIR "/album/c-jpg-s.webp", "photos", "album/c.jpg", true);
  append_cache_fact(&cache, os, "album/a.jpg", "width=24 height=12 orientation=1 duration_ms=0");
  append_cache_fact(&cache, os, "album/c.jpg", "width=24 height=12 orientation=1 duration_ms=0");
  TEST_CHECK(os_memory_is_file(os, OUTPUT_CACHE_FILE, buf_cstr(&cache)));
}

// An output cache in another format is not used, so the build probes every source, and the verbose
// log says why.
static void test_ignores_output_cache_of_another_format(void) {
  struct Os* os = os_memory();
  struct Arena arena = test_arena();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  TEST_CHECK(execute_build(os, &log) == 0);
  const struct Str cache = file_contents(os, OUTPUT_CACHE_FILE, &arena);
  struct Buf other = {.arena = &arena};
  buf_append(&other, S("output cache v0\n"));
  buf_append(&other, str_cut(cache, '\n').tail);
  os_memory_write_bytes(os, str_from_cstr(OUTPUT_CACHE_FILE), buf_str(&other));
  os_memory_fail(os, OS_MEMORY_OP_READ, "photos", EACCES);

  const struct BuildOptions options = {.config_path = SITE_CONFIG_PATH_DEFAULT, .is_verbose = true};
  TEST_CHECK(execute_build_with_options(&options, os, &log) == -1);
  TEST_CHECK(
      test_log_is(&log, "failed to probe media: %s ('photos/album/one.jpg')\n", strerror(EACCES)));
  TEST_CHECK(has_printed_line(os, 2,
                              "output cache not used, probing every source: output cache is not in "
                              "the current format"));
}

// The build cache of an earlier fram release is no output of this one, so the stale-output prune
// removes it as it removes any output the build no longer makes.
static void test_prunes_the_old_build_cache(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  os_memory_write(os, "public/_fram/v1-build-cache.tsv", "an older format\n");

  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(!os_memory_has_path(os, "public/_fram/v1-build-cache.tsv"));
  TEST_CHECK(os_memory_has_path(os, OUTPUT_CACHE_FILE));
}

// A build whose probe fails stops before it writes anything, so the output cache an earlier build
// left is unchanged.
static void test_failed_probe_keeps_output_cache(void) {
  struct Os* os = os_memory();
  struct Arena arena = test_arena();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  TEST_CHECK(execute_build(os, &log) == 0);
  const struct Str cache = file_contents(os, OUTPUT_CACHE_FILE, &arena);
  TEST_CHECK(cache.len > 0);
  os_memory_write(os, "photos/album/bad.jpg", "not jpeg");

  TEST_CHECK(execute_build(os, &log) == -1);
  const struct Str errors = buf_str(&log.buf);
  TEST_CHECK(str_startswith(errors, S("failed to probe media: failed to probe JPEG: ")) &&
             str_endswith(errors, S(" ('photos/album/bad.jpg')\n")));
  TEST_MSG("log: '%.*s'", STR_FMT(errors));
  TEST_CHECK(log.errors == 1);
  TEST_CHECK(str_eq(file_contents(os, OUTPUT_CACHE_FILE, &arena), cache));
}

// A dry run uses the output cache as a build does, so it reads no source, and it leaves the output
// cache unchanged.
static void test_dry_run_uses_output_cache_and_leaves_it(void) {
  struct Os* os = os_memory();
  struct Arena arena = test_arena();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  TEST_CHECK(execute_build(os, &log) == 0);
  const struct FileSnapshot built = snapshot_files(os, "public", &arena);
  os_memory_fail(os, OS_MEMORY_OP_READ, "photos", EACCES);

  TEST_CHECK(execute_dry_run(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(is_snapshot(os, "public", &built));
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
  static const char* const photo_paths[] = {"photos/a.jpg", "photos/b.jpg", "photos/c.jpg"};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(worker_counts) / sizeof(worker_counts[0])); i++) {
    struct Os* os = os_memory();
    write_gallery_fixture(os, NULL);
    for (ptrdiff_t j = 0; j < (ptrdiff_t)(sizeof(photo_paths) / sizeof(photo_paths[0])); j++) {
      write_jpeg(os, photo_paths[j]);
    }
    const struct BuildOptions options = {.config_path = SITE_CONFIG_PATH_DEFAULT,
                                         .worker_count = worker_counts[i],
                                         .is_verbose = true};
    TEST_CHECK(cmd_build_run(&options, os) == EXIT_CODE_OK);
    TEST_CHECK(os_memory_is_printed(os, 1, ""));
    struct Arena arena = test_arena();
    struct Buf probe_line = {.arena = &arena};
    struct Buf derivative_line = {.arena = &arena};
    struct Buf album_line = {.arena = &arena};
    buf_printf(&probe_line, "probing media, workers: %td", worker_counts[i]);
    buf_printf(&derivative_line, "generating derivatives, workers: %td", worker_counts[i]);
    buf_printf(&album_line, "rendering albums, workers: %td", worker_counts[i]);
    TEST_CHECK(has_printed_line(os, 2, buf_cstr(&probe_line)));
    TEST_CHECK(has_printed_line(os, 2, buf_cstr(&derivative_line)));
    TEST_CHECK(has_printed_line(os, 2, buf_cstr(&album_line)));
    // The requested count changes only how the work is scheduled, never the output.
    TEST_CHECK(os_memory_is_file(os, "public/index.html",
                                 "<!doctype html><title>Test Gallery - Test Gallery</title><ul>"
                                 "<li>a.jpg</li><li>b.jpg</li><li>c.jpg</li></ul>"));
  }
}

// An album of many items builds. A page render's temporaries grow with the items it lists, about a
// kilobyte each.
static void test_builds_a_large_album(void) {
  enum { ITEM_COUNT = 500 };
  struct Os* os = os_memory();
  write_gallery_fixture(os, NULL);
  os_memory_write(os, "templates/album.html", LARGE_ALBUM_TEMPLATE);
  struct Arena arena = test_arena();
  struct Str jpeg = {0};
  if (!TEST_CHECK(test_jpeg_encode(2, 2, &arena, &jpeg) == 0)) {
    return;
  }
  for (int i = 0; i < ITEM_COUNT; i++) {
    char path[64];
    (void)snprintf(path, sizeof(path), "photos/big/%05d.jpg", i);
    os_memory_write_bytes(os, str_from_cstr(path), jpeg);
  }
  const struct BuildOptions options = {.config_path = SITE_CONFIG_PATH_DEFAULT, .worker_count = 1};
  struct Log log = test_log();
  TEST_CHECK(execute_build_with_options(&options, os, &log) == 0);
  TEST_MSG("errors: %s", buf_cstr(&log.buf));
}

// A gallery with no media skips the probe and derivative phases, so the verbose log carries no
// status or progress line for either. A phase with no jobs returns before it prints, so an empty
// gallery does not report a worker count for work it never starts. The album page phase still runs,
// for the root album alone.
static void test_skips_empty_phases_in_verbose_output(void) {
  struct Os* os = os_memory();
  write_gallery_fixture(os, NULL);
  // A file that is not media keeps `media_dir` present while the gallery holds no media.
  os_memory_write(os, "photos/notes.txt", "not media\n");
  const struct BuildOptions options = {
      .config_path = SITE_CONFIG_PATH_DEFAULT, .worker_count = 3, .is_verbose = true};

  TEST_CHECK(cmd_build_run(&options, os) == EXIT_CODE_OK);
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
  TEST_CHECK(os_memory_is_printed(os, 2,
                                  "loading config\n"
                                  "discovering media\n"
                                  "discovering templates and static files\n"
                                  "compiling templates\n"
                                  "planning albums\n"
                                  "loading output cache\n"
                                  "no output cache, probing every source\n"
                                  "building output manifest\n"
                                  "copying static files\n"
                                  "rendering albums, workers: 3\n"
                                  "\rrendering albums 1/1\n"
                                  "writing album pages\n"
                                  "rendering aggregate templates\n"
                                  "writing output cache\n"
                                  "removing stale outputs\n"
                                  "removed stale outputs: 0\n"
                                  "build complete\n"));
}

// A build that succeeds leaves its log empty, which is the other half of `cmd_build_execute`'s
// `log` contract. Without this a phase that logged on the success path would go unnoticed, since
// every other test that inspects the log is a failure test.
static void test_leaves_log_empty_on_success(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");

  TEST_CHECK(execute_build(os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
}

// `cmd_build_run` reports failure as `EXIT_CODE_FAILURE` and prints the collected diagnostic to
// standard error exactly once, which no other test asserts. Every other test routes failures
// through `cmd_build_execute` to inspect the message, so without this the boundary itself is
// unasserted: the exit code and the single print.
static void test_run_prints_diagnostic_once_and_fails(void) {
  // No `fram.toml`, the earliest failure, so the diagnostic is a single known line.
  struct Os* os = os_memory();
  const struct BuildOptions options = {.config_path = SITE_CONFIG_PATH_DEFAULT};

  TEST_CHECK(cmd_build_run(&options, os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
  // Compared whole rather than by substring: "exactly once" is the claim, and only a whole-buffer
  // comparison can tell one print from two.
  struct Arena arena = test_arena();
  struct Buf expected = {.arena = &arena};
  buf_printf(&expected, "failed to read config: %s ('fram.toml')\n", strerror(ENOENT));
  TEST_CHECK(os_memory_is_printed(os, 2, buf_cstr(&expected)));
}

// A build whose arena is too small for its data ends in `os_oom` rather than failing with a
// diagnostic or overrunning the arena. Under the test build's `os_oom` that is the process exiting
// with `TEST_OOM_EXIT_STATUS`. `main.c`'s `os_oom` exits with `EXIT_CODE_FAILURE` instead, so this
// test does not pin the production exit status.
static void test_tiny_arena_ends_in_oom(void) {
  struct Os* os = os_memory();
  write_gallery_fixture(os, NULL);
  TEST_CHECK(test_is_oom(build_in_tiny_arena, os));
}

// A missing `fram.toml` is logged with its cause, and nothing prints it before the command
// boundary. The cause is part of the claim: a missing config must not read like an unreadable one.
static void test_reports_missing_config(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(os_memory_is_printed(os, 2, ""));
  // Exact, not by substring: a substring check would also pass for this message with something
  // appended to it.
  TEST_CHECK(test_log_is(&log, "failed to read config: %s ('fram.toml')\n", strerror(ENOENT)));
}

// `media_dir` is required, so an absent one is a failure, reported as a resolution failure carrying
// the system cause. `ENOENT` and `EACCES` need different fixes and nothing later would report
// either, so the cause has to appear here. Only the optional read root is skipped when missing.
// Compared exactly, so a caller that appended the path a second time would fail.
static void test_reports_absent_media_dir(void) {
  struct Os* os = os_memory();
  write_gallery_fixture(os, NULL);
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to resolve config directory 'media_dir': %s ('photos')\n",
                         strerror(ENOENT)));
}

// `templates_dir` is required the same way, and reports the key the user has to fix rather than the
// one that happened to be checked first. A root that cannot be inspected reports its own cause.
static void test_reports_absent_templates_dir(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "fram.toml", SITE_CONFIG);
  write_jpeg(os, "photos/album/one.jpg");
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to resolve config directory 'templates_dir': %s ('templates')\n",
                         strerror(ENOENT)));

  os_memory_fail(os, OS_MEMORY_OP_STAT, "photos", EACCES);
  log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to resolve config directory 'media_dir': %s ('photos')\nfailed "
                         "to resolve config directory 'templates_dir': %s ('templates')\n",
                         strerror(EACCES), strerror(ENOENT)));
}

// A `templates_dir` whose subdirectory cannot be listed stops the build while it lists the inputs,
// because a partial the walk could not see could be neither compiled nor protected. The reason
// names the directory that failed, so the configured root is not repeated ahead of it.
static void test_reports_an_unlistable_templates_subdirectory(void) {
  struct Os* os = os_memory();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  os_memory_write(os, "templates/partials/card.html", "card");
  os_memory_fail(os, OS_MEMORY_OP_LIST, "templates/partials", EACCES);
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(
      &log, "failed to list templates: cannot open directory: %s ('templates/partials')\n",
      strerror(EACCES)));
  TEST_CHECK(!os_memory_has_path(os, "public"));
}

// A root that fails its check stops the build before the overlap check, so a missing `media_dir` is
// reported alone, even beside an `output_dir` inside `templates_dir`.
static void test_stops_at_a_bad_root_before_the_overlap_check(void) {
  struct Os* os = os_memory();
  write_gallery_fixture(os, "output_dir = \"templates/site\"\n");
  struct Log log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to resolve config directory 'media_dir': %s ('photos')\n",
                         strerror(ENOENT)));
  TEST_CHECK(!os_memory_has_path(os, "templates/site"));
}

// A directory key the config load rejects stops the build with that one error, before any root is
// checked.
static void test_stops_at_a_rejected_directory_key(void) {
  static const char* const cases[][4] = {
      {"media_dir = 5\n", "photos/album/one.jpg", "templates/album.html",
       "config key 'media_dir' must be a string"},
      {"media_dir = \"/\"\n", "photos/album/one.jpg", "templates/album.html",
       "config key 'media_dir' must not be empty"},
      {"media_dir = \"photos\"\ntemplates_dir = [\"layouts\"]\n", "photos/album/one.jpg",
       "layouts/album.html", "config key 'templates_dir' must be a string"},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    TEST_CASE(cases[i][0]);
    struct Os* os = os_memory();
    struct Arena config_arena = test_arena();
    struct Buf config = {.arena = &config_arena};
    buf_append(&config, S("title = \"Test Gallery\"\n"
                          "description = \"Test description\"\n"
                          "author = \"Tests\"\n"));
    buf_append(&config, str_from_cstr(cases[i][0]));
    buf_append(&config, S("[derivatives.s]\nwidth = 8\nheight = 8\nquality = 70\n"));
    os_memory_write(os, "fram.toml", buf_cstr(&config));
    write_jpeg(os, cases[i][1]);
    os_memory_write(os, cases[i][2], ALBUM_TEMPLATE);
    struct Log log = test_log();
    TEST_CHECK(execute_build(os, &log) == -1);
    TEST_CHECK(test_log_is_one(&log, cases[i][3]));
  }
  TEST_CASE_(NULL);
}

// A rejected `static_dir` stops the build too, so its default, here a regular file, is never
// checked.
static void test_stops_at_a_rejected_static_dir(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, "static_dir = \"\"\n");
  write_jpeg(os, "photos/album/one.jpg");
  os_memory_write(os, "static", "not a directory\n");

  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is_one(&log, "config key 'static_dir' must not be empty"));
}

// A `templates_dir` that exists but is a regular file is reported as the wrong type, which is its
// own cause rather than an `errno`, so the message carries no system text.
static void test_reports_templates_dir_that_is_a_file(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  os_memory_write(os, "fram.toml", SITE_CONFIG);
  write_jpeg(os, "photos/album/one.jpg");
  os_memory_write(os, "templates", "not a directory\n");

  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to resolve config directory 'templates_dir': not a directory "
                         "('templates')\n"));
}

// A configured `output_dir` that already exists as a regular file is reported once the manifest has
// passed, where the build creates `output_dir`, before any derivative is written. The reason names
// the component that failed and the caller does not repeat the configured root, so the message is
// compared whole.
static void test_reports_unusable_output_dir(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  // `public` is the default `output_dir`, so a file there is what makes `os_mkdirs` fail.
  os_memory_write(os, "public", "not a directory");

  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(
      &log, "failed to prepare output directory: exists and is not a directory ('public')\n"));
}

// An `output_dir` that is a dangling symlink is reported once the manifest has passed, where the
// build creates `output_dir`, before any derivative is written. A dangling symlink has no identity,
// so the manifest sees nothing there, and `os_mkdirs` then fails the inspection that follows
// `mkdir`'s `EEXIST`. The message is compared whole.
static void test_reports_dangling_output_dir_symlink(void) {
  struct Os* os = os_memory();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  // `public` is the default `output_dir`, so a dangling symlink there is what makes `os_mkdirs`
  // fail.
  os_memory_symlink(os, "missing", "public");
  struct Log log = test_log();

  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(
      &log, "failed to prepare output directory: cannot inspect directory: %s ('public')\n",
      strerror(ENOENT)));
}

// An `output_dir` below `media_dir` is refused before media is discovered or probed, so no earlier
// build's output can be read back as a source, and it is refused although it does not exist yet.
// The verbose phase lines show that the build stopped before the media walk, and the refused build
// creates none of the nested directories.
static void test_rejects_output_dir_inside_media_dir(void) {
  struct Os* os = os_memory();
  write_gallery_fixture(os, "output_dir = \"photos/generated/site\"\n");
  write_jpeg(os, "photos/album/one.jpg");
  const struct BuildOptions options = {.config_path = SITE_CONFIG_PATH_DEFAULT, .is_verbose = true};

  TEST_CHECK(cmd_build_run(&options, os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
  TEST_CHECK(os_memory_is_printed(os, 2,
                                  "loading config\n"
                                  "output directory would write inside 'media_dir': "
                                  "'photos/generated/site'\n"));
  TEST_CHECK(!os_memory_has_path(os, "photos/generated"));
}

// An `output_dir` that holds `media_dir`, such as `output_dir = "."`, is refused before media is
// discovered, so the gallery's own files never share a tree with its outputs, and the refused build
// writes nothing beside them.
static void test_rejects_output_dir_holding_media_dir(void) {
  struct Os* os = os_memory();
  write_gallery_fixture(os, "output_dir = \".\"\n");
  write_jpeg(os, "photos/album/one.jpg");
  const struct BuildOptions options = {.config_path = SITE_CONFIG_PATH_DEFAULT, .is_verbose = true};

  TEST_CHECK(cmd_build_run(&options, os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
  TEST_CHECK(os_memory_is_printed(os, 2,
                                  "loading config\n"
                                  "output directory would hold 'media_dir': '.'\n"));
  // The config, the template, and the source.
  TEST_CHECK(os_memory_files(os, ".").count == 3);
}

// An `output_dir` that holds the directory of the config that `-c` names is refused, although it
// holds no input root and no `fram.toml` in the working directory, so the build protects the config
// it read.
static void test_rejects_output_dir_holding_config_named_by_path(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "sub/fram.toml",
                  "title = \"Test Gallery\"\n"
                  "description = \"Test description\"\n"
                  "author = \"Tests\"\n"
                  "media_dir = \"../photos\"\n"
                  "templates_dir = \"../templates\"\n"
                  "output_dir = \".\"\n");
  os_memory_write(os, "templates/album.html", ALBUM_TEMPLATE);
  write_jpeg(os, "photos/album/one.jpg");
  struct Log log = test_log();
  const struct BuildOptions options = {.config_path = S("sub/fram.toml")};

  TEST_CHECK(execute_build_with_options(&options, os, &log) == -1);
  TEST_CHECK(test_log_is(&log, "output directory would hold the config: 'sub'\n"));
  // The config, the template, and the source.
  TEST_CHECK(os_memory_files(os, ".").count == 3);
}

// Slugging is lossy, so two different source names can claim one output path. The manifest reports
// the collision and names both sources instead of letting the second write win, once, at the first
// output the two share, although they share both derivatives and the original. A non-ASCII byte
// folds to hex, so `caf\xC3\xA9` produces the slug a literal `cafc3a9` also produces. The refused
// build creates no `output_dir`, because nothing is written before the manifest passes.
static void test_rejects_colliding_source_names(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/caf\xC3\xA9.jpg");
  write_jpeg(os, "photos/album/cafc3a9.jpg");

  TEST_CHECK(execute_build(os, &log) == -1);
  // Exact, not by substring: a substring check would also pass for this message with something
  // appended to it.
  TEST_CHECK(
      test_log_is(&log,
                  "duplicate output path for 'photos/album/cafc3a9.jpg' and "
                  "'photos/album/caf\xC3\xA9.jpg': "
                  "'public/_fram/v2-s-8x8c-q70-m-16x16-q75-f1000/album/cafc3a9-jpg-s.webp'\n"));
  TEST_CHECK(log.errors == 1);
  TEST_CHECK(!os_memory_has_path(os, "public"));
}

// A static file copied to the output root shares its paths with the generated pages, so one named
// after the root album page is rejected as a duplicate of it rather than overwriting it or being
// overwritten. The refused build creates no `output_dir`.
static void test_rejects_static_file_colliding_with_album_page(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  os_memory_write(os, "static/index.html", "static\n");

  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "duplicate output path for 'photos' and "
                         "'static/index.html': 'public/index.html'\n"));
  TEST_CHECK(!os_memory_has_path(os, "public"));
}

// An album template that fails to parse fails the build when the templates load, before anything is
// written. The diagnostic carries the parser's cause, its position and the template name.
static void test_reports_bad_template(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, "templates.album = \"broken.html\"\n");
  // The section never closes, so the template is read but fails to compile.
  os_memory_write(os, "templates/broken.html", "{{#media}}unclosed");
  write_jpeg(os, "photos/one.jpg");

  TEST_CHECK(execute_build(os, &log) == -1);
  // Exact, not by substring: a substring check would also pass for this message with something
  // appended to it.
  TEST_CHECK(test_log_is(
      &log, "section-opening tag has no closer at line 1, column 1 (in 'broken.html')\n"));
  TEST_CHECK(!os_memory_has_path(os, "public"));
}

// A missing template that a `[templates]` key names fails the build when the templates load, naming
// the key, and a template that both keys name names the first in table order.
static void test_reports_a_missing_configured_template_with_its_key(void) {
  static const struct {
    const char* config;
    const char* missing;
    const char* key;
  } cases[] = {
      {"templates.album = \"missing.html\"\n", "missing.html", "templates.album"},
      {"templates.aggregates = [\"sitemap.xml\"]\n", "sitemap.xml", "templates.aggregates"},
      {"templates.album = \"page.html\"\ntemplates.aggregates = [\"page.html\"]\n", "page.html",
       "templates.album"},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    TEST_CASE(cases[i].config);
    struct Os* os = os_memory();
    struct Log log = test_log();
    write_gallery_fixture(os, cases[i].config);
    write_jpeg(os, "photos/one.jpg");

    TEST_CHECK(execute_build(os, &log) == -1);
    TEST_CHECK(test_log_is(&log, "failed to read template: %s ('templates/%s') (for '%s')\n",
                           strerror(ENOENT), cases[i].missing, cases[i].key));
    TEST_CHECK(!os_memory_has_path(os, "public"));
  }
}

// A dry run loads every template, so an album template that does not compile fails it with the
// diagnostic a real build gives, and no listing is printed.
static void test_dry_run_reports_bad_template(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, "templates.album = \"broken.html\"\n");
  os_memory_write(os, "templates/broken.html", "{{#media}}unclosed");
  write_jpeg(os, "photos/one.jpg");

  TEST_CHECK(execute_dry_run(os, &log) == -1);
  TEST_CHECK(test_log_is(
      &log, "section-opening tag has no closer at line 1, column 1 (in 'broken.html')\n"));
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
}

// A dry run of two sources on one output path fails with the duplicate-path diagnostic a real build
// gives, and no listing is printed.
static void test_dry_run_rejects_colliding_source_names(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/caf\xC3\xA9.jpg");
  write_jpeg(os, "photos/album/cafc3a9.jpg");

  TEST_CHECK(execute_dry_run(os, &log) == -1);
  TEST_CHECK(
      test_log_is(&log,
                  "duplicate output path for 'photos/album/cafc3a9.jpg' and "
                  "'photos/album/caf\xC3\xA9.jpg': "
                  "'public/_fram/v2-s-8x8c-q70-m-16x16-q75-f1000/album/cafc3a9-jpg-s.webp'\n"));
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
}

// A build that fails removes nothing, so a stale file from an earlier build is still there after a
// render failure.
static void test_failed_build_removes_no_stale_output(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/one.jpg");
  os_memory_write(os, "public/notes.txt", "hand");
  os_memory_write(os, "templates/album.html", "{{#media}}unclosed");

  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(log.errors == 1);
  TEST_CHECK(os_memory_is_file(os, "public/notes.txt", "hand"));
}

// A stale entry that cannot be removed fails the build after every output is written, and the
// failure names the entry.
static void test_reports_stale_output_that_cannot_be_removed(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/one.jpg");
  os_memory_write(os, "public/notes.txt", "hand");
  os_memory_fail(os, OS_MEMORY_OP_REMOVE, "public/notes.txt", EACCES);

  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to remove stale output: %s ('public/notes.txt')\n",
                         strerror(EACCES)));
  TEST_CHECK(os_memory_has_path(os, "public/index.html"));
  TEST_CHECK(os_memory_has_path(os, "public/notes.txt"));
}

// Two albums that fail to render through one template fail for one cause. The report prints the
// first album's line whole and counts the second, and both count as errors. This is the test that
// reaches the build's page-render arm.
static void test_reports_a_shared_render_failure_once(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, "templates.album = \"raw.html\"\n");
  // A triple mustache of `album.title` fails the page render of both albums.
  os_memory_write(os, "templates/raw.html", "{{{album.title}}}");
  write_jpeg(os, "photos/album/one.jpg");

  TEST_CHECK(execute_build(os, &log) == -1);
  // Albums are sorted with the root first, so the root album's page is the one printed.
  TEST_CHECK(test_log_is(&log,
                         "unescaped Mustache interpolation is supported only for a key that ends "
                         "in '_html' (in 'raw.html') (while rendering 'index.html')\n\xE2\x80\xA6 "
                         "and 1 more failure\n"));
  TEST_CHECK(log.errors == 2);
}

// Two media items that both fail to probe log one line each, so neither is lost to the other. The
// lines differ in the source they name, so the report collapses neither, and a log that overwrote,
// or that ran two messages together, would still satisfy every single-item test.
static void test_reports_one_line_per_failing_media_item(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  // Not JPEG data, so the probe fails for both sources.
  os_memory_write(os, "photos/album/a.jpg", "not a JPEG\n");
  os_memory_write(os, "photos/album/b.jpg", "not a JPEG\n");

  TEST_CHECK(execute_build(os, &log) == -1);
  // Sources are walked in sorted order, so `a.jpg` precedes `b.jpg`. Compared whole, with the
  // separator in the middle, so a dropped newline or a lost line both fail here.
  TEST_CHECK(test_log_is(&log,
                         "failed to probe media: failed to probe JPEG: Not a JPEG file: starts "
                         "with 0x6e 0x6f ('photos/album/a.jpg')\n"
                         "failed to probe media: failed to probe JPEG: Not a JPEG file: starts "
                         "with 0x6e 0x6f ('photos/album/b.jpg')\n"));
  TEST_CHECK(log.errors == 2);
}

// Several independent mistakes in one gallery are all reported by one build, and the build writes
// nothing: two sources that cannot be probed, two sources whose names slug alike, and a static file
// that claims the root album's page. Each kind comes from a different phase, so the order is the
// phase order, and within a phase it is the sorted walk order. The probe failures do not stop the
// build before the manifest, because no output path depends on a probed dimension.
static void test_reports_every_independent_error(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  os_memory_write(os, "photos/album/bad-a.jpg", "not a JPEG\n");
  os_memory_write(os, "photos/album/bad-b.jpg", "not a JPEG\n");
  write_jpeg(os, "photos/album/caf\xC3\xA9.jpg");
  write_jpeg(os, "photos/album/cafc3a9.jpg");
  os_memory_write(os, "static/index.html", "static\n");

  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to probe media: failed to probe JPEG: Not a JPEG file: starts "
                         "with 0x6e 0x6f ('photos/album/bad-a.jpg')\n"
                         "failed to probe media: failed to probe JPEG: Not a JPEG file: starts "
                         "with 0x6e 0x6f ('photos/album/bad-b.jpg')\n"
                         "duplicate output path for 'photos/album/cafc3a9.jpg' and "
                         "'photos/album/caf\xC3\xA9.jpg': "
                         "'public/_fram/v2-s-8x8c-q70-m-16x16-q75-f1000/album/cafc3a9-jpg-s.webp'\n"
                         "duplicate output path for 'photos' and 'static/index.html': "
                         "'public/index.html'\n"));
  TEST_CHECK(log.errors == 4);
  TEST_CHECK(!os_memory_has_path(os, "public"));
}

// A JPEG over the media read limit is rejected by its probe read. The file is sparse, so the
// fixture costs no memory.
static void test_rejects_oversize_source_at_probe(void) {
  const ptrdiff_t big_source_len = (ptrdiff_t)4 * 1024 * 1024 * 1024;
  struct Os* os = os_memory();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/big.jpg");
  os_memory_truncate(os, "photos/big.jpg", big_source_len);
  const struct BuildOptions options = {.config_path = SITE_CONFIG_PATH_DEFAULT, .worker_count = 1};
  struct Log log = test_log();

  TEST_CHECK(execute_build_with_options(&options, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to probe media: exceeds max file size (%d bytes) at %td "
                         "bytes ('photos/big.jpg')\n",
                         IMAGE_INPUT_LEN_MAX, big_source_len));
}

// An output path that cannot be opened as a file fails the build at the write phase, after every
// album has rendered successfully. That is the last of `cmd_build_execute`'s phase arms and the
// only one reached with a complete set of rendered pages in hand, so a write failure reported as
// success would otherwise pass the suite with the pages silently missing. One page path is an
// existing directory. The phase stops at its first failure, so a second build, where a new source
// changes the root album's page, fails that page, which is written first, with an injected
// `EACCES`, as an unwritable directory would refuse it.
static void test_reports_unwritable_output(void) {
  struct Os* os = os_memory();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  // A directory at the page path cannot be opened as a file.
  os_memory_mkdir(os, "public/album/index.html");
  struct Log log = test_log();

  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to write page for 'album': %s ('public/album/index.html')\n",
                         strerror(EISDIR)));

  write_jpeg(os, "photos/two.jpg");
  os_memory_fail(os, OS_MEMORY_OP_WRITE, "public/index.html", EACCES);
  log = test_log();
  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to write page for the root album: %s ('public/index.html')\n",
                         strerror(EACCES)));
}

// A `static_dir` that exists but is a regular file is reported. Skipping an absent optional root
// must not also skip a misconfigured one, which is why the skip asks whether anything is there
// rather than whether a directory is there: the wrong type survives the skip and reaches the type
// check, which names it.
static void test_reports_static_dir_that_is_a_file(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  os_memory_write(os, "static", "not a directory\n");

  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to resolve config directory 'static_dir': not a directory "
                         "('static')\n"));
}

// A `static_dir` that cannot be inspected is reported with its cause rather than skipped as absent,
// because it may well be there. A symlink that resolves to itself stands in for any such cause.
static void test_reports_static_dir_that_cannot_be_inspected(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  write_gallery_fixture(os, NULL);
  write_jpeg(os, "photos/album/one.jpg");
  os_memory_symlink(os, "static", "static");

  TEST_CHECK(execute_build(os, &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to resolve config directory 'static_dir': %s ('static')\n",
                         strerror(ELOOP)));
}

TEST_LIST = {
    {"honors configured album template", test_honors_configured_album_template},
    {"writes exactly manifest outputs", test_writes_exactly_manifest_outputs},
    {"tolerates trailing slash on media_dir", test_tolerates_trailing_slash_on_media_dir},
    {"builds from config in a subdirectory", test_builds_from_config_in_a_subdirectory},
    {"builds with no static dir", test_builds_with_no_static_dir},
    {"copies static files when present", test_copies_static_files_when_present},
    {"distinguishes same name in different dirs", test_distinguishes_same_name_in_different_dirs},
    {"builds an album with no media of its own", test_builds_an_album_with_no_media_of_its_own},
    {"renders configured sort orders", test_renders_configured_sort_orders},
    {"rebuild succeeds and repeats its outputs", test_rebuild_succeeds_and_repeats_its_outputs},
    {"rebuild removes stale outputs", test_rebuild_removes_stale_outputs},
    {"rebuild skips output dir linked from input dir",
     test_rebuild_skips_output_dir_linked_from_media_dir},
    {"dry run lists every output and writes nothing",
     test_dry_run_lists_every_output_and_writes_nothing},
    {"dry run keeps unchanged outputs", test_dry_run_keeps_unchanged_outputs},
    {"dry run lists stale recipe and keeps it", test_dry_run_lists_stale_recipe_and_keeps_it},
    {"dry run keeps unchanged static files", test_dry_run_keeps_unchanged_static_files},
    {"dry run attempts no change", test_dry_run_attempts_no_change},
    {"dry run leaves no state", test_dry_run_leaves_no_state},
    {"warm build reads no source and repeats its bytes",
     test_warm_build_reads_no_source_and_repeats_its_bytes},
    {"force probes every source", test_force_probes_every_source},
    {"force writes every output", test_force_writes_every_output},
    {"rebuild leaves unchanged outputs untouched", test_rebuild_leaves_unchanged_outputs_untouched},
    {"changed source remakes its outputs", test_changed_source_remakes_its_outputs},
    {"deleted derivative is made again", test_deleted_derivative_is_made_again},
    {"rewrites probe facts without deleted sources",
     test_rewrites_probe_facts_without_deleted_sources},
    {"ignores output cache of another format", test_ignores_output_cache_of_another_format},
    {"prunes the old build cache", test_prunes_the_old_build_cache},
    {"failed probe keeps output cache", test_failed_probe_keeps_output_cache},
    {"dry run uses output cache and leaves it", test_dry_run_uses_output_cache_and_leaves_it},
    {"honors requested worker count", test_honors_requested_worker_count},
    {"builds a large album", test_builds_a_large_album},
    {"skips empty phases in verbose output", test_skips_empty_phases_in_verbose_output},
    {"leaves log empty on success", test_leaves_log_empty_on_success},
    {"run prints diagnostic once and fails", test_run_prints_diagnostic_once_and_fails},
    {"tiny arena ends in oom", test_tiny_arena_ends_in_oom},
    {"reports missing config", test_reports_missing_config},
    {"reports absent input dir", test_reports_absent_media_dir},
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
    {"rejects output dir inside input dir", test_rejects_output_dir_inside_media_dir},
    {"rejects output dir holding input dir", test_rejects_output_dir_holding_media_dir},
    {"rejects output dir holding config named by path",
     test_rejects_output_dir_holding_config_named_by_path},
    {"rejects colliding source names", test_rejects_colliding_source_names},
    {"rejects static file colliding with album page",
     test_rejects_static_file_colliding_with_album_page},
    {"reports bad template", test_reports_bad_template},
    {"reports a missing configured template with its key",
     test_reports_a_missing_configured_template_with_its_key},
    {"dry run reports bad template", test_dry_run_reports_bad_template},
    {"dry run rejects colliding source names", test_dry_run_rejects_colliding_source_names},
    {"failed build removes no stale output", test_failed_build_removes_no_stale_output},
    {"reports stale output that cannot be removed",
     test_reports_stale_output_that_cannot_be_removed},
    {"reports a shared render failure once", test_reports_a_shared_render_failure_once},
    {"failed render keeps the static copies it made",
     test_failed_render_keeps_the_static_copies_it_made},
    {"failed build keeps the probe facts", test_failed_build_keeps_the_probe_facts},
    {"failed copy gets no row", test_failed_copy_gets_no_row},
    {"failed static copy gets no row", test_failed_static_copy_gets_no_row},
    {"interrupted build keeps the probe facts", test_interrupted_build_keeps_the_probe_facts},
    {"reports one line per failing media item", test_reports_one_line_per_failing_media_item},
    {"reports every independent error", test_reports_every_independent_error},
    {"rejects oversize source at probe", test_rejects_oversize_source_at_probe},
    {"reports unwritable output", test_reports_unwritable_output},
    {"reports static dir that is a file", test_reports_static_dir_that_is_a_file},
    {"reports static dir that cannot be inspected",
     test_reports_static_dir_that_cannot_be_inspected},
    {NULL, NULL},
};
