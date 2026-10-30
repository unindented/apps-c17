#include <acutest.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/arena.h"
#include "core/exit_code.h"
#include "core/log.h"
#include "core/str.h"
#include "os/os.h"
#include "os/os_memory.h"
#include "runtime/build_run.h"
#include "runtime/job.h"
#include "runtime/pool.h"
#include "testing/test_support.h"

/** Small arenas, enough for a build that logs a line or two. */
static const struct BuildRunLens LENS = {
    .log_len = 4096,
    .perm_len = 64 * 1024,
    .scratch_len = 64 * 1024,
};

/** What a test build saw and what it does. */
struct TestBuild {
  /** Receives the worker count the build ran with. */
  ptrdiff_t worker_count;

  /** Receives the bytes of the first worker's perm. */
  ptrdiff_t worker_perm_len;

  /** Whether the build logs an error and a warning and fails. */
  bool is_failing;

  /** Whether the build logs a warning and succeeds. */
  bool is_warning;
};

/**
 * @brief Records the worker count and checks each worker has its arenas, as `BuildRunExecuteFn`.
 *
 * @param worker_count  Workers there are arenas for.
 * @param worker_arenas One perm and one scratch per worker. Must not be `NULL`.
 * @param os            Host the build runs on. Must not be `NULL`.
 * @param perm          The main thread's perm.
 * @param scratch       The main thread's scratch.
 * @param log           Log of the build. Must not be `NULL`.
 * @param userdata      The `struct TestBuild`.
 * @return `-1` for a failing build, and `0` otherwise.
 */
static int execute(ptrdiff_t worker_count,
                   struct JobArenas* worker_arenas,
                   struct Os* os,
                   struct Arena perm,
                   struct Arena scratch,
                   struct Log* log,
                   void* userdata) {
  (void)os;
  struct TestBuild* build = userdata;
  build->worker_count = worker_count;
  build->worker_perm_len = worker_arenas[0].perm.limit - worker_arenas[0].perm.beg;
  TEST_CHECK(perm.beg != NULL && scratch.beg != NULL && perm.beg != scratch.beg);
  for (ptrdiff_t i = 0; i < worker_count; i++) {
    TEST_CHECK(worker_arenas[i].perm.beg != NULL && worker_arenas[i].scratch.beg != NULL);
  }
  if (build->is_warning) {
    log_warn(log, "skipped a file");
  }
  if (!build->is_failing) {
    return 0;
  }
  log_warn(log, "skipped a file");
  return log_error(log, "failed to build");
}

// A build runs on the workers asked for, and a request of `0` runs one per CPU. A build that
// succeeds and logs nothing prints nothing.
static void test_run_resolves_the_worker_count(void) {
  struct Os* os = os_memory();
  struct TestBuild build = {0};
  TEST_CHECK(build_run(&LENS, 2, execute, &build, os) == EXIT_CODE_OK);
  TEST_CHECK(build.worker_count == 2);
  TEST_CHECK(build_run(&LENS, 0, execute, &build, os) == EXIT_CODE_OK);
  TEST_CHECK(build.worker_count == pool_resolve_worker_count());
  TEST_CHECK(os_memory_is_printed(os, 2, ""));
}

// However many workers are asked for, a build starts at most four per CPU.
static void test_run_caps_the_workers_at_four_per_cpu(void) {
  struct Os* os = os_memory();
  struct TestBuild build = {0};
  const ptrdiff_t cpu_count = pool_resolve_worker_count();
  TEST_CHECK(build_run(&LENS, 100 * cpu_count, execute, &build, os) == EXIT_CODE_OK);
  TEST_CHECK(build.worker_count == BUILD_RUN_WORKER_PER_CPU_MAX * cpu_count);
}

// Under an address-space limit a build halves its workers, down to one, before it halves any arena.
// With `LENS`, four workers need 679,936 bytes, two need 409,600 and one needs 274,432. So 512 KiB
// holds two workers at full size, and 192 KiB holds one worker on arenas halved once, which
// `arena_reserve_halving` halves once more to keep room beside them.
static void test_run_halves_workers_before_arenas_under_limit(void) {
  const struct {
    ptrdiff_t len_max;
    ptrdiff_t worker_count;
    ptrdiff_t worker_perm_len;
  } cases[] = {
      {512 * 1024, 2, LENS.perm_len},
      {192 * 1024, 1, LENS.perm_len / 4},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct Os* os = os_memory();
    struct TestBuild build = {0};
    arena_reserve_len_max = cases[i].len_max;
    TEST_CHECK(build_run(&LENS, 4, execute, &build, os) == EXIT_CODE_OK);
    arena_reserve_len_max = PTRDIFF_MAX;
    TEST_CHECK(build.worker_count == cases[i].worker_count);
    TEST_CHECK(build.worker_perm_len == cases[i].worker_perm_len);
    TEST_MSG("case %td", i);
  }
}

// A build that succeeds with a warning prints the warning, so it still says what it left out.
static void test_run_prints_the_warnings_of_a_build_that_succeeds(void) {
  struct Os* os = os_memory();
  struct TestBuild build = {.is_warning = true};
  TEST_CHECK(build_run(&LENS, 1, execute, &build, os) == EXIT_CODE_OK);
  TEST_CHECK(os_memory_is_printed(os, 2, "warning: skipped a file\n"));
}

// A failed build prints its errors and then its warnings, and fails the command.
static void test_run_prints_errors_then_warnings(void) {
  struct Os* os = os_memory();
  struct TestBuild build = {.is_failing = true};
  TEST_CHECK(build_run(&LENS, 1, execute, &build, os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_is_printed(os, 2, "failed to build\nwarning: skipped a file\n"));
}

TEST_LIST = {
    {"run resolves the worker count", test_run_resolves_the_worker_count},
    {"run caps the workers at four per CPU", test_run_caps_the_workers_at_four_per_cpu},
    {"run halves workers before arenas under limit",
     test_run_halves_workers_before_arenas_under_limit},
    {"run prints the warnings of a build that succeeds",
     test_run_prints_the_warnings_of_a_build_that_succeeds},
    {"run prints errors then warnings", test_run_prints_errors_then_warnings},
    {NULL, NULL},
};
