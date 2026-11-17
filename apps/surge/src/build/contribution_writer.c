#include "build/contribution_writer.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "build/contributions.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/path.h"
#include "core/str.h"
#include "domain/timestamp.h"
#include "os/oom.h"
#include "output/manifest_builder.h"
#include "output/output_writer.h"
#include "vcs/repository.h"

/** Directory of the contributions files below `output_dir`. */
#define CONTRIBUTION_WRITER_DIR "contributions"

/** What makes one year's file, for `output_writer_produce`. */
struct ContributionMake {
  /** Every commit that counts, from `contributions_merge`. */
  const struct ContributionCommit* commits;

  /** Number of entries in `commits`. */
  ptrdiff_t commit_count;

  /** The year of the file. */
  int64_t year;
};

/**
 * @brief Returns the attributes of one year's file: the page recipe, the id of its commits, one
 *        line `<oid> <day> <0 or 1>` each, in byte order of `oid`, and its last counted day.
 *
 * @param commits      Every commit that counts. Must not be `NULL`.
 * @param commit_count Number of entries in `commits`.
 * @param year         The year, with its last counted day. Must not be `NULL`.
 * @param recipe_id    Id of the page recipe.
 * @param arena        Arena that owns the attributes. Must not be `NULL`.
 * @return The attributes.
 */
static struct Str contribution_writer_register_attributes(const struct ContributionCommit* commits,
                                                          ptrdiff_t commit_count,
                                                          const struct ContributionYear* year,
                                                          struct Str recipe_id,
                                                          struct Arena* arena)
    __attribute__((nonnull(1, 3, 5)));

/**
 * @brief Makes one year's file, as `OutputWriterMakeFn`.
 *
 * @param perm      Arena that owns the bytes. Must not be `NULL`.
 * @param scratch   Arena for the calendar.
 * @param bytes_out Receives the bytes. Must not be `NULL`.
 * @param log       Unused, as the make cannot fail.
 * @param userdata  The `struct ContributionMake`.
 * @return `0`.
 */
static int contribution_writer_write_make(struct Arena* perm,
                                          struct Arena scratch,
                                          struct Str* bytes_out,
                                          struct Log* log,
                                          void* userdata) __attribute__((nonnull(1, 3, 4, 5)));

/**
 * @brief Returns a year's calendar as one line of JSON and a newline.
 *
 * @param calendar The year's calendar. Must not be `NULL`.
 * @param year     The year.
 * @param perm     Arena that owns the text. Must not be `NULL`.
 * @param scratch  Arena for each date.
 * @return The text.
 */
static struct Str calendar_json(const struct ContributionCalendar* calendar,
                                int64_t year,
                                struct Arena* perm,
                                struct Arena scratch) __attribute__((nonnull(1, 3)));

struct Str contribution_writer_path(int64_t year, struct Arena* arena) {
  struct Buf path = {.arena = arena};
  buf_printf(&path, CONTRIBUTION_WRITER_DIR "/%04" PRId64 ".json", year);
  return buf_str(&path);
}

int contribution_writer_register(struct ManifestBuilder* builder,
                                 const struct RepoModel* repos,
                                 ptrdiff_t repo_count,
                                 struct Str recipe_id,
                                 struct Arena scratch,
                                 struct Log* log) {
  ptrdiff_t commit_count = 0;
  const struct ContributionCommit* commits =
      contributions_merge(repos, repo_count, &scratch, &commit_count);
  ptrdiff_t year_count = 0;
  const struct ContributionYear* years =
      contributions_years(commits, commit_count, &scratch, &year_count);
  int rc = 0;
  for (ptrdiff_t i = year_count - 1; i >= 0; i--) {
    struct Arena year_scratch = scratch;
    const struct Str relative_path = contribution_writer_path(years[i].year, &year_scratch);
    const struct Str attributes = contribution_writer_register_attributes(
        commits, commit_count, &years[i], recipe_id, &year_scratch);
    if (manifest_builder_add_output(builder, relative_path, relative_path, attributes, year_scratch,
                                    log) != 0) {
      rc = -1;
    }
  }
  return rc;
}

int contribution_writer_write(const struct OutputWriter* writer,
                              struct Str output_dir,
                              const struct RepoModel* repos,
                              ptrdiff_t repo_count,
                              const struct OutputCache* cache,
                              struct Arena scratch,
                              struct Log* log) {
  ptrdiff_t commit_count = 0;
  const struct ContributionCommit* commits =
      contributions_merge(repos, repo_count, &scratch, &commit_count);
  ptrdiff_t year_count = 0;
  const struct ContributionYear* years =
      contributions_years(commits, commit_count, &scratch, &year_count);
  int rc = 0;
  for (ptrdiff_t i = year_count - 1; i >= 0; i--) {
    struct Arena file_scratch = scratch;
    const struct Str relative_path = contribution_writer_path(years[i].year, &file_scratch);
    oom_set_job(relative_path, NULL);
    const struct Str file_path = path_join(output_dir, relative_path, &file_scratch);
    // The file grows in one half of the scratch, and the calendar and the read of the file in the
    // output use the other.
    const char* region_end = file_scratch.limit != NULL ? file_scratch.limit : file_scratch.end;
    struct Arena file_arena = arena_split(&file_scratch, (region_end - file_scratch.beg) / 2);
    struct ContributionMake make = {
        .commits = commits,
        .commit_count = commit_count,
        .year = years[i].year,
    };
    if (output_writer_produce(writer, file_path, cache, contribution_writer_write_make, &make,
                              file_arena, file_scratch, S("failed to write contributions"),
                              log) != 0) {
      rc = -1;
    }
  }
  return rc;
}

static struct Str contribution_writer_register_attributes(const struct ContributionCommit* commits,
                                                          ptrdiff_t commit_count,
                                                          const struct ContributionYear* year,
                                                          struct Str recipe_id,
                                                          struct Arena* arena) {
  struct Buf text = {.arena = arena};
  for (ptrdiff_t i = 0; i < commit_count; i++) {
    if (timestamp_civil_date(commits[i].day).year == year->year) {
      buf_printf(&text, "%.*s %" PRId64 " %d\n", STR_FMT(commits[i].oid), commits[i].day,
                 commits[i].is_llm_assisted);
    }
  }
  const struct Str commits_id = repository_text_id(buf_str(&text), arena);
  const struct Str end_date = timestamp_format_date(year->end_day, arena);
  struct Buf attributes = {.arena = arena};
  buf_printf(&attributes, "recipe=%.*s commits=%.*s end=%.*s", STR_FMT(recipe_id),
             STR_FMT(commits_id), STR_FMT(end_date));
  return buf_str(&attributes);
}

static int contribution_writer_write_make(struct Arena* perm,
                                          struct Arena scratch,
                                          struct Str* bytes_out,
                                          struct Log* log,
                                          void* userdata) {
  (void)log;
  const struct ContributionMake* make = userdata;
  struct ContributionCalendar* calendar = arena_new(&scratch, 1, struct ContributionCalendar);
  contributions_tally_year(make->commits, make->commit_count, make->year, scratch, calendar);
  *bytes_out = calendar_json(calendar, make->year, perm, scratch);
  return 0;
}

static struct Str calendar_json(const struct ContributionCalendar* calendar,
                                int64_t year,
                                struct Arena* perm,
                                struct Arena scratch) {
  // Every value is a number, a `YYYY-MM-DD` date, a month's short name or a flag, so none holds a
  // byte that JSON escapes.
  struct Buf text = {.arena = perm};
  struct Arena date_scratch = scratch;
  const struct Str start_date = timestamp_format_date(calendar->start_day, &date_scratch);
  const struct Str end_date = timestamp_format_date(calendar->end_day, &date_scratch);
  buf_printf(&text,
             "{\"year\":%" PRId64
             ",\"count\":%td,\"llm_count\":%td,\"day_count\":%td,\"llm_day_count\":%td,"
             "\"start_date\":\"%.*s\",\"end_date\":\"%.*s\",\"weeks\":[",
             year, calendar->count, calendar->llm_count, calendar->day_count,
             calendar->llm_day_count, STR_FMT(start_date), STR_FMT(end_date));
  for (ptrdiff_t w = 0; w < calendar->week_count; w++) {
    const struct Str month = contributions_week_month(calendar, w);
    buf_printf(&text, "%s{\"month\":\"%.*s\",\"days\":[", w == 0 ? "" : ",", STR_FMT(month));
    for (ptrdiff_t d = 0; d < 7; d++) {
      const struct ContributionDay* day = &calendar->days[w * 7 + d];
      date_scratch = scratch;
      const struct Str date = timestamp_format_date(day->day, &date_scratch);
      buf_printf(&text,
                 "%s{\"date\":\"%.*s\",\"weekday\":%td,\"count\":%td,\"llm_count\":%td,"
                 "\"level\":%d,\"has_llm\":%s,\"is_outside\":%s,\"is_after_end\":%s}",
                 d == 0 ? "" : ",", STR_FMT(date), d, day->count, day->llm_count, day->level,
                 day->llm_count > 0 ? "true" : "false", day->is_outside ? "true" : "false",
                 day->is_after_end ? "true" : "false");
    }
    buf_append(&text, S("]}"));
  }
  buf_append(&text, S("]}\n"));
  return buf_str(&text);
}
