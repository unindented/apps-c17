#include "build/contributions.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "core/arena.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "domain/timestamp.h"

/** One commit that counts: its id, its local day and whether it is LLM-assisted. */
struct CountedCommit {
  /** Hex id of the commit. */
  struct Str oid;

  /** Day number of its author time in its recorded offset. */
  int64_t day;

  /** Whether its trailers mark it LLM-assisted. */
  bool is_llm_assisted;
};

/**
 * @brief Lists every commit of every repo's contributions whose date RFC 3339 can write, each once
 *        by `oid`, in byte order of `oid`.
 *
 * @param repos      What the scan found. Must not be `NULL` unless `repo_count` is `0`.
 * @param repo_count Number of entries in `repos`.
 * @param arena      Arena that owns the list. Must not be `NULL`.
 * @param count_out  Receives the number of commits. Must not be `NULL`.
 * @return The commits.
 */
static struct CountedCommit* contributions_tally_merge(const struct RepoModel* repos,
                                                       ptrdiff_t repo_count,
                                                       struct Arena* arena,
                                                       ptrdiff_t* count_out)
    __attribute__((nonnull(3, 4), returns_nonnull));

/**
 * @brief Sets the `level` of each day of the window with commits, from the quartiles of the
 *        window's non-empty days.
 *
 * The quartiles are nearest-rank: the `k`th, for `k` from 1 to 3, is the count at index
 * `ceil(k * n / 4) - 1` of the `n` non-empty counts in ascending order. So with one non-empty day,
 * or with every non-empty day equal, each is level 1.
 *
 * @param calendar Calendar whose days are counted. Must not be `NULL`.
 * @param scratch  Arena for the sorted counts.
 */
static void contributions_tally_levels(struct ContributionCalendar* calendar, struct Arena scratch)
    __attribute__((nonnull(1)));

/**
 * @brief Orders two counted commits by `oid` in byte order, for `qsort`.
 *
 * @param a First `struct CountedCommit`. Must not be `NULL`.
 * @param b Second `struct CountedCommit`. Must not be `NULL`.
 * @return Negative, zero or positive as `a` sorts before, with, or after `b`.
 */
static int compare_oids(const void* a, const void* b) __attribute__((nonnull(1, 2)));

/**
 * @brief Orders two counts in ascending order, for `qsort`.
 *
 * @param a First `ptrdiff_t`. Must not be `NULL`.
 * @param b Second `ptrdiff_t`. Must not be `NULL`.
 * @return Negative, zero or positive as `a` is less than, equal to, or greater than `b`.
 */
static int compare_counts(const void* a, const void* b) __attribute__((nonnull(1, 2)));

void contributions_tally(const struct RepoModel* repos,
                         ptrdiff_t repo_count,
                         struct Arena scratch,
                         struct ContributionCalendar* calendar_out) {
  *calendar_out = (struct ContributionCalendar){0};
  ptrdiff_t commit_count = 0;
  const struct CountedCommit* commits =
      contributions_tally_merge(repos, repo_count, &scratch, &commit_count);
  if (commit_count == 0) {
    return;
  }
  int64_t end_day = commits[0].day;
  for (ptrdiff_t i = 1; i < commit_count; i++) {
    end_day = commits[i].day > end_day ? commits[i].day : end_day;
  }
  calendar_out->end_day = end_day;
  calendar_out->start_day = end_day - (CONTRIBUTIONS_WINDOW_DAY_COUNT - 1);
  // 1970-01-01 was a Thursday, weekday 4 counted from Sunday.
  const int64_t end_weekday = ((end_day % 7) + 7 + 4) % 7;
  const int64_t first_day = end_day - end_weekday - (CONTRIBUTIONS_WEEK_COUNT - 1) * 7;
  for (ptrdiff_t i = 0; i < CONTRIBUTIONS_DAY_COUNT; i++) {
    struct ContributionDay* day = &calendar_out->days[i];
    day->day = first_day + i;
    day->is_outside = day->day < calendar_out->start_day || day->day > end_day;
  }
  for (ptrdiff_t i = 0; i < commit_count; i++) {
    if (commits[i].day < calendar_out->start_day) {
      continue;
    }
    struct ContributionDay* day = &calendar_out->days[commits[i].day - first_day];
    day->count++;
    day->llm_count += commits[i].is_llm_assisted;
    calendar_out->count++;
    calendar_out->llm_count += commits[i].is_llm_assisted;
  }
  for (ptrdiff_t i = 0; i < CONTRIBUTIONS_DAY_COUNT; i++) {
    calendar_out->day_count += calendar_out->days[i].count > 0;
    calendar_out->llm_day_count += calendar_out->days[i].llm_count > 0;
  }
  contributions_tally_levels(calendar_out, scratch);
}

static struct CountedCommit* contributions_tally_merge(const struct RepoModel* repos,
                                                       ptrdiff_t repo_count,
                                                       struct Arena* arena,
                                                       ptrdiff_t* count_out) {
  ptrdiff_t total = 0;
  for (ptrdiff_t i = 0; i < repo_count; i++) {
    total += repos[i].contribution_count;
  }
  struct CountedCommit* commits = arena_new(arena, total, struct CountedCommit);
  ptrdiff_t count = 0;
  for (ptrdiff_t i = 0; i < repo_count; i++) {
    for (ptrdiff_t j = 0; j < repos[i].contribution_count; j++) {
      const struct Contribution* contribution = &repos[i].contributions[j];
      if (timestamp_is_writable(contribution->time, contribution->offset_minutes)) {
        commits[count++] = (struct CountedCommit){
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

static void contributions_tally_levels(struct ContributionCalendar* calendar,
                                       struct Arena scratch) {
  ptrdiff_t* counts = arena_new(&scratch, CONTRIBUTIONS_DAY_COUNT, ptrdiff_t);
  ptrdiff_t n = 0;
  for (ptrdiff_t i = 0; i < CONTRIBUTIONS_DAY_COUNT; i++) {
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
  for (ptrdiff_t i = 0; i < CONTRIBUTIONS_DAY_COUNT; i++) {
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

static int compare_oids(const void* a, const void* b) {
  return str_compare(((const struct CountedCommit*)a)->oid, ((const struct CountedCommit*)b)->oid);
}

static int compare_counts(const void* a, const void* b) {
  const ptrdiff_t x = *(const ptrdiff_t*)a;
  const ptrdiff_t y = *(const ptrdiff_t*)b;
  return (x > y) - (x < y);
}
