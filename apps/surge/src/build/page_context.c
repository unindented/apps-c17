#include "build/page_context.h"

#include <stdbool.h>

#include "build/page_feed.h"
#include "build/page_paths.h"
#include "build/page_refs.h"
#include "build/page_release.h"
#include "build/page_tree.h"
#include "build/template_value.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "domain/site_config.h"
#include "domain/surge_version.h"
#include "output/output_path.h"

/**
 * @brief Returns a `ref` map with the branch's name and its links.
 *
 * @param branch_name      Name of the branch below `refs/heads/`.
 * @param tree_url         Link to its root's tree page, or empty.
 * @param commits_url      Link to its log, or empty.
 * @param commits_feed_url Link to its commit feed, or empty.
 * @param arena            Arena that owns the map. Must not be `NULL`.
 * @return The map.
 */
/**
 * @brief Returns the default branch of a repo when it names a commit, which gives it a log, or
 *        empty otherwise, as for an empty repo.
 *
 * @param repo What the scan found for the repo. Must not be `NULL`.
 * @return The branch name below `refs/heads/`, or empty.
 */
static struct Str logged_default_branch(const struct RepoModel* repo) __attribute__((nonnull(1)));

static struct TemplateValue* branch_map(struct Str branch_name,
                                        struct Str tree_url,
                                        struct Str commits_url,
                                        struct Str commits_feed_url,
                                        struct Arena* arena)
    __attribute__((nonnull(5), returns_nonnull));

struct TemplateValue* page_context_new(const struct SiteConfig* site_config,
                                       struct Str relative_path,
                                       struct Arena* arena) {
  return page_context_new_with_path_to_root(site_config, output_path_to_root(relative_path, arena),
                                            arena);
}

struct TemplateValue* page_context_new_with_path_to_root(const struct SiteConfig* site_config,
                                                         struct Str path_to_root,
                                                         struct Arena* arena) {
  struct TemplateValue* site = template_value_map(arena);
  template_value_set_text(site, S("title"), site_config->title, arena);
  template_value_set_text(site, S("description"), site_config->description, arena);
  template_value_set_text(site, S("author"), site_config->author, arena);
  template_value_set_text(site, S("base_url"), site_config->base_url, arena);
  struct TemplateValue* root = template_value_map(arena);
  template_value_set(root, S("site"), site, arena);
  template_value_set_text(root, S("generator"), surge_generator_string(), arena);
  template_value_set_text(root, S("path_to_root"), path_to_root, arena);
  return root;
}

struct TemplateValue* page_context_repo(const struct SiteConfig* site_config,
                                        const struct RepoModel* repo,
                                        struct Str path_to_root,
                                        struct Arena* arena) {
  const struct Str name = repo->config->name;
  struct TemplateValue* map = template_value_map(arena);
  template_value_set_text(map, S("name"), name, arena);
  template_value_set_text(map, S("description"), repo->description, arena);
  template_value_set_text(map, S("default_branch"), repo->default_branch, arena);
  template_value_set_text(
      map, S("url"), page_context_repo_link(path_to_root, name, PAGE_TREE_HOME_PAGE, arena), arena);
  template_value_set_text(map, S("clone_url"), page_context_clone_url(site_config, name, arena),
                          arena);
  template_value_set_text(
      map, S("branches_url"),
      page_context_repo_link(path_to_root, name, PAGE_REFS_BRANCHES_PAGE, arena), arena);
  template_value_set_text(
      map, S("branches_json_url"),
      page_context_repo_link(path_to_root, name, PAGE_REFS_BRANCHES_JSON_PAGE, arena), arena);
  template_value_set_text(map, S("tags_url"),
                          page_context_repo_link(path_to_root, name, PAGE_REFS_TAGS_PAGE, arena),
                          arena);
  template_value_set_text(map, S("tags_feed_url"),
                          page_context_repo_link(path_to_root, name, PAGE_FEED_TAGS_PAGE, arena),
                          arena);
  const struct Str branch = logged_default_branch(repo);
  template_value_set_text(map, S("commits_url"),
                          branch.len > 0 ? page_path_link(path_to_root, PAGE_PATH_COMMITS, name,
                                                          branch, (struct Str){0}, arena)
                                         : (struct Str){0},
                          arena);
  const bool has_releases = repo->release_project_count > 0;
  template_value_set_bool(map, S("has_releases"), has_releases, arena);
  template_value_set_text(
      map, S("releases_url"),
      has_releases ? str_concat(path_to_root, page_release_index_path(name, arena), arena)
                   : (struct Str){0},
      arena);
  template_value_set_text(
      map, S("releases_feed_url"),
      has_releases ? page_context_repo_link(path_to_root, name, PAGE_FEED_RELEASES_PAGE, arena)
                   : (struct Str){0},
      arena);
  return map;
}

struct Str page_context_repo_text(const struct RepoModel* repo, struct Arena* arena) {
  // A branch name holds no `NUL`, so each ends at the `NUL` after it, and the description, which
  // may hold any byte, comes last.
  struct Buf text = {.arena = arena};
  buf_append(&text, logged_default_branch(repo));
  buf_append_char(&text, '\0');
  buf_append_char(&text, repo->release_project_count > 0 ? '1' : '0');
  buf_append(&text, repo->default_branch);
  buf_append_char(&text, '\0');
  buf_append(&text, repo->description);
  return buf_str(&text);
}

struct TemplateValue* page_context_branch(const struct RepoModel* repo,
                                          const struct BranchTree* tree,
                                          struct Str path_to_root,
                                          struct Arena* arena) {
  const struct Str repo_name = repo->config->name;
  const struct Str branch = tree->branch->short_name;
  const struct Str tree_url =
      page_path_link(path_to_root, PAGE_PATH_TREE, repo_name, branch, (struct Str){0}, arena);
  const struct Str commits_url =
      page_path_link(path_to_root, PAGE_PATH_COMMITS, repo_name, branch, (struct Str){0}, arena);
  struct Buf commits_feed_url = {.arena = arena};
  page_path_append_dir_link(&commits_feed_url, path_to_root, PAGE_PATH_COMMITS, repo_name, branch);
  buf_append(&commits_feed_url, S(".atom"));
  return branch_map(branch, tree_url, commits_url, buf_str(&commits_feed_url), arena);
}

struct TemplateValue* page_context_unborn_branch(struct Str branch_name, struct Arena* arena) {
  return branch_map(branch_name, (struct Str){0}, (struct Str){0}, (struct Str){0}, arena);
}

struct Str page_context_clone_url(const struct SiteConfig* site_config,
                                  struct Str repo_name,
                                  struct Arena* arena) {
  struct Buf url = {.arena = arena};
  buf_printf(&url, "%.*s/%.*s.git", STR_FMT(site_config->base_url), STR_FMT(repo_name));
  return buf_str(&url);
}

struct Str page_context_repo_link(struct Str path_to_root,
                                  struct Str repo_name,
                                  struct Str page,
                                  struct Arena* arena) {
  struct Buf link = {.arena = arena};
  buf_printf(&link, "%.*s%.*s/%.*s", STR_FMT(path_to_root), STR_FMT(repo_name), STR_FMT(page));
  return buf_str(&link);
}

static struct Str logged_default_branch(const struct RepoModel* repo) {
  if (repo->default_ref == NULL) {
    return (struct Str){0};
  }
  // A branch has a log exactly when it names a commit, whose summary its slot then holds.
  const struct CommitSummary* commit = &repo->ref_commits[repo->default_ref - repo->refset.refs];
  return commit->oid.len > 0 ? repo->default_branch : (struct Str){0};
}

static struct TemplateValue* branch_map(struct Str branch_name,
                                        struct Str tree_url,
                                        struct Str commits_url,
                                        struct Str commits_feed_url,
                                        struct Arena* arena) {
  struct TemplateValue* map = template_value_map(arena);
  template_value_set_text(map, S("name"), branch_name, arena);
  template_value_set_text(map, S("tree_url"), tree_url, arena);
  template_value_set_text(map, S("commits_url"), commits_url, arena);
  template_value_set_text(map, S("commits_feed_url"), commits_feed_url, arena);
  return map;
}
