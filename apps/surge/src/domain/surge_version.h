#ifndef SURGE_VERSION_H
#define SURGE_VERSION_H

#include "core/str.h"

/**
 * @brief Returns the version string fixed when the build tree was configured.
 *
 * @return The non-empty, statically allocated version string.
 */
struct Str surge_version_string(void);

/**
 * @brief Returns the tool name followed by its version, such as `surge 1.2.3`.
 *
 * `--version` prints this, and templates expose it as `generator`, for a page's
 * `<meta name="generator">` tag.
 *
 * @return The statically allocated generator string.
 */
struct Str surge_generator_string(void);

#endif
