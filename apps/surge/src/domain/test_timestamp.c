#include <acutest.h>
#include <limits.h>
#include <stdint.h>

#include "core/arena.h"
#include "core/str.h"
#include "domain/timestamp.h"
#include "testing/test_support.h"

// A time is written in its own offset, with the offset after it.
static void test_format_writes_local_time_and_offset(void) {
  struct Arena arena = test_arena();
  TEST_CHECK(
      str_eq(timestamp_format_rfc3339(1767956400, 60, &arena), S("2026-01-09T12:00:00+01:00")));
  TEST_CHECK(
      str_eq(timestamp_format_rfc3339(1767956400, -330, &arena), S("2026-01-09T05:30:00-05:30")));
}

// UTC is written `Z`, as every date the apps write spells it.
static void test_format_writes_utc_offset(void) {
  struct Arena arena = test_arena();
  TEST_CHECK(str_eq(timestamp_format_rfc3339(0, 0, &arena), S("1970-01-01T00:00:00Z")));
}

// A leap day and the end of a leap year land on their own dates.
static void test_format_handles_leap_years(void) {
  struct Arena arena = test_arena();
  TEST_CHECK(str_eq(timestamp_format_rfc3339(951782400, 0, &arena), S("2000-02-29T00:00:00Z")));
  TEST_CHECK(str_eq(timestamp_format_rfc3339(1735689599, 0, &arena), S("2024-12-31T23:59:59Z")));
}

// A time before the epoch, and an offset that moves the date back across it, fall on the day
// before.
static void test_format_handles_times_before_the_epoch(void) {
  struct Arena arena = test_arena();
  TEST_CHECK(str_eq(timestamp_format_rfc3339(-1, 0, &arena), S("1969-12-31T23:59:59Z")));
  TEST_CHECK(str_eq(timestamp_format_rfc3339(0, -60, &arena), S("1969-12-31T23:00:00-01:00")));
}

// The first and the last second of the years RFC 3339 writes are written as they are, and an offset
// of 23:59 either way is kept. The bounds are file-local to `timestamp.c`, so they are spelled out
// here, and changing them there must update this test.
static void test_format_keeps_the_bounds(void) {
  struct Arena arena = test_arena();
  TEST_CHECK(str_eq(timestamp_format_rfc3339(INT64_C(-62167219200), 0, &arena),
                    S("0000-01-01T00:00:00Z")));
  TEST_CHECK(str_eq(timestamp_format_rfc3339(INT64_C(253402300799), 0, &arena),
                    S("9999-12-31T23:59:59Z")));
  TEST_CHECK(str_eq(timestamp_format_rfc3339(0, 1439, &arena), S("1970-01-01T23:59:00+23:59")));
  TEST_CHECK(str_eq(timestamp_format_rfc3339(0, -1439, &arena), S("1969-12-31T00:01:00-23:59")));
}

// A time outside the years 0000 to 9999, in UTC or only in its offset, is written as the epoch,
// down to the extremes of `int64_t`.
static void test_format_writes_out_of_range_times_as_the_epoch(void) {
  struct Arena arena = test_arena();
  TEST_CHECK(str_eq(timestamp_format_rfc3339(INT64_MAX, 60, &arena), S("1970-01-01T00:00:00Z")));
  TEST_CHECK(str_eq(timestamp_format_rfc3339(INT64_MIN, -60, &arena), S("1970-01-01T00:00:00Z")));
  TEST_CHECK(str_eq(timestamp_format_rfc3339(INT64_C(253402300800), 0, &arena),
                    S("1970-01-01T00:00:00Z")));
  TEST_CHECK(str_eq(timestamp_format_rfc3339(INT64_C(253402300799), 1, &arena),
                    S("1970-01-01T00:00:00Z")));
  TEST_CHECK(str_eq(timestamp_format_rfc3339(INT64_C(-62167219200), -1, &arena),
                    S("1970-01-01T00:00:00Z")));
}

// An offset over 23:59 either way, such as the -6039 minutes libgit2 parses from `+-9999`, is
// written as `Z`.
static void test_format_writes_out_of_range_offsets_as_utc(void) {
  struct Arena arena = test_arena();
  TEST_CHECK(
      str_eq(timestamp_format_rfc3339(1767956400, -6039, &arena), S("2026-01-09T11:00:00Z")));
  TEST_CHECK(str_eq(timestamp_format_rfc3339(1767956400, 1440, &arena), S("2026-01-09T11:00:00Z")));
  TEST_CHECK(
      str_eq(timestamp_format_rfc3339(1767956400, INT_MIN, &arena), S("2026-01-09T11:00:00Z")));
}

TEST_LIST = {
    {"format writes local time and offset", test_format_writes_local_time_and_offset},
    {"format writes utc offset", test_format_writes_utc_offset},
    {"format handles leap years", test_format_handles_leap_years},
    {"format handles times before the epoch", test_format_handles_times_before_the_epoch},
    {"format keeps the bounds", test_format_keeps_the_bounds},
    {"format writes out of range times as the epoch",
     test_format_writes_out_of_range_times_as_the_epoch},
    {"format writes out of range offsets as utc", test_format_writes_out_of_range_offsets_as_utc},
    {NULL, NULL},
};
