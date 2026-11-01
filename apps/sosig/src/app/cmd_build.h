#ifndef SOSIG_CMD_BUILD_H
#define SOSIG_CMD_BUILD_H

#include <stdbool.h>
#include <stddef.h>

#include "cli/cli.h"
#include "core/arena.h"
#include "core/exit_code.h"
#include "core/str.h"

struct JobArenas;
struct Log;
struct Os;

/**
 * @brief Runs the `build` command for the site that `options->config_path` configures, as
 *        `CliBuildFn`.
 *
 * This is a thin boundary over `cmd_build_execute`. It gives `build_run` the build's arena sizes,
 * and `build_run` resolves the worker count, reserves the build's memory in one block, for fewer
 * workers when an address-space limit refuses the block, runs `cmd_build_execute`, prints the
 * build's errors to `stderr` and then its warnings, in one `os_print` call each, and releases the
 * block. A build with only warnings succeeds and still says what it skipped. Progress lines under
 * verbose mode are status, not diagnostics. If the build runs out of memory, the program's `os_oom`
 * prints the log instead.
 *
 * @param options Build knobs such as worker count and verbosity. Must not be `NULL`.
 * @param os      Host the build reads, writes, and prints its status lines and log on. Must not be
 *                `NULL`.
 * @return `EXIT_CODE_OK` on a complete build, or `EXIT_CODE_FAILURE` on any error.
 */
enum ExitCode cmd_build_run(const struct BuildOptions* options, struct Os* os)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Runs the build pipeline, collecting every error into a log.
 *
 * Performs no error printing itself so callers can inspect the log directly. `cmd_build_run` is the
 * boundary that prints it. Until the first write, the build keeps going after an error wherever the
 * next phase still has something to check, so one run reports every source that does not parse and
 * every output collision, in that order, sources in walk order. A config that fails stops it at
 * once. It stops before writing anything if any error was recorded. A dry run runs the same phases,
 * changes nothing under `output_dir`, and prints to `stdout` what it would have written, kept, and
 * removed.
 *
 * The main thread's data that outlives a phase, from the configuration and the source paths to the
 * manifest, goes in `perm`, and its temporaries and a parallel phase's job logs in `scratch`. Each
 * worker allocates only from its own `worker_arenas` entry. A build whose data outgrows an arena
 * ends in `os_oom`.
 *
 * @param options       Build knobs. Its worker count must be positive. Must not be `NULL`.
 * @param worker_arenas One perm and one scratch per worker, `options->worker_count` of them. Must
 *                      not be `NULL`.
 * @param os            Host every phase reads from and writes to, and status lines print on. Must
 *                      not be `NULL`.
 * @param perm          The main thread's perm. The build uses it up, and nothing it allocates
 *                      outlives the call.
 * @param scratch       The main thread's scratch.
 * @param log           Log that receives every error. Its arena must not overlap the other arenas,
 *                      because the phases copy arenas by value while they log. Left empty on
 *                      success. Must not be `NULL`.
 * @return `0` on a complete build, or `-1` on any error, with every error in `log`.
 */
int cmd_build_execute(const struct BuildOptions* options,
                      struct JobArenas* worker_arenas,
                      struct Os* os,
                      struct Arena perm,
                      struct Arena scratch,
                      struct Log* log) __attribute__((nonnull(1, 2, 3, 6)));

#endif
