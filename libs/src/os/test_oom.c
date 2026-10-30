#include <acutest.h>
#include <stddef.h>

#include "core/log.h"
#include "core/str.h"
#include "os/oom.h"
#include "os/os_memory.h"
#include "testing/test_support.h"

// The report prints the command's log, then the thread's job log, then the line naming the file the
// thread works on, so the errors found before memory ran out are not lost.
static void test_oom_report_prints_logs_and_file(void) {
  struct Log log = test_log();
  (void)log_error(&log, "failed to probe media 'input/a.jpg'");
  struct Log job_log = test_log();
  (void)log_error(&job_log, "unknown key 'x'");
  oom_set_log(&log);
  oom_set_job(S("input/b.jpg"), &job_log);
  struct Os* os = os_memory();
  oom_report(os, S("app"));
  TEST_CHECK(os_memory_is_printed(os, 2,
                                  "failed to probe media 'input/a.jpg'\nunknown key 'x'\n"
                                  "app: out of memory (in 'input/b.jpg')\n"));
  oom_set_log(NULL);
  oom_set_job((struct Str){0}, NULL);
}

// A file named by a view into a longer string is printed up to the view's length only.
static void test_oom_report_names_file_view(void) {
  oom_set_job(str_head(S("input/b.jpg.tmp"), S("input/b.jpg").len), NULL);
  struct Os* os = os_memory();
  oom_report(os, S("app"));
  TEST_CHECK(os_memory_is_printed(os, 2, "app: out of memory (in 'input/b.jpg')\n"));
  oom_set_job((struct Str){0}, NULL);
}

// With no log and no file named, the report is the bare line.
static void test_oom_report_without_context_is_one_line(void) {
  struct Os* os = os_memory();
  oom_report(os, S("app"));
  TEST_CHECK(os_memory_is_printed(os, 2, "app: out of memory\n"));
}

TEST_LIST = {
    {"oom report prints logs and file", test_oom_report_prints_logs_and_file},
    {"oom report names file view", test_oom_report_names_file_view},
    {"oom report without context is one line", test_oom_report_without_context_is_one_line},
    {NULL, NULL},
};
