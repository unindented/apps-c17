#ifndef SURGE_REPO_SCAN_H
#define SURGE_REPO_SCAN_H

#include <stdbool.h>
#include <stddef.h>

#include "core/arena.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "vcs/pack_export.h"

struct BuildCache;
struct JobArenas;
struct Log;
struct Os;
struct SiteConfig;

/** What the clone export of one repo writes, and which of it the build keeps. */
struct RepoExport {
  /** Bytes of `<r>.git/HEAD`, naming the default branch. */
  struct Str head;

  /** Bytes of `<r>.git/info/refs`. */
  struct Str info_refs;

  /** Id of `info_refs`, which the build cache records. */
  struct Str info_refs_id;

  /** Bytes of `<r>.git/objects/info/packs`. */
  struct Str packs;

  /** Hex name of the pack this build publishes, or empty for a repo with nothing to export. */
  struct Str pack_id;

  /** Size of the pack in bytes. */
  ptrdiff_t pack_len;

  /** Size of the idx in bytes. */
  ptrdiff_t idx_len;

  /** Whether the pack in the output is current, so the build keeps it without a new one. */
  bool is_pack_kept;

  /** The pack this build wrote below `TMPDIR`, which it streams into the output, when not kept. */
  struct PackExport built;

  /**
   * The pack the build cache names as the one before, which stays one build, unlisted, for a client
   * that read a stale `info/refs`. Empty when there is none, or it is no longer on disk.
   */
  struct Str previous_pack_id;
};

/** What the scan of one repo found. */
struct RepoScan {
  /** What the pages read. */
  struct RepoModel model;

  /** What the clone export writes. */
  struct RepoExport export;

  /** Id of what the repo's pages read besides its refs, which the build cache records. */
  struct Str repo_id;

  /** Id of what the repo's `repo` map shows, from `page_context_repo_text`, which every page of the
      repo reads. */
  struct Str repo_map_id;

  /** Id of what the repo's release outputs read, as `release_scan_id` gives it. */
  struct Str releases_id;
};

/**
 * @brief Opens each repo of `site_config` on the main thread, to find its git directory and its
 *        work tree for the overlap check, before any input tree is walked.
 *
 * @param site_config Loaded configuration. Must not be `NULL`.
 * @param perm        Arena that owns the paths. Must not be `NULL`.
 * @param scans       One slot per repo of `site_config`, zeroed, whose `model` receives `config`,
 *                    `git_dir` and `work_dir`. Must not be `NULL` unless `site_config` has no repo.
 *                    A repo whose name or path the config rejected is skipped, since the load
 *                    reported it, so the overlap check still runs against every other repo.
 *
 * @param log         Log that receives every repo that cannot be opened. Must not be `NULL`.
 * @return `0` when every repo the config named opened, or `-1` otherwise.
 */
int repo_scan_locate(const struct SiteConfig* site_config,
                     struct Arena* perm,
                     struct RepoScan* scans,
                     struct Log* log) __attribute__((nonnull(1, 2, 4)));

/**
 * @brief Scans every repo, one job each on the worker pool: its refs, its default branch and the
 *        date of its newest commit, its description, and its clone export.
 *
 * The clone export keeps the pack in the output when the build cache records the same `info/refs`
 * and the pack and the idx are there with their recorded sizes. Otherwise the job writes a new pack
 * below `TMPDIR`, so the manifest knows its name before the first write.
 *
 * @param site_config   Loaded configuration. Must not be `NULL`.
 * @param cache         Build cache an earlier build left, or an empty one. Must not be `NULL`.
 * @param worker_count  Workers to run the jobs on. Must be positive.
 * @param worker_arenas One perm and one scratch per worker, whose perm keeps what the jobs find.
 *                      Must not be `NULL`.
 * @param is_verbose    Whether each finished job prints a progress line.
 * @param os            Host the output is inspected on. Must not be `NULL`.
 * @param scratch       Arena for the jobs' logs.
 * @param scans         Slots `repo_scan_locate` filled, one per repo. Must not be `NULL` unless
 *                      `site_config` has no repo.
 * @param log           Log that receives every failure. Must not be `NULL`.
 * @return `0` when every repo scanned, or `-1` otherwise.
 */
int repo_scan_run(const struct SiteConfig* site_config,
                  const struct BuildCache* cache,
                  ptrdiff_t worker_count,
                  struct JobArenas* worker_arenas,
                  bool is_verbose,
                  struct Os* os,
                  struct Arena scratch,
                  struct RepoScan* scans,
                  struct Log* log) __attribute__((nonnull(1, 2, 4, 6, 9)));

/**
 * @brief Finds the tree of a scanned repo's default branch.
 *
 * @param model What the scan found for the repo. Must not be `NULL`.
 * @return The tree, or `NULL` for a repo whose default branch has none, such as an empty one.
 */
const struct BranchTree* repo_scan_default_tree(const struct RepoModel* model)
    __attribute__((nonnull(1)));

/**
 * @brief Removes every pack the scan wrote below `TMPDIR`.
 *
 * @param scans      Scanned repos. Must not be `NULL` unless `scan_count` is `0`.
 * @param scan_count Number of entries in `scans`.
 */
void repo_scan_remove_packs(const struct RepoScan* scans, ptrdiff_t scan_count);

#endif
