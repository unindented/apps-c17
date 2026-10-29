#ifndef SURGE_REPO_PAGES_H
#define SURGE_REPO_PAGES_H

#include <stdbool.h>
#include <stddef.h>

#include "core/arena.h"
#include "core/str.h"

struct BuildCache;
struct JobArenas;
struct Log;
struct ManifestBuilder;
struct Os;
struct OutputWriter;
struct RepoScan;
struct SiteConfig;
struct TemplateSet;

/** The templates of a repo's pages, which every build compiles. */
#define REPO_PAGES_TEMPLATES                                                                      \
  STR_INIT("tree.html"), STR_INIT("blob.html"), STR_INIT("branches.html"), STR_INIT("tags.html"), \
      STR_INIT("commits.html"), STR_INIT("commit.html")

/** What the pages of one repo left out, which a verbose build notes. */
struct RepoPagesNotes {
  /**
   * Relative links of READMEs and Markdown and Org blobs that named no path, counted on each page
   * rendered this build, so a kept page adds none, and a document that two pages show counts twice.
   */
  ptrdiff_t missing_link_count;

  /** Blobs over `raw_len_max`, which got no raw copy. */
  ptrdiff_t raw_skipped_count;
};

/** What the pages phase reads, shared read-only by its jobs. */
struct RepoPagesInputs {
  /** Configuration of the build. */
  const struct SiteConfig* site_config;

  /** What the scan found, one slot per repo of `site_config`. */
  const struct RepoScan* scans;

  /** Compiled templates. */
  const struct TemplateSet* template_set;

  /** Rows an earlier build left, empty when the build ignores them. */
  const struct BuildCache* cache;

  /** Whether every template is as the build cache recorded it. */
  bool is_templates_fresh;

  /**
   * Whether each repo's row and ref rows are as the build cache recorded them, one per repo, which
   * keeps its home and refs pages.
   */
  const bool* is_repo_fresh;

  /**
   * Whether each repo's releases row is as the build cache recorded it, one per repo, which the
   * tags page also needs to be kept, as it links each tag's release.
   */
  const bool* is_releases_fresh;

  /**
   * Whether each repo's repo map row is as the build cache recorded it, one per repo, which every
   * page of the repo also needs to be kept, as each shows the repo's `repo` map.
   */
  const bool* is_repo_map_fresh;
};

/**
 * @brief Records the output path of every page of every repo: its home, its branches and tags
 *        pages, its tag feed and, with release projects, its release feed, and for each branch a
 *        tree page per directory, a blob page per file, a raw copy per file up to `raw_len_max`,
 *        its log pages and its commit feed, and a commit page per commit that a ref reaches.
 *
 * @param builder     Manifest being built. Must not be `NULL`.
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param scans       What the scan found, one slot per repo of `site_config`. Must not be `NULL`.
 * @param scratch     Arena for each path before the manifest copies it.
 * @param log         Log that receives every refusal. Must not be `NULL`.
 * @return `0` when every path was recorded, or `-1` otherwise.
 */
int repo_pages_register(struct ManifestBuilder* builder,
                        const struct SiteConfig* site_config,
                        const struct RepoScan* scans,
                        struct Arena scratch,
                        struct Log* log) __attribute__((nonnull(1, 2, 3, 5)));

/**
 * @brief Renders and writes every page of every repo on the worker pool, or keeps each one the
 *        build cache vouches for, and its raw copies.
 *
 * The jobs are a repo's own pages, then one per directory of each branch, with the directory's
 * files, one per log page of each branch, with the commit feed beside the first, and one per 16
 * commit pages, in repo order, so a worker's next job is mostly of the repo it has open.
 *
 * @param inputs        What the phase reads. Must not be `NULL`.
 * @param writer        Writer every output goes through. Must not be `NULL`.
 * @param worker_count  Workers the phase runs.
 * @param worker_arenas One `JobArenas` per worker. Must not be `NULL`.
 * @param is_verbose    Whether each job prints a progress line.
 * @param os            Host the progress is printed on. Must not be `NULL`.
 * @param perm          Arena that owns the jobs and the notes. Must not be `NULL`.
 * @param scratch       Arena for the job logs.
 * @param notes_out     Receives what each repo's pages left out, one per repo. Must not be `NULL`.
 * @param log           Log that receives every failure and warning. Must not be `NULL`.
 * @return `0` when every page was written or kept, or `-1` otherwise.
 */
int repo_pages_write(const struct RepoPagesInputs* inputs,
                     const struct OutputWriter* writer,
                     ptrdiff_t worker_count,
                     struct JobArenas* worker_arenas,
                     bool is_verbose,
                     struct Os* os,
                     struct Arena* perm,
                     struct Arena scratch,
                     struct RepoPagesNotes** notes_out,
                     struct Log* log) __attribute__((nonnull(1, 2, 4, 6, 7, 9, 10)));

/**
 * @brief Returns the number of build cache rows `repo_pages_append_rows` appends.
 *
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param scans       What the scan found, one slot per repo of `site_config`. Must not be `NULL`.
 * @return The number of page rows.
 */
ptrdiff_t repo_pages_count_rows(const struct SiteConfig* site_config, const struct RepoScan* scans)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Appends a page row for every tag feed, release feed, tree page, blob page, raw copy, log
 *        page and commit feed, named by its output path and holding the ids of what it reads.
 *
 * @param cache       Cache that receives the rows, with room for them. Must not be `NULL`.
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param scans       What the scan found, one slot per repo of `site_config`. Must not be `NULL`.
 * @param arena       Arena that owns the rows' names and ids. Must not be `NULL`.
 */
void repo_pages_append_rows(struct BuildCache* cache,
                            const struct SiteConfig* site_config,
                            const struct RepoScan* scans,
                            struct Arena* arena) __attribute__((nonnull(1, 2, 3, 4)));

#endif
