#include "build/page_index.h"

#include <stdbool.h>
#include <stddef.h>

#include "build/page_context.h"
#include "build/page_paths.h"
#include "build/page_release.h"
#include "build/page_tree.h"
#include "build/template_value.h"
#include "core/arena.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "domain/site_config.h"

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
                                       struct Arena* arena) {
  struct TemplateValue* root = page_context_new(site_config, PAGE_INDEX_PAGE, arena);
  struct TemplateValue* list = template_value_list(repo_count, arena);
  for (ptrdiff_t i = 0; i < repo_count; i++) {
    list->items[i] = page_index_value_repo(site_config, &repos[i], arena);
  }
  template_value_set(root, S("repos"), list, arena);
  return root;
}

struct TemplateValue* page_index_not_found_value(const struct SiteConfig* site_config,
                                                 struct Arena* arena) {
  return page_context_new(site_config, PAGE_INDEX_NOT_FOUND_PAGE, arena);
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
