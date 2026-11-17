#include "build/page_refs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "build/page_commit.h"
#include "build/page_context.h"
#include "build/page_paths.h"
#include "build/page_release.h"
#include "build/template_value.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "domain/site_config.h"
#include "formats/json.h"

/** Hex digits of a commit's short id, as git abbreviates one by default. */
enum { PAGE_REFS_SHORT_OID_LEN = 7 };

/**
 * @brief Returns the data tree of a refs page, before its list.
 *
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param repo        What the scan found for the repo. Must not be `NULL`.
 * @param page        Output path of the page below the repo's directory.
 * @param arena       Arena that owns the tree. Must not be `NULL`.
 * @return The data tree, with `site`, `generator`, `path_to_root` and `repo`.
 */
static struct TemplateValue* page_refs_value_new(const struct SiteConfig* site_config,
                                                 const struct RepoModel* repo,
                                                 struct Str page,
                                                 struct Arena* arena)
    __attribute__((nonnull(1, 2, 4), returns_nonnull));

/**
 * @brief Orders items newest first, then by name in byte order. A `qsort` comparator.
 *
 * @param a A `const struct PageRefsDated*`. Must not be `NULL`.
 * @param b A `const struct PageRefsDated*`. Must not be `NULL`.
 * @return Negative, zero or positive as `a` sorts before, with, or after `b`.
 */
static int page_refs_sort_newest_compare(const void* a, const void* b)
    __attribute__((nonnull(1, 2)));

struct TemplateValue* page_refs_branches_value(const struct SiteConfig* site_config,
                                               const struct RepoModel* repo,
                                               struct Arena* arena) {
  struct TemplateValue* value =
      page_refs_value_new(site_config, repo, PAGE_REFS_BRANCHES_PAGE, arena);
  const struct Str path_to_root = template_value_find(value, S("path_to_root"))->text;
  const struct Str repo_name = repo->config->name;
  ptrdiff_t count = 0;
  for (ptrdiff_t i = 0; i < repo->refset.count; i++) {
    count += repo->refset.refs[i].kind == REF_KIND_BRANCH;
  }
  struct TemplateValue* list = template_value_list(count, arena);
  ptrdiff_t index = 0;
  for (ptrdiff_t i = 0; i < repo->refset.count; i++) {
    const struct Ref* ref = &repo->refset.refs[i];
    if (ref->kind != REF_KIND_BRANCH) {
      continue;
    }
    struct TemplateValue* map = template_value_map(arena);
    template_value_set_text(map, S("name"), ref->short_name, arena);
    template_value_set_bool(map, S("is_default"), ref == repo->default_ref, arena);
    template_value_set_text(map, S("author"), repo->ref_commits[i].author, arena);
    template_value_set_text(map, S("date"), repo->ref_commits[i].date, arena);
    // A branch that names no commit has no pages to link.
    const bool has_pages = ref->commit_oid.len > 0;
    template_value_set_text(map, S("tree_url"),
                            has_pages ? page_path_link(path_to_root, PAGE_PATH_TREE, repo_name,
                                                       ref->short_name, (struct Str){0}, arena)
                                      : (struct Str){0},
                            arena);
    template_value_set_text(map, S("commits_url"),
                            has_pages ? page_path_link(path_to_root, PAGE_PATH_COMMITS, repo_name,
                                                       ref->short_name, (struct Str){0}, arena)
                                      : (struct Str){0},
                            arena);
    template_value_set(map, S("commit"),
                       page_refs_commit(repo, repo->ref_commits[i], path_to_root, arena), arena);
    list->items[index++] = map;
  }
  template_value_set(value, S("branches"), list, arena);
  return value;
}

struct Str page_refs_branches_json(const struct RepoModel* repo,
                                   struct Arena* perm,
                                   struct Arena scratch) {
  const struct Str repo_name = repo->config->name;
  struct Buf text = {.arena = perm};
  buf_append_char(&text, '[');
  bool is_first = true;
  for (ptrdiff_t i = 0; i < repo->refset.count; i++) {
    const struct Ref* ref = &repo->refset.refs[i];
    if (ref->kind != REF_KIND_BRANCH) {
      continue;
    }
    // The link from the output root starts with `<r>/`, which a repo's unreserved name keeps as it
    // is.
    struct Arena link_scratch = scratch;
    const struct Str tree_url =
        ref->commit_oid.len > 0
            ? str_tail(page_path_link((struct Str){0}, PAGE_PATH_TREE, repo_name, ref->short_name,
                                      (struct Str){0}, &link_scratch),
                       repo_name.len + 1)
            : (struct Str){0};
    buf_append(&text, is_first ? S("{\"name\":") : S(",{\"name\":"));
    json_string_append(&text, ref->short_name, link_scratch);
    buf_printf(&text,
               ",\"is_default\":%s,\"tree_url\":", ref == repo->default_ref ? "true" : "false");
    json_string_append(&text, tree_url, link_scratch);
    buf_append_char(&text, '}');
    is_first = false;
  }
  buf_append(&text, S("]\n"));
  return buf_str(&text);
}

struct TemplateValue* page_refs_tags_value(const struct SiteConfig* site_config,
                                           const struct RepoModel* repo,
                                           struct Arena* arena) {
  struct TemplateValue* value = page_refs_value_new(site_config, repo, PAGE_REFS_TAGS_PAGE, arena);
  const struct Str path_to_root = template_value_find(value, S("path_to_root"))->text;
  ptrdiff_t count = 0;
  const struct PageRefsDated* tags = page_refs_tags_newest(repo, arena, &count);
  struct TemplateValue* list = template_value_list(count, arena);
  for (ptrdiff_t index = 0; index < count; index++) {
    const ptrdiff_t i = tags[index].index;
    const struct Ref* ref = &repo->refset.refs[i];
    struct TemplateValue* map = template_value_map(arena);
    template_value_set_text(map, S("name"), ref->short_name, arena);
    template_value_set_bool(map, S("is_annotated"), ref->peeled_oid.len > 0, arena);
    template_value_set_text(map, S("author"), page_refs_tag_author(repo, i), arena);
    template_value_set_text(map, S("date"), repo->ref_notes[i].date, arena);
    template_value_set_text(map, S("signature"), repo->ref_notes[i].signature, arena);
    template_value_set_text(map, S("signature_kind"),
                            page_refs_signature_kind(repo->ref_notes[i].signature), arena);
    // A tag with a release links its page.
    const bool has_release = page_release_find(repo, ref) != NULL;
    template_value_set_text(map, S("release_url"),
                            has_release ? page_release_tag_link(path_to_root, repo->config->name,
                                                                ref->short_name, arena)
                                        : (struct Str){0},
                            arena);
    template_value_set(map, S("commit"),
                       page_refs_commit(repo, repo->ref_commits[i], path_to_root, arena), arena);
    list->items[index] = map;
  }
  template_value_set(value, S("tags"), list, arena);
  return value;
}

struct TemplateValue* page_refs_commit(const struct RepoModel* repo,
                                       struct CommitSummary commit,
                                       struct Str path_to_root,
                                       struct Arena* arena) {
  if (commit.oid.len == 0) {
    return template_value_bool(false, arena);
  }
  struct TemplateValue* map = template_value_map(arena);
  template_value_set_text(map, S("oid"), commit.oid, arena);
  const ptrdiff_t short_len =
      commit.oid.len < PAGE_REFS_SHORT_OID_LEN ? commit.oid.len : PAGE_REFS_SHORT_OID_LEN;
  template_value_set_text(map, S("short_oid"), str_head(commit.oid, short_len), arena);
  template_value_set_text(map, S("subject"), commit.subject, arena);
  template_value_set_text(map, S("author"), commit.author, arena);
  template_value_set_text(map, S("date"), commit.date, arena);
  template_value_set_text(map, S("signature"), commit.signature, arena);
  template_value_set_text(map, S("signature_kind"), page_refs_signature_kind(commit.signature),
                          arena);
  template_value_set_bool(map, S("is_llm_assisted"), commit.llm_tool_count > 0, arena);
  template_value_set(map, S("llm_tools"),
                     page_refs_llm_tools(commit.llm_tools, commit.llm_tool_count, arena), arena);
  template_value_set_text(
      map, S("url"), page_commit_link(path_to_root, repo->config->name, commit.oid, arena), arena);
  return map;
}

struct TemplateValue* page_refs_llm_tools(const struct Str* tools,
                                          ptrdiff_t tool_count,
                                          struct Arena* arena) {
  struct TemplateValue* list = template_value_list(tool_count, arena);
  for (ptrdiff_t i = 0; i < tool_count; i++) {
    struct TemplateValue* tool = template_value_map(arena);
    template_value_set_text(tool, S("name"), tools[i], arena);
    list->items[i] = tool;
  }
  return list;
}

struct PageRefsDated* page_refs_tags_newest(const struct RepoModel* repo,
                                            struct Arena* arena,
                                            ptrdiff_t* count_out) {
  struct PageRefsDated* tags = arena_new(arena, repo->refset.count, struct PageRefsDated);
  ptrdiff_t count = 0;
  for (ptrdiff_t i = 0; i < repo->refset.count; i++) {
    const struct Ref* ref = &repo->refset.refs[i];
    if (ref->kind == REF_KIND_TAG) {
      tags[count++] = (struct PageRefsDated){
          .index = i, .time = repo->ref_notes[i].time, .name = ref->short_name};
    }
  }
  page_refs_sort_newest(tags, count);
  *count_out = count;
  return tags;
}

void page_refs_sort_newest(struct PageRefsDated* items, ptrdiff_t count) {
  if (count > 1) {
    qsort(items, (size_t)count, sizeof(items[0]), page_refs_sort_newest_compare);
  }
}

struct Str page_refs_tag_author(const struct RepoModel* repo, ptrdiff_t index) {
  const struct Str tagger = repo->ref_notes[index].tagger;
  return tagger.len > 0 ? tagger : repo->ref_commits[index].author;
}

struct Str page_refs_signature_kind(struct Str signature) {
  static const struct {
    struct Str opening;
    struct Str kind;
  } kinds[] = {
      {STR_INIT("-----BEGIN PGP SIGNATURE-----"), STR_INIT("PGP")},
      {STR_INIT("-----BEGIN PGP MESSAGE-----"), STR_INIT("PGP")},
      {STR_INIT("-----BEGIN SIGNED MESSAGE-----"), STR_INIT("X.509")},
      {STR_INIT("-----BEGIN SSH SIGNATURE-----"), STR_INIT("SSH")},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(kinds) / sizeof(kinds[0])); i++) {
    if (str_startswith(signature, kinds[i].opening)) {
      return kinds[i].kind;
    }
  }
  return (struct Str){0};
}

static struct TemplateValue* page_refs_value_new(const struct SiteConfig* site_config,
                                                 const struct RepoModel* repo,
                                                 struct Str page,
                                                 struct Arena* arena) {
  struct Buf path = {.arena = arena};
  buf_printf(&path, "%.*s/%.*s", STR_FMT(repo->config->name), STR_FMT(page));
  struct TemplateValue* value = page_context_new(site_config, buf_str(&path), arena);
  const struct Str path_to_root = template_value_find(value, S("path_to_root"))->text;
  template_value_set(value, S("repo"), page_context_repo(site_config, repo, path_to_root, arena),
                     arena);
  return value;
}

static int page_refs_sort_newest_compare(const void* a, const void* b) {
  const struct PageRefsDated* left = a;
  const struct PageRefsDated* right = b;
  if (left->time != right->time) {
    return left->time > right->time ? -1 : 1;
  }
  return str_compare(left->name, right->name);
}
