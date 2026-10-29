#include <acutest.h>
#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "app/cli_spec.h"
#include "cli/cli.h"
#include "cli/cli_dispatch.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/error.h"
#include "core/exit_code.h"
#include "core/str.h"
#include "domain/sosig_version.h"
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

// Every dispatch here runs against the in-memory host, whose root is the working directory a
// command resolves its config path from, and which keeps what the command prints.

/**
 * @brief Returns a fresh host holding a minimal buildable site with two sources.
 *
 * @param site_dir Terminated directory of the site, ending in `/`, or `""` for the root itself.
 *                 Must not be `NULL`.
 * @return The host.
 */
static struct Os* site_host(const char* site_dir) {
  static const char config[] =
      "title = \"Site\"\n"
      "description = \"Example notes\"\n"
      "author = \"Author\"\n"
      "base_url = \"https://example.com\"\n"
      "aggregate_templates = []\n"
      "feed_templates = []\n";
  static const char entry[] =
      "+++\n"
      "title = \"Hello\"\n"
      "date = 2026-07-01T00:00:00Z\n"
      "+++\n"
      "Body\n";
  static const struct {
    const char* name;
    const char* contents;
  } files[] = {
      {"sosig.toml", config},
      {"content/hello.md", entry},
      {"content/world.md", entry},
      {"templates/content.html", "{{{body_html}}}\n"},
  };
  struct Os* os = os_memory();
  struct Arena arena = test_arena();
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(files) / sizeof(files[0])); i++) {
    struct Buf path = {.arena = &arena};
    buf_printf(&path, "%s%s", site_dir, files[i].name);
    os_memory_write(os, buf_cstr(&path), files[i].contents);
  }
  return os;
}

// `--version` writes the sosig version and the grammars the binary links to `stdout`, and reports
// success, so `cli_spec` gives the shared command line both.
static void test_version_action_reports_ok(void) {
  char* argv[] = {"sosig", "--version", NULL};
  struct Os* os = os_memory();
  TEST_CHECK(dispatch(2, argv, os) == EXIT_CODE_OK);
  TEST_CHECK(test_text_is(os_memory_printed(os, 1), "sosig %.*s\ngrammars: %.*s\n",
                          STR_FMT(sosig_version_string()), STR_FMT(highlight_grammar_list())));
  TEST_CHECK(os_memory_is_printed(os, 2, ""));
}

// `--help` writes sosig's summary to `stdout`, named after `argv[0]`, and reports success, and
// `build --help` names what it builds and the default config file, both compared whole. A name that
// is not `"sosig"` shows the text substitutes `argv[0]` rather than spelling the name.
static void test_help_action_reports_ok(void) {
  char* argv[] = {"/opt/bin/mysosig", "--help", NULL};
  struct Os* os = os_memory();
  TEST_CHECK(dispatch(2, argv, os) == EXIT_CODE_OK);
  TEST_CHECK(os_memory_is_printed(
      os, 1,
      "A static site generator.\n"
      "\n"
      "USAGE:\n"
      "    /opt/bin/mysosig <command> [options]\n"
      "\n"
      "COMMANDS:\n"
      "    build   Build the site\n"
      "    config  Print the resolved configuration\n"
      "\n"
      "OPTIONS:\n"
      "    -h, --help     Print this help and exit\n"
      "    -V, --version  Print version information and exit\n"
      "\n"
      "Use '/opt/bin/mysosig <command> --help' for more information about a command.\n"));
  TEST_CHECK(os_memory_is_printed(os, 2, ""));

  char* build_argv[] = {"sosig", "build", "--help", NULL};
  struct Os* build_os = os_memory();
  TEST_CHECK(dispatch(3, build_argv, build_os) == EXIT_CODE_OK);
  TEST_CHECK(os_memory_is_printed(
      build_os, 1,
      "Build the site.\n"
      "\n"
      "USAGE:\n"
      "    sosig build [options]\n"
      "\n"
      "OPTIONS:\n"
      "    -c, --config PATH  Read the configuration from PATH (default: sosig.toml)\n"
      "    -w, --workers N    Use at most N worker threads (default: detected CPU count)\n"
      "    -n, --dry-run      Report what would be written, kept, or removed, changing nothing\n"
      "    --force            Make every output again, keeping nothing\n"
      "    -v, --verbose      Print build progress to stderr\n"
      "    -h, --help         Print this help and exit\n"));
}

// The `config` command is dispatched to `cmd_config_print`, not to a build: the resolved
// configuration lands on `stdout` and no `public/` directory appears. Dispatching it to the wrong
// command would otherwise still exit `0` in a valid site.
static void test_config_command_is_dispatched(void) {
  struct Os* os = site_host("");
  char* argv[] = {"sosig", "config", NULL};
  TEST_CHECK(dispatch(2, argv, os) == EXIT_CODE_OK);
  TEST_CHECK(os_memory_is_printed(os, 1,
                                  "title = \"Site\"\n"
                                  "description = \"Example notes\"\n"
                                  "author = \"Author\"\n"
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
  TEST_CHECK(os_memory_is_printed(os, 2, ""));
  // No page was written, which is what separates `config` from `build`.
  TEST_CHECK(!os_memory_has_path(os, "public/hello.html"));
}

// The `build` command is dispatched to `cmd_build_run` with the parsed options attached, so the
// worker count, the verbose flag, the dry-run flag and the force flag reach the build rather than
// being dropped on the way. A dry run prints its listing and leaves no `public/`, which only a
// carried flag explains. The verbose phase lines naming the requested count, and the one saying the
// output cache is ignored, are the only externally visible evidence that the other three fields
// were carried across. The test asks for one worker, so a dropped count would show the detected
// count, on a host with more than one core. One worker also finishes the jobs in order, so the
// progress lines can be compared.
static void test_build_command_receives_parsed_options(void) {
  struct Os* dry_run_os = site_host("");
  char* dry_run_argv[] = {"sosig", "build", "--dry-run", NULL};
  TEST_CHECK(dispatch(3, dry_run_argv, dry_run_os) == EXIT_CODE_OK);
  TEST_CHECK(os_memory_is_printed(dry_run_os, 1,
                                  "write\tpublic/hello.html\n"
                                  "write\tpublic/world.html\n"
                                  "write\tpublic/_sosig/output-cache.tsv\n"));
  TEST_CHECK(os_memory_is_printed(dry_run_os, 2, ""));
  TEST_CHECK(!os_memory_has_path(dry_run_os, "public"));

  struct Os* os = site_host("");
  char* argv[] = {"sosig", "build", "--verbose", "--workers=1", "--force", NULL};
  TEST_CHECK(dispatch(5, argv, os) == EXIT_CODE_OK);
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
  TEST_CHECK(os_memory_is_printed(os, 2,
                                  "loading config\n"
                                  "discovering content\n"
                                  "discovering templates and static files\n"
                                  "parsing content, workers: 1\n"
                                  "\rparsing content 1/2\rparsing content 2/2\n"
                                  "collecting content entries\n"
                                  "compiling templates\n"
                                  "building output manifest\n"
                                  "ignoring output cache, making every output\n"
                                  "converting bodies, workers: 1\n"
                                  "\rconverting bodies 1/2\rconverting bodies 2/2\n"
                                  "rendering content, workers: 1\n"
                                  "\rrendering content 1/2\rrendering content 2/2\n"
                                  "rendering aggregate templates\n"
                                  "rendering feed templates\n"
                                  "copying static files\n"
                                  "writing output cache\n"
                                  "removing stale outputs\n"
                                  "removed stale outputs: 0\n"
                                  "build complete\n"));
  TEST_CHECK(os_memory_has_path(os, "public/hello.html"));
  TEST_CHECK(os_memory_has_path(os, "public/world.html"));
}

// `-c` reaches both commands, from a working directory that holds no `sosig.toml`. `config` prints
// each directory resolved against the config's directory, and `build` writes below it.
static void test_config_option_reaches_both_commands(void) {
  struct Os* config_os = site_host("sub/");
  char* config_argv[] = {"sosig", "config", "-c", "sub/sosig.toml", NULL};
  TEST_CHECK(dispatch(4, config_argv, config_os) == EXIT_CODE_OK);
  TEST_CHECK(os_memory_is_printed(config_os, 1,
                                  "title = \"Site\"\n"
                                  "description = \"Example notes\"\n"
                                  "author = \"Author\"\n"
                                  "base_url = \"https://example.com\"\n"
                                  "permalink = \"/{section}/{slug}.html\"\n"
                                  "content_dir = \"sub/content\"\n"
                                  "output_dir = \"sub/public\"\n"
                                  "templates_dir = \"sub/templates\"\n"
                                  "static_dir = \"sub/static\"\n"
                                  "content_template = \"content.html\"\n"
                                  "aggregate_templates = []\n"
                                  "feed_templates = []\n"
                                  "feed_count = 25\n"
                                  "frontmatter_ignored_keys = []\n"));
  TEST_CHECK(os_memory_is_printed(config_os, 2, ""));

  struct Os* build_os = site_host("sub/");
  char* build_argv[] = {"sosig", "build", "--config=sub/sosig.toml", NULL};
  TEST_CHECK(dispatch(3, build_argv, build_os) == EXIT_CODE_OK);
  TEST_CHECK(os_memory_is_printed(build_os, 2, ""));
  TEST_CHECK(os_memory_has_path(build_os, "sub/public/hello.html"));
  TEST_CHECK(os_memory_has_path(build_os, "sub/public/world.html"));
  TEST_CHECK(!os_memory_has_path(build_os, "public"));
}

// A command line that fails to parse reports `EXIT_CODE_USAGE`, and the parser's own diagnostic
// goes to `stderr` rather than `stdout`. The exit code is what nothing else pins: `2` is what a
// shell or CI wrapper branches on to tell a bad invocation from a failed build, and
// `libs/src/cli/test_cli.c` asserts only that `cli_parse` produced the message.
static void test_parse_error_reports_usage_code(void) {
  char* argv[] = {"sosig", "--bogus", NULL};
  struct Os* os = os_memory();
  TEST_CHECK(dispatch(2, argv, os) == EXIT_CODE_USAGE);
  TEST_CHECK(os_memory_is_printed(os, 2, "unknown option '--bogus'\n"));
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
}

// A `stdout` that cannot be written turns a successful print into `EXIT_CODE_FAILURE` and prints
// nothing to `stderr`. Nothing else in the suite reaches this arm. Other tests make a printer fail,
// but none goes through `cli_dispatch`, so nothing else pins the mapping from a printer's non-zero
// result to an exit code. The host fails the write as a read-only descriptor does, with `EBADF`.
static void test_version_write_failure_reports_failure(void) {
  char* argv[] = {"sosig", "--version", NULL};
  struct Os* os = os_memory();
  os_memory_fail_print(os, 1, 0, EBADF);
  TEST_CHECK(dispatch(2, argv, os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_is_printed(os, 2, ""));
}

// A dry run whose listing cannot be printed fails the build with a diagnostic, rather than
// reporting success for a listing nobody saw. The host fails the print with `EBADF`.
static void test_dry_run_print_failure_reports_failure(void) {
  struct Os* os = site_host("");
  os_memory_fail_print(os, 1, 0, EBADF);
  char* argv[] = {"sosig", "build", "--dry-run", NULL};
  TEST_CHECK(dispatch(3, argv, os) == EXIT_CODE_FAILURE);
  char reason[ERROR_SYSTEM_MESSAGE_SIZE];
  struct Arena arena = test_arena();
  struct Buf expected = {.arena = &arena};
  buf_printf(&expected, "failed to print dry run: %s\n",
             error_system_message(reason, sizeof(reason), EBADF));
  TEST_CHECK(os_memory_is_printed(os, 2, buf_cstr(&expected)));
}

TEST_LIST = {
    {"version action reports ok", test_version_action_reports_ok},
    {"help action reports ok", test_help_action_reports_ok},
    {"config command is dispatched", test_config_command_is_dispatched},
    {"build command receives parsed options", test_build_command_receives_parsed_options},
    {"config option reaches both commands", test_config_option_reaches_both_commands},
    {"parse error reports usage code", test_parse_error_reports_usage_code},
    {"version write failure reports failure", test_version_write_failure_reports_failure},
    {"dry run print failure reports failure", test_dry_run_print_failure_reports_failure},
    {NULL, NULL},
};
