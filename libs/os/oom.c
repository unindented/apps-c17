#include "os/oom.h"

#include <stddef.h>

#include "core/buf.h"
#include "core/log.h"
#include "core/str.h"
#include "os/os.h"

/** What one thread works on, for the out-of-memory report. */
struct OomJob {
  /** File the thread works on, or empty. */
  struct Str file_path;

  /** Log of the thread's current job, or `NULL`. */
  const struct Log* log;
};

/**
 * The command's log, which the out-of-memory report prints first. Only the main thread writes it,
 * and only while no worker runs, so every worker that reads it sees the value set before it
 * started.
 */
static const struct Log* oom_log;

/** What the calling thread works on. Each thread has its own. */
static _Thread_local struct OomJob oom_job;

void oom_set_log(const struct Log* log) {
  oom_log = log;
}

void oom_set_job(struct Str file_path, const struct Log* log) {
  oom_job = (struct OomJob){file_path, log};
}

void oom_report(struct Os* os, struct Str program) {
  // Each piece is printed as it is, because building one message would need memory. A print that
  // fails cannot be reported either, so the results are discarded.
  if (oom_log != NULL) {
    (void)os_print(os, 2, buf_str(&oom_log->buf));
  }
  if (oom_job.log != NULL) {
    (void)os_print(os, 2, buf_str(&oom_job.log->buf));
  }
  (void)os_print(os, 2, program);
  (void)os_print(os, 2, S(": out of memory"));
  if (oom_job.file_path.len > 0) {
    (void)os_print(os, 2, S(" (in '"));
    (void)os_print(os, 2, oom_job.file_path);
    (void)os_print(os, 2, S("')"));
  }
  (void)os_print(os, 2, S("\n"));
}
