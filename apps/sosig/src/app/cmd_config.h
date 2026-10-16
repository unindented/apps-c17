#ifndef SOSIG_CMD_CONFIG_H
#define SOSIG_CMD_CONFIG_H

#include "core/exit_code.h"
#include "core/str.h"

struct Os;

/**
 * @brief Runs the `config` command: loads the configuration and prints it resolved.
 *
 * On success prints the effective configuration to `stdout`. On a load failure prints the reason to
 * `stderr`. A failed write to `stdout` prints nothing.
 *
 * @param config_path Path of the config file.
 * @param os          Host the configuration is read from and printed on. Must not be `NULL`.
 * @return `EXIT_CODE_OK` on success, or `EXIT_CODE_FAILURE` when the configuration cannot be loaded
 *         or the resolved configuration cannot be written to `stdout`.
 */
enum ExitCode cmd_config_run(struct Str config_path, struct Os* os) __attribute__((nonnull(2)));

#endif
