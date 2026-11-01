#include <acutest.h>
#include <errno.h>

#include "app/cli_spec.h"
#include "cli/cli.h"
#include "cli/cli_dispatch.h"
#include "core/error.h"
#include "core/exit_code.h"
#include "core/str.h"
#include "os/os_memory.h"
#include "testing/test_support.h"

/** The configuration a config of the three required keys prints, every other key at its default. */
#define CONFIG_PRINTED_DEFAULTS                                                    \
  "title = \"Test Gallery\"\n"                                                     \
  "description = \"Test description\"\n"                                           \
  "author = \"Author Name\"\n"                                                     \
  "media_dir = \"media\"\n"                                                        \
  "output_dir = \"public\"\n"                                                      \
  "templates_dir = \"templates\"\n"                                                \
  "static_dir = \"static\"\n"                                                      \
  "album_template = \"album.html\"\n"                                              \
  "aggregate_templates = []\n"                                                     \
  "derivatives = { s = { width = 120, height = 120, quality = 70, crop = true }, " \
  "m = { width = 520, height = 360, quality = 80, crop = true }, "                 \
  "l = { width = 792, height = 594, quality = 85 } }\n"                            \
  "sort = { albums = \"asc\", media = \"asc\" }\n"                                 \
  "video_frame_seconds = 1\n"

/**
 * @brief Runs `fram config -c <config_path>` with the app's own spec, as `main` does.
 *
 * @param config_path Config file to name.
 * @param os          Host to run on. Must not be `NULL`.
 * @return The command's exit code.
 */
static enum ExitCode run_config(struct Str config_path, struct Os* os) {
  struct Arena arena = test_arena();
  char* argv[] = {"fram", "config", "-c", str_cstr(config_path, &arena), NULL};
  struct CliSpec spec;
  cli_spec_init(&spec);
  return cli_dispatch(&spec, 4, argv, os);
}

// A valid `fram.toml` loads and prints, and the command reports success.
static void test_prints_loaded_config(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "fram.toml",
                  "title = \"Test Gallery\"\n"
                  "description = \"Test description\"\n"
                  "author = \"Author Name\"\n");
  TEST_CHECK(run_config(S("fram.toml"), os) == EXIT_CODE_OK);
  // The whole output, not two per-line searches: exact comparison catches a missing, duplicated or
  // reordered key. The empty `stderr` below is the routing half: the configuration goes to
  // `stdout`, and the success path says nothing on `stderr`.
  TEST_CHECK(os_memory_is_printed(os, 2, ""));
  TEST_CHECK(os_memory_is_printed(os, 1, CONFIG_PRINTED_DEFAULTS));
}

// A config whose template array is given as an explicit `[]` prints and reports success. The
// default is empty too, but only the explicit array goes through the loader's array parse. A loader
// that accepted `[]` and left the array `NULL` would hand that null to a `nonnull` parameter and
// abort here. This is the end-to-end half of the pair: `test_print_empty_template_array` covers the
// same composition at the module boundary.
static void test_prints_empty_template_array(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "fram.toml",
                  "title = \"Test Gallery\"\n"
                  "description = \"Test description\"\n"
                  "author = \"Author Name\"\n"
                  "aggregate_templates = []\n");
  TEST_CHECK(run_config(S("fram.toml"), os) == EXIT_CODE_OK);
  // Compared whole, and `stderr` empty, for the reasons at `test_prints_loaded_config`.
  TEST_CHECK(os_memory_is_printed(os, 2, ""));
  TEST_CHECK(os_memory_is_printed(os, 1, CONFIG_PRINTED_DEFAULTS));
}

// A config that `-c` names in a subdirectory prints every directory resolved against that
// subdirectory, the defaults included, and an absolute one as it is.
static void test_prints_paths_resolved_against_config_dir(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "sub/fram.toml",
                  "title = \"Test Gallery\"\n"
                  "description = \"Test description\"\n"
                  "author = \"Author Name\"\n"
                  "media_dir = \"../photos/\"\n"
                  "templates_dir = \"layouts\"\n"
                  "static_dir = \"/srv/static\"\n");
  TEST_CHECK(run_config(S("sub/fram.toml"), os) == EXIT_CODE_OK);
  // Compared whole, and `stderr` empty, for the reasons at `test_prints_loaded_config`.
  TEST_CHECK(os_memory_is_printed(os, 2, ""));
  TEST_CHECK(os_memory_is_printed(
      os, 1,
      "title = \"Test Gallery\"\n"
      "description = \"Test description\"\n"
      "author = \"Author Name\"\n"
      "media_dir = \"sub/../photos\"\n"
      "output_dir = \"sub/public\"\n"
      "templates_dir = \"sub/layouts\"\n"
      "static_dir = \"/srv/static\"\n"
      "album_template = \"album.html\"\n"
      "aggregate_templates = []\n"
      "derivatives = { s = { width = 120, height = 120, quality = 70, crop = true }, "
      "m = { width = 520, height = 360, quality = 80, crop = true }, "
      "l = { width = 792, height = 594, quality = 85 } }\n"
      "sort = { albums = \"asc\", media = \"asc\" }\n"
      "video_frame_seconds = 1\n"));
}

// A missing config is reported with the read failure that caused it, naming the path that `-c`
// gave, so a mistyped path reads as one.
static void test_reports_missing_config_named_by_path(void) {
  struct Os* os = os_memory();
  TEST_CHECK(run_config(S("sub/fram.toml"), os) == EXIT_CODE_FAILURE);
  char reason[ERROR_SYSTEM_MESSAGE_SIZE];
  TEST_CHECK(test_text_is(os_memory_printed(os, 2), "failed to read config: %s ('sub/fram.toml')\n",
                          error_system_message(reason, sizeof(reason), ENOENT)));
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
}

// A missing `fram.toml` is reported with the read failure that caused it, naming the path, so this
// cannot pass on an unrelated failure such as a template error. The reason is derived from the
// running libc rather than hardcoded, so the assertion is the whole claim and stays portable.
static void test_reports_missing_config(void) {
  struct Os* os = os_memory();
  TEST_CHECK(run_config(S("fram.toml"), os) == EXIT_CODE_FAILURE);
  // The diagnostic goes to `stderr`. A failed run prints nothing on `stdout`, so a pipeline
  // redirecting `stdout` to a file does not capture the error into it.
  char reason[ERROR_SYSTEM_MESSAGE_SIZE];
  TEST_CHECK(test_text_is(os_memory_printed(os, 2), "failed to read config: %s ('fram.toml')\n",
                          error_system_message(reason, sizeof(reason), ENOENT)));
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
}

// A well-formed `fram.toml` that omits a required key fails, and fails with the missing-key message
// rather than the wrong-type one. An exact line assertion preserves that distinction.
static void test_reports_missing_required_key(void) {
  struct Os* os = os_memory();
  // Missing the required `author` key.
  os_memory_write(os, "fram.toml",
                  "title = \"Test Gallery\"\n"
                  "description = \"Test description\"\n");
  TEST_CHECK(run_config(S("fram.toml"), os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_is_printed(os, 2, "missing required config key 'author'\n"));
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
}

// A `fram.toml` that parses but holds several invalid keys fails, reports only the first one the
// load checks, and prints no configuration.
static void test_reports_first_invalid_key(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "fram.toml",
                  "base_url = \"https://example.com\"\n"
                  "title = 1\n"
                  "description = \"Test description\"\n"
                  "author = \"Author Name\"\n"
                  "colour = \"blue\"\n"
                  "video_frame_seconds = \"ten\"\n");
  TEST_CHECK(run_config(S("fram.toml"), os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_is_printed(os, 2, "unknown config key 'colour'\n"));
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
}

// A `fram.toml` that loads fine but cannot be written out fails rather than exiting 0 when nobody
// received the output. Nothing is printed to `stderr`.
static void test_fails_on_unwritable_stdout(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "fram.toml",
                  "title = \"Test Gallery\"\n"
                  "description = \"Test description\"\n"
                  "author = \"Author Name\"\n");
  os_memory_fail_print(os, 1, 0, EBADF);
  TEST_CHECK(run_config(S("fram.toml"), os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_is_printed(os, 2, ""));
}

TEST_LIST = {
    {"prints loaded config", test_prints_loaded_config},
    {"prints empty template array", test_prints_empty_template_array},
    {"prints paths resolved against config dir", test_prints_paths_resolved_against_config_dir},
    {"reports missing config named by path", test_reports_missing_config_named_by_path},
    {"reports missing config", test_reports_missing_config},
    {"reports missing required key", test_reports_missing_required_key},
    {"reports first invalid key", test_reports_first_invalid_key},
    {"fails on unwritable stdout", test_fails_on_unwritable_stdout},
    {NULL, NULL},
};
