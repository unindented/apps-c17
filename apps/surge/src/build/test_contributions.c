#include <acutest.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "build/contributions.h"
#include "core/arena.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "testing/test_support.h"

/** Number of entries in a fixed array. */
#define COUNT(array) ((ptrdiff_t)(sizeof(array) / sizeof((array)[0])))

/** Seconds in a day. */
enum { DAY_SECONDS = 86400 };

/** Day number of 2026-01-15, a Thursday, the newest day of most tests. */
enum { DAY_2026_01_15 = 20468 };

/**
 * @brief Returns a contribution at noon UTC of a day, in UTC.
 *
 * @param oid             Terminated hex id. Must not be `NULL`.
 * @param day             Day number, counted from 1970-01-01.
 * @param is_llm_assisted Whether it is LLM-assisted.
 * @return The contribution.
 */
static struct Contribution at_noon(const char* oid, int64_t day, bool is_llm_assisted) {
  return (struct Contribution){
      .oid = str_from_cstr(oid),
      .time = day * DAY_SECONDS + DAY_SECONDS / 2,
      .is_llm_assisted = is_llm_assisted,
  };
}

/**
 * @brief Tallies the contributions of one repo.
 *
 * @param contributions The repo's contributions. Must not be `NULL` unless `count` is `0`.
 * @param count         Number of entries in `contributions`.
 * @param calendar_out  Receives the calendar. Must not be `NULL`.
 */
static void tally_one(struct Contribution* contributions,
                      ptrdiff_t count,
                      struct ContributionCalendar* calendar_out) {
  const struct RepoModel repo = {.contributions = contributions, .contribution_count = count};
  contributions_tally(&repo, 1, test_arena(), calendar_out);
}

/**
 * @brief Finds the calendar's day with the day number `day`.
 *
 * @param calendar The calendar. Must not be `NULL`.
 * @param day      Day number, inside the calendar's weeks.
 * @return The day.
 */
static const struct ContributionDay* find_day(const struct ContributionCalendar* calendar,
                                              int64_t day) {
  return &calendar->days[day - calendar->days[0].day];
}

// The window is the 365 days that end on the newest commit's day, here across a year boundary, in
// 53 weeks from Sunday, so the first week has the days before the window outside and the last the
// days after it.
static void test_tally_ends_the_window_at_the_newest_day(void) {
  struct Contribution contributions[] = {
      at_noon("bb", DAY_2026_01_15, false),
      at_noon("aa", DAY_2026_01_15 - 20, false),
  };
  static struct ContributionCalendar calendar;
  tally_one(contributions, COUNT(contributions), &calendar);
  TEST_CHECK(calendar.end_day == DAY_2026_01_15);
  TEST_CHECK(calendar.start_day == DAY_2026_01_15 - 364);
  // Sunday 2025-01-12 starts the first week, and Saturday 2026-01-17 ends the last.
  TEST_CHECK(calendar.days[0].day == 20100);
  TEST_CHECK(calendar.days[CONTRIBUTIONS_DAY_COUNT - 1].day == 20470);
  ptrdiff_t outside_count = 0;
  for (ptrdiff_t i = 0; i < CONTRIBUTIONS_DAY_COUNT; i++) {
    outside_count += calendar.days[i].is_outside;
  }
  TEST_CHECK(outside_count == 6);
  TEST_CHECK(calendar.days[3].is_outside && !calendar.days[4].is_outside);
  TEST_CHECK(!find_day(&calendar, DAY_2026_01_15)->is_outside);
  TEST_CHECK(find_day(&calendar, DAY_2026_01_15 + 1)->is_outside);
  TEST_CHECK(calendar.count == 2 && calendar.day_count == 2);
}

// A newest day that is a Sunday fills the first week and leaves six days outside the last.
static void test_tally_ending_on_a_sunday_fills_the_first_week(void) {
  struct Contribution contributions[] = {at_noon("aa", DAY_2026_01_15 + 3, false)};
  static struct ContributionCalendar calendar;
  tally_one(contributions, COUNT(contributions), &calendar);
  TEST_CHECK(calendar.days[0].day == calendar.start_day && !calendar.days[0].is_outside);
  TEST_CHECK(calendar.days[CONTRIBUTIONS_DAY_COUNT - 7].day == calendar.end_day);
  TEST_CHECK(calendar.days[CONTRIBUTIONS_DAY_COUNT - 6].is_outside);
}

// A commit before the window counts in no day and in no total, though its week shows.
static void test_tally_leaves_out_a_commit_before_the_window(void) {
  struct Contribution contributions[] = {
      at_noon("aa", DAY_2026_01_15, false),
      at_noon("bb", DAY_2026_01_15 - 365, true),
      at_noon("cc", DAY_2026_01_15 - 366, true),
  };
  static struct ContributionCalendar calendar;
  tally_one(contributions, COUNT(contributions), &calendar);
  TEST_CHECK(calendar.count == 1 && calendar.llm_count == 0);
  TEST_CHECK(calendar.day_count == 1 && calendar.llm_day_count == 0);
  const struct ContributionDay* before = find_day(&calendar, DAY_2026_01_15 - 365);
  TEST_CHECK(before->is_outside && before->count == 0 && before->level == 0);
}

// A commit's day is its author time in its recorded offset, so an offset can move it across
// midnight, and the newest day is the latest local day, not the latest time.
static void test_tally_counts_the_day_in_the_recorded_offset(void) {
  // 2026-01-14T07:30:00Z, which is 2026-01-13 at -08:00.
  struct Contribution west = {.oid = S("aa"), .time = 1768375800, .offset_minutes = -480};
  // 2026-01-14T01:00:00Z, earlier, but 2026-01-14 at +09:00.
  struct Contribution east = {.oid = S("bb"), .time = 1768352400, .offset_minutes = 540};
  struct Contribution contributions[] = {west, east};
  static struct ContributionCalendar calendar;
  tally_one(contributions, COUNT(contributions), &calendar);
  TEST_CHECK(calendar.end_day == DAY_2026_01_15 - 1);
  TEST_CHECK(find_day(&calendar, DAY_2026_01_15 - 2)->count == 1);
  TEST_CHECK(find_day(&calendar, DAY_2026_01_15 - 1)->count == 1);
}

// A day with an LLM-assisted commit and another counts both, and one LLM day.
static void test_tally_counts_llm_and_other_commits_of_a_day(void) {
  struct Contribution contributions[] = {
      at_noon("aa", DAY_2026_01_15, true),
      at_noon("bb", DAY_2026_01_15, false),
      at_noon("cc", DAY_2026_01_15 - 1, false),
  };
  static struct ContributionCalendar calendar;
  tally_one(contributions, COUNT(contributions), &calendar);
  const struct ContributionDay* day = find_day(&calendar, DAY_2026_01_15);
  TEST_CHECK(day->count == 2 && day->llm_count == 1);
  TEST_CHECK(calendar.count == 3 && calendar.llm_count == 1);
  TEST_CHECK(calendar.day_count == 2 && calendar.llm_day_count == 1);
}

// With one non-empty day, or with every non-empty day equal, each is level 1, and an empty day is
// level 0.
static void test_tally_levels_of_one_day_and_of_equal_days(void) {
  static struct ContributionCalendar calendar;
  struct Contribution one[] = {at_noon("aa", DAY_2026_01_15, false)};
  tally_one(one, COUNT(one), &calendar);
  TEST_CHECK(find_day(&calendar, DAY_2026_01_15)->level == 1);
  TEST_CHECK(find_day(&calendar, DAY_2026_01_15 - 1)->level == 0);
  struct Contribution equal[] = {
      at_noon("aa", DAY_2026_01_15, false),     at_noon("bb", DAY_2026_01_15, false),
      at_noon("cc", DAY_2026_01_15 - 3, false), at_noon("dd", DAY_2026_01_15 - 3, false),
      at_noon("ee", DAY_2026_01_15 - 9, false), at_noon("ff", DAY_2026_01_15 - 9, false),
  };
  tally_one(equal, COUNT(equal), &calendar);
  TEST_CHECK(find_day(&calendar, DAY_2026_01_15)->level == 1);
  TEST_CHECK(find_day(&calendar, DAY_2026_01_15 - 3)->level == 1);
  TEST_CHECK(find_day(&calendar, DAY_2026_01_15 - 9)->level == 1);
}

// With two non-empty days of 1 and 3 commits, the nearest-rank quartiles are 1, 1 and 3, so the
// days are levels 1 and 3.
static void test_tally_levels_of_two_days(void) {
  struct Contribution contributions[] = {
      at_noon("aa", DAY_2026_01_15, false),
      at_noon("bb", DAY_2026_01_15, false),
      at_noon("cc", DAY_2026_01_15, false),
      at_noon("dd", DAY_2026_01_15 - 1, false),
  };
  static struct ContributionCalendar calendar;
  tally_one(contributions, COUNT(contributions), &calendar);
  TEST_CHECK(find_day(&calendar, DAY_2026_01_15 - 1)->level == 1);
  TEST_CHECK(find_day(&calendar, DAY_2026_01_15)->level == 3);
}

// With eight non-empty days of 1 to 8 commits, the quartiles are 2, 4 and 6, so each level holds
// two days.
static void test_tally_levels_of_many_days(void) {
  static struct Contribution contributions[36];
  static char oids[36][3];
  ptrdiff_t count = 0;
  for (ptrdiff_t day = 0; day < 8; day++) {
    for (ptrdiff_t i = 0; i <= day; i++) {
      oids[count][0] = (char)('a' + day);
      oids[count][1] = (char)('a' + i);
      oids[count][2] = '\0';
      contributions[count] = at_noon(oids[count], DAY_2026_01_15 - day, false);
      count++;
    }
  }
  TEST_ASSERT(count == COUNT(contributions));
  static struct ContributionCalendar calendar;
  tally_one(contributions, count, &calendar);
  const int levels[] = {1, 1, 2, 2, 3, 3, 4, 4};
  for (ptrdiff_t day = 0; day < 8; day++) {
    const struct ContributionDay* found = find_day(&calendar, DAY_2026_01_15 - day);
    TEST_CHECK(found->count == day + 1);
    TEST_CHECK(found->level == levels[day]);
    TEST_MSG("day with %td commits: level %d", day + 1, found->level);
  }
}

// A commit that two repos reach counts once, by its id.
static void test_tally_counts_a_shared_commit_once(void) {
  struct Contribution first[] = {
      at_noon("aa", DAY_2026_01_15, true),
      at_noon("bb", DAY_2026_01_15 - 1, false),
  };
  struct Contribution second[] = {
      at_noon("cc", DAY_2026_01_15, false),
      at_noon("aa", DAY_2026_01_15, true),
  };
  const struct RepoModel repos[] = {
      {.contributions = first, .contribution_count = COUNT(first)},
      {.contributions = second, .contribution_count = COUNT(second)},
  };
  static struct ContributionCalendar calendar;
  contributions_tally(repos, COUNT(repos), test_arena(), &calendar);
  TEST_CHECK(calendar.count == 3 && calendar.llm_count == 1);
  TEST_CHECK(find_day(&calendar, DAY_2026_01_15)->count == 2);
}

// A commit whose date RFC 3339 cannot write counts nowhere, so it cannot move the window.
static void test_tally_leaves_out_an_unwritable_date(void) {
  struct Contribution contributions[] = {
      at_noon("aa", DAY_2026_01_15, false),
      {.oid = S("bb"), .time = INT64_C(253402300800)},
  };
  static struct ContributionCalendar calendar;
  tally_one(contributions, COUNT(contributions), &calendar);
  TEST_CHECK(calendar.end_day == DAY_2026_01_15 && calendar.count == 1);
}

// With no commit at all, nothing counts.
static void test_tally_of_no_commits_is_empty(void) {
  static struct ContributionCalendar calendar;
  contributions_tally(NULL, 0, test_arena(), &calendar);
  TEST_CHECK(calendar.count == 0 && calendar.day_count == 0);
  const struct RepoModel empty = {0};
  contributions_tally(&empty, 1, test_arena(), &calendar);
  TEST_CHECK(calendar.count == 0);
}

TEST_LIST = {
    {"tally ends the window at the newest day", test_tally_ends_the_window_at_the_newest_day},
    {"tally ending on a sunday fills the first week",
     test_tally_ending_on_a_sunday_fills_the_first_week},
    {"tally leaves out a commit before the window",
     test_tally_leaves_out_a_commit_before_the_window},
    {"tally counts the day in the recorded offset",
     test_tally_counts_the_day_in_the_recorded_offset},
    {"tally counts llm and other commits of a day",
     test_tally_counts_llm_and_other_commits_of_a_day},
    {"tally levels of one day and of equal days", test_tally_levels_of_one_day_and_of_equal_days},
    {"tally levels of two days", test_tally_levels_of_two_days},
    {"tally levels of many days", test_tally_levels_of_many_days},
    {"tally counts a shared commit once", test_tally_counts_a_shared_commit_once},
    {"tally leaves out an unwritable date", test_tally_leaves_out_an_unwritable_date},
    {"tally of no commits is empty", test_tally_of_no_commits_is_empty},
    {NULL, NULL},
};
