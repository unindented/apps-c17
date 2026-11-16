#include "build/page_tree.h"

#include <stdbool.h>
#include <stddef.h>

#include "build/page_context.h"
#include "build/page_paths.h"
#include "build/page_refs.h"
#include "build/submodule_link.h"
#include "build/template_value.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "domain/site_config.h"
#include "domain/tree_path.h"
#include "vcs/filetree.h"

/** Name of each entry kind as a template sees it, indexed by `enum TreeEntryKind`. */
static const struct Str TREE_ENTRY_KIND_NAMES[] = {
    [TREE_ENTRY_DIR] = STR_INIT("dir"),
    [TREE_ENTRY_FILE] = STR_INIT("file"),
    [TREE_ENTRY_EXECUTABLE] = STR_INIT("executable"),
    [TREE_ENTRY_SYMLINK] = STR_INIT("symlink"),
    [TREE_ENTRY_GITLINK] = STR_INIT("gitlink"),
};

/** The `is_<kind>` field of each document kind, with `_` for `-`, indexed by
    `enum DocumentKind`. */
static const struct Str PAGE_TREE_DOCUMENT_FIELDS[] = {
    [DOCUMENT_README] = STR_INIT("is_readme"),
    [DOCUMENT_LICENSE] = STR_INIT("is_license"),
    [DOCUMENT_CODE_OF_CONDUCT] = STR_INIT("is_code_of_conduct"),
    [DOCUMENT_CONTRIBUTING] = STR_INIT("is_contributing"),
    [DOCUMENT_SECURITY] = STR_INIT("is_security"),
    [DOCUMENT_ACCESSIBILITY] = STR_INIT("is_accessibility"),
};

_Static_assert(sizeof(PAGE_TREE_DOCUMENT_FIELDS) / sizeof(PAGE_TREE_DOCUMENT_FIELDS[0]) ==
                   DOCUMENT_KIND_COUNT,
               "every document kind has a field");

/** The `is_<kind>` field of each entry kind, indexed by `enum TreeEntryKind`. */
static const struct Str TREE_ENTRY_KIND_FIELDS[] = {
    [TREE_ENTRY_DIR] = STR_INIT("is_dir"),
    [TREE_ENTRY_FILE] = STR_INIT("is_file"),
    [TREE_ENTRY_EXECUTABLE] = STR_INIT("is_executable"),
    [TREE_ENTRY_SYMLINK] = STR_INIT("is_symlink"),
    [TREE_ENTRY_GITLINK] = STR_INIT("is_gitlink"),
};

/** Number of entry kinds. */
enum { TREE_ENTRY_KIND_COUNT = TREE_ENTRY_GITLINK + 1 };

/**
 * @brief Returns the `documents` list of a tree page.
 *
 * @param repo           What the scan found for the repo. Must not be `NULL`.
 * @param tree           Tree of the branch. Must not be `NULL`.
 * @param documents      The documents found. Must not be `NULL` unless `document_count` is `0`.
 * @param document_count Number of entries in `documents`.
 * @param path_to_root   Prefix from the page back to the output root.
 * @param arena          Arena that owns the list. Must not be `NULL`.
 * @return The list.
 */
static struct TemplateValue* page_tree_value_documents(const struct RepoModel* repo,
                                                       const struct BranchTree* tree,
                                                       const struct TreeDocument* documents,
                                                       ptrdiff_t document_count,
                                                       struct Str path_to_root,
                                                       struct Arena* arena)
    __attribute__((nonnull(1, 2, 6), returns_nonnull));

/**
 * @brief Returns the map of one entry of a listing.
 *
 * @param site_config  Configuration of the build. Must not be `NULL`.
 * @param repo         What the scan found for the repo. Must not be `NULL`.
 * @param tree         Tree of the branch. Must not be `NULL`.
 * @param entry        Entry to show. Must not be `NULL`.
 * @param path_to_root Prefix from the page back to the output root.
 * @param arena        Arena that owns the map. Must not be `NULL`.
 * @return The map.
 */
static struct TemplateValue* page_tree_entries_one(const struct SiteConfig* site_config,
                                                   const struct RepoModel* repo,
                                                   const struct BranchTree* tree,
                                                   const struct TreeEntry* entry,
                                                   struct Str path_to_root,
                                                   struct Arena* arena)
    __attribute__((nonnull(1, 2, 3, 4, 6), returns_nonnull));

/**
 * @brief Returns the link of a symlink's row to its target: the blob page of a file, the tree page
 *        of a directory, or for a symlink the tree page of the directory that holds it.
 *
 * The target is a raw path relative to the symlink's directory, with no `%XX` escape, query or
 * fragment, and no `.md` added.
 *
 * @param repo         What the scan found for the repo. Must not be `NULL`.
 * @param tree         Tree of the branch. Must not be `NULL`.
 * @param entry        Entry of the row. Must not be `NULL`.
 * @param path_to_root Prefix from the page back to the output root.
 * @param arena        Arena that owns the link. Must not be `NULL`.
 * @return The link, or empty for an entry that is no symlink, and for a target that starts with
 *         `/`, was cut short, leaves the root, or names no such path of the branch.
 */
static struct Str page_tree_entries_one_link_url(const struct RepoModel* repo,
                                                 const struct BranchTree* tree,
                                                 const struct TreeEntry* entry,
                                                 struct Str path_to_root,
                                                 struct Arena* arena)
    __attribute__((nonnull(1, 2, 3, 5)));

/**
 * @brief Returns the map of one breadcrumb.
 *
 * @param name  Name of the repo or of the path's segment.
 * @param url   Link to the crumb's tree page, empty for the last crumb.
 * @param arena Arena that owns the map. Must not be `NULL`.
 * @return The map.
 */
static struct TemplateValue* page_tree_breadcrumbs_one(struct Str name,
                                                       struct Str url,
                                                       struct Arena* arena)
    __attribute__((nonnull(3), returns_nonnull));

struct TemplateValue* page_tree_value(const struct SiteConfig* site_config,
                                      const struct RepoModel* repo,
                                      const struct BranchTree* tree,
                                      const struct TreeDir* dir,
                                      bool is_home,
                                      const struct TreeDocument* documents,
                                      ptrdiff_t document_count,
                                      struct Arena* arena) {
  const struct Str repo_name = repo->config->name;
  // Only the home of an empty repo has no tree, and so no directory.
  const bool has_tree = tree != NULL && dir != NULL;
  const struct Str page =
      has_tree && !is_home
          ? page_path(PAGE_PATH_TREE, repo_name, tree->branch->short_name, dir->path, arena)
          : page_tree_home_path(repo_name, arena);
  struct TemplateValue* value = page_context_new(site_config, page, arena);
  const struct Str path_to_root = template_value_find(value, S("path_to_root"))->text;
  template_value_set(value, S("repo"), page_context_repo(site_config, repo, path_to_root, arena),
                     arena);
  template_value_set_bool(value, S("is_home"), is_home, arena);
  template_value_set_bool(value, S("is_empty"), !has_tree, arena);
  const bool is_root = !has_tree || dir->path.len == 0;
  // The root's page shows the branch's newest commit, as a repo home does.
  struct CommitSummary commit = {0};
  if (has_tree && is_root) {
    commit = repo->ref_commits[tree->branch - repo->refset.refs];
  }
  template_value_set(value, S("commit"), page_refs_commit(repo, commit, path_to_root, arena),
                     arena);
  template_value_set_text(value, S("path"), has_tree ? dir->path : S(""), arena);
  template_value_set_bool(value, S("is_root"), is_root, arena);
  if (has_tree) {
    template_value_set(value, S("ref"), page_context_branch(repo, tree, path_to_root, arena),
                       arena);
    template_value_set(value, S("breadcrumbs"),
                       page_tree_breadcrumbs(repo, tree, dir->path, path_to_root, arena), arena);
    template_value_set(value, S("tree_entries"),
                       page_tree_entries(site_config, repo, tree, dir, path_to_root, arena), arena);
    template_value_set(
        value, S("documents"),
        page_tree_value_documents(repo, tree, documents, document_count, path_to_root, arena),
        arena);
  } else {
    template_value_set(value, S("ref"), page_context_unborn_branch(repo->default_branch, arena),
                       arena);
    template_value_set(value, S("breadcrumbs"), template_value_list(0, arena), arena);
    template_value_set(value, S("tree_entries"), template_value_list(0, arena), arena);
    template_value_set(value, S("documents"), template_value_list(0, arena), arena);
  }
  return value;
}

struct Str page_tree_home_path(struct Str repo_name, struct Arena* arena) {
  return str_concat(str_concat(repo_name, S("/"), arena), PAGE_TREE_HOME_PAGE, arena);
}

struct TemplateValue* page_tree_entries(const struct SiteConfig* site_config,
                                        const struct RepoModel* repo,
                                        const struct BranchTree* tree,
                                        const struct TreeDir* dir,
                                        struct Str path_to_root,
                                        struct Arena* arena) {
  struct TemplateValue* list = template_value_list(dir->entry_count, arena);
  for (ptrdiff_t i = 0; i < dir->entry_count; i++) {
    list->items[i] =
        page_tree_entries_one(site_config, repo, tree, &dir->entries[i], path_to_root, arena);
  }
  return list;
}

struct TemplateValue* page_tree_breadcrumbs(const struct RepoModel* repo,
                                            const struct BranchTree* tree,
                                            struct Str path,
                                            struct Str path_to_root,
                                            struct Arena* arena) {
  const struct Str repo_name = repo->config->name;
  const struct Str branch = tree->branch->short_name;
  // One crumb for the repo, and one for each segment of the path.
  ptrdiff_t count = 1;
  if (path.len > 0) {
    count++;
    for (ptrdiff_t i = 0; i < path.len; i++) {
      count += path.data[i] == '/';
    }
  }
  struct TemplateValue* list = template_value_list(count, arena);
  const struct Str repo_url = count > 1 ? page_path_link(path_to_root, PAGE_PATH_TREE, repo_name,
                                                         branch, (struct Str){0}, arena)
                                        : (struct Str){0};
  list->items[0] = page_tree_breadcrumbs_one(repo_name, repo_url, arena);
  ptrdiff_t index = 1;
  ptrdiff_t start = 0;
  for (ptrdiff_t i = 0; path.len > 0 && i <= path.len; i++) {
    if (i < path.len && path.data[i] != '/') {
      continue;
    }
    const struct Str name = str_head(str_tail(path, start), i - start);
    const struct Str url = i < path.len ? page_path_link(path_to_root, PAGE_PATH_TREE, repo_name,
                                                         branch, str_head(path, i), arena)
                                        : (struct Str){0};
    list->items[index++] = page_tree_breadcrumbs_one(name, url, arena);
    start = i + 1;
  }
  return list;
}

static struct TemplateValue* page_tree_value_documents(const struct RepoModel* repo,
                                                       const struct BranchTree* tree,
                                                       const struct TreeDocument* documents,
                                                       ptrdiff_t document_count,
                                                       struct Str path_to_root,
                                                       struct Arena* arena) {
  struct TemplateValue* list = template_value_list(document_count, arena);
  for (ptrdiff_t i = 0; i < document_count; i++) {
    const struct TreeDocument* document = &documents[i];
    struct TemplateValue* map = template_value_map(arena);
    template_value_set_text(map, S("kind"), document_kind_name(document->kind), arena);
    // A template names each kind as it likes, so it gets one flag per kind, not a title.
    for (ptrdiff_t kind = 0; kind < DOCUMENT_KIND_COUNT; kind++) {
      template_value_set_bool(map, PAGE_TREE_DOCUMENT_FIELDS[kind], kind == document->kind, arena);
    }
    // The README sorts first, so the main document is it, or else the first that the page has.
    template_value_set_bool(map, S("is_main"), i == 0, arena);
    template_value_set_text(map, S("path"), document->entry->path, arena);
    template_value_set_text(map, S("url"),
                            page_path_link(path_to_root, PAGE_PATH_BLOB, repo->config->name,
                                           tree->branch->short_name, document->entry->path, arena),
                            arena);
    template_value_set_text(map, S("rendered_html"), document->rendered_html, arena);
    list->items[i] = map;
  }
  return list;
}

static struct TemplateValue* page_tree_entries_one(const struct SiteConfig* site_config,
                                                   const struct RepoModel* repo,
                                                   const struct BranchTree* tree,
                                                   const struct TreeEntry* entry,
                                                   struct Str path_to_root,
                                                   struct Arena* arena) {
  const struct Str repo_name = repo->config->name;
  const struct Str branch = tree->branch->short_name;
  struct TemplateValue* map = template_value_map(arena);
  template_value_set_text(map, S("name"), entry->name, arena);
  template_value_set_text(map, S("kind"), TREE_ENTRY_KIND_NAMES[entry->kind], arena);
  for (ptrdiff_t i = 0; i < TREE_ENTRY_KIND_COUNT; i++) {
    template_value_set_bool(map, TREE_ENTRY_KIND_FIELDS[i], (ptrdiff_t)entry->kind == i, arena);
  }
  struct Str url = {0};
  if (entry->is_linked && entry->kind == TREE_ENTRY_DIR) {
    url = page_path_link(path_to_root, PAGE_PATH_TREE, repo_name, branch, entry->path, arena);
  } else if (filetree_entry_has_pages(entry)) {
    url = page_path_link(path_to_root, PAGE_PATH_BLOB, repo_name, branch, entry->path, arena);
  }
  template_value_set_text(map, S("url"), url, arena);
  template_value_set_bool(map, S("is_linked"), url.len > 0, arena);
  template_value_set_text(map, S("link_target"), entry->link_target, arena);
  template_value_set_text(map, S("link_url"),
                          page_tree_entries_one_link_url(repo, tree, entry, path_to_root, arena),
                          arena);
  template_value_set_text(map, S("submodule_url"),
                          submodule_link_shown_url(entry->submodule_url, arena), arena);
  template_value_set_text(
      map, S("submodule_commit_url"),
      entry->submodule_url.len > 0
          ? submodule_link_commit_url(entry->submodule_url, entry->oid, site_config, repo_name,
                                      path_to_root, arena)
          : (struct Str){0},
      arena);
  template_value_set_text(map, S("oid"), entry->oid, arena);
  struct Buf size = {.arena = arena};
  if (entry->kind != TREE_ENTRY_DIR && entry->kind != TREE_ENTRY_GITLINK) {
    buf_printf(&size, "%td", entry->len);
  }
  template_value_set_text(map, S("size"), buf_str(&size), arena);
  return map;
}

static struct Str page_tree_entries_one_link_url(const struct RepoModel* repo,
                                                 const struct BranchTree* tree,
                                                 const struct TreeEntry* entry,
                                                 struct Str path_to_root,
                                                 struct Arena* arena) {
  const struct Str target = entry->link_target;
  // A target shorter than its blob was cut, so the path it names is not the symlink's.
  if (entry->kind != TREE_ENTRY_SYMLINK || target.len == 0 || target.len < entry->len ||
      str_startswith(target, S("/"))) {
    return (struct Str){0};
  }
  struct Str path = {0};
  if (!tree_path_resolve(tree_path_dir(entry->path), target, arena, &path)) {
    return (struct Str){0};
  }
  enum PagePathKind kind = PAGE_PATH_TREE;
  if (path.len > 0 && tree_path_find_file(tree, path) != NULL) {
    kind = PAGE_PATH_BLOB;
  } else if (tree_path_has_dir(tree, path)) {
    kind = PAGE_PATH_TREE;
  } else if (tree_path_find_symlink(tree, path) != NULL) {
    // A symlink has no page, so the link goes one hop, to the listing that shows its row.
    path = tree_path_dir(path);
  } else {
    return (struct Str){0};
  }
  return page_path_link(path_to_root, kind, repo->config->name, tree->branch->short_name, path,
                        arena);
}

static struct TemplateValue* page_tree_breadcrumbs_one(struct Str name,
                                                       struct Str url,
                                                       struct Arena* arena) {
  struct TemplateValue* crumb = template_value_map(arena);
  template_value_set_text(crumb, S("name"), name, arena);
  template_value_set_text(crumb, S("url"), url, arena);
  template_value_set_bool(crumb, S("is_last"), url.len == 0, arena);
  return crumb;
}
