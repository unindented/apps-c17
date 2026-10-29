#include "domain/timestamp.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/str.h"

/** Seconds in a day. */
enum { TIMESTAMP_DAY_SECONDS = 86400 };

/** Largest offset from UTC that RFC 3339 writes, 23:59 either way, in minutes. */
enum { TIMESTAMP_OFFSET_MINUTES_MAX = 23 * 60 + 59 };

/** Earliest local time RFC 3339 writes, `0000-01-01T00:00:00`, in seconds since the Unix epoch. */
#define TIMESTAMP_SECONDS_MIN INT64_C(-62167219200)

/** Latest local time RFC 3339 writes, `9999-12-31T23:59:59`, in seconds since the Unix epoch. */
#define TIMESTAMP_SECONDS_MAX INT64_C(253402300799)

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
 * @brief Returns the calendar date of a day number, counted from 1970-01-01.
 *
 * This is Howard Hinnant's `civil_from_days`, which works in 400-year eras, so it is exact for
 * every day an `int64_t` time can name and needs no table.
 *
 * @param days Days since 1970-01-01, negative before it.
 * @return The date.
 */
static struct CivilDate civil_from_days(int64_t days);

struct Str timestamp_format_rfc3339(int64_t seconds, int offset_minutes, struct Arena* arena) {
  // libgit2 parses a time and an offset with no range check, so either can be out of range.
  if (offset_minutes < -TIMESTAMP_OFFSET_MINUTES_MAX ||
      offset_minutes > TIMESTAMP_OFFSET_MINUTES_MAX) {
    offset_minutes = 0;
  }
  // The time is bounded before the offset is added, so the sum cannot overflow. A time out of range
  // is the epoch in UTC.
  const int64_t offset_seconds = (int64_t)offset_minutes * 60;
  const bool is_in_range = seconds >= TIMESTAMP_SECONDS_MIN && seconds <= TIMESTAMP_SECONDS_MAX &&
                           seconds + offset_seconds >= TIMESTAMP_SECONDS_MIN &&
                           seconds + offset_seconds <= TIMESTAMP_SECONDS_MAX;
  if (!is_in_range) {
    offset_minutes = 0;
  }
  const int64_t local = is_in_range ? seconds + offset_seconds : 0;
  // Floor division, so a time before the epoch falls on the day before, not the day after.
  int64_t days = local / TIMESTAMP_DAY_SECONDS;
  int64_t second_of_day = local % TIMESTAMP_DAY_SECONDS;
  if (second_of_day < 0) {
    second_of_day += TIMESTAMP_DAY_SECONDS;
    days--;
  }
  const struct CivilDate date = civil_from_days(days);
  struct Buf text = {.arena = arena};
  buf_printf(&text, "%04" PRId64 "-%02d-%02dT%02d:%02d:%02d", date.year, date.month, date.day,
             (int)(second_of_day / 3600), (int)(second_of_day / 60 % 60),
             (int)(second_of_day % 60));
  // A zero offset is spelled `Z`, as every date the apps write spells it.
  if (offset_minutes == 0) {
    buf_append_char(&text, 'Z');
    return buf_str(&text);
  }
  const int offset_abs = offset_minutes < 0 ? -offset_minutes : offset_minutes;
  buf_printf(&text, "%c%02d:%02d", offset_minutes < 0 ? '-' : '+', offset_abs / 60,
             offset_abs % 60);
  return buf_str(&text);
}

static struct CivilDate civil_from_days(int64_t days) {
  const int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t day_of_era = z - era * 146097;
  const int64_t year_of_era =
      (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
  const int64_t day_of_year =
      day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
  const int64_t month_index = (5 * day_of_year + 2) / 153;
  const int day = (int)(day_of_year - (153 * month_index + 2) / 5 + 1);
  const int month = (int)(month_index < 10 ? month_index + 3 : month_index - 9);
  const int64_t year = year_of_era + era * 400 + (month <= 2 ? 1 : 0);
  return (struct CivilDate){.year = year, .month = month, .day = day};
}
