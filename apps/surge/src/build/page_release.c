#include "build/page_release.h"

#include <stdbool.h>
#include <stddef.h>

#include "build/page_context.h"
#include "build/page_refs.h"
#include "build/readme.h"
#include "build/template_value.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "domain/semver.h"
#include "domain/site_config.h"
#include "domain/url_segment.h"
#include "markup/markdown.h"

/** A target of a release archive, and the platform mise names it. */
struct PageReleasePlatform {
  /** The target, as the archive's name holds it. */
  struct Str target;

  /** mise's `<os>-<arch>` key. */
  struct Str key;
};

/** The archives the mise example names, in its order. */
static const struct PageReleasePlatform PAGE_RELEASE_PLATFORMS[] = {
    {STR_INIT("x86_64-linux-musl"), STR_INIT("linux-x64")},
    {STR_INIT("aarch64-linux-musl"), STR_INIT("linux-arm64")},
    {STR_INIT("aarch64-macos"), STR_INIT("macos-arm64")},
};

/**
 * @brief Returns the data tree of a release page, before its own fields.
 *
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param repo        What the scan found for the repo. Must not be `NULL`.
 * @param page        Output path of the page.
 * @param arena       Arena that owns the tree. Must not be `NULL`.
 * @return The data tree, with `site`, `generator`, `path_to_root` and `repo`.
 */
static struct TemplateValue* page_release_value_new(const struct SiteConfig* site_config,
                                                    const struct RepoModel* repo,
                                                    struct Str page,
                                                    struct Arena* arena)
    __attribute__((nonnull(1, 2, 4), returns_nonnull));

/**
 * @brief Returns the map of a release project, with its releases.
 *
 * @param site_config  Configuration of the build. Must not be `NULL`.
 * @param repo         What the scan found for the repo. Must not be `NULL`.
 * @param project      The project, or `NULL` for no project.
 * @param path_to_root Prefix from the page back to the output root.
 * @param perm         Arena that owns the map. Must not be `NULL`.
 * @param scratch      Arena for rendering the notes.
 * @return The map, or `false` for no project, so a section over it enters only for a project.
 */
static struct TemplateValue* page_release_value_project(const struct SiteConfig* site_config,
                                                        const struct RepoModel* repo,
                                                        const struct ReleaseProject* project,
                                                        struct Str path_to_root,
                                                        struct Arena* perm,
                                                        struct Arena scratch)
    __attribute__((nonnull(1, 2, 5), returns_nonnull));

/**
 * @brief Returns the map of a release.
 *
 * @param repo         What the scan found for the repo. Must not be `NULL`.
 * @param release      The release. Must not be `NULL`.
 * @param path_to_root Prefix from the page back to the output root.
 * @param perm         Arena that owns the map. Must not be `NULL`.
 * @param scratch      Arena for rendering the notes.
 * @return The map.
 */
static struct TemplateValue* page_release_value_release(const struct RepoModel* repo,
                                                        const struct Release* release,
                                                        struct Str path_to_root,
                                                        struct Arena* perm,
                                                        struct Arena scratch)
    __attribute__((nonnull(1, 2, 4), returns_nonnull));

/**
 * @brief Returns the link from a page to a file of a release project.
 *
 * @param path_to_root Prefix from the page back to the output root.
 * @param repo_name    Name of the repo.
 * @param project_name Name of the project.
 * @param file_name    Name of the file.
 * @param arena        Arena that owns the link. Must not be `NULL`.
 * @return The link.
 */
static struct Str project_link(struct Str path_to_root,
                               struct Str repo_name,
                               struct Str project_name,
                               struct Str file_name,
                               struct Arena* arena) __attribute__((nonnull(5)));

/**
 * @brief Returns a number as decimal text.
 *
 * @param number The number.
 * @param arena  Arena that owns the text. Must not be `NULL`.
 * @return The text.
 */
static struct Str number_text(ptrdiff_t number, struct Arena* arena) __attribute__((nonnull(2)));

/**
 * @brief Appends the URL of a release's directory of downloads below `base_url`, the tag given as a
 *        lead followed by mise's `{{ version }}`.
 *
 * @param buffer      Buffer that receives the URL. Must not be `NULL`.
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param repo_name   Name of the repo.
 * @param tag_lead    The tag's text before its version, such as `alpha-v`.
 */
static void append_download_url(struct Buf* buffer,
                                const struct SiteConfig* site_config,
                                struct Str repo_name,
                                struct Str tag_lead) __attribute__((nonnull(1, 2)));

struct Str page_release_index_path(struct Str repo_name, struct Arena* arena) {
  return str_concat(repo_name, S("/releases/index.html"), arena);
}

struct Str page_release_project_path(struct Str repo_name,
                                     struct Str project_name,
                                     struct Str file_name,
                                     struct Arena* arena) {
  struct Buf path = {.arena = arena};
  buf_printf(&path, "%.*s/releases/%.*s/%.*s", STR_FMT(repo_name), STR_FMT(project_name),
             STR_FMT(file_name));
  return buf_str(&path);
}

struct Str page_release_tag_path(struct Str repo_name, struct Str tag_name, struct Arena* arena) {
  struct Buf path = {.arena = arena};
  buf_printf(&path, "%.*s/releases/tag/%.*s/index.html", STR_FMT(repo_name), STR_FMT(tag_name));
  return buf_str(&path);
}

struct Str page_release_asset_path(struct Str repo_name,
                                   struct Str tag_name,
                                   struct Str asset_name,
                                   struct Arena* arena) {
  struct Buf path = {.arena = arena};
  buf_printf(&path, "%.*s/releases/download/%.*s/%.*s", STR_FMT(repo_name), STR_FMT(tag_name),
             STR_FMT(asset_name));
  return buf_str(&path);
}

struct Str page_release_tag_link(struct Str path_to_root,
                                 struct Str repo_name,
                                 struct Str tag_name,
                                 struct Arena* arena) {
  struct Buf link = {.arena = arena};
  buf_printf(&link, "%.*s%.*s/releases/tag/", STR_FMT(path_to_root), STR_FMT(repo_name));
  url_segment_append_path(&link, tag_name);
  buf_append(&link, S("/index.html"));
  return buf_str(&link);
}

const struct Release* page_release_find(const struct RepoModel* repo, const struct Ref* tag) {
  // The releases are in the refset's order, so their tags' addresses ascend.
  ptrdiff_t low = 0;
  ptrdiff_t high = repo->release_count;
  while (low < high) {
    const ptrdiff_t middle = low + (high - low) / 2;
    const struct Ref* middle_tag = repo->releases[middle].tag;
    if (middle_tag == tag) {
      return &repo->releases[middle];
    }
    if (middle_tag < tag) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  return NULL;
}

struct TemplateValue* page_release_index_value(const struct SiteConfig* site_config,
                                               const struct RepoModel* repo,
                                               struct Arena* perm,
                                               struct Arena scratch) {
  const struct Str page = page_release_index_path(repo->config->name, perm);
  struct TemplateValue* value = page_release_value_new(site_config, repo, page, perm);
  const struct Str path_to_root = template_value_find(value, S("path_to_root"))->text;
  struct TemplateValue* projects = template_value_list(repo->release_project_count, perm);
  ptrdiff_t release_count = 0;
  for (ptrdiff_t i = 0; i < repo->release_project_count; i++) {
    const struct ReleaseProject* project = &repo->release_projects[i];
    projects->items[i] =
        page_release_value_project(site_config, repo, project, path_to_root, perm, scratch);
    release_count += project->release_count;
  }
  ptrdiff_t unprojected_count = 0;
  for (ptrdiff_t i = 0; i < repo->release_count; i++) {
    unprojected_count += repo->releases[i].project_index < 0;
  }
  struct TemplateValue* releases = template_value_list(release_count + unprojected_count, perm);
  ptrdiff_t index = 0;
  for (ptrdiff_t i = 0; i < repo->release_project_count; i++) {
    const struct ReleaseProject* project = &repo->release_projects[i];
    for (ptrdiff_t j = 0; j < project->release_count; j++) {
      releases->items[index++] =
          page_release_value_release(repo, project->releases[j], path_to_root, perm, scratch);
    }
  }
  for (ptrdiff_t i = 0; i < repo->release_count; i++) {
    if (repo->releases[i].project_index < 0) {
      releases->items[index++] =
          page_release_value_release(repo, &repo->releases[i], path_to_root, perm, scratch);
    }
  }
  template_value_set(value, S("projects"), projects, perm);
  template_value_set(value, S("releases"), releases, perm);
  return value;
}

struct TemplateValue* page_release_project_value(const struct SiteConfig* site_config,
                                                 const struct RepoModel* repo,
                                                 const struct ReleaseProject* project,
                                                 struct Arena* perm,
                                                 struct Arena scratch) {
  const struct Str page =
      page_release_project_path(repo->config->name, project->name, S("index.html"), perm);
  struct TemplateValue* value = page_release_value_new(site_config, repo, page, perm);
  const struct Str path_to_root = template_value_find(value, S("path_to_root"))->text;
  template_value_set(
      value, S("project"),
      page_release_value_project(site_config, repo, project, path_to_root, perm, scratch), perm);
  return value;
}

struct TemplateValue* page_release_tag_value(const struct SiteConfig* site_config,
                                             const struct RepoModel* repo,
                                             const struct Release* release,
                                             struct Arena* perm,
                                             struct Arena scratch) {
  const struct Str page = page_release_tag_path(repo->config->name, release->tag->short_name, perm);
  struct TemplateValue* value = page_release_value_new(site_config, repo, page, perm);
  const struct Str path_to_root = template_value_find(value, S("path_to_root"))->text;
  template_value_set(value, S("release"),
                     page_release_value_release(repo, release, path_to_root, perm, scratch), perm);
  const struct ReleaseProject* project =
      release->project_index >= 0 ? &repo->release_projects[release->project_index] : NULL;
  template_value_set(
      value, S("project"),
      page_release_value_project(site_config, repo, project, path_to_root, perm, scratch), perm);
  return value;
}

struct Str page_release_versions_text(const struct ReleaseProject* project, struct Arena* arena) {
  struct Buf text = {.arena = arena};
  for (ptrdiff_t i = project->version_count - 1; i >= 0; i--) {
    buf_append(&text, project->releases[i]->version);
    buf_append_char(&text, '\n');
  }
  return buf_str(&text);
}

struct Str page_release_versions_json(const struct ReleaseProject* project, struct Arena* arena) {
  // A listed version and a date hold no byte JSON escapes.
  struct Buf text = {.arena = arena};
  buf_append(&text, S("{\"versions\":["));
  for (ptrdiff_t i = project->version_count - 1; i >= 0; i--) {
    const struct Release* release = project->releases[i];
    buf_printf(&text, "%s{\"version\":\"%.*s\",\"date\":\"%.*s\"}",
               i == project->version_count - 1 ? "" : ",", STR_FMT(release->version),
               STR_FMT(release->note.date));
  }
  buf_append(&text, S("]}\n"));
  return buf_str(&text);
}

struct Str page_release_mise_toml(const struct SiteConfig* site_config,
                                  const struct RepoModel* repo,
                                  const struct ReleaseProject* project,
                                  struct Arena* arena) {
  if (project->version_count == 0) {
    return (struct Str){0};
  }
  // mise's `latest` passes over pre-releases, so the archives are the newest release's, when the
  // project has one.
  const struct Release* newest = project->releases[0];
  for (ptrdiff_t i = 0; i < project->version_count; i++) {
    if (project->releases[i]->semver.pre.len == 0) {
      newest = project->releases[i];
      break;
    }
  }
  const struct Str tag_name = newest->tag->short_name;
  const struct Str tag_lead = str_head(tag_name, tag_name.len - newest->version.len);
  const struct Str repo_name = repo->config->name;
  struct Buf platforms = {.arena = arena};
  for (ptrdiff_t i = 0;
       i < (ptrdiff_t)(sizeof(PAGE_RELEASE_PLATFORMS) / sizeof(PAGE_RELEASE_PLATFORMS[0])); i++) {
    const struct PageReleasePlatform* platform = &PAGE_RELEASE_PLATFORMS[i];
    struct Arena name_scratch = *arena;
    struct Buf name = {.arena = &name_scratch};
    buf_printf(&name, "%.*s-%.*s-%.*s.tar.gz", STR_FMT(project->name), STR_FMT(newest->version),
               STR_FMT(platform->target));
    bool is_found = false;
    for (ptrdiff_t j = 0; j < newest->asset_count && !is_found; j++) {
      is_found = str_eq(newest->assets[j].name, buf_str(&name));
    }
    if (!is_found) {
      continue;
    }
    // The archive's name is checked before anything grows in the arena, so the list grows in place.
    buf_printf(&platforms, "%.*s = { url = \"", STR_FMT(platform->key));
    append_download_url(&platforms, site_config, repo_name, tag_lead);
    buf_printf(&platforms, "/%.*s-{{ version }}-%.*s.tar.gz\" }\n", STR_FMT(project->name),
               STR_FMT(platform->target));
  }
  if (platforms.len == 0) {
    return (struct Str){0};
  }
  const struct Str platform_lines = buf_str(&platforms);
  const struct Str versions_text =
      page_release_project_path(repo_name, project->name, PAGE_RELEASE_VERSIONS_TEXT, arena);
  const struct Str versions_json =
      page_release_project_path(repo_name, project->name, PAGE_RELEASE_VERSIONS_JSON, arena);
  struct Buf text = {.arena = arena};
  buf_printf(&text, "[tools.\"http:%.*s\"]\nversion = \"latest\"\n", STR_FMT(project->name));
  buf_printf(&text, "version_list_url = \"%.*s/%.*s\"\n", STR_FMT(site_config->base_url),
             STR_FMT(versions_text));
  buf_printf(&text, "version_date_url = \"%.*s/%.*s\"\n", STR_FMT(site_config->base_url),
             STR_FMT(versions_json));
  buf_append(&text, S("version_date_expr = '" PAGE_RELEASE_VERSION_DATE_EXPR "'\n"));
  if (newest->has_sums) {
    buf_append(&text, S("checksum_url = \""));
    append_download_url(&text, site_config, repo_name, tag_lead);
    buf_append(&text, S("/SHA256SUMS\"\n"));
  }
  buf_printf(&text, "strip_components = 1\nbin_path = \"bin\"\n\n[tools.\"http:%.*s\".platforms]\n",
             STR_FMT(project->name));
  buf_append(&text, platform_lines);
  return buf_str(&text);
}

static struct TemplateValue* page_release_value_new(const struct SiteConfig* site_config,
                                                    const struct RepoModel* repo,
                                                    struct Str page,
                                                    struct Arena* arena) {
  struct TemplateValue* value = page_context_new(site_config, page, arena);
  const struct Str path_to_root = template_value_find(value, S("path_to_root"))->text;
  template_value_set(value, S("repo"), page_context_repo(site_config, repo, path_to_root, arena),
                     arena);
  return value;
}

static struct TemplateValue* page_release_value_project(const struct SiteConfig* site_config,
                                                        const struct RepoModel* repo,
                                                        const struct ReleaseProject* project,
                                                        struct Str path_to_root,
                                                        struct Arena* perm,
                                                        struct Arena scratch) {
  if (project == NULL) {
    return template_value_bool(false, perm);
  }
  const struct Str repo_name = repo->config->name;
  const struct Str name = project->name;
  struct TemplateValue* map = template_value_map(perm);
  template_value_set_text(map, S("name"), name, perm);
  template_value_set_text(map, S("url"),
                          project_link(path_to_root, repo_name, name, S("index.html"), perm), perm);
  template_value_set_text(
      map, S("versions_url"),
      project_link(path_to_root, repo_name, name, PAGE_RELEASE_VERSIONS_TEXT, perm), perm);
  template_value_set_text(
      map, S("versions_json_url"),
      project_link(path_to_root, repo_name, name, PAGE_RELEASE_VERSIONS_JSON, perm), perm);
  template_value_set_text(
      map, S("mise_toml"),
      project->is_mise ? page_release_mise_toml(site_config, repo, project, perm) : (struct Str){0},
      perm);
  struct TemplateValue* releases = template_value_list(project->release_count, perm);
  for (ptrdiff_t i = 0; i < project->release_count; i++) {
    releases->items[i] =
        page_release_value_release(repo, project->releases[i], path_to_root, perm, scratch);
  }
  template_value_set(map, S("releases"), releases, perm);
  return map;
}

static struct TemplateValue* page_release_value_release(const struct RepoModel* repo,
                                                        const struct Release* release,
                                                        struct Str path_to_root,
                                                        struct Arena* perm,
                                                        struct Arena scratch) {
  const struct Str repo_name = repo->config->name;
  const struct Str tag_name = release->tag->short_name;
  struct TemplateValue* map = template_value_map(perm);
  template_value_set_text(map, S("tag"), tag_name, perm);
  template_value_set_text(map, S("version"), release->version, perm);
  template_value_set_text(map, S("date"), release->note.date, perm);
  template_value_set_text(map, S("url"),
                          page_release_tag_link(path_to_root, repo_name, tag_name, perm), perm);
  const struct ReleaseProject* project =
      release->project_index >= 0 ? &repo->release_projects[release->project_index] : NULL;
  bool is_listed = false;
  for (ptrdiff_t i = 0; project != NULL && i < project->version_count && !is_listed; i++) {
    is_listed = project->releases[i] == release;
  }
  template_value_set_bool(map, S("is_listed"), is_listed, perm);
  template_value_set_text(map, S("project_name"), project != NULL ? project->name : (struct Str){0},
                          perm);
  template_value_set_text(
      map, S("project_url"),
      project != NULL ? project_link(path_to_root, repo_name, project->name, S("index.html"), perm)
                      : (struct Str){0},
      perm);
  template_value_set(map, S("commit"),
                     page_refs_commit(repo, repo->ref_commits[release->tag - repo->refset.refs],
                                      path_to_root, perm),
                     perm);
  template_value_set_text(
      map, S("notes_html"),
      markdown_to_html(release->note.message, NULL, README_TOP_HEADING_LEVEL, perm, scratch), perm);
  template_value_set_text(map, S("signature"), release->note.signature, perm);
  template_value_set_text(map, S("signature_kind"),
                          page_refs_signature_kind(release->note.signature), perm);
  template_value_set_bool(map, S("has_sums"), release->has_sums, perm);
  template_value_set_bool(map, S("is_mise"), project != NULL && project->is_mise, perm);
  struct TemplateValue* assets = template_value_list(release->asset_count, perm);
  for (ptrdiff_t i = 0; i < release->asset_count; i++) {
    const struct ReleaseAsset* asset = &release->assets[i];
    struct TemplateValue* asset_map = template_value_map(perm);
    template_value_set_text(asset_map, S("name"), asset->name, perm);
    struct Buf url = {.arena = perm};
    buf_printf(&url, "%.*s%.*s/releases/download/", STR_FMT(path_to_root), STR_FMT(repo_name));
    url_segment_append_path(&url, tag_name);
    buf_append_char(&url, '/');
    url_segment_append(&url, asset->name);
    template_value_set_text(asset_map, S("url"), buf_str(&url), perm);
    template_value_set_text(asset_map, S("size"), number_text(asset->len, perm), perm);
    assets->items[i] = asset_map;
  }
  template_value_set(map, S("assets"), assets, perm);
  return map;
}

static struct Str project_link(struct Str path_to_root,
                               struct Str repo_name,
                               struct Str project_name,
                               struct Str file_name,
                               struct Arena* arena) {
  return str_concat(path_to_root,
                    page_release_project_path(repo_name, project_name, file_name, arena), arena);
}

static struct Str number_text(ptrdiff_t number, struct Arena* arena) {
  struct Buf text = {.arena = arena};
  buf_printf(&text, "%td", number);
  return buf_str(&text);
}

static void append_download_url(struct Buf* buffer,
                                const struct SiteConfig* site_config,
                                struct Str repo_name,
                                struct Str tag_lead) {
  buf_printf(buffer, "%.*s/%.*s/releases/download/", STR_FMT(site_config->base_url),
             STR_FMT(repo_name));
  url_segment_append_path(buffer, tag_lead);
  buf_append(buffer, S("{{ version }}"));
}
