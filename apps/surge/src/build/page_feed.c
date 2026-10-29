#include "build/page_feed.h"

#include <stdbool.h>
#include <stddef.h>

#include "build/page_commit.h"
#include "build/page_context.h"
#include "build/page_paths.h"
#include "build/page_refs.h"
#include "build/page_release.h"
#include "build/readme.h"
#include "build/template_value.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "domain/site_config.h"
#include "domain/url_segment.h"
#include "formats/rfc3339.h"
#include "markup/markdown.h"

/** What one entry of a feed shows. */
struct FeedEntry {
  /** The entry's unique id, an absolute URL. */
  struct Str id;

  /** The entry's title. */
  struct Str title;

  /** Absolute URL of the page the entry links. */
  struct Str url;

  /** RFC 3339 date of the entry. */
  struct Str modified;

  /** Name of the entry's author, or empty. */
  struct Str author;

  /** Plain text of the entry, or empty. */
  struct Str content;

  /** HTML of the entry, or empty. */
  struct Str content_html;
};

/**
 * @brief Returns the data tree of a feed: `site`, `generator`, `path_to_root`, `repo` and `feed`.
 *
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param repo        What the scan found for the repo. Must not be `NULL`.
 * @param page        Output path of the feed.
 * @param url         Absolute URL of the feed, which is also its id.
 * @param html_url    Absolute URL of the page the feed follows.
 * @param modified    Date of the newest entry, or empty.
 * @param arena       Arena that owns the tree. Must not be `NULL`.
 * @return The data tree.
 */
static struct TemplateValue* page_feed_value_new(const struct SiteConfig* site_config,
                                                 const struct RepoModel* repo,
                                                 struct Str page,
                                                 struct Str url,
                                                 struct Str html_url,
                                                 struct Str modified,
                                                 struct Arena* arena)
    __attribute__((nonnull(1, 2, 7), returns_nonnull));

/**
 * @brief Returns the map of one entry.
 *
 * @param entry The entry. Must not be `NULL`.
 * @param arena Arena that owns the map. Must not be `NULL`.
 * @return The map.
 */
static struct TemplateValue* entry_map(const struct FeedEntry* entry, struct Arena* arena)
    __attribute__((nonnull(1, 2), returns_nonnull));

/**
 * @brief Returns how many of the newest items a feed keeps: all, up to `feed_count`.
 *
 * @param count      Number of items.
 * @param feed_count Most items a feed keeps.
 * @return The number of items kept.
 */
static ptrdiff_t kept_count(ptrdiff_t count, ptrdiff_t feed_count);

/**
 * @brief Returns the prefix of every absolute link, `base_url` and a `/`.
 *
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param arena       Arena that owns the prefix. Must not be `NULL`.
 * @return The prefix.
 */
static struct Str absolute_root(const struct SiteConfig* site_config, struct Arena* arena)
    __attribute__((nonnull(1, 2)));

struct Str page_feed_commits_path(struct Str repo_name,
                                  struct Str branch_name,
                                  struct Arena* arena) {
  struct Buf path = {.arena = arena};
  buf_printf(&path, "%.*s/commits/%.*s.atom", STR_FMT(repo_name), STR_FMT(branch_name));
  return buf_str(&path);
}

struct TemplateValue* page_feed_commits_value(const struct SiteConfig* site_config,
                                              const struct RepoModel* repo,
                                              const struct BranchTree* tree,
                                              const struct CommitDetail* commits,
                                              ptrdiff_t commit_count,
                                              struct Arena* arena) {
  const struct Str repo_name = repo->config->name;
  const struct Str branch = tree->branch->short_name;
  const struct Str root = absolute_root(site_config, arena);
  struct Buf url = {.arena = arena};
  page_path_append_dir_link(&url, root, PAGE_PATH_COMMITS, repo_name, branch);
  buf_append(&url, S(".atom"));
  const struct Str html_url =
      page_path_link(root, PAGE_PATH_COMMITS, repo_name, branch, (struct Str){0}, arena);
  struct TemplateValue* feed_entries = template_value_list(commit_count, arena);
  for (ptrdiff_t i = 0; i < commit_count; i++) {
    const struct CommitDetail* commit = &commits[i];
    const struct Str commit_url = page_commit_link(root, repo_name, commit->summary.oid, arena);
    const struct FeedEntry entry = {
        .id = commit_url,
        .title = commit->summary.subject,
        .url = commit_url,
        .modified = commit->summary.date,
        .author = commit->summary.author,
        .content = commit->body,
    };
    feed_entries->items[i] = entry_map(&entry, arena);
  }
  const struct Str modified = commit_count > 0 ? commits[0].summary.date : (struct Str){0};
  struct TemplateValue* value =
      page_feed_value_new(site_config, repo, page_feed_commits_path(repo_name, branch, arena),
                          buf_str(&url), html_url, modified, arena);
  template_value_set(value, S("ref"), page_context_branch(repo, tree, root, arena), arena);
  template_value_set(value, S("feed_entries"), feed_entries, arena);
  return value;
}

struct TemplateValue* page_feed_tags_value(const struct SiteConfig* site_config,
                                           const struct RepoModel* repo,
                                           struct Arena* arena) {
  const struct Str repo_name = repo->config->name;
  const struct Str root = absolute_root(site_config, arena);
  ptrdiff_t count = 0;
  const struct PageRefsDated* items = page_refs_tags_newest(repo, arena, &count);
  count = kept_count(count, site_config->feed_count);
  const struct Str tags_url = page_context_repo_link(root, repo_name, PAGE_REFS_TAGS_PAGE, arena);
  struct TemplateValue* feed_entries = template_value_list(count, arena);
  for (ptrdiff_t i = 0; i < count; i++) {
    const ptrdiff_t index = items[i].index;
    const struct Ref* tag = &repo->refset.refs[index];
    const struct CommitSummary* commit = &repo->ref_commits[index];
    struct Buf id = {.arena = arena};
    buf_printf(&id, "%.*s#", STR_FMT(tags_url));
    url_segment_append(&id, tag->short_name);
    struct Str url = tags_url;
    if (page_release_find(repo, tag) != NULL) {
      url = page_release_tag_link(root, repo_name, tag->short_name, arena);
    } else if (commit->oid.len > 0) {
      url = page_commit_link(root, repo_name, commit->oid, arena);
    }
    const struct FeedEntry entry = {
        .id = buf_str(&id),
        .title = tag->short_name,
        .url = url,
        .modified = repo->ref_notes[index].date,
        .author = page_refs_tag_author(repo, index),
        .content = repo->ref_notes[index].message,
    };
    feed_entries->items[i] = entry_map(&entry, arena);
  }
  const struct Str modified = count > 0 ? repo->ref_notes[items[0].index].date : (struct Str){0};
  struct TemplateValue* value = page_feed_value_new(
      site_config, repo,
      page_context_repo_link((struct Str){0}, repo_name, PAGE_FEED_TAGS_PAGE, arena),
      page_context_repo_link(root, repo_name, PAGE_FEED_TAGS_PAGE, arena), tags_url, modified,
      arena);
  template_value_set(value, S("feed_entries"), feed_entries, arena);
  return value;
}

struct TemplateValue* page_feed_releases_value(const struct SiteConfig* site_config,
                                               const struct RepoModel* repo,
                                               struct Arena* perm,
                                               struct Arena scratch) {
  const struct Str repo_name = repo->config->name;
  const struct Str root = absolute_root(site_config, perm);
  struct PageRefsDated* items = arena_new(&scratch, repo->release_count, struct PageRefsDated);
  for (ptrdiff_t i = 0; i < repo->release_count; i++) {
    const struct Release* release = &repo->releases[i];
    items[i] = (struct PageRefsDated){
        .index = i, .time = release->note.time, .name = release->tag->short_name};
  }
  page_refs_sort_newest(items, repo->release_count);
  const ptrdiff_t count = kept_count(repo->release_count, site_config->feed_count);
  struct TemplateValue* feed_entries = template_value_list(count, perm);
  for (ptrdiff_t i = 0; i < count; i++) {
    const struct Release* release = &repo->releases[items[i].index];
    const struct Str url = page_release_tag_link(root, repo_name, release->tag->short_name, perm);
    const struct CommitSummary* commit = &repo->ref_commits[release->tag - repo->refset.refs];
    const struct FeedEntry entry = {
        .id = url,
        .title = release->tag->short_name,
        .url = url,
        .modified = release->note.date,
        .author = release->note.tagger.len > 0 ? release->note.tagger : commit->author,
        .content_html =
            markdown_to_html(release->note.message, NULL, README_TOP_HEADING_LEVEL, perm, scratch),
    };
    feed_entries->items[i] = entry_map(&entry, perm);
  }
  const struct Str modified =
      count > 0 ? repo->releases[items[0].index].note.date : (struct Str){0};
  struct TemplateValue* value = page_feed_value_new(
      site_config, repo,
      page_context_repo_link((struct Str){0}, repo_name, PAGE_FEED_RELEASES_PAGE, perm),
      page_context_repo_link(root, repo_name, PAGE_FEED_RELEASES_PAGE, perm),
      str_concat(root, page_release_index_path(repo_name, perm), perm), modified, perm);
  template_value_set(value, S("feed_entries"), feed_entries, perm);
  return value;
}

static struct TemplateValue* page_feed_value_new(const struct SiteConfig* site_config,
                                                 const struct RepoModel* repo,
                                                 struct Str page,
                                                 struct Str url,
                                                 struct Str html_url,
                                                 struct Str modified,
                                                 struct Arena* arena) {
  // A feed's every link is absolute, as a reader may show it apart from the feed.
  struct TemplateValue* value = page_context_new(site_config, page, arena);
  template_value_set(value, S("repo"),
                     page_context_repo(site_config, repo, absolute_root(site_config, arena), arena),
                     arena);
  struct TemplateValue* feed = template_value_map(arena);
  template_value_set_text(feed, S("id"), url, arena);
  template_value_set_text(feed, S("url"), url, arena);
  template_value_set_text(feed, S("html_url"), html_url, arena);
  template_value_set_text(feed, S("modified"), modified.len > 0 ? modified : RFC3339_EPOCH, arena);
  template_value_set(value, S("feed"), feed, arena);
  return value;
}

static struct TemplateValue* entry_map(const struct FeedEntry* entry, struct Arena* arena) {
  struct TemplateValue* map = template_value_map(arena);
  const struct {
    struct Str key;
    struct Str text;
  } fields[] = {
      {S("id"), entry->id},
      {S("title"), entry->title},
      {S("url"), entry->url},
      {S("modified"), entry->modified.len > 0 ? entry->modified : RFC3339_EPOCH},
      {S("author"), entry->author},
      {S("content"), entry->content},
      {S("content_html"), entry->content_html},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(fields) / sizeof(fields[0])); i++) {
    template_value_set_text(map, fields[i].key, fields[i].text, arena);
  }
  return map;
}

static ptrdiff_t kept_count(ptrdiff_t count, ptrdiff_t feed_count) {
  return count < feed_count ? count : feed_count;
}

static struct Str absolute_root(const struct SiteConfig* site_config, struct Arena* arena) {
  return str_concat(site_config->base_url, S("/"), arena);
}
