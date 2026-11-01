#ifndef SURGE_CMD_BUILD_H
#define SURGE_CMD_BUILD_H

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
 * @brief Runs the build pipeline, collecting every error and warning into a log.
 *
 * The first phase that fails stops the build, after it reports every error of its own, so a build
 * that fails before its writes leaves `output_dir` as it was. A dry run runs the same phases,
 * changes nothing under `output_dir`, and prints what it would have written, kept, and removed.
 * Output is byte-identical for any worker count.
 *
 * @param options       Build knobs, whose worker count is positive. Must not be `NULL`.
 * @param worker_arenas One perm and one scratch per worker. Must not be `NULL`.
 * @param os            Host every phase reads from and writes to. Must not be `NULL`.
 * @param perm          The main thread's perm.
 * @param scratch       The main thread's scratch.
 * @param log           Log that receives every error and warning, in an arena of its own. Must not
 *                      be `NULL`.
 * @return `0` on a complete build, or `-1` on any error.
 */
int cmd_build_execute(const struct BuildOptions* options,
                      struct JobArenas* worker_arenas,
                      struct Os* os,
                      struct Arena perm,
                      struct Arena scratch,
                      struct Log* log) __attribute__((nonnull(1, 2, 3, 6)));

#endif
