#ifndef APPS_CLI_CLI_H
#define APPS_CLI_CLI_H

#include <stdbool.h>
#include <stddef.h>

#include "core/exit_code.h"
#include "core/str.h"

struct Arena;
struct Log;
struct Os;

/** What the command line asks of one build. */
struct BuildOptions {
  /** Config file the build reads. */
  struct Str config_path;

  /**
   * Worker threads the build's parallel phases use, or `0` for one per online CPU. The build runs
   * at most `BUILD_RUN_WORKER_PER_CPU_MAX` of `runtime/build_run.h` per online CPU, and a phase
   * runs at most one per job.
   */
  ptrdiff_t worker_count;

  /** Whether to print phase progress and status messages to `stderr`. */
  bool is_verbose;

  /**
   * Whether to run every phase but change nothing under `output_dir`, printing to `stdout` what the
   * build would write, keep, and remove.
   */
  bool is_dry_run;

  /** Whether to ignore the build cache for this build, then write it afresh. */
  bool is_forced;
};

/** Action the parsed command line selected, resolved by precedence in `cli_parse`. */
enum CliAction {
  /** Print the version and exit successfully. Wins over every other flag. */
  CLI_ACTION_VERSION,

  /** Print usage help and exit successfully. Wins over parse errors but not version. */
  CLI_ACTION_HELP,

  /** Run the selected command. See `command` for which one. */
  CLI_ACTION_RUN,

  /** Reject the command line. `error_message` explains the failure. */
  CLI_ACTION_ERROR,
};

/** Subcommand selected on the command line. `CLI_COMMAND_NONE` until a valid command parses. */
enum CliCommand {
  /** No recognized command. Only version and help may run without one. */
  CLI_COMMAND_NONE,

  /** Build the output, the site or the gallery. */
  CLI_COMMAND_BUILD,

  /** Print the resolved configuration. */
  CLI_COMMAND_CONFIG,
};

/** Parsed command line for one invocation of an app. */
struct CliOptions {
  /** Selected action after applying flag precedence. */
  enum CliAction action;

  /** Command the positional argument selected. It also selects the help or usage text. */
  enum CliCommand command;

  /**
   * Config file that `-c` named, as a copy in the arena `cli_parse` was given, or the app's
   * `config_path_default` when `-c` was not given.
   */
  struct Str config_path;

  /**
   * Requested worker count, or `0` when `--workers` was not given. `0` is never a valid request,
   * because `cli_parse_workers_option` rejects it, so it serves as the unset sentinel. The build
   * caps the count at four per online CPU. `BuildOptions.worker_count` documents `0` the same way.
   */
  ptrdiff_t worker_count;

  /** Whether the caller requested progress and status messages. */
  bool is_verbose;

  /** Whether the build only reports what it would write, keep, or remove, changing nothing. */
  bool is_dry_run;

  /** Whether the build ignores its build cache, and writes it afresh. */
  bool is_forced;

  /**
   * Failure diagnostic, non-empty exactly when `action` is `CLI_ACTION_ERROR`. It is a formatted
   * copy in the arena `cli_parse` was given, so it holds no pointer into `argv` or into copt's
   * scratch storage.
   */
  struct Str error_message;
};

/**
 * @brief Builds the output that `options` describe, as the app's `cmd_build_run` does.
 *
 * @param options What the command line asks of the build. Must not be `NULL`.
 * @param os      Host the build runs against. Must not be `NULL`.
 * @return The exit code of the build.
 */
typedef enum ExitCode (*CliBuildFn)(const struct BuildOptions* options, struct Os* os);

/**
 * @brief Loads the config at `config_path` and prints it resolved, as the app's `config` command
 *        does.
 *
 * @param config_path Config file to load.
 * @param os          Host the config is read and printed on. Must not be `NULL`.
 * @param perm        Arena that owns the loaded config. Must not be `NULL`.
 * @param scratch     Arena for the load and the printed text.
 * @param log         Log that receives every error of the load. Must not be `NULL`.
 * @return `0` when the config loaded with no error and printed, or `-1` otherwise.
 */
typedef int (*CliConfigPrintFn)(struct Str config_path,
                                struct Os* os,
                                struct Arena* perm,
                                struct Arena scratch,
                                struct Log* log);

/** What an app tells the shared command line about itself. */
struct CliSpec {
  /** Sentence the help summary starts with, such as `A static site generator.`. */
  struct Str description;

  /** What `build` builds, as the help names it, such as `site`. */
  struct Str noun;

  /** Config file read without `-c`, such as `sosig.toml`. */
  struct Str config_path_default;

  /** First line of `--version`: the app's name and version, such as `sosig 0.5.0`. */
  struct Str generator;

  /** Grammars the binary links, which `--version` lists on a second line, or empty for none. */
  struct Str grammars;

  /** Runs the `build` command. Must not be `NULL`. */
  CliBuildFn build;

  /** Runs the `config` command in the memory `cli_dispatch` reserves for it. Must not be `NULL`. */
  CliConfigPrintFn config_print;
};

/**
 * @brief Parses `argv` into `options_out`, resolving the action by fixed precedence.
 *
 * Never prints or exits. The caller inspects `options_out->action` and acts on it. On a rejected
 * command line, `options_out->action` is `CLI_ACTION_ERROR` and `options_out->error_message` holds
 * the first diagnostic. May permute `argv[1..argc-1]` in place (it reorders options ahead of
 * positional arguments). It leaves `argv[0]` untouched.
 *
 * `options_out` retains no pointer into `argv`, so the result stays valid however the caller later
 * reorders or overwrites `argv`. `error_message` is a formatted copy in `arena`, so the diagnostic
 * does not retain a pointer into copt's scratch storage either. A short option's spelling lives in
 * the parser that `cli_parse` keeps on its own stack, so a later change to avoid the copy would
 * hand back a pointer that dangles as soon as `cli_parse` returns.
 *
 * @param spec        What the app tells the command line about itself. Must not be `NULL`.
 * @param argc        Argument count.
 * @param argv        Argument vector. Elements after `argv[0]` may be reordered. Must not be
 *                    `NULL`.
 * @param arena       Arena that owns `error_message` and a `config_path` that `-c` named. Must not
 *                    be `NULL`.
 * @param options_out Receives the fully resolved parse result. Must not be `NULL`.
 */
void cli_parse(const struct CliSpec* spec,
               int argc,
               char** argv,
               struct Arena* arena,
               struct CliOptions* options_out) __attribute__((nonnull(1, 3, 4, 5)));

/**
 * @brief Writes the app's `<name> <version>` line, and the `grammars: <list>` line when the binary
 *        links grammars, to standard output.
 *
 * The lines are formatted whole and printed in one `os_print` call.
 *
 * @param spec    What the app tells the command line about itself. Must not be `NULL`.
 * @param os      Host to print on. Must not be `NULL`.
 * @param scratch Arena the lines are formatted in.
 * @return `0` on success, or `-1` when the write fails.
 */
int cli_print_version(const struct CliSpec* spec, struct Os* os, struct Arena scratch)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Writes usage and option help for `command` to standard output.
 *
 * Prints command-specific help when a command is selected, otherwise the top-level summary. The
 * text is formatted whole and printed in one `os_print` call.
 *
 * @param spec         What the app tells the command line about itself. Must not be `NULL`.
 * @param program_name Program name to show in usage lines, such as `argv[0]`.
 * @param command      Command whose help to print, or `CLI_COMMAND_NONE` for the summary.
 * @param os           Host to print on. Must not be `NULL`.
 * @param scratch      Arena the text is formatted in. The program name is bounded only by
 *                     `ARG_MAX`, so it must hold several times that.
 * @return `0` on success, or `-1` when the write fails.
 */
int cli_print_usage(const struct CliSpec* spec,
                    struct Str program_name,
                    enum CliCommand command,
                    struct Os* os,
                    struct Arena scratch) __attribute__((nonnull(1, 4)));

#endif
