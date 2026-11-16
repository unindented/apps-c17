#include "build/page_index.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "build/contributions.h"
#include "build/page_context.h"
#include "build/page_paths.h"
#include "build/page_release.h"
#include "build/page_tree.h"
#include "build/template_value.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "domain/site_config.h"
#include "domain/timestamp.h"
#include "output/output_path.h"

/** Short English name of each month, indexed by its number from 1. */
static const struct Str PAGE_INDEX_MONTH_NAMES[] = {
    STR_INIT(""),    STR_INIT("Jan"), STR_INIT("Feb"), STR_INIT("Mar"), STR_INIT("Apr"),
    STR_INIT("May"), STR_INIT("Jun"), STR_INIT("Jul"), STR_INIT("Aug"), STR_INIT("Sep"),
    STR_INIT("Oct"), STR_INIT("Nov"), STR_INIT("Dec"),
};

/**
 * @brief Returns the `contributions` map of the site index, or `false` when no commit counts.
 *
 * @param repos      What the scan found. Must not be `NULL` unless `repo_count` is `0`.
 * @param repo_count Number of entries in `repos`.
 * @param perm       Arena that owns the map. Must not be `NULL`.
 * @param scratch    Arena for the calendar and the commits it merges.
 * @return The map.
 */
static struct TemplateValue* page_index_value_contributions(const struct RepoModel* repos,
                                                            ptrdiff_t repo_count,
                                                            struct Arena* perm,
                                                            struct Arena scratch)
    __attribute__((nonnull(3), returns_nonnull));

/**
 * @brief Returns the map of one day of the contributions calendar.
 *
 * @param day     The day. Must not be `NULL`.
 * @param weekday Its weekday, `0` for Sunday.
 * @param arena   Arena that owns the map. Must not be `NULL`.
 * @return The map.
 */
static struct TemplateValue* page_index_value_contributions_day(const struct ContributionDay* day,
                                                                ptrdiff_t weekday,
                                                                struct Arena* arena)
    __attribute__((nonnull(1, 3), returns_nonnull));

/**
 * @brief Returns a day number as a `YYYY-MM-DD` date.
 *
 * @param day   Day number, counted from 1970-01-01.
 * @param arena Arena that owns the text. Must not be `NULL`.
 * @return The date.
 */
static struct Str date_text(int64_t day, struct Arena* arena) __attribute__((nonnull(2)));

/**
 * @brief Returns a decimal number as text.
 *
 * @param number Number to write.
 * @param arena  Arena that owns the text. Must not be `NULL`.
 * @return The text.
 */
static struct Str number_text(ptrdiff_t number, struct Arena* arena) __attribute__((nonnull(2)));

/**
 * @brief Returns the map of one repo in the site index.
 *
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param repo        What the scan found for the repo. Must not be `NULL`.
 * @param arena       Arena that owns the map. Must not be `NULL`.
 * @return The map.
 */
static struct TemplateValue* page_index_value_repo(const struct SiteConfig* site_config,
                                                   const struct RepoModel* repo,
                                                   struct Arena* arena)
    __attribute__((nonnull(1, 2, 3), returns_nonnull));

/**
 * @brief Returns the map of one project of a repo in the site index.
 *
 * @param repo    What the scan found for the project's repo. Must not be `NULL`.
 * @param project The project's table in the config. Must not be `NULL`.
 * @param arena   Arena that owns the map. Must not be `NULL`.
 * @return The map.
 */
static struct TemplateValue* page_index_value_project(const struct RepoModel* repo,
                                                      const struct SurgeProject* project,
                                                      struct Arena* arena)
    __attribute__((nonnull(1, 2, 3), returns_nonnull));

struct TemplateValue* page_index_value(const struct SiteConfig* site_config,
                                       const struct RepoModel* repos,
                                       ptrdiff_t repo_count,
                                       struct Arena* perm,
                                       struct Arena scratch) {
  struct TemplateValue* root = page_context_new(site_config, PAGE_INDEX_PAGE, perm);
  struct TemplateValue* list = template_value_list(repo_count, perm);
  for (ptrdiff_t i = 0; i < repo_count; i++) {
    list->items[i] = page_index_value_repo(site_config, &repos[i], perm);
  }
  template_value_set(root, S("repos"), list, perm);
  template_value_set(root, S("contributions"),
                     page_index_value_contributions(repos, repo_count, perm, scratch), perm);
  return root;
}

struct TemplateValue* page_index_not_found_value(const struct SiteConfig* site_config,
                                                 struct Arena* arena) {
  return page_context_new_with_path_to_root(
      site_config, output_path_not_found_root(site_config->base_url, arena), arena);
}

static struct TemplateValue* page_index_value_contributions(const struct RepoModel* repos,
                                                            ptrdiff_t repo_count,
                                                            struct Arena* perm,
                                                            struct Arena scratch) {
  struct ContributionCalendar* calendar = arena_new(&scratch, 1, struct ContributionCalendar);
  contributions_tally(repos, repo_count, scratch, calendar);
  if (calendar->count == 0) {
    return template_value_bool(false, perm);
  }
  struct TemplateValue* map = template_value_map(perm);
  const struct {
    struct Str key;
    ptrdiff_t number;
  } numbers[] = {
      {S("count"), calendar->count},
      {S("llm_count"), calendar->llm_count},
      {S("day_count"), calendar->day_count},
      {S("llm_day_count"), calendar->llm_day_count},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(numbers) / sizeof(numbers[0])); i++) {
    template_value_set_text(map, numbers[i].key, number_text(numbers[i].number, perm), perm);
  }
  template_value_set_text(map, S("start_date"), date_text(calendar->start_day, perm), perm);
  template_value_set_text(map, S("end_date"), date_text(calendar->end_day, perm), perm);
  struct TemplateValue* weeks = template_value_list(CONTRIBUTIONS_WEEK_COUNT, perm);
  for (ptrdiff_t w = 0; w < CONTRIBUTIONS_WEEK_COUNT; w++) {
    const struct ContributionDay* week_days = &calendar->days[w * 7];
    // A week names the month whose 1st it holds inside the window.
    struct Str month = {0};
    struct TemplateValue* days = template_value_list(7, perm);
    for (ptrdiff_t d = 0; d < 7; d++) {
      const struct CivilDate date = timestamp_civil_date(week_days[d].day);
      if (date.day == 1 && !week_days[d].is_outside) {
        month = PAGE_INDEX_MONTH_NAMES[date.month];
      }
      days->items[d] = page_index_value_contributions_day(&week_days[d], d, perm);
    }
    struct TemplateValue* week = template_value_map(perm);
    template_value_set_text(week, S("month"), month, perm);
    template_value_set(week, S("days"), days, perm);
    weeks->items[w] = week;
  }
  template_value_set(map, S("weeks"), weeks, perm);
  return map;
}

static struct TemplateValue* page_index_value_contributions_day(const struct ContributionDay* day,
                                                                ptrdiff_t weekday,
                                                                struct Arena* arena) {
  struct TemplateValue* map = template_value_map(arena);
  template_value_set_text(map, S("date"), date_text(day->day, arena), arena);
  template_value_set_text(map, S("weekday"), number_text(weekday, arena), arena);
  template_value_set_text(map, S("count"), number_text(day->count, arena), arena);
  template_value_set_text(map, S("llm_count"), number_text(day->llm_count, arena), arena);
  template_value_set_text(map, S("level"), number_text(day->level, arena), arena);
  template_value_set_bool(map, S("has_llm"), day->llm_count > 0, arena);
  template_value_set_bool(map, S("is_outside"), day->is_outside, arena);
  return map;
}

static struct Str date_text(int64_t day, struct Arena* arena) {
  const struct CivilDate date = timestamp_civil_date(day);
  struct Buf text = {.arena = arena};
  buf_printf(&text, "%04" PRId64 "-%02d-%02d", date.year, date.month, date.day);
  return buf_str(&text);
}

static struct Str number_text(ptrdiff_t number, struct Arena* arena) {
  struct Buf text = {.arena = arena};
  buf_printf(&text, "%td", number);
  return buf_str(&text);
}

static struct TemplateValue* page_index_value_repo(const struct SiteConfig* site_config,
                                                   const struct RepoModel* repo,
                                                   struct Arena* arena) {
  const struct SurgeRepo* repo_config = repo->config;
  struct TemplateValue* map = template_value_map(arena);
  template_value_set_text(map, S("name"), repo_config->name, arena);
  template_value_set_text(map, S("description"), repo->description, arena);
  // The index is at the output root, so a home's output path links it. A repo name uses only
  // unreserved bytes, so it needs no encoding.
  template_value_set_text(map, S("url"), page_tree_home_path(repo_config->name, arena), arena);
  template_value_set_text(map, S("clone_url"),
                          page_context_clone_url(site_config, repo_config->name, arena), arena);
  template_value_set_text(map, S("default_branch"), repo->default_branch, arena);
  template_value_set_text(map, S("modified"), repo->modified, arena);
  struct TemplateValue* projects = template_value_list(repo_config->project_count, arena);
  for (ptrdiff_t i = 0; i < repo_config->project_count; i++) {
    projects->items[i] = page_index_value_project(repo, &repo_config->projects[i], arena);
  }
  template_value_set(map, S("projects"), projects, arena);
  return map;
}

static struct TemplateValue* page_index_value_project(const struct RepoModel* repo,
                                                      const struct SurgeProject* project,
                                                      struct Arena* arena) {
  struct TemplateValue* map = template_value_map(arena);
  template_value_set_text(map, S("name"), project->name, arena);
  template_value_set_text(map, S("description"), project->description, arena);
  // An empty repo, or a default branch that names no commit, has no tree page to link.
  const bool has_tree = repo->default_ref != NULL && repo->default_ref->commit_oid.len > 0;
  template_value_set_text(map, S("url"),
                          has_tree
                              ? page_path_link((struct Str){0}, PAGE_PATH_TREE, repo->config->name,
                                               repo->default_branch, project->path, arena)
                              : (struct Str){0},
                          arena);
  const bool has_releases = project->tag_prefix.len > 0;
  template_value_set_bool(map, S("has_releases"), has_releases, arena);
  template_value_set_text(
      map, S("releases_url"),
      has_releases
          ? page_release_project_path(repo->config->name, project->name, S("index.html"), arena)
          : (struct Str){0},
      arena);
  return map;
}
