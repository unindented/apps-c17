#ifndef SURGE_URL_SEGMENT_H
#define SURGE_URL_SEGMENT_H

#include "core/str.h"

struct Buf;

/**
 * @brief Appends `segment` with every byte outside RFC 3986's unreserved set, `A-Z`, `a-z`, `0-9`,
 *        `-`, `.`, `_` and `~`, written as `%XX` in uppercase hex.
 *
 * A tree name or a ref name can hold any byte but `/` and `NUL`, so each one is a segment of its
 * own. `+` is encoded too, so no host reads it as a space.
 *
 * @param buffer  Buffer that receives the encoded segment. Must not be `NULL`.
 * @param segment Bytes of one path segment.
 */
void url_segment_append(struct Buf* buffer, struct Str segment) __attribute__((nonnull(1)));

/**
 * @brief Appends a `/`-separated path with each segment encoded as `url_segment_append` does, and
 *        each `/` kept.
 *
 * @param buffer Buffer that receives the encoded path. Must not be `NULL`.
 * @param path   Path whose segments are encoded, such as a ref name `feature/x`.
 */
void url_segment_append_path(struct Buf* buffer, struct Str path) __attribute__((nonnull(1)));

#endif
