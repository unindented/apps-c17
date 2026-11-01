#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "app/wrap_input.h"
#include "core/arena.h"
#include "core/log.h"
#include "core/str.h"
#include "os/oom.h"
#include "os/os_memory.h"
#include "testing/test_support.h"

// These tests run on the in-memory host, so each sets up its files and standard input in the host
// and reads what the input printed back from it. `os/test_os.c` in `libs/` checks that the host
// behaves as the POSIX one does for every property relied on here.

// Stable file input succeeds silently in check mode.
static void test_file_check_ok_result(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "a.c", "// one two\n");
  struct Log log = test_log();

  const enum WrapInputResult result =
      wrap_input_process_file(S("a.c"), 40, false, true, os, test_arena(), test_arena(), &log);

  TEST_CHECK(result == WRAP_INPUT_RESULT_OK);
  TEST_CHECK(test_log_is_empty(&log));
}

// Stable standard input succeeds silently in check mode.
static void test_stdin_check_ok_result(void) {
  struct Os* os = os_memory();
  os_memory_stdin(os, S("// one two\n"), 0);
  struct Log log = test_log();

  const enum WrapInputResult result =
      wrap_input_process_stdin(40, true, os, test_arena(), test_arena(), &log);

  TEST_CHECK(result == WRAP_INPUT_RESULT_OK);
  TEST_CHECK(test_log_is_empty(&log));
}

// File check mode retains the distinct changed result and path attribution, and writes nothing.
static void test_file_check_change_result(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "a.c", "// one\n// two\n");
  struct Log log = test_log();

  const enum WrapInputResult result =
      wrap_input_process_file(S("a.c"), 40, false, true, os, test_arena(), test_arena(), &log);

  TEST_CHECK(result == WRAP_INPUT_RESULT_CHANGED);
  TEST_CHECK(test_log_is(&log, "would rewrap 'a.c'\n"));
  TEST_CHECK(os_memory_is_file(os, "a.c", "// one\n// two\n"));
  TEST_CHECK(os_memory_printed(os, 1).len == 0);
}

// Standard input in check mode names it `stdin` when it reports a change.
static void test_stdin_check_change_result(void) {
  struct Os* os = os_memory();
  os_memory_stdin(os, S("// one\n// two\n"), 0);
  struct Log log = test_log();

  const enum WrapInputResult result =
      wrap_input_process_stdin(40, true, os, test_arena(), test_arena(), &log);

  TEST_CHECK(result == WRAP_INPUT_RESULT_CHANGED);
  TEST_CHECK(test_log_is(&log, "would rewrap 'stdin'\n"));
}

// A file in the default mode prints its rewritten source to standard output, and in-place mode
// writes it back to the file instead, printing nothing.
static void test_file_emits_rewritten_source(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "a.c", "// one\n// two\n");
  struct Log log = test_log();

  const enum WrapInputResult printed_result =
      wrap_input_process_file(S("a.c"), 40, false, false, os, test_arena(), test_arena(), &log);
  TEST_CHECK(printed_result == WRAP_INPUT_RESULT_OK);
  TEST_CHECK(os_memory_is_printed(os, 1, "// one two\n"));
  TEST_CHECK(os_memory_is_file(os, "a.c", "// one\n// two\n"));

  struct Os* in_place_os = os_memory();
  os_memory_write(in_place_os, "a.c", "// one\n// two\n");
  const enum WrapInputResult written_result = wrap_input_process_file(
      S("a.c"), 40, true, false, in_place_os, test_arena(), test_arena(), &log);
  TEST_CHECK(written_result == WRAP_INPUT_RESULT_OK);
  TEST_CHECK(os_memory_is_file(in_place_os, "a.c", "// one two\n"));
  TEST_CHECK(os_memory_printed(in_place_os, 1).len == 0);
  TEST_CHECK(test_log_is_empty(&log));
}

// A file names itself for the out-of-memory report, in every mode, and the report leads with the
// program's name.
static void test_file_names_itself_for_oom_report(void) {
  static const struct {
    bool is_in_place;
    bool is_check;
  } cases[] = {{false, true}, {true, false}, {false, false}};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct Os* os = os_memory();
    os_memory_write(os, "a.c", "// one two\n");
    struct Log log = test_log();
    const enum WrapInputResult result =
        wrap_input_process_file(S("a.c"), 40, cases[i].is_in_place, cases[i].is_check, os,
                                test_arena(), test_arena(), &log);
    struct Os* report_os = os_memory();
    oom_report(report_os, S("cwrap"));
    oom_set_job((struct Str){0}, NULL);

    TEST_CHECK(result == WRAP_INPUT_RESULT_OK);
    TEST_CHECK(os_memory_is_printed(report_os, 2, "cwrap: out of memory (in 'a.c')\n"));
    TEST_MSG("case %td", i);
  }
}

// Standard input names itself for the out-of-memory report as `stdin`.
static void test_stdin_names_itself_for_oom_report(void) {
  struct Os* os = os_memory();
  os_memory_stdin(os, S("// one two\n"), 0);
  struct Log log = test_log();
  const enum WrapInputResult result =
      wrap_input_process_stdin(40, false, os, test_arena(), test_arena(), &log);
  struct Os* report_os = os_memory();
  oom_report(report_os, S("cwrap"));
  oom_set_job((struct Str){0}, NULL);

  TEST_CHECK(result == WRAP_INPUT_RESULT_OK);
  TEST_CHECK(os_memory_is_printed(report_os, 2, "cwrap: out of memory (in 'stdin')\n"));
}

// A file read error remains distinct from a check-mode change and keeps the existing diagnostic,
// for a missing file and for a read that fails after the open.
static void test_file_read_error_result(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  const enum WrapInputResult missing_result = wrap_input_process_file(
      S("missing.c"), 40, false, true, os, test_arena(), test_arena(), &log);
  os_memory_write(os, "a.c", "// one\n");
  os_memory_fail(os, OS_MEMORY_OP_READ, "a.c", EIO);
  const enum WrapInputResult failed_result =
      wrap_input_process_file(S("a.c"), 40, false, true, os, test_arena(), test_arena(), &log);

  TEST_CHECK(missing_result == WRAP_INPUT_RESULT_ERROR);
  TEST_CHECK(failed_result == WRAP_INPUT_RESULT_ERROR);
  TEST_CHECK(test_log_is(&log,
                         "failed to read file: %s ('missing.c')\n"
                         "failed to read file: %s ('a.c')\n",
                         strerror(ENOENT), strerror(EIO)));
}

// A source file over `SOURCE_LEN_MAX` is rejected from its size before it is read, naming the limit
// and the size. The file is extended as `truncate` would, so the host holds only its first byte.
static void test_file_oversize_error_result(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "a.c", "a");
  const ptrdiff_t oversize_len = (ptrdiff_t)SOURCE_LEN_MAX + 1;
  os_memory_truncate(os, "a.c", oversize_len);

  struct Log log = test_log();
  const enum WrapInputResult result =
      wrap_input_process_file(S("a.c"), 40, false, true, os, test_arena(), test_arena(), &log);

  TEST_CHECK(result == WRAP_INPUT_RESULT_ERROR);
  TEST_CHECK(test_log_is(&log,
                         "failed to read file: exceeds max file size (%d bytes) at %td bytes "
                         "('a.c')\n",
                         SOURCE_LEN_MAX, oversize_len));
}

// A standard input read failure is attributed to `stdin` and remains an operational error.
static void test_stdin_read_error_result(void) {
  struct Os* os = os_memory();
  os_memory_stdin(os, S("// one\n"), EIO);
  struct Log log = test_log();

  const enum WrapInputResult result =
      wrap_input_process_stdin(40, true, os, test_arena(), test_arena(), &log);

  TEST_CHECK(result == WRAP_INPUT_RESULT_ERROR);
  TEST_CHECK(test_log_is(&log, "failed to read input: %s (from 'stdin')\n", strerror(EIO)));
}

// A file whose output cannot be written to standard output fails with the write's own reason,
// naming the file, whether the stream fails at once or after a short write.
static void test_file_stdout_error_result(void) {
  static const int error_numbers[] = {EPIPE, EIO};
  static const ptrdiff_t accepted_lens[] = {0, 4};
  for (ptrdiff_t i = 0; i < 2; i++) {
    struct Os* os = os_memory();
    os_memory_write(os, "a.c", "// one\n// two\n");
    os_memory_fail_print(os, 1, accepted_lens[i], error_numbers[i]);
    struct Log log = test_log();
    const enum WrapInputResult result =
        wrap_input_process_file(S("a.c"), 40, false, false, os, test_arena(), test_arena(), &log);

    TEST_CHECK(result == WRAP_INPUT_RESULT_ERROR);
    TEST_CHECK(test_log_is(&log, "failed to write rewritten file to 'stdout': %s (for 'a.c')\n",
                           strerror(error_numbers[i])));
    TEST_CHECK(os_memory_printed(os, 1).len == accepted_lens[i]);
  }
}

// An in-place write that fails leaves the file as it was and reports the write's own reason.
static void test_file_in_place_write_error_result(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "a.c", "// one\n// two\n");
  os_memory_fail(os, OS_MEMORY_OP_WRITE, "a.c", ENOSPC);
  struct Log log = test_log();
  const enum WrapInputResult result =
      wrap_input_process_file(S("a.c"), 40, true, false, os, test_arena(), test_arena(), &log);

  TEST_CHECK(result == WRAP_INPUT_RESULT_ERROR);
  TEST_CHECK(test_log_is(&log, "failed to write file: %s ('a.c')\n", strerror(ENOSPC)));
  TEST_CHECK(os_memory_is_file(os, "a.c", "// one\n// two\n"));
}

// Standard input whose output cannot be written fails the same way, naming the stream.
static void test_stdin_stdout_error_result(void) {
  struct Os* os = os_memory();
  os_memory_stdin(os, S("// one\n"), 0);
  os_memory_fail_print(os, 1, 0, EPIPE);
  struct Log log = test_log();
  const enum WrapInputResult result =
      wrap_input_process_stdin(40, false, os, test_arena(), test_arena(), &log);

  TEST_CHECK(result == WRAP_INPUT_RESULT_ERROR);
  TEST_CHECK(test_log_is(&log, "failed to write rewritten input to 'stdout': %s (from 'stdin')\n",
                         strerror(EPIPE)));
}

TEST_LIST = {
    {"file check ok result", test_file_check_ok_result},
    {"stdin check ok result", test_stdin_check_ok_result},
    {"file check change result", test_file_check_change_result},
    {"stdin check change result", test_stdin_check_change_result},
    {"file emits rewritten source", test_file_emits_rewritten_source},
    {"file names itself for oom report", test_file_names_itself_for_oom_report},
    {"stdin names itself for oom report", test_stdin_names_itself_for_oom_report},
    {"file read error result", test_file_read_error_result},
    {"file oversize error result", test_file_oversize_error_result},
    {"stdin read error result", test_stdin_read_error_result},
    {"file stdout error result", test_file_stdout_error_result},
    {"file in place write error result", test_file_in_place_write_error_result},
    {"stdin stdout error result", test_stdin_stdout_error_result},
    {NULL, NULL},
};
