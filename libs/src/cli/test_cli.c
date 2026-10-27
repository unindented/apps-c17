#include <acutest.h>
#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "cli/cli.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/str.h"
#include "os/os_memory.h"
#include "testing/test_support.h"

// The printing tests run against the in-memory host, which keeps what `cli_print_version` and
// `cli_print_usage` print. The parsing tests reach no host.

/** What a test app tells the command line about itself. */
static const struct CliSpec TEST_SPEC = {
    .description = STR_INIT("A static tool."),
    .noun = STR_INIT("thing"),
    .config_path_default = STR_INIT("tool.toml"),
    .generator = STR_INIT("tool 1.0"),
    .grammars = STR_INIT("c, json"),
};

/**
 * @brief Parses a terminated argument vector with `TEST_SPEC`.
 *
 * @param argv `NULL`-terminated argument vector. Must not be `NULL`.
 * @return The resolved CLI options.
 */
static struct CliOptions parse(char** argv) {
  int argc = 0;
  while (argv[argc] != NULL) {
    argc++;
  }
  // The message lives in a reservation that is never released, so it outlives this call.
  struct Arena arena = test_arena();
  struct CliOptions options;
  cli_parse(&TEST_SPEC, argc, argv, &arena, &options);
  return options;
}

/**
 * @brief Prints the usage text of `command` for the program `tool` of `TEST_SPEC` on a fresh host.
 *
 * @param command Command whose help to print.
 * @return The host, whose standard output holds the text. Records a failure when the print fails.
 */
static struct Os* print_usage(enum CliCommand command) {
  struct Os* os = os_memory();
  TEST_CHECK(cli_print_usage(&TEST_SPEC, S("tool"), command, os, test_arena()) == 0);
  return os;
}

// The `build` command resolves to the run action with no worker override.
static void test_build_command_runs(void) {
  char* argv[] = {"tool", "build", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.command == CLI_COMMAND_BUILD);
  TEST_CHECK(options.worker_count == 0);
}

// The `config` command resolves to the run action with the default config path.
static void test_config_command_runs(void) {
  char* argv[] = {"tool", "config", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.command == CLI_COMMAND_CONFIG);
  TEST_CHECK(str_eq(options.config_path, S("tool.toml")));
}

// `-c` and `--config` name the config file, before or after the command, for both commands.
static void test_config_option_sets_path(void) {
  char* short_argv[] = {"tool", "-c", "site/tool.toml", "config", NULL};
  char* long_argv[] = {"tool", "config", "--config=other.toml", NULL};
  char* build_argv[] = {"tool", "build", "--config", "sub/tool.toml", NULL};
  struct CliOptions short_options = parse(short_argv);
  struct CliOptions long_options = parse(long_argv);
  struct CliOptions build_options = parse(build_argv);
  TEST_CHECK(short_options.action == CLI_ACTION_RUN);
  TEST_CHECK(str_eq(short_options.config_path, S("site/tool.toml")));
  TEST_CHECK(long_options.action == CLI_ACTION_RUN);
  TEST_CHECK(str_eq(long_options.config_path, S("other.toml")));
  TEST_CHECK(build_options.action == CLI_ACTION_RUN);
  TEST_CHECK(build_options.command == CLI_COMMAND_BUILD);
  TEST_CHECK(str_eq(build_options.config_path, S("sub/tool.toml")));
}

// `--verbose` enables verbose output.
static void test_verbose_long_flag(void) {
  char* argv[] = {"tool", "build", "--verbose", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.is_verbose);
}

// `-v` enables verbose output.
static void test_verbose_short_flag(void) {
  char* argv[] = {"tool", "build", "-v", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.is_verbose);
}

// `--dry-run` asks the build to report rather than change.
static void test_dry_run_long_flag(void) {
  char* argv[] = {"tool", "build", "--dry-run", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.is_dry_run);
}

// `-n` asks the build to report rather than change.
static void test_dry_run_short_flag(void) {
  char* argv[] = {"tool", "build", "-n", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.is_dry_run);
}

// `--dry-run` combines with the other build flags, each keeping its own effect.
static void test_dry_run_with_other_build_flags(void) {
  char* argv[] = {"tool", "build", "--dry-run", "-w", "2", "-v", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.is_dry_run);
  TEST_CHECK(options.worker_count == 2);
  TEST_CHECK(options.is_verbose);
}

// `--force` asks the build to ignore its build cache. It has no short spelling.
static void test_force_long_flag(void) {
  char* argv[] = {"tool", "build", "--force", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.is_forced);
  TEST_CHECK(!options.is_dry_run);
}

// `--workers 4` sets the worker count from a separate token.
static void test_workers_long_flag_separate_value(void) {
  char* argv[] = {"tool", "build", "--workers", "4", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.worker_count == 4);
}

// `-w 8` sets the worker count from a separate token.
static void test_workers_short_flag_separate_value(void) {
  char* argv[] = {"tool", "build", "-w", "8", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.worker_count == 8);
}

// `-w4` sets the worker count from a value attached to the flag.
static void test_workers_short_flag_attached_value(void) {
  char* argv[] = {"tool", "build", "-w4", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.worker_count == 4);
}

// `--workers=16` sets the worker count from an `=`-joined value.
static void test_workers_long_flag_equals_value(void) {
  char* argv[] = {"tool", "build", "--workers=16", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.worker_count == 16);
}

// An `=`-joined value inside a short cluster reaches the one flag it is attached to, leaving the
// valueless flag ahead of it alone. This is the case the rejection in
// `test_attached_value_rejected_on_valueless_short_flags` must not claim. Both flags live in one
// `argv` element, so a check that only asked whether that element contains an `=` would reject
// `--verbose` here for a value belonging to `--workers`.
static void test_workers_short_flag_in_cluster_equals_value(void) {
  char* argv[] = {"tool", "build", "-vw=4", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.is_verbose);
  TEST_CHECK(options.worker_count == 4);
}

// A command flag before the command and another after it are both applied in one parse. The
// all-flags-after-the-command placement is `test_options_after_build_command`.
static void test_mixed_flag_order(void) {
  char* argv[] = {"tool", "-v", "build", "--workers", "2", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.command == CLI_COMMAND_BUILD);
  TEST_CHECK(options.is_verbose);
  TEST_CHECK(options.worker_count == 2);
}

// `argv[0]` survives a parse that permutes the rest of the vector, which is the clause
// `cli_dispatch` depends on. It reads `argv[0]` after `cli_parse` returns to name the program in
// the usage text. This command line reorders because the option follows the positional. The test
// fails if a copt update rotates the complete vector. It also fails if `copt_init` stops preserving
// `argv[0]`.
static void test_program_name_survives_permutation(void) {
  char* argv[] = {"tool", "build", "--workers", "2", NULL};
  char* program_name = argv[0];
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.worker_count == 2);
  TEST_CHECK(argv[0] == program_name);
  TEST_CHECK(strcmp(argv[0], "tool") == 0);
}

// Flags placed after the command are still parsed.
static void test_options_after_build_command(void) {
  char* argv[] = {"tool", "build", "-w", "3", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.worker_count == 3);
}

// `--help` with no command resolves to the help action with no command selected.
static void test_help_long_flag(void) {
  char* argv[] = {"tool", "--help", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_HELP);
  TEST_CHECK(options.command == CLI_COMMAND_NONE);
}

// `-h` with no command resolves to the help action with no command selected.
static void test_help_short_flag(void) {
  char* argv[] = {"tool", "-h", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_HELP);
  TEST_CHECK(options.command == CLI_COMMAND_NONE);
}

// `build --help` resolves to the help action scoped to the build command.
static void test_build_command_help(void) {
  char* argv[] = {"tool", "build", "--help", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_HELP);
  TEST_CHECK(options.command == CLI_COMMAND_BUILD);
}

// `config --help` resolves to the help action scoped to the config command.
static void test_config_command_help(void) {
  char* argv[] = {"tool", "config", "--help", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_HELP);
  TEST_CHECK(options.command == CLI_COMMAND_CONFIG);
}

// `--version` resolves to the version action.
static void test_version_long_flag(void) {
  char* argv[] = {"tool", "--version", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_VERSION);
  TEST_CHECK(options.error_message.len == 0);
}

// `-V` resolves to the version action.
static void test_version_short_flag(void) {
  char* argv[] = {"tool", "-V", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_VERSION);
}

// Version wins over help regardless of their order.
static void test_version_flag_wins_over_help(void) {
  char* help_first[] = {"tool", "--help", "--version", NULL};
  struct CliOptions options = parse(help_first);
  TEST_CHECK(options.action == CLI_ACTION_VERSION);

  char* version_first[] = {"tool", "--version", "--help", NULL};
  options = parse(version_first);
  TEST_CHECK(options.action == CLI_ACTION_VERSION);
}

// An informational flag takes priority over a parse error, and discards the diagnostic that error
// recorded, so `error_message` never describes an outcome other than the one selected. The cluster
// case is here because it looks like an exception and is not. `-Vx` is `-V` alongside the unknown
// option `-x`, the same shape as the two spelled-out cases above, so it prints the version rather
// than reporting the `-x`. A value glued to the flag itself is the shape that does not survive this
// precedence, per `test_attached_value_rejected_on_valueless_short_flags`.
static void test_informational_flag_clears_diagnostic(void) {
  char* help_argv[] = {"tool", "--frobnicate", "--help", NULL};
  struct CliOptions options = parse(help_argv);
  TEST_CHECK(options.action == CLI_ACTION_HELP);
  TEST_CHECK(options.error_message.len == 0);

  char* version_argv[] = {"tool", "--frobnicate", "--version", NULL};
  options = parse(version_argv);
  TEST_CHECK(options.action == CLI_ACTION_VERSION);
  TEST_CHECK(options.error_message.len == 0);

  char* cluster_argv[] = {"tool", "-Vx", NULL};
  options = parse(cluster_argv);
  TEST_CHECK(options.action == CLI_ACTION_VERSION);
  TEST_CHECK(options.error_message.len == 0);
}

// A valid `--version` wins in either order next to a rejected `--version=1`. The rejected spelling
// neither requests the version nor cancels the valid request, so the result does not depend on
// which comes last.
static void test_valid_version_flag_wins_over_rejected_spelling(void) {
  char* valid_first[] = {"tool", "--version", "--version=1", NULL};
  struct CliOptions options = parse(valid_first);
  TEST_CHECK(options.action == CLI_ACTION_VERSION);
  TEST_CHECK(options.error_message.len == 0);

  char* rejected_first[] = {"tool", "--version=1", "--version", NULL};
  options = parse(rejected_first);
  TEST_CHECK(options.action == CLI_ACTION_VERSION);
  TEST_CHECK(options.error_message.len == 0);
}

// A non-numeric worker count is rejected with a diagnostic naming the offending value.
static void test_invalid_worker_count_rejected(void) {
  char* argv[] = {"tool", "build", "-w", "abc", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(
      str_eq(options.error_message, S("option '--workers' must be a positive integer: 'abc'")));
}

// A worker count of zero is rejected, naming the offending value.
static void test_zero_worker_count_rejected(void) {
  char* argv[] = {"tool", "build", "--workers", "0", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(
      str_eq(options.error_message, S("option '--workers' must be a positive integer: '0'")));
}

// A negative worker count in attached form reaches `parse_size`'s sign rejection, and the
// diagnostic names the offending value. Attached form (`=`) is required so the `-1` is taken as the
// option's value rather than a separate token.
static void test_negative_worker_count_rejected(void) {
  char* argv[] = {"tool", "build", "--workers=-1", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(
      str_eq(options.error_message, S("option '--workers' must be a positive integer: '-1'")));
}

// A worker count above the CPU count is accepted. The build caps it at four per online CPU.
static void test_large_worker_count_accepted(void) {
  char* argv[] = {"tool", "build", "--workers", "5000", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.worker_count == 5000);
}

// `-w` or `--workers` with no following value is rejected with a diagnostic that it requires a
// worker count. Both spellings name the option `--workers`, as its other diagnostics do.
static void test_missing_worker_count_rejected(void) {
  char* short_argv[] = {"tool", "build", "-w", NULL};
  struct CliOptions options = parse(short_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(str_eq(options.error_message, S("option '--workers' requires a worker count")));

  char* long_argv[] = {"tool", "build", "--workers", NULL};
  options = parse(long_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(str_eq(options.error_message, S("option '--workers' requires a worker count")));
}

// A flag that takes no value rejects an attached `=value` and names the complete token. copt
// matches only the text before `=`. Without this check, it discards the value and enables
// `--verbose=0`. The test covers all five flags that take no value. `--version` and `--help` clear
// `error_message`, so they could otherwise hide this rejection. The
// `test_workers_long_flag_equals_value` test confirms that `--workers=N` still works.
static void test_attached_value_rejected_on_valueless_flags(void) {
  char* verbose_argv[] = {"tool", "build", "--verbose=0", NULL};
  struct CliOptions options = parse(verbose_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(!options.is_verbose);
  TEST_CHECK(str_eq(options.error_message, S("option does not take a value: '--verbose=0'")));

  char* dry_run_argv[] = {"tool", "build", "--dry-run=0", NULL};
  options = parse(dry_run_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(!options.is_dry_run);
  TEST_CHECK(str_eq(options.error_message, S("option does not take a value: '--dry-run=0'")));

  char* force_argv[] = {"tool", "build", "--force=1", NULL};
  options = parse(force_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(!options.is_forced);
  TEST_CHECK(str_eq(options.error_message, S("option does not take a value: '--force=1'")));

  char* help_argv[] = {"tool", "--help=nope", NULL};
  options = parse(help_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(str_eq(options.error_message, S("option does not take a value: '--help=nope'")));

  char* version_argv[] = {"tool", "--version=nope", NULL};
  options = parse(version_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(str_eq(options.error_message, S("option does not take a value: '--version=nope'")));
}

// The short form of each valueless flag rejects an attached value and names the complete element.
// copt stops its one-letter comparison at `=`, so `-V=1` matches `V`. Without this check, the short
// form would print the version while the long form failed. An informational flag clears
// `error_message`, so the rejection must prevent `-V` from becoming a version request.
static void test_attached_value_rejected_on_valueless_short_flags(void) {
  char* verbose_argv[] = {"tool", "build", "-v=0", NULL};
  struct CliOptions options = parse(verbose_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(!options.is_verbose);
  TEST_CHECK(str_eq(options.error_message, S("option does not take a value: '-v=0'")));

  char* help_argv[] = {"tool", "-h=1", NULL};
  options = parse(help_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(str_eq(options.error_message, S("option does not take a value: '-h=1'")));

  char* version_argv[] = {"tool", "-V=1", NULL};
  options = parse(version_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(str_eq(options.error_message, S("option does not take a value: '-V=1'")));
}

// A rejected attached value ends its short cluster, so the letters after the `=` are not read as
// flags. Otherwise the `V` in `-v=V` would request the version and the `h` in `-v=h` would request
// help, and either would clear the rejection.
static void test_attached_value_rejection_ends_short_cluster(void) {
  char* version_argv[] = {"tool", "-v=V", NULL};
  struct CliOptions options = parse(version_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(!options.is_verbose);
  TEST_CHECK(str_eq(options.error_message, S("option does not take a value: '-v=V'")));

  char* help_argv[] = {"tool", "-v=h", NULL};
  options = parse(help_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(str_eq(options.error_message, S("option does not take a value: '-v=h'")));
}

// An unrecognized short option is rejected with a diagnostic naming the option.
static void test_unknown_short_option(void) {
  char* argv[] = {"tool", "build", "-x", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(str_eq(options.error_message, S("unknown option '-x'")));
}

// A value attached to an unknown short option ends its cluster, so the `V` in `-x=V` does not
// request the version. The diagnostic names the unknown option rather than the value.
static void test_unknown_short_option_value_ends_cluster(void) {
  char* argv[] = {"tool", "build", "-x=V", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(str_eq(options.error_message, S("unknown option '-x'")));
}

// An unrecognized long option is rejected with a diagnostic naming the option.
static void test_unknown_long_option(void) {
  char* argv[] = {"tool", "build", "--frobnicate", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(str_eq(options.error_message, S("unknown option '--frobnicate'")));
}

// `-c` with no path is rejected with a diagnostic that names the option by its long spelling.
static void test_config_option_requires_path(void) {
  char* argv[] = {"tool", "config", "-c", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(str_eq(options.error_message, S("option '--config' requires a path")));
}

// `--config=` gives an empty path, which names the option as a missing path does.
static void test_config_option_rejects_empty_path(void) {
  char* argv[] = {"tool", "config", "--config=", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(str_eq(options.error_message, S("option '--config' requires a path")));
}

// No arguments at all are reported as an error with a diagnostic message.
static void test_no_args_rejected(void) {
  char* argv[] = {"tool", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(str_eq(options.error_message, S("no command specified")));
}

// The config command rejects a build-only flag like `-v`, naming the command and the flag.
static void test_config_command_rejects_verbose(void) {
  char* argv[] = {"tool", "config", "-v", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(
      str_eq(options.error_message, S("command 'config' does not accept option '--verbose'")));
}

// The config command rejects a build-only flag like `-w`, naming the command and the flag.
static void test_config_command_rejects_workers(void) {
  char* argv[] = {"tool", "config", "-w", "4", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(
      str_eq(options.error_message, S("command 'config' does not accept option '--workers'")));
}

// The config command rejects a build-only flag like `-n`, naming the command and the flag.
static void test_config_command_rejects_dry_run(void) {
  char* argv[] = {"tool", "config", "-n", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(
      str_eq(options.error_message, S("command 'config' does not accept option '--dry-run'")));
}

// The config command rejects a build-only flag like `--force`, naming the command and the flag.
static void test_config_command_rejects_force(void) {
  char* argv[] = {"tool", "config", "--force", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(str_eq(options.error_message, S("command 'config' does not accept option '--force'")));
}

// With two unsupported flags, the reported one is fixed by the `cli_flags` table order rather than
// by the order the user typed them. That is the claim the table's own comment makes, and what
// `reject_unsupported_flags` returning after the first match implements. Both input orders are
// asserted because correct behavior must not depend on an unspecified detail. Each single-flag test
// above passes whichever entry comes first.
static void test_config_command_rejects_first_flag_in_table_order(void) {
  char* verbose_first[] = {"tool", "config", "-v", "-w", "4", NULL};
  struct CliOptions options = parse(verbose_first);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(
      str_eq(options.error_message, S("command 'config' does not accept option '--verbose'")));

  char* workers_first[] = {"tool", "config", "-w", "4", "-v", NULL};
  options = parse(workers_first);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(
      str_eq(options.error_message, S("command 'config' does not accept option '--verbose'")));
}

// An unrecognized command is rejected with a diagnostic naming the command.
static void test_unknown_command(void) {
  char* argv[] = {"tool", "serve", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(str_eq(options.error_message, S("unknown command 'serve'")));
}

// A trailing positional argument the command does not accept is rejected, naming the argument.
static void test_extra_positional_argument(void) {
  char* argv[] = {"tool", "build", "extra", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(str_eq(options.error_message, S("unexpected argument 'extra'")));
}

// `cli_print_version` writes the app's `<name> <version>` line and a `grammars: <list>` line,
// naming the grammars the build links, to standard output and nothing else.
static void test_print_version_writes_version_and_grammars(void) {
  struct Os* os = os_memory();
  TEST_CHECK(cli_print_version(&TEST_SPEC, os, test_arena()) == 0);
  TEST_CHECK(os_memory_is_printed(os, 1, "tool 1.0\ngrammars: c, json\n"));
  TEST_CHECK(os_memory_is_printed(os, 2, ""));
}

// An app that links no grammars prints its version line alone.
static void test_print_version_without_grammars_writes_version_alone(void) {
  struct CliSpec spec = TEST_SPEC;
  spec.grammars = (struct Str){0};
  struct Os* os = os_memory();
  TEST_CHECK(cli_print_version(&spec, os, test_arena()) == 0);
  TEST_CHECK(os_memory_is_printed(os, 1, "tool 1.0\n"));
}

// A standard output that refuses the write makes `cli_print_version` report `-1` with the write's
// own `errno`, which lets `cli_dispatch` name a reason rather than exiting silently. The host fails
// the write as a read-only descriptor does, with `EBADF`.
static void test_print_version_reports_write_failure(void) {
  struct Os* os = os_memory();
  os_memory_fail_print(os, 1, 0, EBADF);
  errno = 0;
  TEST_CHECK(cli_print_version(&TEST_SPEC, os, test_arena()) == -1);
  TEST_CHECK(errno == EBADF);
}

// `cli_print_usage` substitutes the caller's program name into the usage line, in the right
// position for the selected command, alongside command-specific option content. The name passed
// here is deliberately *not* `"tool"`. With the real program name the assertion passes just as well
// against a hard-coded usage line, so it would prove nothing about substitution. `cli_dispatch`
// passes `argv[0]`, so a user invoking `/usr/local/bin/tool build --help` has to see that path
// back. The whole texts are asserted below.
static void test_print_usage_names_program_and_command(void) {
  static const struct Str prog_name = STR_INIT("/opt/bin/mytool");
  const struct {
    enum CliCommand command;
    const char* usage_line;
    const char* needle;
  } cases[] = {
      {CLI_COMMAND_BUILD, "    /opt/bin/mytool build [options]\n", "-w, --workers N"},
      {CLI_COMMAND_CONFIG, "    /opt/bin/mytool config [options]\n", "-h, --help"},
      {CLI_COMMAND_NONE, "    /opt/bin/mytool <command> [options]\n", "-V, --version"},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct Os* os = os_memory();
    TEST_CHECK(cli_print_usage(&TEST_SPEC, prog_name, cases[i].command, os, test_arena()) == 0);
    struct Arena arena = test_arena();
    const char* text = str_cstr(os_memory_printed(os, 1), &arena);
    TEST_CHECK(strstr(text, cases[i].usage_line) != NULL);
    TEST_CHECK(strstr(text, cases[i].needle) != NULL);
    if (cases[i].command == CLI_COMMAND_BUILD) {
      TEST_CHECK(strstr(text, "-n, --dry-run") != NULL);
      TEST_CHECK(strstr(text, "    --force            Ignore the build cache for this build\n") !=
                 NULL);
    }
    // The command-less help substitutes the name a second time, in the trailer.
    if (cases[i].command == CLI_COMMAND_NONE) {
      TEST_CHECK(strstr(text, "Use '/opt/bin/mytool <command> --help'") != NULL);
    }
  }
}

// The help of `config` names its option, compared whole.
static void test_print_usage_names_config_option(void) {
  TEST_CHECK(os_memory_is_printed(
      print_usage(CLI_COMMAND_CONFIG), 1,
      "Print the resolved configuration as TOML.\n"
      "\n"
      "USAGE:\n"
      "    tool config [options]\n"
      "\n"
      "OPTIONS:\n"
      "    -c, --config PATH  Read the configuration from PATH (default: tool.toml)\n"
      "    -h, --help         Print this help and exit\n"));
}

// The help of `build` names each of its options, compared whole.
static void test_print_usage_names_build_options(void) {
  TEST_CHECK(os_memory_is_printed(
      print_usage(CLI_COMMAND_BUILD), 1,
      "Build the thing.\n"
      "\n"
      "USAGE:\n"
      "    tool build [options]\n"
      "\n"
      "OPTIONS:\n"
      "    -c, --config PATH  Read the configuration from PATH (default: tool.toml)\n"
      "    -w, --workers N    Use at most N worker threads (default: detected CPU count)\n"
      "    -n, --dry-run      Report what would be written, kept, or removed, changing nothing\n"
      "    --force            Ignore the build cache for this build\n"
      "    -v, --verbose      Print build progress to stderr\n"
      "    -h, --help         Print this help and exit\n"));
}

// The help summary starts with the app's description and names what `build` builds, compared whole.
static void test_print_usage_names_the_app(void) {
  TEST_CHECK(
      os_memory_is_printed(print_usage(CLI_COMMAND_NONE), 1,
                           "A static tool.\n"
                           "\n"
                           "USAGE:\n"
                           "    tool <command> [options]\n"
                           "\n"
                           "COMMANDS:\n"
                           "    build   Build the thing\n"
                           "    config  Print the resolved configuration\n"
                           "\n"
                           "OPTIONS:\n"
                           "    -h, --help     Print this help and exit\n"
                           "    -V, --version  Print version information and exit\n"
                           "\n"
                           "Use 'tool <command> --help' for more information about a command.\n"));
}

// `cli_print_usage` reports a failed write the same way, so neither informational action can exit
// non-zero without a reason.
static void test_print_usage_reports_write_failure(void) {
  struct Os* os = os_memory();
  os_memory_fail_print(os, 1, 0, EBADF);
  errno = 0;
  TEST_CHECK(cli_print_usage(&TEST_SPEC, S("tool"), CLI_COMMAND_NONE, os, test_arena()) == -1);
  TEST_CHECK(errno == EBADF);
}

TEST_LIST = {
    {"build command runs", test_build_command_runs},
    {"config command runs", test_config_command_runs},
    {"config option sets path", test_config_option_sets_path},
    {"verbose long flag", test_verbose_long_flag},
    {"verbose short flag", test_verbose_short_flag},
    {"dry run long flag", test_dry_run_long_flag},
    {"dry run short flag", test_dry_run_short_flag},
    {"dry run with other build flags", test_dry_run_with_other_build_flags},
    {"force long flag", test_force_long_flag},
    {"workers long flag separate value", test_workers_long_flag_separate_value},
    {"workers short flag separate value", test_workers_short_flag_separate_value},
    {"workers short flag attached value", test_workers_short_flag_attached_value},
    {"workers long flag equals value", test_workers_long_flag_equals_value},
    {"workers short flag in cluster equals value", test_workers_short_flag_in_cluster_equals_value},
    {"mixed flag order", test_mixed_flag_order},
    {"program name survives permutation", test_program_name_survives_permutation},
    {"options after build command", test_options_after_build_command},
    {"help long flag", test_help_long_flag},
    {"help short flag", test_help_short_flag},
    {"build command help", test_build_command_help},
    {"config command help", test_config_command_help},
    {"version long flag", test_version_long_flag},
    {"version short flag", test_version_short_flag},
    {"version flag wins over help", test_version_flag_wins_over_help},
    {"informational flag clears diagnostic", test_informational_flag_clears_diagnostic},
    {"valid version flag wins over rejected spelling",
     test_valid_version_flag_wins_over_rejected_spelling},
    {"invalid worker count rejected", test_invalid_worker_count_rejected},
    {"zero worker count rejected", test_zero_worker_count_rejected},
    {"negative worker count rejected", test_negative_worker_count_rejected},
    {"large worker count accepted", test_large_worker_count_accepted},
    {"missing worker count rejected", test_missing_worker_count_rejected},
    {"attached value rejected on valueless flags", test_attached_value_rejected_on_valueless_flags},
    {"attached value rejected on valueless short flags",
     test_attached_value_rejected_on_valueless_short_flags},
    {"attached value rejection ends short cluster",
     test_attached_value_rejection_ends_short_cluster},
    {"unknown short option", test_unknown_short_option},
    {"unknown short option value ends cluster", test_unknown_short_option_value_ends_cluster},
    {"unknown long option", test_unknown_long_option},
    {"config option requires path", test_config_option_requires_path},
    {"config option rejects empty path", test_config_option_rejects_empty_path},
    {"no args rejected", test_no_args_rejected},
    {"config command rejects verbose", test_config_command_rejects_verbose},
    {"config command rejects workers", test_config_command_rejects_workers},
    {"config command rejects dry run", test_config_command_rejects_dry_run},
    {"config command rejects force", test_config_command_rejects_force},
    {"config command rejects first flag in table order",
     test_config_command_rejects_first_flag_in_table_order},
    {"unknown command", test_unknown_command},
    {"extra positional argument", test_extra_positional_argument},
    {"print version writes version and grammars", test_print_version_writes_version_and_grammars},
    {"print version without grammars writes version alone",
     test_print_version_without_grammars_writes_version_alone},
    {"print version reports write failure", test_print_version_reports_write_failure},
    {"print usage names program and command", test_print_usage_names_program_and_command},
    {"print usage names config option", test_print_usage_names_config_option},
    {"print usage names build options", test_print_usage_names_build_options},
    {"print usage names the app", test_print_usage_names_the_app},
    {"print usage reports write failure", test_print_usage_reports_write_failure},
    {NULL, NULL},
};
