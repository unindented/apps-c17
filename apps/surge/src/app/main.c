#include "app/cli_spec.h"
#include "cli/cli.h"
#include "cli/cli_dispatch.h"
#include "core/arena.h"
#include "core/str.h"
#include "os/os_posix.h"

/**
 * @brief Reports that the program ran out of memory and ends the process with `EXIT_CODE_FAILURE`.
 *
 * This is the executable's member of the `os/os.h` interface that `core/arena.h` declares, which
 * `cli_oom_exit` explains. The reasons the program can stop there are in the "Out of memory exits
 * the process" decision in ARCHITECTURE.md.
 */
_Noreturn void os_oom(void) {
  cli_oom_exit(os_posix(), S("surge"));
}

/**
 * @brief Process entry point. Delegates to `cli_dispatch` on the POSIX host.
 *
 * It only calls `cli_dispatch`, so every command decision stays in code the unit tests can link.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return The exit code `cli_dispatch` resolved.
 */
int main(int argc, char** argv) {
  struct CliSpec spec;
  cli_spec_init(&spec);
  // The cast is required, not cosmetic. The underlying type of `enum ExitCode` is implementation
  // defined and may be unsigned, so returning one uncast trips `-Wsign-conversion` under `-Werror`.
  // That looks like an inconsistency to tidy away, and tidying it breaks the build.
  return (int)cli_dispatch(&spec, argc, argv, os_posix());
}
