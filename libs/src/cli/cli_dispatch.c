#include "cli/cli_dispatch.h"

#include <pthread.h>
#include <stdlib.h>

#include "cli/cli.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/exit_code.h"
#include "core/log.h"
#include "core/str.h"
#include "os/oom.h"
#include "os/os.h"

/**
 * @brief Runs the command an accepted command line selected. A step of `cli_dispatch`.
 *
 * @param spec    What the app tells the command line about itself. Must not be `NULL`.
 * @param options Parsed command line, whose action is `CLI_ACTION_RUN`. Must not be `NULL`.
 * @param os      Host the command runs against. Must not be `NULL`.
 * @return The exit code of the command.
 */
static enum ExitCode cli_dispatch_command(const struct CliSpec* spec,
                                          const struct CliOptions* options,
                                          struct Os* os) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Runs the `config` command: loads the config at `config_path` and prints it resolved, in a
 *        reservation of its own. A step of `cli_dispatch_command`.
 *
 * A load that read the file still reports every bad key, and none of them may be printed as the
 * resolved config, so any error fails the command and prints the load's log instead.
 *
 * @param spec        What the app tells the command line about itself. Must not be `NULL`.
 * @param config_path Config file to load.
 * @param os          Host the config is read and printed on. Must not be `NULL`.
 * @return `EXIT_CODE_OK` when the config printed, or `EXIT_CODE_FAILURE` otherwise.
 */
static enum ExitCode cli_dispatch_command_config(const struct CliSpec* spec,
                                                 struct Str config_path,
                                                 struct Os* os) __attribute__((nonnull(1, 3)));

/**
 * @brief Prints what a command line that runs no command asked for: the version, the help, or the
 *        diagnostic of a rejected command line. A step of `cli_dispatch`.
 *
 * @param spec    What the app tells the command line about itself. Must not be `NULL`.
 * @param options Parsed command line, whose action is not `CLI_ACTION_RUN`. Must not be `NULL`.
 * @param argv    Argument vector `options` was parsed from. Must not be `NULL`.
 * @param os      Host to print on. Must not be `NULL`.
 * @param scratch Arena the help text is formatted in.
 * @return The exit code of the action.
 */
static enum ExitCode cli_dispatch_print(const struct CliSpec* spec,
                                        const struct CliOptions* options,
                                        char* const* argv,
                                        struct Os* os,
                                        struct Arena scratch) __attribute__((nonnull(1, 2, 3, 4)));

enum ExitCode cli_dispatch(const struct CliSpec* spec, int argc, char** argv, struct Os* os) {
  // The command line's diagnostic quotes an argument, which only `ARG_MAX` bounds, so it is
  // formatted into a reservation of its own that holds several times the largest `ARG_MAX` in use.
  // Under an address-space limit the reservation is halved, and the version and the help text still
  // fit.
  enum { CLI_DISPATCH_RESERVE_LEN = 16 * 1024 * 1024 };
  struct Arena block;
  (void)arena_reserve_halving(CLI_DISPATCH_RESERVE_LEN, &block);
  struct Arena arena = block;
  struct CliOptions options;
  cli_parse(spec, argc, argv, &arena, &options);
  // A command reserves its own memory. Everything else prints from this reservation: the diagnostic
  // `cli_parse` formatted there, or the help text, which quotes `argv[0]`.
  const enum ExitCode rc = options.action == CLI_ACTION_RUN
                               ? cli_dispatch_command(spec, &options, os)
                               : cli_dispatch_print(spec, &options, argv, os, arena);
  arena_release(block);
  return rc;
}

static enum ExitCode cli_dispatch_command(const struct CliSpec* spec,
                                          const struct CliOptions* options,
                                          struct Os* os) {
  switch (options->command) {
    case CLI_COMMAND_BUILD: {
      const struct BuildOptions build_options = {
          .config_path = options->config_path,
          .worker_count = options->worker_count,
          .is_verbose = options->is_verbose,
          .is_dry_run = options->is_dry_run,
          .is_forced = options->is_forced,
      };
      return spec->build(&build_options, os);
    }
    case CLI_COMMAND_CONFIG:
      return cli_dispatch_command_config(spec, options->config_path, os);
    case CLI_COMMAND_NONE:
      break;
  }
  // Unreachable: `cli_parse` only resolves `CLI_ACTION_RUN` alongside a real command, and the
  // switch covers every `enum CliCommand`. This is deliberately not a `default:` case, so
  // `-Wswitch` still fails the build when someone adds a command without a case here. A value
  // outside the enum means memory corruption, which there is no sensible exit code for.
  abort();
}

static enum ExitCode cli_dispatch_command_config(const struct CliSpec* spec,
                                                 struct Str config_path,
                                                 struct Os* os) {
  // A config file is read whole and capped at 1 MiB, as `toml_load_config` caps it, and its parse
  // and its printed text are a few times its size, so a third of 16 MiB for each of the loaded
  // values, the log and the scratch holds any config with room to spare.
  enum { CLI_CONFIG_RESERVE_LEN = 16 * 1024 * 1024 };
  struct Arena block;
  const int shift = arena_reserve_halving(CLI_CONFIG_RESERVE_LEN, &block);
  struct Arena scratch = block;
  struct Arena perm = arena_split(&scratch, (CLI_CONFIG_RESERVE_LEN / 3) >> shift);
  struct Arena log_arena = arena_split(&scratch, (CLI_CONFIG_RESERVE_LEN / 3) >> shift);
  struct Log log = {.buf = {.arena = &log_arena}};
  oom_set_log(&log);
  const int rc = spec->config_print(config_path, os, &perm, scratch, &log);
  if (rc != 0 && log.buf.len > 0) {
    (void)os_print(os, 2, buf_str(&log.buf));
  }
  oom_set_log(NULL);
  arena_release(block);
  return rc == 0 ? EXIT_CODE_OK : EXIT_CODE_FAILURE;
}

static enum ExitCode cli_dispatch_print(const struct CliSpec* spec,
                                        const struct CliOptions* options,
                                        char* const* argv,
                                        struct Os* os,
                                        struct Arena scratch) {
  switch (options->action) {
    case CLI_ACTION_VERSION:
      // A failed write, such as to a closed `stdout`, exits with `EXIT_CODE_FAILURE`.
      return cli_print_version(spec, os, scratch) == 0 ? EXIT_CODE_OK : EXIT_CODE_FAILURE;
    case CLI_ACTION_HELP: {
      // `CLI_ACTION_HELP` means `cli_parse` saw `--help`, so `argc >= 2` and `argv[0]` is a string.
      // The `argc == 0` case that ISO C permits never arrives here. With no arguments no command
      // resolves, and `cli_parse` returns `CLI_ACTION_ERROR` instead.
      const struct Str program_name = str_from_cstr(argv[0]);
      return cli_print_usage(spec, program_name, options->command, os, scratch) == 0
                 ? EXIT_CODE_OK
                 : EXIT_CODE_FAILURE;
    }
    case CLI_ACTION_ERROR:
      // A diagnostic that cannot be printed has nowhere else to go, and the exit code still says
      // the command line was rejected.
      (void)os_print(os, 2, options->error_message);
      (void)os_print(os, 2, S("\n"));
      return EXIT_CODE_USAGE;
    case CLI_ACTION_RUN:
      break;
  }
  // Unreachable: `cli_dispatch` passes a command line that runs a command to `cli_dispatch_command`
  // instead. This is deliberately not a `default:` case, so `-Wswitch` still fails the build when
  // someone adds an action without a case here.
  abort();
}

_Noreturn void cli_oom_exit(struct Os* os, struct Str program) {
  static pthread_mutex_t report_lock = PTHREAD_MUTEX_INITIALIZER;
  (void)pthread_mutex_lock(&report_lock);
  oom_report(os, program);
  _Exit(EXIT_CODE_FAILURE);
}
