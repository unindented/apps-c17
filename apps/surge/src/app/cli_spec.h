#ifndef SURGE_CLI_SPEC_H
#define SURGE_CLI_SPEC_H

#include "cli/cli.h"

/**
 * @brief Fills `spec` with what surge tells the shared command line about itself: its description
 *        and noun, its default config file, its version text, and its `build` and `config`
 *        commands.
 *
 * @param spec Spec to fill, whose strings are static once filled. Must not be `NULL`.
 */
void cli_spec_init(struct CliSpec* spec) __attribute__((nonnull(1)));

#endif
