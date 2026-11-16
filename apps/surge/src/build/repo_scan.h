#ifndef SURGE_REPO_SCAN_H
#define SURGE_REPO_SCAN_H

#include <stdbool.h>
#include <stddef.h>

#include "core/arena.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "output/output_cache.h"
#include "vcs/pack_export.h"

struct JobArenas;
struct Log;
struct Manifest;
struct SiteConfig;

/** What the clone export of one repo writes, and which of it the build keeps. */
struct RepoExport {
  /** Bytes of `<r>.git/HEAD`, naming the default branch. */
  struct Str head;

  /** Bytes of `<r>.git/info/refs`. */
  struct Str info_refs;

  /** Id of `info_refs`, which the pack is made from and the output cache records. */
  struct Str info_refs_id;

  /** Bytes of `<r>.git/objects/info/packs`. */
  struct Str packs;

  /**
   * Hex name of the pack this build publishes, or empty for a repo with nothing to export. A dry
   * run does not make a pack whose refs changed, so it names that pack by `info_refs_id`.
   */
  struct Str pack_id;

  /** Size of the pack in bytes, or `0` for a pack that a dry run did not make. */
  ptrdiff_t pack_len;

  /**
   * Whether the pack in the output is current, so the build keeps it without a new one. Its pack
   * and its idx then declare the attributes the output cache recorded for them.
   */
  bool is_pack_kept;

  /** Attributes the output cache recorded for the kept pack. */
  struct Str pack_attributes;

  /** Attributes the output cache recorded for the kept pack's idx. */
  struct Str idx_attributes;

  /** The pack this build wrote below `TMPDIR`, which it streams into the output, when not kept. */
  struct PackExport built;

  /**
   * The pack that was current in the build before, which stays one build, unlisted, for a client
   * that read a stale `info/refs`. Empty when there is none, or it is no longer on disk.
   */
  struct Str previous_pack_id;

  /** Id of the `info/refs` the pack before was made for, which the output cache recorded. */
  struct Str previous_info_refs_id;

  /** Attributes the output cache recorded for the pack before. */
  struct Str previous_pack_attributes;

  /** Attributes the output cache recorded for the pack before's idx. */
  struct Str previous_idx_attributes;
};

/** What the scan of one repo found. */
struct RepoScan {
  /** What the pages read. */
  struct RepoModel model;

  /** What the clone export writes. */
  struct RepoExport export;

  /** Id of what the repo's pages read besides its refs, which its home and refs pages and the site
   * index declare.
   */
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
 * @param log         Log that receives every repo that cannot be opened. Must not be `NULL`.
 * @return `0` when every repo the config named opened, or `-1` otherwise.
 */
int repo_scan_locate(const struct SiteConfig* site_config,
                     struct Arena* perm,
                     struct RepoScan* scans,
                     struct Log* log) __attribute__((nonnull(1, 2, 4)));

/**
 * @brief Scans every repo, one job each on the worker pool: its refs, its default branch and the
 *        date of its newest commit, its description, the author time and LLM flag of every commit
 *        of its default branch, and its clone export.
 *
 * The clone export keeps the pack in the output when the output cache's fact for the export records
 * the same `info/refs`, and the cache records the pack and the idx, which are there with their
 * recorded sizes. Otherwise the job writes a new pack below `TMPDIR`, so the manifest knows its
 * name before the first write, except in a dry run, which makes no pack.
 *
 * @param site_config   Loaded configuration. Must not be `NULL`.
 * @param cache         Output cache an earlier build left, or an empty one. Must not be `NULL`.
 * @param is_dry_run    Whether the build is a dry run.
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
                  const struct OutputCache* cache,
                  bool is_dry_run,
                  ptrdiff_t worker_count,
                  struct JobArenas* worker_arenas,
                  bool is_verbose,
                  struct Os* os,
                  struct Arena scratch,
                  struct RepoScan* scans,
                  struct Log* log) __attribute__((nonnull(1, 2, 5, 7, 10)));

/**
 * @brief Returns the output cache's fact for one repo's clone export: key `<repo>.git`, value
 *        `pack=<id> refs=<id>`, a pack in the output and the id of the `info/refs` it was made for.
 *
 * The pack is the one this build publishes once `manifest` marks its pack and its idx, and
 * otherwise the pack before it, which the build keeps before its first page. So a build that fails
 * before it writes the new pack leaves the old one known, and the next build keeps it one build
 * while it publishes the new `info/refs`.
 *
 * @param scan       Scanned repo. Must not be `NULL`.
 * @param manifest   Manifest of the build, whose marks say what it wrote or kept. Must not be
 *                   `NULL`.
 * @param output_dir Output directory the manifest's paths are below.
 * @param arena      Arena that owns the fact. Must not be `NULL`.
 * @return The fact, whose key is empty when neither pack is in the output.
 */
struct OutputCacheFact repo_scan_export_fact(const struct RepoScan* scan,
                                             const struct Manifest* manifest,
                                             struct Str output_dir,
                                             struct Arena* arena) __attribute__((nonnull(1, 2, 4)));

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
