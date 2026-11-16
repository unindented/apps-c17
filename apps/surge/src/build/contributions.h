#ifndef SURGE_CONTRIBUTIONS_H
#define SURGE_CONTRIBUTIONS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/arena.h"

struct RepoModel;

/** Columns of the calendar, one per Sunday-to-Saturday week. */
enum { CONTRIBUTIONS_WEEK_COUNT = 53 };

/** Days of the calendar: every day of its weeks, the ones outside its window included. */
enum { CONTRIBUTIONS_DAY_COUNT = CONTRIBUTIONS_WEEK_COUNT * 7 };

/** Days of the window, which ends with the newest commit's day: 52 weeks and one day. */
enum { CONTRIBUTIONS_WINDOW_DAY_COUNT = 365 };

/** One day of the calendar. */
struct ContributionDay {
  /** Day number, counted from 1970-01-01. */
  int64_t day;

  /** Commits on the day, `0` outside the window. */
  ptrdiff_t count;

  /** LLM-assisted commits on the day, `0` outside the window. */
  ptrdiff_t llm_count;

  /**
   * `0` for a day without commits, else `1` to `4`: one more than the number of quartiles of the
   * window's non-empty days that are less than `count`.
   */
  int level;

  /** Whether the day is before the window's first day or after its last. */
  bool is_outside;
};

/** The contributions calendar of the site index. */
struct ContributionCalendar {
  /** First day of the window, 364 days before `end_day`. */
  int64_t start_day;

  /** Last day of the window: the newest local day of a counted commit. */
  int64_t end_day;

  /** Commits in the window, `0` when no commit counts at all. */
  ptrdiff_t count;

  /** LLM-assisted commits in the window. */
  ptrdiff_t llm_count;

  /** Days of the window with commits. */
  ptrdiff_t day_count;

  /** Days of the window with LLM-assisted commits. */
  ptrdiff_t llm_day_count;

  /** The days of the weeks, oldest first, each week from Sunday to Saturday. */
  struct ContributionDay days[CONTRIBUTIONS_DAY_COUNT];
};

/**
 * @brief Counts the commits of every repo's default branch into the calendar of the 53 weeks that
 *        end with the week of the newest one.
 *
 * A commit counts once, by `oid`, however many repos reach it, on the day of its author time in the
 * offset git recorded. A commit whose date RFC 3339 cannot write does not count, and a commit
 * before the window counts in no day. The window ends with the newest counted day, not the build's
 * date, so the same repositories always give the same calendar.
 *
 * @param repos        What the scan found, each with its `contributions`. Must not be `NULL` unless
 *                     `repo_count` is `0`.
 * @param repo_count   Number of entries in `repos`.
 * @param scratch      Arena for the merged commits.
 * @param calendar_out Receives the calendar, whose `count` is `0` when no commit counts. Must not
 *                     be `NULL`.
 */
void contributions_tally(const struct RepoModel* repos,
                         ptrdiff_t repo_count,
                         struct Arena scratch,
                         struct ContributionCalendar* calendar_out) __attribute__((nonnull(4)));

#endif
