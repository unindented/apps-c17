#ifndef SOSIG_CMD_CONFIG_H
#define SOSIG_CMD_CONFIG_H

#include "core/arena.h"
#include "core/str.h"

struct Log;
struct Os;

/**
 * @brief Loads the config at `config_path` and prints it resolved as TOML, as `CliConfigPrintFn`.
 *
 * A load that read the file still reports every bad key, and none of them may be printed as the
 * resolved config, so any error fails the command, and `cli_dispatch` prints the log instead.
 *
 * @param config_path Config file to load.
 * @param os          Host the config is read and printed on. Must not be `NULL`.
 * @param perm        Arena that owns the loaded config. Must not be `NULL`.
 * @param scratch     Arena for the load and the printed text.
 * @param log         Log that receives every error of the load. Must not be `NULL`.
 * @return `0` when the config loaded with no error and printed, or `-1` otherwise.
 */
int cmd_config_print(struct Str config_path,
                     struct Os* os,
                     struct Arena* perm,
                     struct Arena scratch,
                     struct Log* log) __attribute__((nonnull(2, 3, 5)));

#endif
