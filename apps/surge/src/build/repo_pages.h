#ifndef SURGE_REPO_PAGES_H
#define SURGE_REPO_PAGES_H

#include <stdbool.h>
#include <stddef.h>

#include "core/arena.h"
#include "core/str.h"

struct JobArenas;
struct Log;
struct ManifestBuilder;
struct Os;
struct OutputCache;
struct OutputWriter;
struct RepoScan;
struct SiteConfig;
struct TemplateSet;

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

  /** Output cache of the previous build, or an empty one. */
  const struct OutputCache* cache;
};

/**
 * @brief Records the output path of every page of every repo: its home, its branches and tags
 *        pages, its tag feed and, with release projects, its release feed, and for each branch a
 *        tree page per directory, a blob page per file, a raw copy per file up to `raw_len_max`,
 *        its log pages and its commit feed, and a commit page per commit that a ref reaches.
 *
 * Each output declares the ids of what it reads as its attributes, so the output writer keeps it
 * while they hold. Every page declares the page recipe and its repo's `repo` map, as
 * `recipe=<id> map=<id>`, and then its own inputs: `repo=` and `refs=` for the home and the refs
 * pages, with `releases=` for the tags page, `feed=` for a feed, or `releases=` for the release
 * feed, `tree=`, with `root=` and `commit=` when they decide it, for a tree page, `blob=` and
 * `mode=`, with `root=`, for a blob page, `log=` for a log page, and `commit=` for a commit page. A
 * raw copy declares only `blob=`.
 *
 * @param builder     Manifest being built. Must not be `NULL`.
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param scans       What the scan found, one slot per repo of `site_config`. Must not be `NULL`.
 * @param recipe_id   Id of the page recipe.
 * @param scratch     Arena for each path and its attributes before the manifest copies them.
 * @param log         Log that receives every refusal. Must not be `NULL`.
 * @return `0` when every path was recorded, or `-1` otherwise.
 */
int repo_pages_register(struct ManifestBuilder* builder,
                        const struct SiteConfig* site_config,
                        const struct RepoScan* scans,
                        struct Str recipe_id,
                        struct Arena scratch,
                        struct Log* log) __attribute__((nonnull(1, 2, 3, 6)));

/**
 * @brief Makes and writes every page of every repo and its raw copies on the worker pool, through
 *        `output_writer_produce`, which keeps each one the output cache vouches for.
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

#endif
