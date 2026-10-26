#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include "core/log.h"
#include "core/str.h"
#include "os/os.h"
#include "os/os_memory.h"
#include "runtime/read_root.h"
#include "testing/test_support.h"

// A directory is usable, and so is an optional root that does not exist, which is the usual case
// for an optional one.
static void test_require_accepts_directories_and_missing_optional_roots(void) {
  struct Os* os = os_memory();
  os_memory_mkdir(os, "content");
  struct ReadRoot roots[] = {
      {.dir_path = S("content"), .key = S("content_dir")},
      {.dir_path = S("static"), .key = S("static_dir"), .is_optional = true},
  };
  struct Log log = test_log();
  TEST_CHECK(read_root_require(roots, 2, os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(roots[0].is_usable && roots[1].is_usable);
}

// Every root that is missing, not a directory, or cannot be inspected is reported in one run, with
// its key, its cause and its path, an optional one included unless it is missing, and a root whose
// key was rejected is neither checked nor usable.
static void test_require_reports_every_unusable_root(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "templates", "a file");
  os_memory_write(os, "releases", "a file");
  os_memory_mkdir(os, "locked/static");
  os_memory_fail(os, OS_MEMORY_OP_STAT, "locked/static", EACCES);
  struct ReadRoot roots[] = {
      {.dir_path = S("content"), .key = S("content_dir")},
      {.dir_path = S("templates"), .key = S("templates_dir")},
      {.dir_path = S("locked/static"), .key = S("static_dir"), .is_optional = true},
      {.dir_path = S("gone"), .key = S("media_dir"), .is_rejected = true},
      {.dir_path = S("releases"), .key = S("releases_dir"), .is_optional = true},
  };
  struct Log log = test_log();
  TEST_CHECK(read_root_require(roots, 5, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to resolve config directory 'content_dir': %s ('content')\n"
                         "failed to resolve config directory 'templates_dir': not a directory "
                         "('templates')\n"
                         "failed to resolve config directory 'static_dir': %s ('locked/static')\n"
                         "failed to resolve config directory 'releases_dir': not a directory "
                         "('releases')\n",
                         strerror(ENOENT), strerror(EACCES)));
  TEST_CHECK(!roots[0].is_usable && !roots[1].is_usable && !roots[2].is_usable &&
             !roots[3].is_usable && !roots[4].is_usable);
}

TEST_LIST = {
    {"require accepts directories and missing optional roots",
     test_require_accepts_directories_and_missing_optional_roots},
    {"require reports every unusable root", test_require_reports_every_unusable_root},
    {NULL, NULL},
};
