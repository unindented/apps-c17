#ifndef SURGE_PAGE_RELEASE_H
#define SURGE_PAGE_RELEASE_H

#include "core/arena.h"
#include "core/str.h"

struct Ref;
struct Release;
struct ReleaseProject;
struct RepoModel;
struct SiteConfig;
struct TemplateValue;

/** Name of a repo's releases page's template. */
#define PAGE_RELEASE_INDEX_TEMPLATE S("releases.html")

/** Name of a release project's page's template. */
#define PAGE_RELEASE_PROJECT_TEMPLATE S("release-project.html")

/** Name of a release's page's template. */
#define PAGE_RELEASE_TAG_TEMPLATE S("release.html")

/** The release templates, which a build needs when a repo has a release project. */
#define PAGE_RELEASE_TEMPLATES \
  STR_INIT("releases.html"), STR_INIT("release-project.html"), STR_INIT("release.html")

/** File name of a release project's version list, which mise's `version_list_url` reads. */
#define PAGE_RELEASE_VERSIONS_TEXT S("versions.txt")

/** File name of a release project's version dates, which mise's `version_date_url` reads. */
#define PAGE_RELEASE_VERSIONS_JSON S("versions.json")

/**
 * The `version_date_expr` that reads `versions.json`: an expr-lang expression that gives mise a
 * `[version, date]` pair per version.
 */
#define PAGE_RELEASE_VERSION_DATE_EXPR "map(fromJSON(body).versions, { [#.version, #.date] })"

/**
 * @brief Returns the output path of a repo's releases page, `<r>/releases/index.html`.
 *
 * @param repo_name Name of the repo.
 * @param arena     Arena that owns the path. Must not be `NULL`.
 * @return The path.
 */
struct Str page_release_index_path(struct Str repo_name, struct Arena* arena)
    __attribute__((nonnull(2)));

/**
 * @brief Returns the output path of a file of a release project, `<r>/releases/<project>/<file>`.
 *
 * @param repo_name    Name of the repo.
 * @param project_name Name of the project.
 * @param file_name    Name of the file, such as `index.html` or `versions.txt`.
 * @param arena        Arena that owns the path. Must not be `NULL`.
 * @return The path.
 */
struct Str page_release_project_path(struct Str repo_name,
                                     struct Str project_name,
                                     struct Str file_name,
                                     struct Arena* arena) __attribute__((nonnull(4)));

/**
 * @brief Returns the output path of a release's page, `<r>/releases/tag/<tag>/index.html`.
 *
 * @param repo_name Name of the repo.
 * @param tag_name  Name of the tag below `refs/tags/`.
 * @param arena     Arena that owns the path. Must not be `NULL`.
 * @return The path.
 */
struct Str page_release_tag_path(struct Str repo_name, struct Str tag_name, struct Arena* arena)
    __attribute__((nonnull(3)));

/**
 * @brief Returns the output path of a release's asset, `<r>/releases/download/<tag>/<asset>`.
 *
 * @param repo_name  Name of the repo.
 * @param tag_name   Name of the tag below `refs/tags/`.
 * @param asset_name Name of the asset.
 * @param arena      Arena that owns the path. Must not be `NULL`.
 * @return The path.
 */
struct Str page_release_asset_path(struct Str repo_name,
                                   struct Str tag_name,
                                   struct Str asset_name,
                                   struct Arena* arena) __attribute__((nonnull(4)));

/**
 * @brief Returns the link from a page to a release's page, with the tag's segments encoded.
 *
 * @param path_to_root Prefix from the page back to the output root.
 * @param repo_name    Name of the repo.
 * @param tag_name     Name of the tag below `refs/tags/`.
 * @param arena        Arena that owns the link. Must not be `NULL`.
 * @return The link.
 */
struct Str page_release_tag_link(struct Str path_to_root,
                                 struct Str repo_name,
                                 struct Str tag_name,
                                 struct Arena* arena) __attribute__((nonnull(4)));

/**
 * @brief Finds the release of a tag.
 *
 * @param repo What the scan found for the repo. Must not be `NULL`.
 * @param tag  A ref of `repo`'s refset. Must not be `NULL`.
 * @return The release, or `NULL` when the tag has none.
 */
const struct Release* page_release_find(const struct RepoModel* repo, const struct Ref* tag)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Returns the data tree of a repo's releases page, `<r>/releases/index.html`.
 *
 * `apps/surge/README.md#data-trees` lists its keys, in the `releases.html` row, and the keys of the
 * project and release maps.
 *
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param repo        What the scan found for the repo. Must not be `NULL`.
 * @param perm        Arena that owns the tree. Must not be `NULL`.
 * @param scratch     Arena for rendering the notes.
 * @return The data tree.
 */
struct TemplateValue* page_release_index_value(const struct SiteConfig* site_config,
                                               const struct RepoModel* repo,
                                               struct Arena* perm,
                                               struct Arena scratch)
    __attribute__((nonnull(1, 2, 3), returns_nonnull));

/**
 * @brief Returns the data tree of a release project's page, `<r>/releases/<project>/index.html`.
 *
 * `apps/surge/README.md#data-trees` lists its keys, in the `release-project.html` row.
 *
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param repo        What the scan found for the repo. Must not be `NULL`.
 * @param project     One of `repo`'s release projects. Must not be `NULL`.
 * @param perm        Arena that owns the tree. Must not be `NULL`.
 * @param scratch     Arena for rendering the notes.
 * @return The data tree.
 */
struct TemplateValue* page_release_project_value(const struct SiteConfig* site_config,
                                                 const struct RepoModel* repo,
                                                 const struct ReleaseProject* project,
                                                 struct Arena* perm,
                                                 struct Arena scratch)
    __attribute__((nonnull(1, 2, 3, 4), returns_nonnull));

/**
 * @brief Returns the data tree of a release's page, `<r>/releases/tag/<tag>/index.html`.
 *
 * `apps/surge/README.md#data-trees` lists its keys, in the `release.html` row.
 *
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param repo        What the scan found for the repo. Must not be `NULL`.
 * @param release     One of `repo`'s releases. Must not be `NULL`.
 * @param perm        Arena that owns the tree. Must not be `NULL`.
 * @param scratch     Arena for rendering the notes.
 * @return The data tree.
 */
struct TemplateValue* page_release_tag_value(const struct SiteConfig* site_config,
                                             const struct RepoModel* repo,
                                             const struct Release* release,
                                             struct Arena* perm,
                                             struct Arena scratch)
    __attribute__((nonnull(1, 2, 3, 4), returns_nonnull));

/**
 * @brief Returns a release project's `versions.txt`: its listed versions, one per line, oldest
 *        first, so the newest is last, as mise reads it.
 *
 * @param project The project. Must not be `NULL`.
 * @param arena   Arena that owns the text. Must not be `NULL`.
 * @return The text.
 */
struct Str page_release_versions_text(const struct ReleaseProject* project, struct Arena* arena)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Returns a release project's `versions.json`, `{"versions":[{"version":…,"date":…},…]}`,
 *        with the versions of `versions.txt` in its order and each release's date.
 *
 * @param project The project. Must not be `NULL`.
 * @param arena   Arena that owns the text. Must not be `NULL`.
 * @return The text.
 */
struct Str page_release_versions_json(const struct ReleaseProject* project, struct Arena* arena)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Returns the example `mise.toml` of a release project for mise's `http:` backend, built
 *        from `base_url` and the archives of its newest listed release that is no pre-release, or
 *        of its newest when all are, as mise's `latest` passes over pre-releases.
 *
 * `apps/surge/README.md#version-lists-and-mise` describes the example.
 *
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param repo        What the scan found for the repo. Must not be `NULL`.
 * @param project     One of `repo`'s release projects. Must not be `NULL`.
 * @param arena       Arena that owns the text. Must not be `NULL`.
 * @return The example, or empty when that release has no such archive.
 */
struct Str page_release_mise_toml(const struct SiteConfig* site_config,
                                  const struct RepoModel* repo,
                                  const struct ReleaseProject* project,
                                  struct Arena* arena) __attribute__((nonnull(1, 2, 3, 4)));

#endif
