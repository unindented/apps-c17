#include <acutest.h>
#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "app/cli_dispatch.h"
#include "core/arena.h"
#include "core/exit_code.h"
#include "core/str.h"
#include "domain/fram_version.h"
#include "os/os_memory.h"
#include "test_jpeg_fixture.h"
#include "testing/test_support.h"

/**
 * @brief Returns a host holding a minimal buildable gallery with two photos.
 *
 * Both photos are in the root album, so the probe and derivative phases run two jobs and the album
 * page render runs one. Beside `fram.toml`, `sub/fram.toml` names the same media and templates from
 * a subdirectory, and leaves `output_dir` and `static_dir` at their defaults, which are below
 * `sub`.
 *
 * @return The host.
 */
static struct Os* gallery_fixture(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "fram.toml",
                  "title = \"Gallery\"\n"
                  "description = \"Test description\"\n"
                  "author = \"Author\"\n");
  os_memory_write(os, "sub/fram.toml",
                  "title = \"Gallery\"\n"
                  "description = \"Test description\"\n"
                  "author = \"Author\"\n"
                  "media_dir = \"../media\"\n"
                  "templates_dir = \"../templates\"\n");
  os_memory_write(os, "templates/album.html", "<title>{{album.title}}</title>\n");
  struct Arena arena = test_arena();
  struct Str jpeg = {0};
  if (TEST_CHECK(test_jpeg_encode(24, 12, &arena, &jpeg) == 0)) {
    os_memory_write_bytes(os, S("media/a.jpg"), jpeg);
    os_memory_write_bytes(os, S("media/b.jpg"), jpeg);
  }
  return os;
}

// `--version` writes the version to `stdout` and reports success.
static void test_version_action_reports_ok(void) {
  char* argv[] = {"fram", "--version", NULL};
  struct Os* os = os_memory();
  TEST_CHECK(cli_dispatch(2, argv, os) == EXIT_CODE_OK);
  const struct Str version = fram_version_string();
  TEST_CHECK(test_text_is(os_memory_printed(os, 1), "fram %.*s\n", STR_FMT(version)));
  TEST_CHECK(os_memory_is_printed(os, 2, ""));
}

// `--help` writes the usage text to `stdout` and reports success. The program name comes from
// `argv[0]`. Using a name that is deliberately not `"fram"` proves the function substitutes it
// rather than hard-coding it. `test_cli.c` pins the text itself, so this test asserts only the
// plumbing.
static void test_help_action_reports_ok(void) {
  char* argv[] = {"/opt/bin/myfram", "--help", NULL};
  struct Os* os = os_memory();
  TEST_CHECK(cli_dispatch(2, argv, os) == EXIT_CODE_OK);
  TEST_CHECK(str_startswith(os_memory_printed(os, 1),
                            S("A static photo and video gallery generator.\n\nUSAGE:\n"
                              "    /opt/bin/myfram <command> [options]\n")));
  TEST_CHECK(os_memory_is_printed(os, 2, ""));
}

// `config -c` is dispatched to `cmd_config_run` with the path it names, not to a build: the
// configuration of `sub/fram.toml`, with every directory resolved against `sub`, lands on `stdout`,
// and no `public/` directory appears. Dispatching it to the wrong command would otherwise still
// exit `0` in a valid gallery, and dropping the path would print the fixture's own `fram.toml`.
static void test_config_command_is_dispatched(void) {
  struct Os* os = gallery_fixture();
  char* argv[] = {"fram", "config", "-c", "sub/fram.toml", NULL};
  TEST_CHECK(cli_dispatch(4, argv, os) == EXIT_CODE_OK);
  TEST_CHECK(os_memory_is_printed(
      os, 1,
      "title = \"Gallery\"\n"
      "description = \"Test description\"\n"
      "author = \"Author\"\n"
      "media_dir = \"sub/../media\"\n"
      "output_dir = \"sub/public\"\n"
      "templates_dir = \"sub/../templates\"\n"
      "static_dir = \"sub/static\"\n"
      "album_template = \"album.html\"\n"
      "aggregate_templates = []\n"
      "derivatives = { s = { width = 120, height = 120, quality = 70, crop = true }, "
      "m = { width = 520, height = 360, quality = 80, crop = true }, "
      "l = { width = 792, height = 594, quality = 85 } }\n"
      "sort = { albums = \"asc\", media = \"asc\" }\n"
      "video_frame_seconds = 1\n"));
  TEST_CHECK(os_memory_is_printed(os, 2, ""));
  // No page was written, which is what separates `config` from `build`.
  TEST_CHECK(!os_memory_has_path(os, "public/index.html"));
  TEST_CHECK(!os_memory_has_path(os, "sub/public"));
}

// The `build` command is dispatched to `cmd_build_run` with the parsed options attached, so the
// config path, the worker count, the verbose flag, the dry-run flag and the force flag reach the
// build rather than being dropped on the way. A dry run of `sub/fram.toml` prints a listing below
// `sub/public` and leaves no output directory, which only a carried path and flag explain. The
// verbose phase lines naming the requested count, and the one saying the build cache is ignored,
// are the only externally visible evidence that the other three fields were carried across. The
// test asks for one worker, so a dropped count would show the detected count, on a host with more
// than one core. One worker also finishes the jobs in order, so the progress lines can be compared.
static void test_build_command_receives_parsed_options(void) {
  struct Os* os = gallery_fixture();
  char* dry_run_argv[] = {"fram", "build", "--dry-run", "-c", "sub/fram.toml", NULL};
  TEST_CHECK(cli_dispatch(5, dry_run_argv, os) == EXIT_CODE_OK);
  // The derivative lines name the recipe directory, which every change to the default derivatives
  // renames, so only the first and the last line are spelled out.
  const struct Str listing = os_memory_printed(os, 1);
  TEST_CHECK(str_startswith(listing, S("write\tsub/public/index.html\n")));
  TEST_CHECK(str_endswith(listing, S("write\tsub/public/_fram/v1-build-cache.tsv\n")));
  TEST_MSG("stdout: '%.*s'", STR_FMT(listing));
  TEST_CHECK(os_memory_is_printed(os, 2, ""));
  TEST_CHECK(!os_memory_has_path(os, "sub/public"));
  TEST_CHECK(!os_memory_has_path(os, "public"));

  // The host keeps what the dry run printed, so the build prints nothing to `stdout` when the
  // stream still holds the listing alone.
  const ptrdiff_t listing_len = listing.len;
  char* argv[] = {"fram", "build", "--verbose", "--workers=1", "--force", NULL};
  TEST_CHECK(cli_dispatch(5, argv, os) == EXIT_CODE_OK);
  TEST_CHECK(os_memory_printed(os, 1).len == listing_len);
  TEST_CHECK(os_memory_is_printed(os, 2,
                                  "loading config\n"
                                  "discovering media\n"
                                  "discovering templates and static files\n"
                                  "compiling templates\n"
                                  "planning albums\n"
                                  "ignoring build cache, probing every source\n"
                                  "probing media, workers: 1\n"
                                  "\rprobing media 1/2\rprobing media 2/2\n"
                                  "building output manifest\n"
                                  "copying static files\n"
                                  "writing build cache\n"
                                  "generating derivatives, workers: 1\n"
                                  "\rgenerating derivatives 1/2\rgenerating derivatives 2/2\n"
                                  "rendering albums, workers: 1\n"
                                  "\rrendering albums 1/1\n"
                                  "writing album pages\n"
                                  "rendering aggregate templates\n"
                                  "removing stale outputs\n"
                                  "removed stale outputs: 0\n"
                                  "build complete\n"));
  TEST_CHECK(os_memory_has_path(os, "public/index.html"));
  TEST_CHECK(os_memory_has_path(os, "public/_fram/originals/a-jpg.jpg"));
  TEST_CHECK(os_memory_has_path(os, "public/_fram/originals/b-jpg.jpg"));
}

// A command line that fails to parse reports `EXIT_CODE_USAGE`, and the parser's own diagnostic
// goes to `stderr` rather than `stdout`. The exit code is what nothing else pins: `2` is what a
// shell or CI wrapper branches on to tell a bad invocation from a failed build, and `test_cli.c`
// asserts only that `cli_parse` produced the message.
static void test_parse_error_reports_usage_code(void) {
  char* argv[] = {"fram", "--bogus", NULL};
  struct Os* os = os_memory();
  TEST_CHECK(cli_dispatch(2, argv, os) == EXIT_CODE_USAGE);
  TEST_CHECK(os_memory_is_printed(os, 2, "unknown option '--bogus'\n"));
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
}

// A `stdout` that cannot be written turns a successful print into `EXIT_CODE_FAILURE` and prints
// nothing to `stderr`. Nothing else in the suite reaches this arm. Other tests make a printer fail,
// but none goes through `cli_dispatch`, so nothing else pins the mapping from a printer's non-zero
// result to an exit code.
static void test_version_write_failure_reports_failure(void) {
  char* argv[] = {"fram", "--version", NULL};
  struct Os* os = os_memory();
  os_memory_fail_print(os, 1, 0, EBADF);
  TEST_CHECK(cli_dispatch(2, argv, os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_is_printed(os, 2, ""));
}

// A dry run whose listing cannot be printed fails the build with a diagnostic, rather than
// reporting success for a listing nobody saw.
static void test_dry_run_print_failure_reports_failure(void) {
  struct Os* os = gallery_fixture();
  os_memory_fail_print(os, 1, 0, EBADF);
  char* argv[] = {"fram", "build", "--dry-run", NULL};
  TEST_CHECK(cli_dispatch(3, argv, os) == EXIT_CODE_FAILURE);
  TEST_CHECK(
      test_text_is(os_memory_printed(os, 2), "failed to print dry run: %s\n", strerror(EBADF)));
}

TEST_LIST = {
    {"version action reports ok", test_version_action_reports_ok},
    {"help action reports ok", test_help_action_reports_ok},
    {"config command is dispatched", test_config_command_is_dispatched},
    {"build command receives parsed options", test_build_command_receives_parsed_options},
    {"parse error reports usage code", test_parse_error_reports_usage_code},
    {"version write failure reports failure", test_version_write_failure_reports_failure},
    {"dry run print failure reports failure", test_dry_run_print_failure_reports_failure},
    {NULL, NULL},
};
