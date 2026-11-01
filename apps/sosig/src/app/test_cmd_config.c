#include <acutest.h>
#include <errno.h>
#include <stddef.h>

#include "app/cli_spec.h"
#include "cli/cli.h"
#include "cli/cli_dispatch.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/error.h"
#include "core/exit_code.h"
#include "core/str.h"
#include "os/os_memory.h"
#include "testing/test_support.h"

/**
 * @brief Runs `sosig config -c <config_path>` with the app's own spec, as `main` does.
 *
 * @param config_path Config file to name.
 * @param os          Host to run on. Must not be `NULL`.
 * @return The command's exit code.
 */
static enum ExitCode run_config(struct Str config_path, struct Os* os) {
  struct Arena arena = test_arena();
  char* argv[] = {"sosig", "config", "-c", str_cstr(config_path, &arena), NULL};
  struct CliSpec spec;
  cli_spec_init(&spec);
  return cli_dispatch(&spec, 4, argv, os);
}

// Every run here is against the in-memory host, whose root is the working directory the command
// resolves the config path from, and which keeps what the command prints.

// A valid `sosig.toml` loads and prints, and the command reports success.
static void test_prints_loaded_config(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "sosig.toml",
                  "title = \"Test Site\"\n"
                  "description = \"Example notes\"\n"
                  "author = \"Author Name\"\n"
                  "base_url = \"https://example.com\"\n"
                  "frontmatter_ignored_keys = [\"identifier\", \"signature\"]\n");

  TEST_CHECK(run_config(S("sosig.toml"), os) == EXIT_CODE_OK);
  // The whole output, not two per-line searches: exact comparison catches a missing, duplicated or
  // reordered key. The empty `stderr` below is the routing half: the configuration goes to
  // `stdout`, and the success path says nothing on `stderr`.
  TEST_CHECK(os_memory_is_printed(os, 2, ""));
  TEST_CHECK(os_memory_is_printed(os, 1,
                                  "title = \"Test Site\"\n"
                                  "description = \"Example notes\"\n"
                                  "author = \"Author Name\"\n"
                                  "base_url = \"https://example.com\"\n"
                                  "permalink = \"/{section}/{slug}.html\"\n"
                                  "content_dir = \"content\"\n"
                                  "output_dir = \"public\"\n"
                                  "templates_dir = \"templates\"\n"
                                  "static_dir = \"static\"\n"
                                  "content_template = \"content.html\"\n"
                                  "aggregate_templates = [\"index.html\"]\n"
                                  "feed_templates = [\"atom.xml\"]\n"
                                  "feed_count = 25\n"
                                  "frontmatter_ignored_keys = [\"identifier\", \"signature\"]\n"));
}

// A config whose template arrays are empty prints and reports success. A loader that accepted `[]`
// and left the array `NULL` would hand that null to a `nonnull` parameter and abort here. This is
// the end-to-end half of the pair: `test_print_empty_template_arrays` covers the same composition
// at the module boundary.
static void test_prints_empty_template_arrays(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "sosig.toml",
                  "title = \"Test Site\"\n"
                  "description = \"Example notes\"\n"
                  "author = \"Author Name\"\n"
                  "base_url = \"https://example.com\"\n"
                  "aggregate_templates = []\n"
                  "feed_templates = []\n");

  TEST_CHECK(run_config(S("sosig.toml"), os) == EXIT_CODE_OK);
  // Compared whole, and `stderr` empty, for the reasons at `test_prints_loaded_config`.
  TEST_CHECK(os_memory_is_printed(os, 2, ""));
  TEST_CHECK(os_memory_is_printed(os, 1,
                                  "title = \"Test Site\"\n"
                                  "description = \"Example notes\"\n"
                                  "author = \"Author Name\"\n"
                                  "base_url = \"https://example.com\"\n"
                                  "permalink = \"/{section}/{slug}.html\"\n"
                                  "content_dir = \"content\"\n"
                                  "output_dir = \"public\"\n"
                                  "templates_dir = \"templates\"\n"
                                  "static_dir = \"static\"\n"
                                  "content_template = \"content.html\"\n"
                                  "aggregate_templates = []\n"
                                  "feed_templates = []\n"
                                  "feed_count = 25\n"
                                  "frontmatter_ignored_keys = []\n"));
}

// `config -c sub/sosig.toml` prints every directory resolved against `sub`, a default as well as a
// configured one, and leaves an absolute directory as it is.
static void test_prints_directories_resolved_against_config_dir(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "sub/sosig.toml",
                  "title = \"Test Site\"\n"
                  "description = \"Example notes\"\n"
                  "author = \"Author Name\"\n"
                  "base_url = \"https://example.com\"\n"
                  "templates_dir = \"layouts\"\n"
                  "static_dir = \"/srv/static\"\n");

  TEST_CHECK(run_config(S("sub/sosig.toml"), os) == EXIT_CODE_OK);
  // Compared whole, and `stderr` empty, for the reasons at `test_prints_loaded_config`.
  TEST_CHECK(os_memory_is_printed(os, 2, ""));
  TEST_CHECK(os_memory_is_printed(os, 1,
                                  "title = \"Test Site\"\n"
                                  "description = \"Example notes\"\n"
                                  "author = \"Author Name\"\n"
                                  "base_url = \"https://example.com\"\n"
                                  "permalink = \"/{section}/{slug}.html\"\n"
                                  "content_dir = \"sub/content\"\n"
                                  "output_dir = \"sub/public\"\n"
                                  "templates_dir = \"sub/layouts\"\n"
                                  "static_dir = \"/srv/static\"\n"
                                  "content_template = \"content.html\"\n"
                                  "aggregate_templates = [\"index.html\"]\n"
                                  "feed_templates = [\"atom.xml\"]\n"
                                  "feed_count = 25\n"
                                  "frontmatter_ignored_keys = []\n"));
}

// A missing config is reported with the read failure that caused it, naming the path, so this
// cannot pass on an unrelated failure such as a template error. The reason is derived from the
// running libc rather than hardcoded, so the assertion is the whole claim and stays portable.
static void test_reports_missing_config(void) {
  // The default path, and one that `-c` names, which the diagnostic names in its place.
  static const char* const config_paths[] = {"sosig.toml", "sub/sosig.toml"};

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(config_paths) / sizeof(config_paths[0])); i++) {
    TEST_CASE(config_paths[i]);
    char reason[ERROR_SYSTEM_MESSAGE_SIZE];
    struct Arena arena = test_arena();
    struct Buf expected = {.arena = &arena};
    buf_printf(&expected, "failed to read config: %s ('%s')\n",
               error_system_message(reason, sizeof(reason), ENOENT), config_paths[i]);

    struct Os* os = os_memory();
    TEST_CHECK(run_config(str_from_cstr(config_paths[i]), os) == EXIT_CODE_FAILURE);
    // The diagnostic goes to `stderr`. A failed run prints nothing on `stdout`, so a pipeline
    // redirecting `stdout` to a file does not capture the error into it.
    TEST_CHECK(os_memory_is_printed(os, 2, buf_cstr(&expected)));
    TEST_CHECK(os_memory_is_printed(os, 1, ""));
  }
  TEST_CASE_(NULL);
}

// A well-formed `sosig.toml` that omits a required key fails, and fails with the missing-key
// message rather than the wrong-type one. An exact line assertion preserves that distinction.
static void test_reports_missing_required_key(void) {
  struct Os* os = os_memory();
  // Missing the required `author` key.
  os_memory_write(os, "sosig.toml",
                  "title = \"Test Site\"\n"
                  "description = \"Example notes\"\n"
                  "base_url = \"https://example.com\"\n");

  TEST_CHECK(run_config(S("sosig.toml"), os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_is_printed(os, 2, "missing required config key 'author'\n"));
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
}

// A `sosig.toml` that parses but holds several invalid keys fails, reports only the first that the
// load checks, and prints no configuration.
static void test_reports_first_invalid_key(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "sosig.toml",
                  "title = 1\n"
                  "description = \"Example notes\"\n"
                  "author = \"Author Name\"\n"
                  "base_url = \"https://example.com\"\n"
                  "feed_count = \"ten\"\n");

  TEST_CHECK(run_config(S("sosig.toml"), os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_is_printed(os, 2, "config key 'title' must be a string\n"));
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
}

// A `sosig.toml` that loads fine but cannot be written out fails rather than exiting 0 when nobody
// received the output. The host fails the write to `stdout` as a read-only descriptor does, with
// `EBADF`. Nothing is printed to `stderr`.
static void test_fails_on_unwritable_stdout(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "sosig.toml",
                  "title = \"Test Site\"\n"
                  "description = \"Example notes\"\n"
                  "author = \"Author Name\"\n"
                  "base_url = \"https://example.com\"\n");
  os_memory_fail_print(os, 1, 0, EBADF);

  TEST_CHECK(run_config(S("sosig.toml"), os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_is_printed(os, 2, ""));
}

TEST_LIST = {
    {"prints loaded config", test_prints_loaded_config},
    {"prints empty template arrays", test_prints_empty_template_arrays},
    {"prints directories resolved against config dir",
     test_prints_directories_resolved_against_config_dir},
    {"reports missing config", test_reports_missing_config},
    {"reports missing required key", test_reports_missing_required_key},
    {"reports first invalid key", test_reports_first_invalid_key},
    {"fails on unwritable stdout", test_fails_on_unwritable_stdout},
    {NULL, NULL},
};
