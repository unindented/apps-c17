#ifndef SURGE_RELEASE_SCAN_H
#define SURGE_RELEASE_SCAN_H

#include <stdbool.h>

#include "core/arena.h"
#include "core/str.h"

struct Log;
struct Os;
struct RepoModel;
struct SurgeRepo;

/** Name of the asset that holds a release's checksums, which mise's `checksum_url` reads. */
#define RELEASE_SCAN_SUMS_NAME S("SHA256SUMS")

/**
 * @brief Reports whether a repo has release projects: a project with a `tag_prefix`, or else a
 *        `releases_dir`, which gives the repo a project of its own.
 *
 * @param repo The repo's table in the config. Must not be `NULL`.
 * @return `true` when the repo has a release project.
 */
bool release_scan_has_projects(const struct SurgeRepo* repo) __attribute__((nonnull(1)));

/**
 * @brief Finds a repo's release projects and releases: each tag that has a directory below the
 *        repo's `releases_dir`, the regular files in it, and the note of its tag, which the scan
 *        read.
 *
 * The release projects are the repo's projects with a `tag_prefix`, in config order, or, for a repo
 * with `releases_dir` and no such project, one named after the repo. A release's version is its tag
 * without its project's prefix, or for the repo's own project without a leading `v`. A release
 * whose tag no prefix fits belongs to no project. Each warns, and keeps its page: a release in no
 * project, a version that is not `MAJOR.MINOR.PATCH[-PRE]`, a version another tag of its project
 * already has, and a release without `SHA256SUMS`.
 *
 * @param model   Scanned repo, whose refs are loaded and which receives `releases` and
 *                `release_projects`. Must not be `NULL`.
 * @param os      Host `releases_dir` is read on. Must not be `NULL`.
 * @param perm    Arena that owns what the model receives. Must not be `NULL`.
 * @param scratch Arena for the paths and the sort.
 * @param log     Log that receives the warnings and every failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when a directory or a file cannot be read.
 */
int release_scan_load(struct RepoModel* model,
                      struct Os* os,
                      struct Arena* perm,
                      struct Arena scratch,
                      struct Log* log) __attribute__((nonnull(1, 2, 3, 5)));

/**
 * @brief Returns the id of what a repo's release outputs read besides the templates and the page
 *        recipe: its release projects and whether each has `mise`, and each release's tag, the
 *        object the tag names, its version, and its assets' names, sizes and modification times.
 *
 * A moved branch leaves it as it is, so the release outputs stay.
 *
 * @param model Scanned repo whose releases are found. Must not be `NULL`.
 * @param arena Arena that owns the id. Must not be `NULL`.
 * @return The id.
 */
struct Str release_scan_id(const struct RepoModel* model, struct Arena* arena)
    __attribute__((nonnull(1, 2)));

#endif
