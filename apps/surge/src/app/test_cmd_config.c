#include <acutest.h>
#include <errno.h>

#include "app/cli_spec.h"
#include "cli/cli.h"
#include "cli/cli_dispatch.h"
#include "core/arena.h"
#include "core/exit_code.h"
#include "core/str.h"
#include "os/os_memory.h"
#include "testing/test_support.h"

/** A config with every required key and one repo. */
#define CONFIG_TEXT                          \
  "title = \"Code\"\n"                       \
  "description = \"Repositories\"\n"         \
  "author = \"Ada\"\n"                       \
  "base_url = \"https://git.example.org\"\n" \
  "[[repo]]\n"                               \
  "name = \"one\"\n"                         \
  "path = \"one\"\n"

/**
 * @brief Runs `surge config -c <config_path>` with the app's own spec, as `main` does.
 *
 * @param config_path Config file to name.
 * @param os          Host to run on. Must not be `NULL`.
 * @return The command's exit code.
 */
static enum ExitCode run_config(struct Str config_path, struct Os* os) {
  struct Arena arena = test_arena();
  char* argv[] = {"surge", "config", "-c", str_cstr(config_path, &arena), NULL};
  struct CliSpec spec;
  cli_spec_init(&spec);
  return cli_dispatch(&spec, 4, argv, os);
}

// A valid config prints resolved, starting with the site keys, and nothing goes to `stderr`.
// `test_site_config.c` pins the whole text, so this checks the plumbing and the resolved path.
static void test_prints_resolved_config(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "site/surge.toml", CONFIG_TEXT);
  TEST_CHECK(run_config(S("site/surge.toml"), os) == EXIT_CODE_OK);
  const struct Str printed = os_memory_printed(os, 1);
  TEST_CHECK(str_startswith(printed, S("title = \"Code\"\n")));
  TEST_CHECK(str_endswith(printed, S("[[repo]]\nname = \"one\"\npath = \"site/one\"\n")));
  TEST_CHECK(os_memory_is_printed(os, 2, ""));
}

// A config with a bad key prints the error to `stderr` and no config.
static void test_reports_bad_keys(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "surge.toml", CONFIG_TEXT "colour = 1\n");
  TEST_CHECK(run_config(S("surge.toml"), os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_is_printed(os, 2, "unknown config key 'repo[0].colour'\n"));
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
}

// A missing config names the path it read.
static void test_reports_missing_config(void) {
  struct Os* os = os_memory();
  TEST_CHECK(run_config(S("surge.toml"), os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_is_printed(
      os, 2, "failed to read config: No such file or directory ('surge.toml')\n"));
}

// A config that cannot be written to `stdout` fails, and prints nothing to `stderr`.
static void test_reports_print_failure(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "surge.toml", CONFIG_TEXT);
  os_memory_fail_print(os, 1, 0, EPIPE);
  TEST_CHECK(run_config(S("surge.toml"), os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_is_printed(os, 2, ""));
}

TEST_LIST = {
    {"prints resolved config", test_prints_resolved_config},
    {"reports bad keys", test_reports_bad_keys},
    {"reports missing config", test_reports_missing_config},
    {"reports print failure", test_reports_print_failure},
    {NULL, NULL},
};
