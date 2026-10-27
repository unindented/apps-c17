#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "cli/cli.h"
#include "cli/cli_dispatch.h"
#include "core/arena.h"
#include "core/exit_code.h"
#include "core/log.h"
#include "core/str.h"
#include "os/os.h"
#include "os/os_memory.h"
#include "testing/test_support.h"

// Every dispatch here runs against the in-memory host, which keeps what the dispatch prints, with a
// test app whose commands record what reached them.

/** Largest config path the test commands keep, in bytes. */
enum { TEST_PATH_LEN_MAX = 64 };

/**
 * What the test app's commands received. The paths are copies, because the dispatch releases the
 * memory its own copies are in before it returns.
 */
struct Received {
  /** Options the last `build` received, but for the config path. */
  struct BuildOptions build_options;

  /** Copy of the config path the last `build` received, terminated. */
  char build_config_path[TEST_PATH_LEN_MAX];

  /** Number of `build` calls. */
  int build_count;

  /** Copy of the config path the last `config` received, terminated. */
  char config_path[TEST_PATH_LEN_MAX];

  /** Whether `config` fails its load. */
  bool is_config_failing;
};

/** What the test app's commands received in the current test. */
static struct Received received;

/**
 * @brief Copies `path` into `buffer`, terminated.
 *
 * @param path   Path to copy, shorter than `TEST_PATH_LEN_MAX`.
 * @param buffer Buffer of `TEST_PATH_LEN_MAX` bytes. Must not be `NULL`.
 */
static void copy_path(struct Str path, char* buffer) {
  TEST_ASSERT(path.len < TEST_PATH_LEN_MAX);
  for (ptrdiff_t i = 0; i < path.len; i++) {
    buffer[i] = path.data[i];
  }
  buffer[path.len] = '\0';
}

/**
 * @brief Records the options and succeeds, as `CliBuildFn`.
 *
 * @param options Options of the build. Must not be `NULL`.
 * @param os      Host, unused. Must not be `NULL`.
 * @return `EXIT_CODE_OK`.
 */
static enum ExitCode test_build(const struct BuildOptions* options, struct Os* os) {
  (void)os;
  received.build_options = *options;
  received.build_options.config_path = (struct Str){0};
  copy_path(options->config_path, received.build_config_path);
  received.build_count++;
  return EXIT_CODE_OK;
}

/**
 * @brief Records the config path, and either prints `config` or fails with a logged error, as
 *        `CliConfigPrintFn`.
 *
 * @param config_path Config file named.
 * @param os          Host to print on. Must not be `NULL`.
 * @param perm        Unused. Must not be `NULL`.
 * @param scratch     Unused.
 * @param log         Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` for a failing load.
 */
static int test_config_print(struct Str config_path,
                             struct Os* os,
                             struct Arena* perm,
                             struct Arena scratch,
                             struct Log* log) {
  (void)scratch;
  (void)perm;
  copy_path(config_path, received.config_path);
  if (received.is_config_failing) {
    return log_error(log, "unknown config key 'colour'");
  }
  return os_print(os, 1, S("config\n"));
}

/** What the test app tells the command line about itself. */
static const struct CliSpec TEST_SPEC = {
    .description = STR_INIT("A static tool."),
    .noun = STR_INIT("thing"),
    .config_path_default = STR_INIT("tool.toml"),
    .generator = STR_INIT("tool 1.0"),
    .build = test_build,
    .config_print = test_config_print,
};

/**
 * @brief Dispatches a terminated argument vector with `TEST_SPEC`, after clearing what the commands
 *        received.
 *
 * @param argv `NULL`-terminated argument vector. Must not be `NULL`.
 * @param os   Host to run on. Must not be `NULL`.
 * @return The dispatch's exit code.
 */
static enum ExitCode dispatch(char** argv, struct Os* os) {
  const bool is_config_failing = received.is_config_failing;
  received = (struct Received){.is_config_failing = is_config_failing};
  int argc = 0;
  while (argv[argc] != NULL) {
    argc++;
  }
  return cli_dispatch(&TEST_SPEC, argc, argv, os);
}

// `build` reaches the app's build with every parsed option, the config path included.
static void test_build_command_receives_parsed_options(void) {
  char* argv[] = {"tool", "build", "-c", "sub/tool.toml", "-w", "3", "-v", "-n", "--force", NULL};
  struct Os* os = os_memory();
  TEST_CHECK(dispatch(argv, os) == EXIT_CODE_OK);
  TEST_CHECK(received.build_count == 1);
  TEST_CHECK(strcmp(received.build_config_path, "sub/tool.toml") == 0);
  TEST_CHECK(received.build_options.worker_count == 3);
  TEST_CHECK(received.build_options.is_verbose && received.build_options.is_dry_run &&
             received.build_options.is_forced);
}

// `config` reaches the app's config print with the default path, and prints what it prints.
static void test_config_command_prints_the_config(void) {
  char* argv[] = {"tool", "config", NULL};
  struct Os* os = os_memory();
  TEST_CHECK(dispatch(argv, os) == EXIT_CODE_OK);
  TEST_CHECK(received.build_count == 0);
  TEST_CHECK(strcmp(received.config_path, "tool.toml") == 0);
  TEST_CHECK(os_memory_is_printed(os, 1, "config\n"));
  TEST_CHECK(os_memory_is_printed(os, 2, ""));
}

// `--version` writes the version to `stdout` and reports success.
static void test_version_action_reports_ok(void) {
  char* argv[] = {"tool", "--version", NULL};
  struct Os* os = os_memory();
  TEST_CHECK(dispatch(argv, os) == EXIT_CODE_OK);
  TEST_CHECK(os_memory_is_printed(os, 1, "tool 1.0\n"));
}

// `--help` writes the usage text named after `argv[0]` and reports success. `test_cli.c` pins the
// text itself, so this test asserts only the plumbing.
static void test_help_action_reports_ok(void) {
  char* argv[] = {"/opt/bin/mytool", "--help", NULL};
  struct Os* os = os_memory();
  TEST_CHECK(dispatch(argv, os) == EXIT_CODE_OK);
  struct Arena arena = test_arena();
  TEST_CHECK(strstr(str_cstr(os_memory_printed(os, 1), &arena),
                    "    /opt/bin/mytool <command> [options]\n") != NULL);
}

// A config that fails to load prints the load's errors to `stderr` and fails the command.
static void test_config_command_reports_a_failed_load(void) {
  char* argv[] = {"tool", "config", NULL};
  struct Os* os = os_memory();
  received.is_config_failing = true;
  TEST_CHECK(dispatch(argv, os) == EXIT_CODE_FAILURE);
  received.is_config_failing = false;
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
  TEST_CHECK(os_memory_is_printed(os, 2, "unknown config key 'colour'\n"));
}

// A command line that fails to parse reports `EXIT_CODE_USAGE`, and the parser's own diagnostic
// goes to `stderr` rather than `stdout`, so a shell or CI wrapper tells a bad invocation from a
// failed build.
static void test_parse_error_reports_usage_code(void) {
  char* argv[] = {"tool", "frobnicate", NULL};
  struct Os* os = os_memory();
  TEST_CHECK(dispatch(argv, os) == EXIT_CODE_USAGE);
  TEST_CHECK(os_memory_is_printed(os, 1, ""));
  TEST_CHECK(os_memory_is_printed(os, 2, "unknown command 'frobnicate'\n"));
}

// A `stdout` that cannot be written turns the version and the help into `EXIT_CODE_FAILURE` and
// prints nothing to `stderr`. The host fails the write as a read-only descriptor does.
static void test_print_failure_reports_failure(void) {
  char* version_argv[] = {"tool", "--version", NULL};
  char* help_argv[] = {"tool", "--help", NULL};
  char** argvs[] = {version_argv, help_argv};
  for (ptrdiff_t i = 0; i < 2; i++) {
    struct Os* os = os_memory();
    os_memory_fail_print(os, 1, 0, EBADF);
    TEST_CHECK(dispatch(argvs[i], os) == EXIT_CODE_FAILURE);
    TEST_CHECK(os_memory_is_printed(os, 2, ""));
  }
}

TEST_LIST = {
    {"build command receives parsed options", test_build_command_receives_parsed_options},
    {"config command prints the config", test_config_command_prints_the_config},
    {"version action reports ok", test_version_action_reports_ok},
    {"help action reports ok", test_help_action_reports_ok},
    {"config command reports a failed load", test_config_command_reports_a_failed_load},
    {"parse error reports usage code", test_parse_error_reports_usage_code},
    {"print failure reports failure", test_print_failure_reports_failure},
    {NULL, NULL},
};
