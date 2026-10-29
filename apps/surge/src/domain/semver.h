#ifndef SURGE_SEMVER_H
#define SURGE_SEMVER_H

#include <stddef.h>

#include "core/str.h"

/** A version as `versions.txt` lists it: `MAJOR.MINOR.PATCH`, with an optional `-PRE`. */
struct SemVer {
  /** Major version. */
  ptrdiff_t major;

  /** Minor version. */
  ptrdiff_t minor;

  /** Patch version. */
  ptrdiff_t patch;

  /** Pre-release identifiers after the `-`, joined by `.`, or empty for a release. */
  struct Str pre;
};

/**
 * @brief Parses `MAJOR.MINOR.PATCH[-PRE]` as Semantic Versioning 2.0.0 writes it: decimal numbers
 *        without leading zeros, and pre-release identifiers of ASCII letters, digits and `-`, a
 *        numeric one without leading zeros. Build metadata after a `+` is not accepted.
 *
 * @param text        Text of the version.
 * @param version_out Receives the version, which views `text`. Must not be `NULL`.
 * @return `0` on success, or `-1` when `text` is not such a version.
 */
int semver_parse(struct Str text, struct SemVer* version_out) __attribute__((nonnull(2)));

/**
 * @brief Orders two versions by Semantic Versioning precedence: by their numbers, a pre-release
 *        before its release, and pre-releases by their identifiers, numeric ones by value and
 *        before alphanumeric ones, which compare in ASCII order, and a shorter list first.
 *
 * @param a First version. Must not be `NULL`.
 * @param b Second version. Must not be `NULL`.
 * @return Negative, zero or positive as `a` precedes, equals, or follows `b`.
 */
int semver_compare(const struct SemVer* a, const struct SemVer* b) __attribute__((nonnull(1, 2)));

#endif
