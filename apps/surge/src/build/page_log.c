#include "build/page_log.h"

#include <stdbool.h>
#include <stddef.h>

#include "build/page_context.h"
#include "build/page_paths.h"
#include "build/page_refs.h"
#include "build/template_value.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "domain/site_config.h"

/**
 * @brief Returns the `page` map of one log page: its number, the page count, and the links to the
 *        first, previous, next and last pages.
 *
 * @param path_to_root Prefix from the page back to the output root.
 * @param repo_name    Name of the repo.
 * @param branch       Name of the branch.
 * @param page_index   Index of the page.
 * @param page_count   Number of the branch's log pages.
 * @param arena        Arena that owns the map. Must not be `NULL`.
 * @return The map.
 */
static struct TemplateValue* page_log_value_page(struct Str path_to_root,
                                                 struct Str repo_name,
                                                 struct Str branch,
                                                 ptrdiff_t page_index,
                                                 ptrdiff_t page_count,
                                                 struct Arena* arena)
    __attribute__((nonnull(6), returns_nonnull));

/**
 * @brief Returns the link to one log page from a page whose way back to the output root is
 *        `path_to_root`, or empty for an index outside the pages.
 *
 * @param path_to_root Prefix from the linking page back to the output root.
 * @param repo_name    Name of the repo.
 * @param ref_name     Name of the branch.
 * @param page_index   Index of the page.
 * @param page_count   Number of the branch's log pages.
 * @param arena        Arena that owns the link. Must not be `NULL`.
 * @return The link.
 */
static struct Str page_log_value_link(struct Str path_to_root,
                                      struct Str repo_name,
                                      struct Str ref_name,
                                      ptrdiff_t page_index,
                                      ptrdiff_t page_count,
                                      struct Arena* arena) __attribute__((nonnull(6)));

/**
 * @brief Returns the list of the refs that name a commit.
 *
 * @param repo         What the scan found for the repo. Must not be `NULL`.
 * @param commit_oid   Hex id of the commit.
 * @param path_to_root Prefix from the page back to the output root.
 * @param arena        Arena that owns the list. Must not be `NULL`.
 * @return The list.
 */
static struct TemplateValue* page_log_value_refs(const struct RepoModel* repo,
                                                 struct Str commit_oid,
                                                 struct Str path_to_root,
                                                 struct Arena* arena)
    __attribute__((nonnull(1, 4), returns_nonnull));

ptrdiff_t page_log_count(ptrdiff_t commit_count, ptrdiff_t page_size) {
  return commit_count == 0 ? 1 : commit_count / page_size + (commit_count % page_size != 0);
}

struct Str page_log_path(struct Str repo_name,
                         struct Str ref_name,
                         ptrdiff_t page_index,
                         struct Arena* arena) {
  if (page_index == 0) {
    return page_path(PAGE_PATH_COMMITS, repo_name, ref_name, (struct Str){0}, arena);
  }
  struct Buf path = {.arena = arena};
  buf_append(&path, page_path_dir(PAGE_PATH_COMMITS, repo_name, ref_name, arena));
  buf_printf(&path, "/page-%td.html", page_index + 1);
  return buf_str(&path);
}

struct TemplateValue* page_log_value(const struct SiteConfig* site_config,
                                     const struct RepoModel* repo,
                                     const struct BranchTree* tree,
                                     ptrdiff_t page_index,
                                     ptrdiff_t page_count,
                                     const struct CommitSummary* commits,
                                     ptrdiff_t commit_count,
                                     struct Arena* arena) {
  const struct Str repo_name = repo->config->name;
  const struct Str branch = tree->branch->short_name;
  struct TemplateValue* value =
      page_context_new(site_config, page_log_path(repo_name, branch, page_index, arena), arena);
  const struct Str path_to_root = template_value_find(value, S("path_to_root"))->text;
  template_value_set(value, S("repo"), page_context_repo(site_config, repo, path_to_root, arena),
                     arena);
  template_value_set(value, S("ref"), page_context_branch(repo, tree, path_to_root, arena), arena);
  template_value_set(
      value, S("page"),
      page_log_value_page(path_to_root, repo_name, branch, page_index, page_count, arena), arena);
  struct TemplateValue* list = template_value_list(commit_count, arena);
  for (ptrdiff_t i = 0; i < commit_count; i++) {
    struct TemplateValue* entry = page_refs_commit(repo, commits[i], path_to_root, arena);
    template_value_set(entry, S("refs"),
                       page_log_value_refs(repo, commits[i].oid, path_to_root, arena), arena);
    list->items[i] = entry;
  }
  template_value_set(value, S("commits"), list, arena);
  return value;
}

void page_log_append_refs(struct Buf* buffer, const struct RepoModel* repo, struct Str commit_oid) {
  for (ptrdiff_t i = 0; i < repo->refset.count; i++) {
    const struct Ref* ref = &repo->refset.refs[i];
    if (str_eq(ref->commit_oid, commit_oid)) {
      buf_printf(buffer, "%s %.*s\n", ref->kind == REF_KIND_BRANCH ? "branch" : "tag",
                 STR_FMT(ref->short_name));
    }
  }
}

static struct TemplateValue* page_log_value_page(struct Str path_to_root,
                                                 struct Str repo_name,
                                                 struct Str branch,
                                                 ptrdiff_t page_index,
                                                 ptrdiff_t page_count,
                                                 struct Arena* arena) {
  struct TemplateValue* page = template_value_map(arena);
  struct Buf number = {.arena = arena};
  buf_printf(&number, "%td", page_index + 1);
  template_value_set_text(page, S("number"), buf_str(&number), arena);
  struct Buf count = {.arena = arena};
  buf_printf(&count, "%td", page_count);
  template_value_set_text(page, S("count"), buf_str(&count), arena);
  template_value_set_bool(page, S("is_first"), page_index == 0, arena);
  template_value_set_bool(page, S("is_last"), page_index == page_count - 1, arena);
  const ptrdiff_t targets[] = {0, page_index - 1, page_index + 1, page_count - 1};
  const struct Str names[] = {S("first_url"), S("previous_url"), S("next_url"), S("last_url")};
  for (ptrdiff_t i = 0; i < 4; i++) {
    // The first and last links of the page itself, like its previous and next past the ends, are
    // empty.
    const ptrdiff_t target = targets[i] == page_index ? -1 : targets[i];
    template_value_set_text(
        page, names[i],
        page_log_value_link(path_to_root, repo_name, branch, target, page_count, arena), arena);
  }
  return page;
}

static struct Str page_log_value_link(struct Str path_to_root,
                                      struct Str repo_name,
                                      struct Str ref_name,
                                      ptrdiff_t page_index,
                                      ptrdiff_t page_count,
                                      struct Arena* arena) {
  if (page_index < 0 || page_index >= page_count) {
    return (struct Str){0};
  }
  if (page_index == 0) {
    return page_path_link(path_to_root, PAGE_PATH_COMMITS, repo_name, ref_name, (struct Str){0},
                          arena);
  }
  struct Buf link = {.arena = arena};
  page_path_append_dir_link(&link, path_to_root, PAGE_PATH_COMMITS, repo_name, ref_name);
  buf_printf(&link, "/page-%td.html", page_index + 1);
  return buf_str(&link);
}

static struct TemplateValue* page_log_value_refs(const struct RepoModel* repo,
                                                 struct Str commit_oid,
                                                 struct Str path_to_root,
                                                 struct Arena* arena) {
  ptrdiff_t count = 0;
  for (ptrdiff_t i = 0; i < repo->refset.count; i++) {
    count += str_eq(repo->refset.refs[i].commit_oid, commit_oid);
  }
  struct TemplateValue* list = template_value_list(count, arena);
  ptrdiff_t index = 0;
  for (ptrdiff_t i = 0; i < repo->refset.count; i++) {
    const struct Ref* ref = &repo->refset.refs[i];
    if (!str_eq(ref->commit_oid, commit_oid)) {
      continue;
    }
    const bool is_branch = ref->kind == REF_KIND_BRANCH;
    struct TemplateValue* map = template_value_map(arena);
    template_value_set_text(map, S("name"), ref->short_name, arena);
    template_value_set_text(map, S("kind"), is_branch ? S("branch") : S("tag"), arena);
    template_value_set_bool(map, S("is_branch"), is_branch, arena);
    template_value_set_bool(map, S("is_tag"), !is_branch, arena);
    template_value_set_text(
        map, S("url"),
        is_branch
            ? page_path_link(path_to_root, PAGE_PATH_TREE, repo->config->name, ref->short_name,
                             (struct Str){0}, arena)
            : page_context_repo_link(path_to_root, repo->config->name, PAGE_REFS_TAGS_PAGE, arena),
        arena);
    list->items[index++] = map;
  }
  return list;
}
