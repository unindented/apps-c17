#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/path.h"
#include "core/str.h"
#include "os/os.h"
#include "os/os_posix.h"
#include "testing/test_support.h"

/** Largest file a test here reads back, in bytes. Every fixture file is a few bytes. */
enum { TEST_FILE_LEN_MAX = 1024 };

/** Operation of every call whose failure is a broken fixture, which `fixture_log` receives. */
#define FIXTURE_OPERATION S("failed to set up fixture")

/**
 * @brief Reports whether a directory holds exactly the given number of entries.
 *
 * The `.` and `..` entries are not counted.
 *
 * @param dir_path    Directory to list. Must not be `NULL`.
 * @param entry_count Number of entries the directory must hold.
 * @return `true` when the directory holds exactly `entry_count` entries, or `false` when it holds
 *         another number or cannot be listed.
 */
static bool has_entry_count(const char* dir_path, ptrdiff_t entry_count) {
  DIR* dir = opendir(dir_path);
  if (dir == NULL) {
    return false;
  }
  ptrdiff_t found_count = 0;
  const struct dirent* entry = NULL;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
      found_count++;
    }
  }
  return closedir(dir) == 0 && found_count == entry_count;
}

/**
 * @brief Returns the log of every call whose failure is a broken fixture rather than behavior under
 *        test, such as writing a fixture file or a read whose result alone is checked.
 *
 * One log serves every such call in a test process. Each failing call is also a failed check, so
 * the log adds nothing a test asserts on.
 *
 * @return The shared log.
 */
static struct Log* fixture_log(void) {
  static struct Arena arena;
  static struct Log log;
  if (log.buf.arena == NULL) {
    arena = test_arena();
    log.buf.arena = &arena;
  }
  return &log;
}

/**
 * @brief Joins a relative path onto a fixture directory, keeping a terminator after it.
 *
 * The terminator lets a test hand `data` straight to a system call that sets up the fixture, while
 * the host calls under test read the counted string.
 *
 * @param dir           Terminated fixture directory. Must not be `NULL`.
 * @param relative_path Terminated path below `dir`. Must not be `NULL`.
 * @param arena         Arena that owns the joined path. Must not be `NULL`.
 * @return The joined path, its `data` terminated.
 */
static struct Str fixture_path(const char* dir, const char* relative_path, struct Arena* arena) {
  const struct Str path = path_join(str_from_cstr(dir), str_from_cstr(relative_path), arena);
  return (struct Str){str_cstr(path, arena), path.len};
}

/**
 * Signals `count_interrupt` handles before it stops the interrupted write's reader. At one signal
 * each millisecond that is about ten seconds, far past the tenth of a second the write takes.
 */
enum { INTERRUPT_COUNT_MAX = 10000 };

/** Signals `count_interrupt` has handled since a test last reset it. */
static volatile sig_atomic_t interrupt_count = 0;

/** Process `count_interrupt` kills once it has handled `INTERRUPT_COUNT_MAX` signals, or `0`. */
static volatile sig_atomic_t interrupt_reader_pid = 0;

/** Read end `count_interrupt` closes once it has handled `INTERRUPT_COUNT_MAX` signals, or `-1`. */
static volatile sig_atomic_t interrupt_hold_fd = -1;

/**
 * @brief Counts one delivered signal. Past `INTERRUPT_COUNT_MAX`, it ends the write it interrupts.
 *
 * Installed without `SA_RESTART`, so each delivery interrupts a blocked system call. A write to a
 * FIFO blocks for as long as any read end is open, so a reader that died or never started would
 * hang the test. Killing the reader and closing the held read end leaves the FIFO with no reader,
 * so the write fails with `EPIPE` and the test fails instead. `kill` and `close` are safe to call
 * from a signal handler.
 *
 * @param signal_number Signal delivered. Unused.
 */
static void count_interrupt(int signal_number) {
  (void)signal_number;
  interrupt_count++;
  if (interrupt_count == INTERRUPT_COUNT_MAX) {
    if (interrupt_reader_pid > 0) {
      (void)kill((pid_t)interrupt_reader_pid, SIGKILL);
    }
    if (interrupt_hold_fd >= 0) {
      (void)close(interrupt_hold_fd);
      interrupt_hold_fd = -1;
    }
  }
}

/**
 * @brief Opens `fifo_path` for reading, waits, then reads it to end of file.
 *
 * The wait leaves a writer blocked on a full FIFO long enough for signals to interrupt it. This
 * runs in a forked child, so it reports through its return value rather than test assertions.
 *
 * @param fifo_path    FIFO to read. Must not be `NULL`.
 * @param expected     Bytes the writer sends. Must hold at least `expected_len` bytes. Must not be
 *                     `NULL`.
 * @param expected_len Number of bytes the writer sends.
 * @return `0` when exactly `expected` arrived, or `-1` on an open or read failure or other bytes.
 */
static int drain_fifo(const char* fifo_path, const char* expected, size_t expected_len) {
  const int fd = open(fifo_path, O_RDONLY);
  if (fd < 0) {
    return -1;
  }
  const struct timespec delay = {.tv_nsec = 100 * 1000 * 1000};
  (void)nanosleep(&delay, NULL);
  char buffer[4096];
  size_t total = 0;
  int rc = 0;
  for (;;) {
    const ssize_t nread = read(fd, buffer, sizeof(buffer));
    if (nread == 0) {
      break;
    }
    if (nread < 0 || total + (size_t)nread > expected_len ||
        memcmp(buffer, expected + total, (size_t)nread) != 0) {
      rc = -1;
      break;
    }
    total += (size_t)nread;
  }
  (void)close(fd);
  return rc == 0 && total == expected_len ? 0 : -1;
}

// These tests check what only a real filesystem has: FIFOs, modes, permission bits, interrupted
// writes, a file size limit and the open-file limit. The contract tests in `test_os.c` check the
// rest of `os.h` against this host and the in-memory one.
//
// Every call here runs with the operation `op`, so an expected failure reads `op: <cause>`. A
// fixture write that fails is a broken test rather than behavior under test, and it shows up as a
// test log that is not empty where the test checks for one.

// A FIFO is rejected as not a regular file instead of blocking the open until a writer opens it,
// leaving the outputs untouched.
static void test_read_rejects_fifo(void) {
  struct Log log = test_log();
  char root_dir[] = "/tmp/apps-fs-read-fifo.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  struct Arena arena = test_arena();
  struct Str fifo = fixture_path(root_dir, "fifo.md", &arena);
  const struct Str sentinel = S("unchanged");
  struct Str file_data = sentinel;
  if (!TEST_CHECK(mkfifo(fifo.data, 0600) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(os_read(os_posix(), fifo, TEST_FILE_LEN_MAX, OS_READ_BYTES, &arena, &file_data,
                     S("op"), &log) == -1);
  TEST_CHECK(file_data.data == sentinel.data && file_data.len == sentinel.len);
  TEST_CHECK(test_log_is(&log, "op: not a regular file ('%.*s')\n", STR_FMT(fifo)));

cleanup:
  remove_fixture_tree(root_dir);
}

// A file the process may not read fails the read with `EACCES`.
static void test_read_rejects_unreadable_file(void) {
  // Root bypasses the permission bits, so the file would open like any other and the assertions
  // below would fail for a reason that says nothing about the code under test.
  if (geteuid() == 0) {
    return;
  }
  char root_dir[] = "/tmp/apps-os-posix-unreadable.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }
  char file_path[PATH_MAX];
  const int file_path_n = snprintf(file_path, sizeof(file_path), "%s/a.c", root_dir);
  if (!TEST_CHECK(file_path_n > 0 && (size_t)file_path_n < sizeof(file_path)) ||
      write_fixture_file(root_dir, "a.c", "a") != 0) {
    remove_fixture_tree(root_dir);
    return;
  }
  struct Arena arena = test_arena();
  struct Str file_data = {0};
  struct Log log = test_log();
  int read_rc = 0;
  if (TEST_CHECK(chmod(file_path, 0) == 0)) {
    read_rc = os_read(os_posix(), str_from_cstr(file_path), TEST_FILE_LEN_MAX, OS_READ_TEXT, &arena,
                      &file_data, S("failed to read file"), &log);
  }
  // The fixture is removed before any assertion, so a failed one cannot leave it behind.
  remove_fixture_tree(root_dir);

  TEST_CHECK(read_rc == -1);
  TEST_CHECK(test_log_is(&log, "failed to read file: %s ('%s')\n", strerror(EACCES), file_path));
}

// `os_write` creates a new file with `0666` reduced by the process umask, from every source, so a
// restrictive umask is not discarded and a copy does not take its source's mode. Overwriting an
// existing file keeps that file's mode, because the write goes through the file.
static void test_write_applies_umask_and_keeps_existing_mode(void) {
  struct Log log = test_log();
  char root_dir[] = "/tmp/apps-fs-mode.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  struct Arena arena = test_arena();
  struct Str created = fixture_path(root_dir, "created.html", &arena);
  struct Str existing = fixture_path(root_dir, "existing.html", &arena);
  struct Str source = fixture_path(root_dir, "source.css", &arena);
  struct Str copied = fixture_path(root_dir, "copied.css", &arena);
  const struct OsSource copy = {.kind = OS_SOURCE_FILE, .file_path = source};

  // A restrictive umask is the case a hardcoded mode would discard. Restore it before asserting, so
  // a failure cannot leak the tightened value into later tests.
  const mode_t umask_before = umask(077);
  TEST_CHECK(os_write(os_posix(), created, (struct OsSource){.bytes = S("x")}, S("op"), &log) == 0);
  TEST_CHECK(os_write(os_posix(), existing, (struct OsSource){.bytes = S("x")}, S("op"), &log) ==
             0);
  TEST_CHECK(chmod(existing.data, 0640) == 0);
  TEST_CHECK(os_write(os_posix(), existing, (struct OsSource){.bytes = S("y")}, S("op"), &log) ==
             0);
  // The source is given wide permissions the copy must not inherit.
  TEST_CHECK(os_write(os_posix(), source, (struct OsSource){.bytes = S("body{}")}, S("op"), &log) ==
             0);
  TEST_CHECK(chmod(source.data, 0777) == 0);
  TEST_CHECK(os_write(os_posix(), copied, copy, S("op"), &log) == 0);
  (void)umask(umask_before);

  TEST_CHECK(test_log_is_empty(&log));
  const mode_t expected = (mode_t)(0666 & ~077);
  struct stat st;
  TEST_CHECK(stat(created.data, &st) == 0 && (st.st_mode & 07777) == expected);
  TEST_CHECK(stat(existing.data, &st) == 0 && (st.st_mode & 07777) == 0640);
  TEST_CHECK(stat(copied.data, &st) == 0 && (st.st_mode & 07777) == expected);
  remove_fixture_tree(root_dir);
}

// `os_write` finishes a write that signals interrupt instead of failing with `EINTR`. A FIFO whose
// reader starts late stands in for a slow file, because a write to a local regular file is not
// interrupted. The handler is installed without `SA_RESTART`, so each signal does interrupt the
// blocked `write`. Two ways this could hang are closed: a write blocked on a reader that is gone
// (the handler stops it, see `count_interrupt`), and a reader blocked in its `open` because the
// write failed before it opened the FIFO (the reader is then stopped, not awaited).
static void test_write_retries_interrupted_write(void) {
  struct Log log = test_log();
  char root_dir[] = "/tmp/apps-fs-write-eintr.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  struct Arena arena = test_arena();
  struct Str fifo = fixture_path(root_dir, "fifo.html", &arena);
  // Larger than any pipe buffer, so the write blocks until the reader drains it.
  static char data[1024 * 1024];
  int hold_fd = -1;
  pid_t reader = -1;
  struct sigaction action = {.sa_handler = count_interrupt};
  struct sigaction alarm_action_before;
  struct sigaction pipe_action_ignore = {.sa_handler = SIG_IGN};
  struct sigaction pipe_action_before;
  const struct itimerval every_millisecond = {.it_interval = {.tv_usec = 1000},
                                              .it_value = {.tv_usec = 1000}};
  const struct itimerval stopped = {0};
  int rc = -1;
  int status = 0;
  if (!TEST_CHECK(mkfifo(fifo.data, 0600) == 0)) {
    goto cleanup;
  }
  // A read end held open lets the write's `open` return at once, so only its `write` blocks.
  hold_fd = open(fifo.data, O_RDONLY | O_NONBLOCK);
  if (!TEST_CHECK(hold_fd >= 0)) {
    goto cleanup;
  }

  for (size_t i = 0; i < sizeof(data); i++) {
    data[i] = (char)('a' + i % 26);
  }

  reader = fork();
  if (!TEST_CHECK(reader >= 0)) {
    goto cleanup;
  }
  if (reader == 0) {
    _exit(drain_fifo(fifo.data, data, sizeof(data)) == 0 ? 0 : 1);
  }

  (void)sigemptyset(&action.sa_mask);
  (void)sigemptyset(&pipe_action_ignore.sa_mask);
  // A write with no reader left fails with `EPIPE` rather than ending the process.
  if (!TEST_CHECK(sigaction(SIGPIPE, &pipe_action_ignore, &pipe_action_before) == 0)) {
    goto cleanup;
  }
  if (!TEST_CHECK(sigaction(SIGALRM, &action, &alarm_action_before) == 0)) {
    (void)sigaction(SIGPIPE, &pipe_action_before, NULL);
    goto cleanup;
  }
  interrupt_count = 0;
  interrupt_reader_pid = reader;
  interrupt_hold_fd = hold_fd;
  if (!TEST_CHECK(setitimer(ITIMER_REAL, &every_millisecond, NULL) == 0)) {
    (void)sigaction(SIGALRM, &alarm_action_before, NULL);
    (void)sigaction(SIGPIPE, &pipe_action_before, NULL);
    goto cleanup;
  }
  rc = os_write(os_posix(), fifo, (struct OsSource){.bytes = (struct Str){data, sizeof(data)}},
                S("op"), &log);
  // Stop the timer before restoring the previous action, so no signal reaches the default one.
  (void)setitimer(ITIMER_REAL, &stopped, NULL);
  (void)sigaction(SIGALRM, &alarm_action_before, NULL);
  (void)sigaction(SIGPIPE, &pipe_action_before, NULL);
  // The handler may have closed the held read end. It owns it from here.
  hold_fd = interrupt_hold_fd;
  interrupt_reader_pid = 0;
  interrupt_hold_fd = -1;
  // A write that failed may have failed before it opened the FIFO, and then the reader still waits
  // in its `open` for a writer. Only a reader that got the whole write is awaited.
  if (rc == 0) {
    TEST_CHECK(waitpid(reader, &status, 0) == reader);
    reader = -1;
  }

  TEST_CHECK(rc == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(interrupt_count > 0);
  TEST_CHECK(rc == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0);

cleanup:
  // A reader that never saw a writer would block in its `open` forever, so it is stopped rather
  // than awaited.
  if (reader > 0) {
    (void)kill(reader, SIGKILL);
    (void)waitpid(reader, NULL, 0);
  }
  if (hold_fd >= 0) {
    (void)close(hold_fd);
  }
  remove_fixture_tree(root_dir);
}

// A FIFO file source is rejected as not a regular file instead of blocking the write until a writer
// opens it, and the destination is never created.
static void test_write_rejects_fifo_source(void) {
  struct Log log = test_log();
  char root_dir[] = "/tmp/apps-fs-copy-fifo.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  struct Arena arena = test_arena();
  struct Str fifo = fixture_path(root_dir, "source.mp4", &arena);
  struct Str dest = fixture_path(root_dir, "copy.mp4", &arena);
  if (!TEST_CHECK(mkfifo(fifo.data, 0600) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(os_write(os_posix(), dest,
                      (struct OsSource){.kind = OS_SOURCE_FILE, .file_path = fifo}, S("op"),
                      &log) == -1);
  TEST_CHECK(test_log_is(&log, "op: not a regular file ('%.*s')\n", STR_FMT(fifo)));
  struct OsStat st;
  TEST_CHECK(os_stat(os_posix(), dest, &st) == -1);

cleanup:
  remove_fixture_tree(root_dir);
}

// A write that the system cuts short, here at a file size limit, fails with the system's message
// naming the destination, and leaves the bytes below the limit and nothing beside the file.
static void test_write_reports_destination_when_write_fails(void) {
  struct Log log = test_log();
  char root_dir[] = "/tmp/apps-fs-write-fsize.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  struct Arena arena = test_arena();
  struct Str output = fixture_path(root_dir, "out.jpg", &arena);
  TEST_CHECK(os_write(os_posix(), output, (struct OsSource){.bytes = S("old")}, S("op"), &log) ==
             0);
  struct rlimit limit;
  struct sigaction fsize_action_ignore = {.sa_handler = SIG_IGN};
  struct sigaction fsize_action_before;
  (void)sigemptyset(&fsize_action_ignore.sa_mask);
  // A write past the limit fails with `EFBIG` once `SIGXFSZ` is ignored, after the bytes below it.
  struct rlimit lowered = {.rlim_cur = 4};
  int rc = 0;
  if (TEST_CHECK(getrlimit(RLIMIT_FSIZE, &limit) == 0) &&
      TEST_CHECK(sigaction(SIGXFSZ, &fsize_action_ignore, &fsize_action_before) == 0)) {
    lowered.rlim_max = limit.rlim_max;
    if (TEST_CHECK(setrlimit(RLIMIT_FSIZE, &lowered) == 0)) {
      rc = os_write(os_posix(), output, (struct OsSource){.bytes = S("longer than four")}, S("op"),
                    &log);
      // Restore the limit before asserting, so a failure cannot cut later tests' files short.
      (void)setrlimit(RLIMIT_FSIZE, &limit);
    }
    (void)sigaction(SIGXFSZ, &fsize_action_before, NULL);
  }

  TEST_CHECK(rc == -1);
  TEST_CHECK(test_log_is(&log, "op: %s ('%.*s')\n", strerror(EFBIG), STR_FMT(output)));
  struct Str data = {0};
  TEST_CHECK(os_read(os_posix(), output, TEST_FILE_LEN_MAX, OS_READ_BYTES, &arena, &data, S("op"),
                     &log) == 0);
  TEST_CHECK(str_eq(data, S("long")));
  struct OsEntry* entries = NULL;
  ptrdiff_t count = 0;
  TEST_CHECK(
      os_list(os_posix(), str_from_cstr(root_dir), &arena, &entries, &count, S("op"), &log) == 0);
  TEST_CHECK(count == 1);
  remove_fixture_tree(root_dir);
}

// A write to an existing file in a directory that may not be written succeeds, because it goes
// through the file, as a rebuilt output in a read-only directory does. A new file there fails,
// naming the destination.
static void test_write_goes_through_existing_file_in_read_only_dir(void) {
  struct Log log = test_log();
  // Root bypasses the permission bits, so the new file would be created.
  if (geteuid() == 0) {
    return;
  }

  char root_dir[] = "/tmp/apps-fs-write-read-only.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  struct Arena arena = test_arena();
  struct Str sealed_dir = fixture_path(root_dir, "sealed", &arena);
  struct Str existing = fixture_path(root_dir, "sealed/a-jpg-s.jpg", &arena);
  struct Str created = fixture_path(root_dir, "sealed/a-jpg-m.jpg", &arena);
  TEST_CHECK(os_write(os_posix(), existing, (struct OsSource){.bytes = S("old")}, S("op"), &log) ==
             0);
  TEST_CHECK(chmod(sealed_dir.data, 0500) == 0);
  const int existing_rc =
      os_write(os_posix(), existing, (struct OsSource){.bytes = S("new")}, S("op"), &log);
  struct Log created_log = test_log();
  const int created_rc =
      os_write(os_posix(), created, (struct OsSource){.bytes = S("new")}, S("op"), &created_log);
  // Restore the mode before checking anything. The cleanup below cannot remove a directory it may
  // not write.
  (void)chmod(sealed_dir.data, 0700);

  TEST_CHECK(existing_rc == 0);
  TEST_CHECK(test_log_is_empty(&log));
  struct Str data = {0};
  TEST_CHECK(os_read(os_posix(), existing, TEST_FILE_LEN_MAX, OS_READ_BYTES, &arena, &data, S("op"),
                     &log) == 0);
  TEST_CHECK(str_eq(data, S("new")));
  TEST_CHECK(created_rc == -1);
  TEST_CHECK(test_log_is(&created_log, "op: %s ('%.*s')\n", strerror(EACCES), STR_FMT(created)));
  remove_fixture_tree(root_dir);
}

// `os_replace` creates a new file with `0666` reduced by the process umask rather than `mkstemp`'s
// `0600`, and replacing an existing file keeps that file's mode. No temporary survives either
// write, which the count of entries left in the fixture directory checks.
static void test_replace_applies_umask_and_keeps_existing_mode(void) {
  char root_dir[] = "/tmp/apps-os-posix-mode.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }
  char created[PATH_MAX];
  const int created_n = snprintf(created, sizeof(created), "%s/created.c", root_dir);
  char existing[PATH_MAX];
  const int existing_n = snprintf(existing, sizeof(existing), "%s/existing.c", root_dir);
  mode_t umask_before = 0;
  struct stat st;
  if (!TEST_CHECK(created_n > 0 && (size_t)created_n < sizeof(created))) {
    goto cleanup;
  }
  if (!TEST_CHECK(existing_n > 0 && (size_t)existing_n < sizeof(existing))) {
    goto cleanup;
  }

  // A umask other than the usual `022` is the case a hardcoded mode would discard. It also must not
  // be `077`, whose result is `mkstemp`'s own `0600` and so could not show the mode was applied.
  // Restore it before asserting, so a failure cannot leak the changed value into later tests.
  umask_before = umask(027);
  TEST_CHECK(os_replace(os_posix(), str_from_cstr(created), S("x"), FIXTURE_OPERATION,
                        fixture_log()) == 0);
  TEST_CHECK(os_replace(os_posix(), str_from_cstr(existing), S("x"), FIXTURE_OPERATION,
                        fixture_log()) == 0);
  TEST_CHECK(chmod(existing, 0640) == 0);
  TEST_CHECK(os_replace(os_posix(), str_from_cstr(existing), S("y"), FIXTURE_OPERATION,
                        fixture_log()) == 0);
  (void)umask(umask_before);

  TEST_CHECK(stat(created, &st) == 0 && (st.st_mode & 07777) == (mode_t)(0666 & ~027));
  TEST_CHECK(stat(existing, &st) == 0 && (st.st_mode & 07777) == 0640);
  TEST_CHECK(has_entry_count(root_dir, 2));

cleanup:
  remove_fixture_tree(root_dir);
}

// Atomic replacement follows a symbolic link and leaves the link itself in place.
static void test_replace_follows_symbolic_link(void) {
  char root_dir[] = "/tmp/apps-os-posix-link.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }
  char target_path[PATH_MAX];
  const int target_path_n = snprintf(target_path, sizeof(target_path), "%s/target.c", root_dir);
  char link_path[PATH_MAX];
  const int link_path_n = snprintf(link_path, sizeof(link_path), "%s/link.c", root_dir);
  struct Arena arena = test_arena();
  struct stat link_st;
  struct Str file_data = {0};
  if (!TEST_CHECK(target_path_n > 0 && (size_t)target_path_n < sizeof(target_path)) ||
      !TEST_CHECK(link_path_n > 0 && (size_t)link_path_n < sizeof(link_path)) ||
      write_fixture_file(root_dir, "target.c", "before") != 0) {
    goto cleanup;
  }
  if (!TEST_CHECK(symlink(target_path, link_path) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(os_replace(os_posix(), str_from_cstr(link_path), S("after"), FIXTURE_OPERATION,
                        fixture_log()) == 0);
  if (!TEST_CHECK(lstat(link_path, &link_st) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(S_ISLNK(link_st.st_mode));
  TEST_CHECK(os_read(os_posix(), str_from_cstr(target_path), TEST_FILE_LEN_MAX, OS_READ_TEXT,
                     &arena, &file_data, FIXTURE_OPERATION, fixture_log()) == 0);
  TEST_CHECK(str_eq(file_data, S("after")));

cleanup:
  remove_fixture_tree(root_dir);
}

// `os_replace` fails when the parent directory is missing, when a parent component is a regular
// file, and when the target is itself a directory, and reports the three as different system
// reasons. A replacement creates no parent directories, so an absent parent is a failure rather
// than something to fill in. A file in the parent position fails the destination `stat` with
// `ENOTDIR` before any temporary file is created.
static void test_replace_rejects_missing_or_file_parent_and_dir_target(void) {
  char root_dir[] = "/tmp/apps-os-posix-write-fail.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  char missing_parent_path[PATH_MAX];
  const int missing_n =
      snprintf(missing_parent_path, sizeof(missing_parent_path), "%s/missing/x.c", root_dir);
  char blocking_file[PATH_MAX];
  const int blocking_n = snprintf(blocking_file, sizeof(blocking_file), "%s/plain.c", root_dir);
  char through_file[PATH_MAX];
  const int through_n = snprintf(through_file, sizeof(through_file), "%s/x.c", blocking_file);
  char dir_target[PATH_MAX];
  const int target_n = snprintf(dir_target, sizeof(dir_target), "%s/target", root_dir);
  struct Log missing_log = test_log();
  struct Log through_log = test_log();
  struct Log dir_log = test_log();
  if (!TEST_CHECK(missing_n > 0 && (size_t)missing_n < sizeof(missing_parent_path))) {
    goto cleanup;
  }
  if (!TEST_CHECK(blocking_n > 0 && (size_t)blocking_n < sizeof(blocking_file))) {
    goto cleanup;
  }
  if (!TEST_CHECK(through_n > 0 && (size_t)through_n < sizeof(through_file))) {
    goto cleanup;
  }
  if (!TEST_CHECK(target_n > 0 && (size_t)target_n < sizeof(dir_target))) {
    goto cleanup;
  }

  TEST_CHECK(os_replace(os_posix(), str_from_cstr(missing_parent_path), S("x"),
                        S("failed to write file"), &missing_log) == -1);
  TEST_CHECK(test_log_is(&missing_log, "failed to write file: %s ('%s')\n", strerror(ENOENT),
                         missing_parent_path));

  TEST_CHECK(os_replace(os_posix(), str_from_cstr(blocking_file), S("x"), FIXTURE_OPERATION,
                        fixture_log()) == 0);
  TEST_CHECK(os_replace(os_posix(), str_from_cstr(through_file), S("x"), S("failed to write file"),
                        &through_log) == -1);
  TEST_CHECK(test_log_is(&through_log, "failed to write file: %s ('%s')\n", strerror(ENOTDIR),
                         through_file));

  if (!TEST_CHECK(mkdir(dir_target, 0700) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(os_replace(os_posix(), str_from_cstr(dir_target), S("x"), S("failed to write file"),
                        &dir_log) == -1);
  TEST_CHECK(
      test_log_is(&dir_log, "failed to write file: %s ('%s')\n", strerror(EISDIR), dir_target));

cleanup:
  remove_fixture_tree(root_dir);
}

// A dangling symbolic link fails with the `ENOENT` that `realpath` reports instead of creating the
// file the link names, and nothing is left in the link's directory. Following the link to a missing
// target would otherwise write a file at a path the caller never passed.
static void test_replace_rejects_dangling_symbolic_link(void) {
  char root_dir[] = "/tmp/apps-os-posix-dangling.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  char target_path[PATH_MAX];
  const int target_n = snprintf(target_path, sizeof(target_path), "%s/missing.c", root_dir);
  char link_path[PATH_MAX];
  const int link_n = snprintf(link_path, sizeof(link_path), "%s/link.c", root_dir);
  struct Log log = test_log();
  struct stat target_st;
  if (!TEST_CHECK(target_n > 0 && (size_t)target_n < sizeof(target_path))) {
    goto cleanup;
  }
  if (!TEST_CHECK(link_n > 0 && (size_t)link_n < sizeof(link_path))) {
    goto cleanup;
  }
  if (!TEST_CHECK(symlink(target_path, link_path) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(os_replace(os_posix(), str_from_cstr(link_path), S("x"), S("failed to write file"),
                        &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to write file: %s ('%s')\n", strerror(ENOENT), link_path));
  TEST_CHECK(lstat(target_path, &target_st) != 0);
  // The link is the only entry, which proves no temporary was left behind.
  TEST_CHECK(has_entry_count(root_dir, 1));

cleanup:
  remove_fixture_tree(root_dir);
}

// A destination whose temporary sibling would not fit in `PATH_MAX` bytes fails as `ENAMETOOLONG`,
// as the system would fail the temporary itself, even though the destination path fits.
static void test_replace_rejects_temporary_path_past_path_max(void) {
  static char long_path[PATH_MAX - 4];
  long_path[0] = '/';
  for (ptrdiff_t i = 1; i < (ptrdiff_t)(sizeof(long_path)); i++) {
    long_path[i] = i % 64 == 0 ? '/' : 'a';
  }
  long_path[sizeof(long_path) - 2] = '/';

  const struct Str path = {long_path, sizeof(long_path)};
  struct Log log = test_log();
  TEST_CHECK(os_replace(os_posix(), path, S("x"), S("failed to write file"), &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to write file: %s ('%.*s')\n", strerror(ENAMETOOLONG),
                         STR_FMT(path)));
}

// `os_mkdirs` fails when `mkdir` itself is refused. The failure is neither the first-party type
// rejection nor the `EEXIST` fallthrough, which `test_os.c` covers, but the system error reported
// straight from the create. A parent that is readable but not writable is the way to produce it.
static void test_mkdirs_rejects_uncreatable_component(void) {
  struct Log log = test_log();
  // Root bypasses the permission bits, so `mkdir` would succeed and the assertions below would fail
  // for a reason that says nothing about the code under test.
  if (geteuid() == 0) {
    return;
  }

  char root_dir[] = "/tmp/apps-fs-mkdir-refused.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  struct Arena arena = test_arena();
  struct Str sealed_dir = fixture_path(root_dir, "sealed", &arena);
  struct Str child_dir = fixture_path(root_dir, "sealed/child", &arena);
  TEST_CHECK(os_mkdirs(os_posix(), sealed_dir, S("op"), &log) == 0);
  TEST_CHECK(chmod(sealed_dir.data, 0500) == 0);

  const int rc = os_mkdirs(os_posix(), child_dir, S("op"), &log);
  // Restore the mode before checking anything, as `test_list_rejects_unstatable_entry` does. The
  // cleanup below cannot remove a directory it may not write.
  (void)chmod(sealed_dir.data, 0700);

  TEST_CHECK(rc == -1);
  TEST_CHECK(test_log_is(&log, "op: cannot create directory: %s ('%.*s')\n", strerror(EACCES),
                         STR_FMT(child_dir)));
  remove_fixture_tree(root_dir);
}

// `os_list` leaves out a FIFO, which names no regular file or directory, and keeps the regular file
// beside it. `test_os.c` checks the rest of the classification against both hosts.
static void test_list_skips_fifo(void) {
  struct Log log = test_log();
  char root_dir[] = "/tmp/apps-fs-list-fifo.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  struct Arena arena = test_arena();
  TEST_CHECK(os_write(os_posix(), fixture_path(root_dir, "file.md", &arena),
                      (struct OsSource){.bytes = S("b")}, S("op"), &log) == 0);
  TEST_CHECK(mkfifo(fixture_path(root_dir, "fifo", &arena).data, 0600) == 0);

  struct OsEntry* entries = NULL;
  ptrdiff_t count = 0;
  TEST_CHECK(
      os_list(os_posix(), str_from_cstr(root_dir), &arena, &entries, &count, S("op"), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(count == 1 && str_eq(entries[0].name, S("file.md")) &&
             entries[0].kind == OS_ENTRY_FILE);
  remove_fixture_tree(root_dir);
}

TEST_LIST = {
    {"read rejects fifo", test_read_rejects_fifo},
    {"read rejects unreadable file", test_read_rejects_unreadable_file},
    {"write applies umask and keeps existing mode",
     test_write_applies_umask_and_keeps_existing_mode},
    {"write retries interrupted write", test_write_retries_interrupted_write},
    {"write rejects fifo source", test_write_rejects_fifo_source},
    {"write reports destination when write fails", test_write_reports_destination_when_write_fails},
    {"write goes through existing file in read only dir",
     test_write_goes_through_existing_file_in_read_only_dir},
    {"replace applies umask and keeps existing mode",
     test_replace_applies_umask_and_keeps_existing_mode},
    {"replace follows symbolic link", test_replace_follows_symbolic_link},
    {"replace rejects missing or file parent and dir target",
     test_replace_rejects_missing_or_file_parent_and_dir_target},
    {"replace rejects dangling symbolic link", test_replace_rejects_dangling_symbolic_link},
    {"replace rejects temporary path past path max",
     test_replace_rejects_temporary_path_past_path_max},
    {"mkdirs rejects uncreatable component", test_mkdirs_rejects_uncreatable_component},
    {"list skips fifo", test_list_skips_fifo},
    {NULL, NULL},
};
