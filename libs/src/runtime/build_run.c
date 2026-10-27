#include "runtime/build_run.h"

#include <stddef.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/exit_code.h"
#include "core/log.h"
#include "core/str.h"
#include "os/oom.h"
#include "os/os.h"
#include "runtime/job.h"
#include "runtime/pool.h"

/** A build's one reservation and the arenas `build_run` splits it into. */
struct BuildRunMemory {
  /** The whole reservation, which the build releases at its end. */
  struct Arena block;

  /** Workers the reservation has arenas for, at most the count asked for. */
  ptrdiff_t worker_count;

  /** Arena of the build's log, which no other code allocates from. */
  struct Arena log_arena;

  /** The main thread's perm. */
  struct Arena perm;

  /** The main thread's scratch, with room for one log share per worker and one more. */
  struct Arena scratch;

  /** One perm and one scratch per worker, in `perm`. */
  struct JobArenas* worker_arenas;
};

/**
 * @brief Returns the workers a build starts: the count requested, or the CPU count for `0`, capped
 *        at `BUILD_RUN_WORKER_PER_CPU_MAX` per CPU.
 *
 * @param count_requested Workers asked for, or `0` for one per CPU.
 * @return The worker count, at least one.
 */
static ptrdiff_t build_run_worker_count(ptrdiff_t count_requested);

/**
 * @brief Reserves one block for every arena of a build with `worker_count` workers, halving the
 *        workers and then the arenas until the reservation fits.
 *
 * @param lens         Bytes of each arena. Must not be `NULL`.
 * @param worker_count Workers to reserve arenas for. Must be positive.
 * @return The reservation, its arenas, and the worker count they are for.
 */
static struct BuildRunMemory build_run_reserve(const struct BuildRunLens* lens,
                                               ptrdiff_t worker_count) __attribute__((nonnull(1)));

enum ExitCode build_run(const struct BuildRunLens* lens,
                        ptrdiff_t worker_count_requested,
                        BuildRunExecuteFn execute,
                        void* userdata,
                        struct Os* os) {
  struct BuildRunMemory memory =
      build_run_reserve(lens, build_run_worker_count(worker_count_requested));
  struct Log log = {.buf = {.arena = &memory.log_arena}};
  oom_set_log(&log);
  const int rc = execute(memory.worker_count, memory.worker_arenas, os, memory.perm, memory.scratch,
                         &log, userdata);
  if (log.buf.len > 0) {
    (void)os_print(os, 2, buf_str(&log.buf));
  }
  if (log.warning_buf.len > 0) {
    (void)os_print(os, 2, buf_str(&log.warning_buf));
  }
  // The names point into memory released below.
  oom_set_log(NULL);
  oom_set_job((struct Str){0}, NULL);
  arena_release(memory.block);
  return rc == 0 ? EXIT_CODE_OK : EXIT_CODE_FAILURE;
}

static ptrdiff_t build_run_worker_count(ptrdiff_t count_requested) {
  const ptrdiff_t cpu_count = pool_resolve_worker_count();
  const ptrdiff_t count = count_requested != 0 ? count_requested : cpu_count;
  const ptrdiff_t count_max = BUILD_RUN_WORKER_PER_CPU_MAX * cpu_count;
  return count < count_max ? count : count_max;
}

static struct BuildRunMemory build_run_reserve(const struct BuildRunLens* lens,
                                               ptrdiff_t worker_count) {
  // The main thread's scratch also holds a parallel phase's job logs, which `job_run` splits into
  // one share per worker and one for the pool's bookkeeping. So the main thread has the log, its
  // perm, its scratch and the pool's share, and each worker adds its perm, scratch and share.
  const ptrdiff_t main_len = lens->log_len + lens->perm_len + lens->scratch_len + lens->log_len;
  const ptrdiff_t worker_len = lens->perm_len + lens->scratch_len + lens->log_len;
  struct BuildRunMemory memory = {0};
  // A limit costs workers before it costs room.
  while (worker_count > 1 &&
         arena_reserve(main_len + worker_len * worker_count, &memory.block) != 0) {
    worker_count /= 2;
  }
  const int shift =
      worker_count > 1 ? 0 : arena_reserve_halving(main_len + worker_len, &memory.block);
  memory.worker_count = worker_count;
  struct Arena rest = memory.block;
  memory.log_arena = arena_split(&rest, lens->log_len >> shift);
  memory.perm = arena_split(&rest, lens->perm_len >> shift);
  memory.scratch =
      arena_split(&rest, (lens->scratch_len + lens->log_len * (worker_count + 1)) >> shift);
  memory.worker_arenas = arena_new(&memory.perm, worker_count, struct JobArenas);
  for (ptrdiff_t i = 0; i < worker_count; i++) {
    memory.worker_arenas[i].perm = arena_split(&rest, lens->perm_len >> shift);
    memory.worker_arenas[i].scratch = arena_split(&rest, lens->scratch_len >> shift);
  }
  return memory;
}
