#ifndef SURGE_TIMESTAMP_H
#define SURGE_TIMESTAMP_H

#include <stdint.h>

#include "core/str.h"

struct Arena;

/**
 * @brief Formats a git time and offset as an RFC 3339 date-time in that offset, such as
 *        `2026-01-09T12:00:00+01:00`.
 *
 * The date is computed from the proleptic Gregorian calendar, without the C library's time zone, so
 * it is the same on every host. An offset of zero is written `Z`, as every date the apps write
 * spells it. RFC 3339 writes an offset of at most 23:59 either way and a year from 0000 to 9999, so
 * a larger offset is written as `Z`, and a time whose local date is outside those years as the
 * epoch, `1970-01-01T00:00:00Z`.
 *
 * @param seconds        Seconds since the Unix epoch, in UTC, any value.
 * @param offset_minutes Offset of the local time from UTC, in minutes, as git records it, any
 *                       value.
 * @param arena          Arena that owns the text. Must not be `NULL`.
 * @return The date-time.
 */
struct Str timestamp_format_rfc3339(int64_t seconds, int offset_minutes, struct Arena* arena)
    __attribute__((nonnull(3)));

#endif
