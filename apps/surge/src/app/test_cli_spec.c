#include <acutest.h>
#include <errno.h>

#include "app/cli_spec.h"
#include "cli/cli.h"
#include "cli/cli_dispatch.h"
#include "core/exit_code.h"
#include "core/str.h"
#include "domain/surge_version.h"
#include "markup/highlight.h"
#include "os/os_memory.h"
#include "testing/test_support.h"

/**
 * @brief Dispatches a command line with the app's own spec, as `main` does.
 *
 * @param argc Argument count.
 * @param argv Argument vector. Must not be `NULL`.
 * @param os   Host to run on. Must not be `NULL`.
 * @return The dispatch's exit code.
 */
static enum ExitCode dispatch(int argc, char** argv, struct Os* os) {
  struct CliSpec spec;
  cli_spec_init(&spec);
  return cli_dispatch(&spec, argc, argv, os);
}

// `--version` writes the surge version and the grammars the binary links to `stdout`, and reports
// success, so `cli_spec` gives the shared command line both.
static void test_version_action_reports_ok(void) {
  char* argv[] = {"surge", "--version", NULL};
  struct Os* os = os_memory();
  TEST_CHECK(dispatch(2, argv, os) == EXIT_CODE_OK);
  TEST_CHECK(test_text_is(os_memory_printed(os, 1), "surge %.*s\ngrammars: %.*s\n",
                          STR_FMT(surge_version_string()), STR_FMT(highlight_grammar_list())));
  TEST_CHECK(os_memory_is_printed(os, 2, ""));
}

// `config -c` is dispatched to `cmd_config_print` with the path it names.
static void test_config_command_is_dispatched(void) {
  char* argv[] = {"surge", "config", "-c", "site.toml", NULL};
  struct Os* os = os_memory();
  os_memory_write(os, "site.toml",
                  "title = \"t\"\ndescription = \"d\"\nauthor = \"a\"\nbase_url = \"https://x\"\n"
                  "[[repo]]\nname = \"r\"\npath = \"r\"\n");
  TEST_CHECK(dispatch(4, argv, os) == EXIT_CODE_OK);
  TEST_CHECK(str_startswith(os_memory_printed(os, 1), S("title = \"t\"\n")));
}

// `--help` writes surge's summary to `stdout`, named after `argv[0]`, and reports success, and
// `build --help` names what it builds and the default config file, both compared whole. A name that
// is not `"surge"` shows the text substitutes `argv[0]` rather than spelling the name.
static void test_help_action_reports_ok(void) {
  char* argv[] = {"/opt/bin/mysurge", "--help", NULL};
  struct Os* os = os_memory();
  TEST_CHECK(dispatch(2, argv, os) == EXIT_CODE_OK);
  TEST_CHECK(os_memory_is_printed(
      os, 1,
      "A static site generator for git repositories.\n"
      "\n"
      "USAGE:\n"
      "    /opt/bin/mysurge <command> [options]\n"
      "\n"
      "COMMANDS:\n"
      "    build   Build the site\n"
      "    config  Print the resolved configuration\n"
      "\n"
      "OPTIONS:\n"
      "    -h, --help     Print this help and exit\n"
      "    -V, --version  Print version information and exit\n"
      "\n"
      "Use '/opt/bin/mysurge <command> --help' for more information about a command.\n"));
  TEST_CHECK(os_memory_is_printed(os, 2, ""));

  char* build_argv[] = {"surge", "build", "--help", NULL};
  struct Os* build_os = os_memory();
  TEST_CHECK(dispatch(3, build_argv, build_os) == EXIT_CODE_OK);
  TEST_CHECK(os_memory_is_printed(
      build_os, 1,
      "Build the site.\n"
      "\n"
      "USAGE:\n"
      "    surge build [options]\n"
      "\n"
      "OPTIONS:\n"
      "    -c, --config PATH  Read the configuration from PATH (default: surge.toml)\n"
      "    -w, --workers N    Use at most N worker threads (default: detected CPU count)\n"
      "    -n, --dry-run      Report what would be written, kept, or removed, changing nothing\n"
      "    --force            Ignore the build cache for this build\n"
      "    -v, --verbose      Print build progress to stderr\n"
      "    -h, --help         Print this help and exit\n"));
}

// A rejected command line prints its diagnostic to `stderr` and reports a usage error.
static void test_parse_error_reports_usage_code(void) {
  char* argv[] = {"surge", "--bogus", NULL};
  struct Os* os = os_memory();
  TEST_CHECK(dispatch(2, argv, os) == EXIT_CODE_USAGE);
  TEST_CHECK(os_memory_is_printed(os, 2, "unknown option '--bogus'\n"));
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
}

// A help text that cannot be written exits with a failure and prints nothing else.
static void test_help_write_failure_reports_failure(void) {
  char* argv[] = {"surge", "--help", NULL};
  struct Os* os = os_memory();
  os_memory_fail_print(os, 1, 0, EPIPE);
  TEST_CHECK(dispatch(2, argv, os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_is_printed(os, 2, ""));
}

TEST_LIST = {
    {"version action reports ok", test_version_action_reports_ok},
    {"config command is dispatched", test_config_command_is_dispatched},
    {"help action reports ok", test_help_action_reports_ok},
    {"parse error reports usage code", test_parse_error_reports_usage_code},
    {"help write failure reports failure", test_help_write_failure_reports_failure},
    {NULL, NULL},
};
