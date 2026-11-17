#include <acutest.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "build/contributions.h"
#include "core/arena.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "domain/timestamp.h"
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
 * @brief Returns the day number of a date.
 *
 * @param year  The year.
 * @param month The month, `1` to `12`.
 * @param day   The day of the month.
 * @return The day number, counted from 1970-01-01.
 */
static int64_t day_of(int64_t year, int month, int day) {
  return timestamp_civil_day((struct CivilDate){.year = year, .month = month, .day = day});
}

/**
 * @brief Tallies the window of the contributions of some repos.
 *
 * @param repos        The repos. Must not be `NULL` unless `repo_count` is `0`.
 * @param repo_count   Number of entries in `repos`.
 * @param calendar_out Receives the calendar. Must not be `NULL`.
 */
static void tally_repos(const struct RepoModel* repos,
                        ptrdiff_t repo_count,
                        struct ContributionCalendar* calendar_out) {
  struct Arena arena = test_arena();
  ptrdiff_t commit_count = 0;
  const struct ContributionCommit* commits =
      contributions_merge(repos, repo_count, &arena, &commit_count);
  contributions_tally(commits, commit_count, arena, calendar_out);
}

/**
 * @brief Tallies the window of the contributions of one repo.
 *
 * @param contributions The repo's contributions. Must not be `NULL` unless `count` is `0`.
 * @param count         Number of entries in `contributions`.
 * @param calendar_out  Receives the calendar. Must not be `NULL`.
 */
static void tally_one(struct Contribution* contributions,
                      ptrdiff_t count,
                      struct ContributionCalendar* calendar_out) {
  const struct RepoModel repo = {.contributions = contributions, .contribution_count = count};
  tally_repos(&repo, 1, calendar_out);
}

/**
 * @brief Tallies one year of the contributions of one repo.
 *
 * @param contributions The repo's contributions, at least one in `year`. Must not be `NULL`.
 * @param count         Number of entries in `contributions`.
 * @param year          The year.
 * @param calendar_out  Receives the calendar. Must not be `NULL`.
 */
static void tally_year_one(struct Contribution* contributions,
                           ptrdiff_t count,
                           int64_t year,
                           struct ContributionCalendar* calendar_out) {
  struct Arena arena = test_arena();
  const struct RepoModel repo = {.contributions = contributions, .contribution_count = count};
  ptrdiff_t commit_count = 0;
  const struct ContributionCommit* commits = contributions_merge(&repo, 1, &arena, &commit_count);
  contributions_tally_year(commits, commit_count, year, arena, calendar_out);
}

/**
 * @brief Counts the days of a calendar that are outside its range, and those after its end.
 *
 * @param calendar            The calendar. Must not be `NULL`.
 * @param after_end_count_out Receives the number of `is_after_end` days. Must not be `NULL`.
 * @return The number of `is_outside` days.
 */
static ptrdiff_t count_outside(const struct ContributionCalendar* calendar,
                               ptrdiff_t* after_end_count_out) {
  ptrdiff_t outside_count = 0;
  *after_end_count_out = 0;
  for (ptrdiff_t i = 0; i < calendar->week_count * 7; i++) {
    TEST_CHECK(!(calendar->days[i].is_outside && calendar->days[i].is_after_end));
    outside_count += calendar->days[i].is_outside;
    *after_end_count_out += calendar->days[i].is_after_end;
  }
  return outside_count;
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
  TEST_CHECK(calendar.week_count == CONTRIBUTIONS_WINDOW_WEEK_COUNT);
  TEST_CHECK(calendar.days[0].day == 20100);
  TEST_CHECK(calendar.days[CONTRIBUTIONS_WINDOW_WEEK_COUNT * 7 - 1].day == 20470);
  // The window ends on its newest day, so none of its days is after its end.
  ptrdiff_t after_end_count = 0;
  TEST_CHECK(count_outside(&calendar, &after_end_count) == 6);
  TEST_CHECK(after_end_count == 0);
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
  TEST_CHECK(calendar.days[CONTRIBUTIONS_WINDOW_WEEK_COUNT * 7 - 7].day == calendar.end_day);
  TEST_CHECK(calendar.days[CONTRIBUTIONS_WINDOW_WEEK_COUNT * 7 - 6].is_outside);
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
  tally_repos(repos, COUNT(repos), &calendar);
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

// A past year is drawn from the week of January 1 to the week of December 31, both Wednesdays in
// 2025, so its first and last weeks each have three days outside it, and it ends on December 31.
// Only its own commits count.
static void test_tally_year_draws_january_to_december(void) {
  struct Contribution contributions[] = {
      at_noon("aa", day_of(2025, 3, 10), false),
      at_noon("bb", day_of(2024, 12, 31), false),
      at_noon("cc", DAY_2026_01_15, false),
  };
  static struct ContributionCalendar calendar;
  tally_year_one(contributions, COUNT(contributions), 2025, &calendar);
  TEST_CHECK(calendar.start_day == day_of(2025, 1, 1));
  TEST_CHECK(calendar.end_day == day_of(2025, 12, 31));
  TEST_CHECK(calendar.week_count == 53);
  // Sunday 2024-12-29 starts the first week, and Saturday 2026-01-03 ends the last.
  TEST_CHECK(calendar.days[0].day == day_of(2024, 12, 29));
  TEST_CHECK(calendar.days[53 * 7 - 1].day == day_of(2026, 1, 3));
  TEST_CHECK(calendar.days[2].is_outside && !calendar.days[3].is_outside);
  TEST_CHECK(!find_day(&calendar, day_of(2025, 12, 31))->is_outside);
  TEST_CHECK(find_day(&calendar, day_of(2026, 1, 1))->is_outside);
  ptrdiff_t after_end_count = -1;
  TEST_CHECK(count_outside(&calendar, &after_end_count) == 6);
  TEST_CHECK(after_end_count == 0);
  TEST_CHECK(calendar.count == 1 && calendar.day_count == 1);
  TEST_CHECK(find_day(&calendar, day_of(2025, 3, 10))->level == 1);
  TEST_CHECK(find_day(&calendar, day_of(2024, 12, 31))->count == 0);
}

// The newest year ends on the newest commit's day, and is still drawn to December 31: its later
// days are after its end, with no count and level 0, and the two days of its last week after
// December 31, a Thursday in 2026, are outside it and not after its end.
static void test_tally_year_marks_the_days_after_the_newest_commit(void) {
  struct Contribution contributions[] = {
      at_noon("aa", DAY_2026_01_15, true),
      at_noon("bb", day_of(2026, 1, 2), false),
      at_noon("cc", day_of(2025, 6, 1), false),
  };
  static struct ContributionCalendar calendar;
  tally_year_one(contributions, COUNT(contributions), 2026, &calendar);
  TEST_CHECK(calendar.start_day == day_of(2026, 1, 1));
  TEST_CHECK(calendar.end_day == DAY_2026_01_15);
  TEST_CHECK(calendar.week_count == 53);
  ptrdiff_t after_end_count = -1;
  TEST_CHECK(count_outside(&calendar, &after_end_count) == 4 + 2);
  TEST_CHECK(after_end_count == day_of(2026, 12, 31) - DAY_2026_01_15);
  const struct ContributionDay* after = find_day(&calendar, DAY_2026_01_15 + 1);
  TEST_CHECK(after->is_after_end && !after->is_outside && after->count == 0 && after->level == 0);
  TEST_CHECK(find_day(&calendar, day_of(2026, 12, 31))->is_after_end);
  const struct ContributionDay* past = find_day(&calendar, day_of(2027, 1, 1));
  TEST_CHECK(past->is_outside && !past->is_after_end);
  TEST_CHECK(!find_day(&calendar, DAY_2026_01_15)->is_after_end);
  TEST_CHECK(calendar.count == 2 && calendar.llm_count == 1);
  TEST_CHECK(calendar.day_count == 2 && calendar.llm_day_count == 1);
}

// A leap year has 366 days inside it: 2024 starts on a Monday and takes 53 weeks, and 2028 starts
// on a Saturday and takes 54, its last week holding only Sunday December 31.
static void test_tally_year_of_a_leap_year(void) {
  static struct ContributionCalendar calendar;
  struct Contribution leap[] = {at_noon("aa", day_of(2024, 2, 29), false)};
  tally_year_one(leap, COUNT(leap), 2024, &calendar);
  ptrdiff_t after_end_count = -1;
  TEST_CHECK(calendar.week_count == 53);
  TEST_CHECK(calendar.week_count * 7 - count_outside(&calendar, &after_end_count) == 366);
  TEST_CHECK(find_day(&calendar, day_of(2024, 2, 29))->count == 1);
  struct Contribution long_leap[] = {at_noon("aa", day_of(2028, 12, 31), false)};
  tally_year_one(long_leap, COUNT(long_leap), 2028, &calendar);
  TEST_CHECK(calendar.week_count == CONTRIBUTIONS_WEEK_COUNT_MAX);
  TEST_CHECK(calendar.week_count * 7 - count_outside(&calendar, &after_end_count) == 366);
  TEST_CHECK(calendar.days[6].day == day_of(2028, 1, 1) && !calendar.days[6].is_outside);
  TEST_CHECK(calendar.days[53 * 7].day == day_of(2028, 12, 31));
  TEST_CHECK(calendar.days[53 * 7 + 1].is_outside);
  TEST_CHECK(test_text_is(contributions_week_month(&calendar, 0), "Jan"));
}

// A commit's year is that of its local day, so an offset can move it into the next year or the one
// before, and each year counts it once.
static void test_tally_year_counts_the_day_in_the_recorded_offset(void) {
  // 2026-01-01T07:30:00Z, which is 2025-12-31 at -08:00.
  struct Contribution west = {.oid = S("aa"), .time = 1767252600, .offset_minutes = -480};
  // 2025-12-31T23:30:00Z, earlier, but 2026-01-01 at +01:00.
  struct Contribution east = {.oid = S("bb"), .time = 1767223800, .offset_minutes = 60};
  struct Contribution contributions[] = {west, east};
  static struct ContributionCalendar calendar;
  tally_year_one(contributions, COUNT(contributions), 2025, &calendar);
  TEST_CHECK(calendar.count == 1 && find_day(&calendar, day_of(2025, 12, 31))->count == 1);
  TEST_CHECK(calendar.end_day == day_of(2025, 12, 31));
  tally_year_one(contributions, COUNT(contributions), 2026, &calendar);
  TEST_CHECK(calendar.count == 1 && find_day(&calendar, day_of(2026, 1, 1))->count == 1);
  TEST_CHECK(calendar.end_day == day_of(2026, 1, 1));
}

// A year's levels come from its own non-empty days: a day of 2 commits is level 2 in a window that
// also holds days of 1 and 8, but level 1 in its year, which holds no other, and the next year's
// quartiles of 1, 1 and 8 put its day of 8 at level 3.
static void test_tally_year_levels_from_its_own_days(void) {
  static struct Contribution contributions[11];
  static char oids[11][3];
  const struct {
    int64_t day;
    ptrdiff_t count;
  } days[] = {
      {day_of(2025, 12, 30), 2},
      {day_of(2026, 1, 2), 8},
      {day_of(2026, 1, 3), 1},
  };
  ptrdiff_t count = 0;
  for (ptrdiff_t d = 0; d < COUNT(days); d++) {
    for (ptrdiff_t i = 0; i < days[d].count; i++) {
      oids[count][0] = (char)('a' + d);
      oids[count][1] = (char)('a' + i);
      oids[count][2] = '\0';
      contributions[count] = at_noon(oids[count], days[d].day, false);
      count++;
    }
  }
  TEST_ASSERT(count == COUNT(contributions));
  static struct ContributionCalendar calendar;
  tally_one(contributions, count, &calendar);
  TEST_CHECK(find_day(&calendar, day_of(2025, 12, 30))->level == 2);
  tally_year_one(contributions, count, 2025, &calendar);
  TEST_CHECK(find_day(&calendar, day_of(2025, 12, 30))->level == 1);
  tally_year_one(contributions, count, 2026, &calendar);
  TEST_CHECK(find_day(&calendar, day_of(2026, 1, 2))->level == 3);
  TEST_CHECK(find_day(&calendar, day_of(2026, 1, 3))->level == 1);
}

// A week names the month whose 1st it holds inside the range, after-end days included, so the
// newest year names every month to December, and a 1st outside the range names none.
static void test_week_month_names_the_months_of_the_range(void) {
  struct Contribution contributions[] = {at_noon("aa", DAY_2026_01_15, false)};
  static struct ContributionCalendar calendar;
  tally_year_one(contributions, COUNT(contributions), 2026, &calendar);
  ptrdiff_t named_count = 0;
  for (ptrdiff_t w = 0; w < calendar.week_count; w++) {
    named_count += contributions_week_month(&calendar, w).len > 0;
  }
  TEST_CHECK(named_count == 12);
  TEST_CHECK(test_text_is(contributions_week_month(&calendar, 0), "Jan"));
  // Tuesday 2026-12-01 is in the week of Sunday 2026-11-29, the 49th.
  TEST_CHECK(test_text_is(contributions_week_month(&calendar, 48), "Dec"));
  // The window's first week holds 2025-01-12, after the 1st, so it names no month.
  tally_one(contributions, COUNT(contributions), &calendar);
  TEST_CHECK(contributions_week_month(&calendar, 0).len == 0);
  TEST_CHECK(test_text_is(contributions_week_month(&calendar, 2), "Feb"));
}

// The years with commits are listed newest first, each with its commits and LLM-assisted ones, and
// a commit that two repos reach counts once.
static void test_years_lists_the_years_newest_first(void) {
  struct Contribution first[] = {
      at_noon("aa", day_of(2025, 3, 1), true),
      at_noon("bb", day_of(2024, 7, 1), false),
      at_noon("cc", DAY_2026_01_15, false),
  };
  struct Contribution second[] = {
      at_noon("dd", day_of(2025, 12, 31), false),
      at_noon("aa", day_of(2025, 3, 1), true),
  };
  const struct RepoModel repos[] = {
      {.contributions = first, .contribution_count = COUNT(first)},
      {.contributions = second, .contribution_count = COUNT(second)},
  };
  struct Arena arena = test_arena();
  ptrdiff_t commit_count = 0;
  const struct ContributionCommit* commits =
      contributions_merge(repos, COUNT(repos), &arena, &commit_count);
  ptrdiff_t year_count = 0;
  const struct ContributionYear* years =
      contributions_years(commits, commit_count, &arena, &year_count);
  TEST_ASSERT(year_count == 3);
  TEST_CHECK(years[0].year == 2026 && years[0].count == 1 && years[0].llm_count == 0);
  TEST_CHECK(years[1].year == 2025 && years[1].count == 2 && years[1].llm_count == 1);
  TEST_CHECK(years[2].year == 2024 && years[2].count == 1 && years[2].llm_count == 0);
}

// With no commit at all, nothing counts.
static void test_tally_of_no_commits_is_empty(void) {
  static struct ContributionCalendar calendar;
  contributions_tally(NULL, 0, test_arena(), &calendar);
  TEST_CHECK(calendar.count == 0 && calendar.day_count == 0);
  const struct RepoModel empty = {0};
  tally_repos(&empty, 1, &calendar);
  TEST_CHECK(calendar.count == 0);
  struct Arena arena = test_arena();
  ptrdiff_t year_count = -1;
  (void)contributions_years(NULL, 0, &arena, &year_count);
  TEST_CHECK(year_count == 0);
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
    {"tally year draws january to december", test_tally_year_draws_january_to_december},
    {"tally year marks the days after the newest commit",
     test_tally_year_marks_the_days_after_the_newest_commit},
    {"tally year of a leap year", test_tally_year_of_a_leap_year},
    {"tally year counts the day in the recorded offset",
     test_tally_year_counts_the_day_in_the_recorded_offset},
    {"tally year levels from its own days", test_tally_year_levels_from_its_own_days},
    {"week month names the months of the range", test_week_month_names_the_months_of_the_range},
    {"years lists the years newest first", test_years_lists_the_years_newest_first},
    {"tally of no commits is empty", test_tally_of_no_commits_is_empty},
    {NULL, NULL},
};
