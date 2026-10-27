#ifndef APPS_RUNTIME_BUILD_RUN_H
#define APPS_RUNTIME_BUILD_RUN_H

#include <stddef.h>

#include "core/arena.h"
#include "core/exit_code.h"

struct JobArenas;
struct Log;
struct Os;

/** Most workers a build starts for each online CPU, however many are requested. */
enum { BUILD_RUN_WORKER_PER_CPU_MAX = 4 };

/**
 * Bytes of each arena of a build, which the app derives from what its phases hold. Each thread has
 * a perm and a scratch, the main thread has the log, and the main thread's scratch also holds one
 * log share per worker and one for the pool, which `job_run` splits off it.
 */
struct BuildRunLens {
  /** Bytes of the build's log, and of each job log share. */
  ptrdiff_t log_len;

  /** Bytes of each thread's perm. */
  ptrdiff_t perm_len;

  /** Bytes of each thread's scratch. */
  ptrdiff_t scratch_len;
};

/**
 * @brief Runs a build in the arenas `build_run` reserved, as the app's `cmd_build_execute` does.
 *
 * @param worker_count  Workers there are arenas for, which the build runs its jobs on.
 * @param worker_arenas One perm and one scratch per worker. Must not be `NULL`.
 * @param os            Host the build runs on. Must not be `NULL`.
 * @param perm          The main thread's perm.
 * @param scratch       The main thread's scratch.
 * @param log           Log that receives every error and warning. Must not be `NULL`.
 * @param userdata      Opaque pointer `build_run` was given.
 * @return `0` on a complete build, or `-1` on any error.
 */
typedef int (*BuildRunExecuteFn)(ptrdiff_t worker_count,
                                 struct JobArenas* worker_arenas,
                                 struct Os* os,
                                 struct Arena perm,
                                 struct Arena scratch,
                                 struct Log* log,
                                 void* userdata);

/**
 * @brief Reserves a build's memory, runs it, prints what it logged, and releases the memory.
 *
 * The worker count is the one requested, or the CPU count for `0`, capped at
 * `BUILD_RUN_WORKER_PER_CPU_MAX` per CPU. The build takes one reservation for every arena. Under an
 * address-space limit it halves the workers first, and only with one worker left does it halve the
 * arenas, so a limit costs workers before it costs room. The log is the out-of-memory report's
 * while the build runs. The errors print first, because they stopped the build, and then the
 * warnings, which still say what a build that succeeded left out.
 *
 * @param lens                   Bytes of each arena. Must not be `NULL`.
 * @param worker_count_requested Workers the user asked for, or `0` for one per CPU.
 * @param execute                Runs the build. Must not be `NULL`.
 * @param userdata               Opaque pointer passed to `execute`.
 * @param os                     Host the build runs and prints on. Must not be `NULL`.
 * @return `EXIT_CODE_OK` when `execute` succeeded, or `EXIT_CODE_FAILURE` otherwise.
 */
enum ExitCode build_run(const struct BuildRunLens* lens,
                        ptrdiff_t worker_count_requested,
                        BuildRunExecuteFn execute,
                        void* userdata,
                        struct Os* os) __attribute__((nonnull(1, 3, 5)));

#endif
