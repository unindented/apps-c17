#ifndef APPS_CLI_CLI_DISPATCH_H
#define APPS_CLI_CLI_DISPATCH_H

#include "core/exit_code.h"
#include "core/str.h"

struct CliSpec;
struct Os;

/**
 * @brief Parses the command line, then runs the requested action or command.
 *
 * Any diagnostic goes to standard error through `os`. When this function cannot write the version
 * or usage text, it returns `EXIT_CODE_FAILURE` and prints nothing.
 *
 * The `config` command reserves its own memory, loads and prints the config through the app's
 * `config_print`, and prints the load's errors when it fails, and nothing else.
 *
 * @param spec What the app tells the command line about itself. Must not be `NULL`.
 * @param argc Argument count.
 * @param argv Argument vector. Must not be `NULL`.
 * @param os   Host the selected command runs against and every message is printed on. Must not be
 *             `NULL`.
 * @return `EXIT_CODE_OK` on success, `EXIT_CODE_USAGE` on a command-line error, or
 *         `EXIT_CODE_FAILURE` when the selected command fails or the version or help text cannot be
 *         written. Does not return when the parse result is internally inconsistent.
 */
enum ExitCode cli_dispatch(const struct CliSpec* spec, int argc, char** argv, struct Os* os)
    __attribute__((nonnull(1, 3, 4)));

/**
 * @brief Prints the out-of-memory report and ends the process with `EXIT_CODE_FAILURE`, as a
 *        program's `os_oom` does.
 *
 * `os_oom` is the executable's member of the `os/os.h` interface that `core/arena.h` declares, and
 * each program defines it, because what running out of memory does is a decision about the program,
 * not about the host: the unit tests define their own, whichever host they link. A program's
 * `os_oom` calls this with its name, so the policy lives here once.
 *
 * It runs when an arena cannot commit the pages an allocation needs, when the system refuses a
 * reservation the program cannot work without, and, where a vendored library allocates with
 * `malloc`, when the wrapper around that library sees `malloc` fail. None of these has a recovery
 * the program could attempt, so the policy is to stop: this is the one place that decides it, and
 * no allocation site checks for failure. `oom_report` prints the log the running command named and
 * names the input in progress, so the user still learns what failed.
 *
 * In a program that runs worker threads it may run on one, so it uses `_Exit`, which runs no
 * `atexit` handlers that another thread could be using. Nothing is buffered to lose, because every
 * print goes through `os_print`. Two workers can run out at once, so a lock lets the first print
 * its report and exit while the second waits for that exit. A program with one thread takes the
 * lock uncontended.
 *
 * @param os      Host to print on. Must not be `NULL`.
 * @param program Name of the program, which leads the message.
 */
_Noreturn void cli_oom_exit(struct Os* os, struct Str program) __attribute__((nonnull(1)));

#endif
