#ifndef SURGE_TIMESTAMP_H
#define SURGE_TIMESTAMP_H

#include <stdbool.h>
#include <stdint.h>

#include "core/str.h"

struct Arena;

/** A calendar date in the proleptic Gregorian calendar. */
struct CivilDate {
  /** Year, such as `2026`. */
  int64_t year;

  /** Month, `1` to `12`. */
  int month;

  /** Day of the month, `1` to `31`. */
  int day;
};

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

/**
 * @brief Reports whether a git time's local date, in the offset that git recorded, is one RFC 3339
 *        can write: a date from the year 0000 to 9999. The offset counts as recorded, also when it
 *        is over 23:59.
 *
 * @param seconds        Seconds since the Unix epoch, in UTC, any value.
 * @param offset_minutes Offset of the local time from UTC, in minutes, as git records it, any
 *                       value.
 * @return `true` when both the time and the local time are in those years.
 */
bool timestamp_is_writable(int64_t seconds, int offset_minutes);

/**
 * @brief Returns the local day of a git time in the offset that git recorded, as a day number
 *        counted from 1970-01-01.
 *
 * @param seconds        Seconds since the Unix epoch, in UTC, for which `timestamp_is_writable`
 *                       holds.
 * @param offset_minutes Offset of the local time from UTC, in minutes, as git records it.
 * @return The day number, negative before 1970-01-01.
 */
int64_t timestamp_local_day(int64_t seconds, int offset_minutes);

/**
 * @brief Returns the calendar date of a day number, counted from 1970-01-01.
 *
 * This is Howard Hinnant's `civil_from_days`, which works in 400-year eras, so it is exact for
 * every day an `int64_t` time can name and needs no table.
 *
 * @param days Days since 1970-01-01, negative before it.
 * @return The date.
 */
struct CivilDate timestamp_civil_date(int64_t days);

#endif
