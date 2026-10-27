#include <acutest.h>
#include <stdbool.h>

#include "core/arena.h"
#include "os/os.h"
#include "os/os_memory.h"
#include "runtime/verbose.h"
#include "testing/test_support.h"

// An enabled status line prints formatted, with its newline, and a disabled one prints nothing.
static void test_printf_prints_only_when_enabled(void) {
  struct Os* os = os_memory();
  const struct Arena scratch = test_arena();
  struct Verbose verbose = {.os = os, .is_enabled = true, .scratch = &scratch};
  verbose_printf(&verbose, "probing media, workers: %d", 3);
  verbose.is_enabled = false;
  verbose_printf(&verbose, "not printed");
  TEST_CHECK(os_memory_is_printed(os, 2, "probing media, workers: 3\n"));
}

TEST_LIST = {
    {"printf prints only when enabled", test_printf_prints_only_when_enabled},
    {NULL, NULL},
};
