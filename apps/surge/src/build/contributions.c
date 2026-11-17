#include "build/contributions.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "core/arena.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "domain/timestamp.h"

/** Short English name of each month, indexed by its number from 1. */
static const struct Str CONTRIBUTIONS_MONTH_NAMES[] = {
    STR_INIT(""),    STR_INIT("Jan"), STR_INIT("Feb"), STR_INIT("Mar"), STR_INIT("Apr"),
    STR_INIT("May"), STR_INIT("Jun"), STR_INIT("Jul"), STR_INIT("Aug"), STR_INIT("Sep"),
    STR_INIT("Oct"), STR_INIT("Nov"), STR_INIT("Dec"),
};

/**
 * @brief Returns the newest day of a non-empty list of commits.
 *
 * @param commits      The commits. Must not be `NULL`.
 * @param commit_count Number of entries in `commits`, at least `1`.
 * @return The day number.
 */
static int64_t newest_day(const struct ContributionCommit* commits, ptrdiff_t commit_count)
    __attribute__((nonnull(1)));

/**
 * @brief Fills the weeks of a calendar whose `start_day` and `end_day` are set: the weeks from the
 *        one of `start_day` to the one of `last_day`, the counts of the commits from `start_day` to
 *        `end_day`, the totals and the levels.
 *
 * @param commits      The commits. Must not be `NULL` unless `commit_count` is `0`.
 * @param commit_count Number of entries in `commits`.
 * @param last_day     Last day of the range, `end_day` or later, at most 54 weeks after the first.
 * @param scratch      Arena for the sorted counts.
 * @param calendar     Calendar to fill. Must not be `NULL`.
 */
static void fill_weeks(const struct ContributionCommit* commits,
                       ptrdiff_t commit_count,
                       int64_t last_day,
                       struct Arena scratch,
                       struct ContributionCalendar* calendar) __attribute__((nonnull(5)));

/**
 * @brief Sets the `level` of each day of a calendar with commits, from the quartiles of the
 *        calendar's non-empty days.
 *
 * The quartiles are nearest-rank: the `k`th, for `k` from 1 to 3, is the count at index
 * `ceil(k * n / 4) - 1` of the `n` non-empty counts in ascending order. So with one non-empty day,
 * or with every non-empty day equal, each is level 1.
 *
 * @param calendar Calendar whose days are counted. Must not be `NULL`.
 * @param scratch  Arena for the sorted counts.
 */
static void fill_weeks_levels(struct ContributionCalendar* calendar, struct Arena scratch)
    __attribute__((nonnull(1)));

/**
 * @brief Returns the weekday of a day number, `0` for Sunday to `6`.
 *
 * @param day Day number, counted from 1970-01-01.
 * @return The weekday.
 */
static int64_t weekday(int64_t day);

/**
 * @brief Orders two counted commits by `oid` in byte order, for `qsort`.
 *
 * @param a First `struct ContributionCommit`. Must not be `NULL`.
 * @param b Second `struct ContributionCommit`. Must not be `NULL`.
 * @return Negative, zero or positive as `a` sorts before, with, or after `b`.
 */
static int compare_oids(const void* a, const void* b) __attribute__((nonnull(1, 2)));

/**
 * @brief Orders two years newest first, for `qsort`.
 *
 * @param a First `struct ContributionYear`. Must not be `NULL`.
 * @param b Second `struct ContributionYear`. Must not be `NULL`.
 * @return Negative, zero or positive as `a` is newer than, the same as, or older than `b`.
 */
static int compare_years(const void* a, const void* b) __attribute__((nonnull(1, 2)));

/**
 * @brief Orders two counts in ascending order, for `qsort`.
 *
 * @param a First `ptrdiff_t`. Must not be `NULL`.
 * @param b Second `ptrdiff_t`. Must not be `NULL`.
 * @return Negative, zero or positive as `a` is less than, equal to, or greater than `b`.
 */
static int compare_counts(const void* a, const void* b) __attribute__((nonnull(1, 2)));

struct ContributionCommit* contributions_merge(const struct RepoModel* repos,
                                               ptrdiff_t repo_count,
                                               struct Arena* arena,
                                               ptrdiff_t* count_out) {
  ptrdiff_t total = 0;
  for (ptrdiff_t i = 0; i < repo_count; i++) {
    total += repos[i].contribution_count;
  }
  struct ContributionCommit* commits = arena_new(arena, total, struct ContributionCommit);
  ptrdiff_t count = 0;
  for (ptrdiff_t i = 0; i < repo_count; i++) {
    for (ptrdiff_t j = 0; j < repos[i].contribution_count; j++) {
      const struct Contribution* contribution = &repos[i].contributions[j];
      if (timestamp_is_writable(contribution->time, contribution->offset_minutes)) {
        commits[count++] = (struct ContributionCommit){
            .oid = contribution->oid,
            .day = timestamp_local_day(contribution->time, contribution->offset_minutes),
            .is_llm_assisted = contribution->is_llm_assisted,
        };
      }
    }
  }
  if (count > 1) {
    qsort(commits, (size_t)count, sizeof(commits[0]), compare_oids);
  }
  // One id is one commit, whose day and flag are the same in every repo that reaches it.
  ptrdiff_t unique_count = 0;
  for (ptrdiff_t i = 0; i < count; i++) {
    if (unique_count == 0 || !str_eq(commits[unique_count - 1].oid, commits[i].oid)) {
      commits[unique_count++] = commits[i];
    }
  }
  *count_out = unique_count;
  return commits;
}

void contributions_tally(const struct ContributionCommit* commits,
                         ptrdiff_t commit_count,
                         struct Arena scratch,
                         struct ContributionCalendar* calendar_out) {
  *calendar_out = (struct ContributionCalendar){0};
  if (commit_count == 0) {
    return;
  }
  calendar_out->end_day = newest_day(commits, commit_count);
  calendar_out->start_day = calendar_out->end_day - (CONTRIBUTIONS_WINDOW_DAY_COUNT - 1);
  fill_weeks(commits, commit_count, calendar_out->end_day, scratch, calendar_out);
}

void contributions_tally_year(const struct ContributionCommit* commits,
                              ptrdiff_t commit_count,
                              int64_t year,
                              struct Arena scratch,
                              struct ContributionCalendar* calendar_out) {
  *calendar_out = (struct ContributionCalendar){0};
  const int64_t last_day =
      timestamp_civil_day((struct CivilDate){.year = year, .month = 12, .day = 31});
  const int64_t newest = newest_day(commits, commit_count);
  calendar_out->start_day =
      timestamp_civil_day((struct CivilDate){.year = year, .month = 1, .day = 1});
  calendar_out->end_day = newest < last_day ? newest : last_day;
  fill_weeks(commits, commit_count, last_day, scratch, calendar_out);
}

struct ContributionYear* contributions_years(const struct ContributionCommit* commits,
                                             ptrdiff_t commit_count,
                                             struct Arena* arena,
                                             ptrdiff_t* count_out) {
  struct ContributionYear* years = arena_new(arena, commit_count, struct ContributionYear);
  for (ptrdiff_t i = 0; i < commit_count; i++) {
    years[i] = (struct ContributionYear){
        .year = timestamp_civil_date(commits[i].day).year,
        .count = 1,
        .llm_count = commits[i].is_llm_assisted,
    };
  }
  if (commit_count > 1) {
    qsort(years, (size_t)commit_count, sizeof(years[0]), compare_years);
  }
  ptrdiff_t year_count = 0;
  for (ptrdiff_t i = 0; i < commit_count; i++) {
    if (year_count > 0 && years[year_count - 1].year == years[i].year) {
      years[year_count - 1].count++;
      years[year_count - 1].llm_count += years[i].llm_count;
    } else {
      years[year_count++] = years[i];
    }
  }
  *count_out = year_count;
  return years;
}

struct Str contributions_week_month(const struct ContributionCalendar* calendar, ptrdiff_t week) {
  const struct ContributionDay* week_days = &calendar->days[week * 7];
  for (ptrdiff_t d = 0; d < 7; d++) {
    const struct CivilDate date = timestamp_civil_date(week_days[d].day);
    if (date.day == 1 && !week_days[d].is_outside) {
      return CONTRIBUTIONS_MONTH_NAMES[date.month];
    }
  }
  return (struct Str){0};
}

static int64_t newest_day(const struct ContributionCommit* commits, ptrdiff_t commit_count) {
  int64_t day = commits[0].day;
  for (ptrdiff_t i = 1; i < commit_count; i++) {
    day = commits[i].day > day ? commits[i].day : day;
  }
  return day;
}

static void fill_weeks(const struct ContributionCommit* commits,
                       ptrdiff_t commit_count,
                       int64_t last_day,
                       struct Arena scratch,
                       struct ContributionCalendar* calendar) {
  const int64_t first_day = calendar->start_day - weekday(calendar->start_day);
  calendar->week_count = (ptrdiff_t)((last_day - first_day) / 7 + 1);
  for (ptrdiff_t i = 0; i < calendar->week_count * 7; i++) {
    struct ContributionDay* day = &calendar->days[i];
    day->day = first_day + i;
    day->is_outside = day->day < calendar->start_day || day->day > last_day;
    day->is_after_end = !day->is_outside && day->day > calendar->end_day;
  }
  for (ptrdiff_t i = 0; i < commit_count; i++) {
    if (commits[i].day < calendar->start_day || commits[i].day > calendar->end_day) {
      continue;
    }
    struct ContributionDay* day = &calendar->days[commits[i].day - first_day];
    day->count++;
    day->llm_count += commits[i].is_llm_assisted;
    calendar->count++;
    calendar->llm_count += commits[i].is_llm_assisted;
  }
  for (ptrdiff_t i = 0; i < calendar->week_count * 7; i++) {
    calendar->day_count += calendar->days[i].count > 0;
    calendar->llm_day_count += calendar->days[i].llm_count > 0;
  }
  fill_weeks_levels(calendar, scratch);
}

static void fill_weeks_levels(struct ContributionCalendar* calendar, struct Arena scratch) {
  const ptrdiff_t day_count = calendar->week_count * 7;
  ptrdiff_t* counts = arena_new(&scratch, day_count, ptrdiff_t);
  ptrdiff_t n = 0;
  for (ptrdiff_t i = 0; i < day_count; i++) {
    if (calendar->days[i].count > 0) {
      counts[n++] = calendar->days[i].count;
    }
  }
  if (n == 0) {
    return;
  }
  qsort(counts, (size_t)n, sizeof(counts[0]), compare_counts);
  ptrdiff_t quartiles[3];
  for (ptrdiff_t k = 1; k <= 3; k++) {
    quartiles[k - 1] = counts[(k * n + 3) / 4 - 1];
  }
  for (ptrdiff_t i = 0; i < day_count; i++) {
    struct ContributionDay* day = &calendar->days[i];
    if (day->count == 0) {
      continue;
    }
    day->level = 1;
    for (ptrdiff_t k = 0; k < 3; k++) {
      day->level += quartiles[k] < day->count;
    }
  }
}

static int64_t weekday(int64_t day) {
  // 1970-01-01 was a Thursday, weekday 4 counted from Sunday.
  return ((day % 7) + 7 + 4) % 7;
}

static int compare_oids(const void* a, const void* b) {
  return str_compare(((const struct ContributionCommit*)a)->oid,
                     ((const struct ContributionCommit*)b)->oid);
}

static int compare_years(const void* a, const void* b) {
  const int64_t x = ((const struct ContributionYear*)a)->year;
  const int64_t y = ((const struct ContributionYear*)b)->year;
  return (x < y) - (x > y);
}

static int compare_counts(const void* a, const void* b) {
  const ptrdiff_t x = *(const ptrdiff_t*)a;
  const ptrdiff_t y = *(const ptrdiff_t*)b;
  return (x > y) - (x < y);
}
