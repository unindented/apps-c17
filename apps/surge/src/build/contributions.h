#ifndef SURGE_CONTRIBUTIONS_H
#define SURGE_CONTRIBUTIONS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/arena.h"
#include "core/str.h"

struct RepoModel;

/** Columns of the window's calendar, one per Sunday-to-Saturday week. */
enum { CONTRIBUTIONS_WINDOW_WEEK_COUNT = 53 };

/** Days of the window, which ends with the newest commit's day: 52 weeks and one day. */
enum { CONTRIBUTIONS_WINDOW_DAY_COUNT = 365 };

/**
 * Most weeks of a calendar: a year takes 53 Sunday-to-Saturday weeks, or 54 when it is a leap year
 * that starts on a Saturday, such as 2028.
 */
enum { CONTRIBUTIONS_WEEK_COUNT_MAX = 54 };

/** Most days of a calendar: every day of its weeks, the ones outside its range included. */
enum { CONTRIBUTIONS_DAY_COUNT_MAX = CONTRIBUTIONS_WEEK_COUNT_MAX * 7 };

/** One commit that counts: its id, its local day and whether it is LLM-assisted. */
struct ContributionCommit {
  /** Hex id of the commit. */
  struct Str oid;

  /** Day number of its author time in its recorded offset, counted from 1970-01-01. */
  int64_t day;

  /** Whether its trailers mark it LLM-assisted. */
  bool is_llm_assisted;
};

/** One day of a calendar. */
struct ContributionDay {
  /** Day number, counted from 1970-01-01. */
  int64_t day;

  /** Commits on the day, `0` when it is outside the range. */
  ptrdiff_t count;

  /** LLM-assisted commits on the day, `0` when it is outside the range. */
  ptrdiff_t llm_count;

  /**
   * `0` for a day without commits, else `1` to `4`: one more than the number of quartiles of the
   * range's non-empty days that are less than `count`.
   */
  int level;

  /** Whether the day is in the first week before the range or in the last week after it. */
  bool is_outside;

  /**
   * Whether the day is in the range but after `end_day`, as the days of the newest year after the
   * newest commit's day are. An `is_outside` day never is.
   */
  bool is_after_end;
};

/** A contributions calendar: the window of the site index, or one calendar year. */
struct ContributionCalendar {
  /** First day of the range: 364 days before `end_day` for the window, or January 1. */
  int64_t start_day;

  /**
   * Last day that counts: the newest local day of a counted commit, or December 31 for a year
   * before the newest.
   */
  int64_t end_day;

  /** Commits in the range, `0` when no commit counts at all. */
  ptrdiff_t count;

  /** LLM-assisted commits in the range. */
  ptrdiff_t llm_count;

  /** Days of the range with commits. */
  ptrdiff_t day_count;

  /** Days of the range with LLM-assisted commits. */
  ptrdiff_t llm_day_count;

  /** Weeks in `days`: `CONTRIBUTIONS_WINDOW_WEEK_COUNT` for the window, 53 or 54 for a year. */
  ptrdiff_t week_count;

  /** The days of the weeks, oldest first, each week from Sunday to Saturday. */
  struct ContributionDay days[CONTRIBUTIONS_DAY_COUNT_MAX];
};

/** The totals of one calendar year with commits. */
struct ContributionYear {
  /** The year, such as `2026`. */
  int64_t year;

  /** Commits in the year. */
  ptrdiff_t count;

  /** LLM-assisted commits in the year. */
  ptrdiff_t llm_count;

  /**
   * Last day of the year that counts, as `contributions_tally_year` gives it: December 31, or for
   * the newest year, the newest commit's day.
   */
  int64_t end_day;
};

/**
 * @brief Lists the commits of every repo's default branch that count, each once by `oid`, in byte
 *        order of `oid`.
 *
 * A commit counts once, by `oid`, however many repos reach it, on the day of its author time in the
 * offset git recorded. A commit whose date RFC 3339 cannot write does not count.
 *
 * @param repos      What the scan found, each with its `contributions`. Must not be `NULL` unless
 *                   `repo_count` is `0`.
 * @param repo_count Number of entries in `repos`.
 * @param arena      Arena that owns the list. Must not be `NULL`.
 * @param count_out  Receives the number of commits. Must not be `NULL`.
 * @return The commits.
 */
struct ContributionCommit* contributions_merge(const struct RepoModel* repos,
                                               ptrdiff_t repo_count,
                                               struct Arena* arena,
                                               ptrdiff_t* count_out)
    __attribute__((nonnull(3, 4), returns_nonnull));

/**
 * @brief Counts commits into the window of the site index: the 53 weeks that end with the week of
 *        the newest one.
 *
 * A commit before the window counts in no day. The window ends with the newest counted day, not the
 * build's date, so the same repositories always give the same calendar.
 *
 * @param commits      Commits from `contributions_merge`. Must not be `NULL` unless `commit_count`
 *                     is `0`.
 * @param commit_count Number of entries in `commits`.
 * @param scratch      Arena for the sorted counts.
 * @param calendar_out Receives the calendar, whose `count` is `0` when no commit counts. Must not
 *                     be `NULL`.
 */
void contributions_tally(const struct ContributionCommit* commits,
                         ptrdiff_t commit_count,
                         struct Arena scratch,
                         struct ContributionCalendar* calendar_out) __attribute__((nonnull(4)));

/**
 * @brief Counts the commits of one calendar year into the weeks from the one of January 1 to the
 *        one of December 31, with levels from the year's own quartiles.
 *
 * The days of the first week before January 1 and of the last week after December 31 are
 * `is_outside`. The newest year ends on the newest commit's day, and its later days are
 * `is_after_end`.
 *
 * @param commits      Commits from `contributions_merge`, at least one of them in `year`. Must not
 *                     be `NULL`.
 * @param commit_count Number of entries in `commits`.
 * @param year         The year, from 0000 to 9999.
 * @param scratch      Arena for the sorted counts.
 * @param calendar_out Receives the calendar. Must not be `NULL`.
 */
void contributions_tally_year(const struct ContributionCommit* commits,
                              ptrdiff_t commit_count,
                              int64_t year,
                              struct Arena scratch,
                              struct ContributionCalendar* calendar_out)
    __attribute__((nonnull(1, 5)));

/**
 * @brief Lists the calendar years with commits, newest first, with their totals and last days.
 *
 * @param commits      Commits from `contributions_merge`. Must not be `NULL` unless `commit_count`
 *                     is `0`.
 * @param commit_count Number of entries in `commits`.
 * @param arena        Arena that owns the list. Must not be `NULL`.
 * @param count_out    Receives the number of years, `0` when no commit counts. Must not be `NULL`.
 * @return The years.
 */
struct ContributionYear* contributions_years(const struct ContributionCommit* commits,
                                             ptrdiff_t commit_count,
                                             struct Arena* arena,
                                             ptrdiff_t* count_out)
    __attribute__((nonnull(3, 4), returns_nonnull));

/**
 * @brief Returns the short English name of the month whose 1st a week of a calendar holds, such as
 *        `Jan`, or empty when no day of the week inside the range is a 1st.
 *
 * @param calendar The calendar. Must not be `NULL`.
 * @param week     Index of the week, below `week_count`.
 * @return The name, a static string.
 */
struct Str contributions_week_month(const struct ContributionCalendar* calendar, ptrdiff_t week)
    __attribute__((nonnull(1)));

#endif
