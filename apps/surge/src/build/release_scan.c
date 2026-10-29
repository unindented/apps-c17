#include "build/release_scan.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/error.h"
#include "core/log.h"
#include "core/path.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "domain/semver.h"
#include "domain/site_config.h"
#include "os/os.h"
#include "vcs/repository.h"

/**
 * @brief Fills the release projects of `model`: its repo's projects with a `tag_prefix`, or the
 *        repo's own project when it has `releases_dir` and none of them.
 *
 * @param model Scanned repo, which receives `release_projects`. Must not be `NULL`.
 * @param perm  Arena that owns the projects. Must not be `NULL`.
 */
static void release_scan_load_projects(struct RepoModel* model, struct Arena* perm)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Reads the release of `tag` when it has a directory below `releases_dir`.
 *
 * @param model       Scanned repo, whose release projects are filled. Must not be `NULL`.
 * @param tag         A tag of the repo. Must not be `NULL`.
 * @param os          Host `releases_dir` is read on. Must not be `NULL`.
 * @param perm        Arena that owns the release. Must not be `NULL`.
 * @param scratch     Arena for the directory's path.
 * @param release_out Receives the release, zeroed when the tag has no directory. Must not be
 *                    `NULL`.
 * @param log         Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the directory or a file cannot be read.
 */
static int release_scan_load_release(const struct RepoModel* model,
                                     const struct Ref* tag,
                                     struct Os* os,
                                     struct Arena* perm,
                                     struct Arena scratch,
                                     struct Release* release_out,
                                     struct Log* log) __attribute__((nonnull(1, 2, 3, 4, 6, 7)));

/**
 * @brief Lists the regular files of a release's directory as its assets, in byte order of name.
 *
 * @param release  The release, which receives `assets` and `has_sums`. Must not be `NULL`.
 * @param dir_path The release's directory.
 * @param os       Host the directory is read on. Must not be `NULL`.
 * @param perm     Arena that owns the assets. Must not be `NULL`.
 * @param log      Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the directory or a file cannot be read.
 */
static int release_scan_load_assets(struct Release* release,
                                    struct Str dir_path,
                                    struct Os* os,
                                    struct Arena* perm,
                                    struct Log* log) __attribute__((nonnull(1, 3, 4, 5)));

/**
 * @brief Finds the project of a release's tag and the version the tag names in it.
 *
 * @param release The release, whose `tag` is set and which receives `project_index`, `version` and
 *                `is_semver`. Must not be `NULL`.
 * @param model   Scanned repo, whose release projects are filled. Must not be `NULL`.
 */
static void release_scan_load_release_version(struct Release* release,
                                              const struct RepoModel* model)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Warns of each release in no project, each whose version does not parse, and each of a
 *        project with `mise` without `SHA256SUMS`.
 *
 * @param model Scanned repo, whose releases are found. Must not be `NULL`.
 * @param log   Log that receives the warnings. Must not be `NULL`.
 */
static void release_scan_load_warn(const struct RepoModel* model, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Fills each release project's releases: those `versions.txt` lists newest first, then the
 *        others in byte order of tag.
 *
 * @param model   Scanned repo, whose releases are found. Must not be `NULL`.
 * @param perm    Arena that owns the lists. Must not be `NULL`.
 * @param scratch Arena for the sort.
 * @param log     Log that receives a warning per repeated version. Must not be `NULL`.
 */
static void release_scan_load_project_releases(struct RepoModel* model,
                                               struct Arena* perm,
                                               struct Arena scratch,
                                               struct Log* log) __attribute__((nonnull(1, 2, 4)));

/**
 * @brief Orders releases for a project's list: versions that parse first, newest first by
 *        precedence and then by tag, and then the others by tag. A `qsort` comparator.
 *
 * @param a A `const struct Release* const*`. Must not be `NULL`.
 * @param b A `const struct Release* const*`. Must not be `NULL`.
 * @return Negative, zero or positive as `a` sorts before, with, or after `b`.
 */
static int compare_releases(const void* a, const void* b) __attribute__((nonnull(1, 2)));

/**
 * @brief Orders releases by tag. A `qsort` comparator.
 *
 * @param a A `const struct Release* const*`. Must not be `NULL`.
 * @param b A `const struct Release* const*`. Must not be `NULL`.
 * @return Negative, zero or positive as `a`'s tag sorts before, with, or after `b`'s.
 */
static int compare_release_tags(const void* a, const void* b) __attribute__((nonnull(1, 2)));

/**
 * @brief Orders assets by name. A `qsort` comparator.
 *
 * @param a A `const struct ReleaseAsset*`. Must not be `NULL`.
 * @param b A `const struct ReleaseAsset*`. Must not be `NULL`.
 * @return Negative, zero or positive as `a`'s name sorts before, with, or after `b`'s.
 */
static int compare_assets(const void* a, const void* b) __attribute__((nonnull(1, 2)));

bool release_scan_has_projects(const struct SurgeRepo* repo) {
  for (ptrdiff_t i = 0; i < repo->project_count; i++) {
    if (repo->projects[i].tag_prefix.len > 0) {
      return true;
    }
  }
  return repo->releases_dir.len > 0;
}

int release_scan_load(struct RepoModel* model,
                      struct Os* os,
                      struct Arena* perm,
                      struct Arena scratch,
                      struct Log* log) {
  release_scan_load_projects(model, perm);
  const struct SurgeRepo* repo = model->config;
  if (repo->releases_dir.len == 0) {
    return 0;
  }
  model->releases = arena_new(perm, model->refset.count, struct Release);
  int rc = 0;
  for (ptrdiff_t i = 0; i < model->refset.count; i++) {
    const struct Ref* ref = &model->refset.refs[i];
    if (ref->kind != REF_KIND_TAG) {
      continue;
    }
    struct Release* release = &model->releases[model->release_count];
    if (release_scan_load_release(model, ref, os, perm, scratch, release, log) != 0) {
      rc = -1;
    } else if (release->tag != NULL) {
      model->release_count++;
    }
  }
  if (rc != 0) {
    return -1;
  }
  release_scan_load_warn(model, log);
  release_scan_load_project_releases(model, perm, scratch, log);
  return 0;
}

struct Str release_scan_id(const struct RepoModel* model, struct Arena* arena) {
  // Each field ends in a `NUL`, which none of them holds, so two different sets of fields never
  // hash the same text.
  struct Buf text = {.arena = arena};
  for (ptrdiff_t i = 0; i < model->release_project_count; i++) {
    const struct ReleaseProject* project = &model->release_projects[i];
    buf_append(&text, project->name);
    buf_append_char(&text, '\0');
    buf_append(&text, project->tag_prefix);
    buf_append_char(&text, '\0');
    buf_append_char(&text, project->is_mise ? '1' : '0');
  }
  for (ptrdiff_t i = 0; i < model->release_count; i++) {
    const struct Release* release = &model->releases[i];
    const struct Str fields[] = {release->tag->name, release->tag->oid, release->version};
    for (ptrdiff_t j = 0; j < (ptrdiff_t)(sizeof(fields) / sizeof(fields[0])); j++) {
      buf_append(&text, fields[j]);
      buf_append_char(&text, '\0');
    }
    for (ptrdiff_t j = 0; j < release->asset_count; j++) {
      const struct ReleaseAsset* asset = &release->assets[j];
      buf_append(&text, asset->name);
      buf_append_char(&text, '\0');
      buf_printf(&text, "%td %" PRId64 " %" PRId64, asset->len, asset->mtime_s, asset->mtime_ns);
      buf_append_char(&text, '\0');
    }
  }
  return repository_text_id(buf_str(&text), arena);
}

static void release_scan_load_projects(struct RepoModel* model, struct Arena* perm) {
  const struct SurgeRepo* repo = model->config;
  ptrdiff_t prefixed_count = 0;
  for (ptrdiff_t i = 0; i < repo->project_count; i++) {
    prefixed_count += repo->projects[i].tag_prefix.len > 0;
  }
  if (!release_scan_has_projects(repo)) {
    return;
  }
  model->release_projects =
      arena_new(perm, prefixed_count > 0 ? prefixed_count : 1, struct ReleaseProject);
  if (prefixed_count == 0) {
    model->release_projects[model->release_project_count++] =
        (struct ReleaseProject){.name = repo->name, .is_mise = repo->is_mise};
    return;
  }
  for (ptrdiff_t i = 0; i < repo->project_count; i++) {
    const struct SurgeProject* project = &repo->projects[i];
    if (project->tag_prefix.len > 0) {
      model->release_projects[model->release_project_count++] = (struct ReleaseProject){
          .name = project->name,
          .tag_prefix = project->tag_prefix,
          .is_mise = project->is_mise,
      };
    }
  }
}

static int release_scan_load_release(const struct RepoModel* model,
                                     const struct Ref* tag,
                                     struct Os* os,
                                     struct Arena* perm,
                                     struct Arena scratch,
                                     struct Release* release_out,
                                     struct Log* log) {
  *release_out = (struct Release){0};
  const struct Str dir_path = path_join(model->config->releases_dir, tag->short_name, &scratch);
  struct OsStat st;
  if (os_stat(os, dir_path, &st) != 0) {
    // A tag without a directory is only a tag.
    if (errno == ENOENT || errno == ENOTDIR) {
      return 0;
    }
    char message[ERROR_SYSTEM_MESSAGE_SIZE];
    return log_error(log, "failed to inspect release directory: %s ('%.*s')",
                     error_system_message(message, sizeof(message), errno), STR_FMT(dir_path));
  }
  if (!st.is_dir) {
    return 0;
  }
  struct Release release = {.tag = tag};
  release_scan_load_release_version(&release, model);
  release.note = model->ref_notes[tag - model->refset.refs];
  if (release_scan_load_assets(&release, dir_path, os, perm, log) != 0) {
    return -1;
  }
  *release_out = release;
  return 0;
}

static int release_scan_load_assets(struct Release* release,
                                    struct Str dir_path,
                                    struct Os* os,
                                    struct Arena* perm,
                                    struct Log* log) {
  struct OsEntry* entries = NULL;
  ptrdiff_t entry_count = 0;
  if (os_list(os, dir_path, perm, &entries, &entry_count, S("failed to list release assets"),
              log) != 0) {
    return -1;
  }
  release->assets = arena_new(perm, entry_count, struct ReleaseAsset);
  char message[ERROR_SYSTEM_MESSAGE_SIZE];
  int rc = 0;
  for (ptrdiff_t i = 0; i < entry_count; i++) {
    const struct OsEntry* entry = &entries[i];
    const struct Str file_path = path_join(dir_path, entry->name, perm);
    struct OsStat st;
    if (entry->kind == OS_ENTRY_FAILED) {
      rc = log_error(log, "failed to list release assets: cannot inspect entry: %s ('%.*s')",
                     error_system_message(message, sizeof(message), entry->error_number),
                     STR_FMT(file_path));
      continue;
    }
    // A directory in a release's directory is no asset, as a download holds only files.
    if (entry->kind != OS_ENTRY_FILE) {
      continue;
    }
    if (os_stat(os, file_path, &st) != 0) {
      rc = log_error(log, "failed to inspect release asset: %s ('%.*s')",
                     error_system_message(message, sizeof(message), errno), STR_FMT(file_path));
      continue;
    }
    release->assets[release->asset_count++] = (struct ReleaseAsset){
        .name = entry->name,
        .source_path = file_path,
        .len = st.len,
        .mtime_s = st.mtime_s,
        .mtime_ns = st.mtime_ns,
    };
    release->has_sums = release->has_sums || str_eq(entry->name, RELEASE_SCAN_SUMS_NAME);
  }
  if (release->asset_count > 0) {
    qsort(release->assets, (size_t)release->asset_count, sizeof(*release->assets), compare_assets);
  }
  return rc;
}

static void release_scan_load_release_version(struct Release* release,
                                              const struct RepoModel* model) {
  const struct Str tag_name = release->tag->short_name;
  release->project_index = -1;
  for (ptrdiff_t i = 0; i < model->release_project_count; i++) {
    const struct Str prefix = model->release_projects[i].tag_prefix;
    if (prefix.len == 0) {
      release->project_index = i;
      const bool has_v = tag_name.len > 1 && tag_name.data[0] == 'v';
      release->version = has_v ? str_tail(tag_name, 1) : tag_name;
      break;
    }
    // No two prefixes of a repo overlap, so at most one fits.
    if (tag_name.len > prefix.len && str_startswith(tag_name, prefix)) {
      release->project_index = i;
      release->version = str_tail(tag_name, prefix.len);
      break;
    }
  }
  if (release->project_index < 0) {
    release->version = tag_name;
  }
  release->is_semver = semver_parse(release->version, &release->semver) == 0;
  if (!release->is_semver) {
    release->semver = (struct SemVer){0};
  }
}

static void release_scan_load_warn(const struct RepoModel* model, struct Log* log) {
  for (ptrdiff_t i = 0; i < model->release_count; i++) {
    const struct Release* release = &model->releases[i];
    const struct Str tag_name = release->tag->short_name;
    if (release->project_index < 0) {
      log_warn(log,
               "release tag fits no project's 'tag_prefix', so no 'versions.txt' lists it: '%.*s' "
               "(in repo '%.*s')",
               STR_FMT(tag_name), STR_FMT(model->config->name));
    } else if (!release->is_semver) {
      log_warn(log,
               "release version is not MAJOR.MINOR.PATCH[-PRE], so 'versions.txt' leaves it out: "
               "'%.*s' (in repo '%.*s', tag '%.*s')",
               STR_FMT(release->version), STR_FMT(model->config->name), STR_FMT(tag_name));
    }
    if (!release->has_sums && release->project_index >= 0 &&
        model->release_projects[release->project_index].is_mise) {
      log_warn(log,
               "release has no '%.*s', so 'mise lock' records no checksums: '%.*s' (in repo "
               "'%.*s')",
               STR_FMT(RELEASE_SCAN_SUMS_NAME), STR_FMT(tag_name), STR_FMT(model->config->name));
    }
  }
}

static void release_scan_load_project_releases(struct RepoModel* model,
                                               struct Arena* perm,
                                               struct Arena scratch,
                                               struct Log* log) {
  for (ptrdiff_t p = 0; p < model->release_project_count; p++) {
    struct ReleaseProject* project = &model->release_projects[p];
    const struct Release** sorted =
        arena_new(&scratch, model->release_count, const struct Release*);
    ptrdiff_t count = 0;
    for (ptrdiff_t i = 0; i < model->release_count; i++) {
      if (model->releases[i].project_index == p) {
        sorted[count++] = &model->releases[i];
      }
    }
    if (count > 0) {
      qsort(sorted, (size_t)count, sizeof(*sorted), compare_releases);
    }
    // A version two tags name, such as `v1.0.0` and `1.0.0`, is listed for the first one only.
    project->releases = arena_new(perm, count, const struct Release*);
    const struct Release** others = arena_new(&scratch, count, const struct Release*);
    ptrdiff_t other_count = 0;
    for (ptrdiff_t i = 0; i < count; i++) {
      const struct Release* release = sorted[i];
      const struct Release* listed =
          project->version_count > 0 ? project->releases[project->version_count - 1] : NULL;
      if (!release->is_semver) {
        others[other_count++] = release;
        continue;
      }
      if (listed != NULL && semver_compare(&release->semver, &listed->semver) == 0) {
        log_warn(log,
                 "release tags name the same version, so 'versions.txt' lists only the first: "
                 "'%.*s', '%.*s' (in repo '%.*s')",
                 STR_FMT(listed->tag->short_name), STR_FMT(release->tag->short_name),
                 STR_FMT(model->config->name));
        others[other_count++] = release;
        continue;
      }
      project->releases[project->version_count++] = release;
    }
    if (other_count > 0) {
      qsort(others, (size_t)other_count, sizeof(*others), compare_release_tags);
    }
    for (ptrdiff_t i = 0; i < other_count; i++) {
      project->releases[project->version_count + i] = others[i];
    }
    project->release_count = count;
  }
}

static int compare_releases(const void* a, const void* b) {
  const struct Release* left = *(const struct Release* const*)a;
  const struct Release* right = *(const struct Release* const*)b;
  if (left->is_semver != right->is_semver) {
    return left->is_semver ? -1 : 1;
  }
  if (left->is_semver) {
    const int order = semver_compare(&right->semver, &left->semver);
    if (order != 0) {
      return order;
    }
  }
  return str_compare(left->tag->short_name, right->tag->short_name);
}

static int compare_release_tags(const void* a, const void* b) {
  const struct Release* left = *(const struct Release* const*)a;
  const struct Release* right = *(const struct Release* const*)b;
  return str_compare(left->tag->short_name, right->tag->short_name);
}

static int compare_assets(const void* a, const void* b) {
  const struct ReleaseAsset* left = a;
  const struct ReleaseAsset* right = b;
  return str_compare(left->name, right->name);
}
