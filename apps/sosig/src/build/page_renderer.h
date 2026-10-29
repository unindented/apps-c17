#ifndef SOSIG_PAGE_RENDERER_H
#define SOSIG_PAGE_RENDERER_H

#include <stdbool.h>
#include <stddef.h>

#include "core/arena.h"
#include "core/str.h"

struct ContentEntry;
struct JobArenas;
struct Log;
struct Os;
struct OutputCache;
struct OutputWriter;
struct SiteConfig;
struct TemplateSet;

/**
 * @brief Renders each parsed content entry through its content template and writes its page.
 *
 * This is the third of the three parallel phases. When `source_entry_count` is 0, it runs no job
 * and returns `0`. It runs only after `entry_renderer_parse_entries` and
 * `entry_renderer_convert_bodies` succeed and the entries are collected and sorted. Every entry's
 * template sees the whole sorted entry set, so a content template resolves `{{#content_entries}}`
 * exactly as an aggregate template does.
 *
 * One job runs per slot of `source_entries`. Each job renders its entry's page into a copy of its
 * worker's `perm` and its temporaries into a copy of its worker's scratch, and hands the page to
 * `output_writer_produce` for the entry's `output_path`, which keeps an existing file of the same
 * bytes and writes any other. Each job leaves both regions as it found them, so at most one page
 * per worker is held in memory. `NULL` slots are skipped. A failed write is that entry's
 * diagnostic, like a failed render, and leaves the other pages written. Each finished job reports
 * progress when `is_verbose`. Failing jobs' errors go to `log` in entry order, as `job_run`
 * collects them.
 *
 * Call this only after `output_manifest_populate` accepts every output path. Jobs write
 * concurrently, and the manifest is what guarantees that no two of them target the same file, so
 * each job marks only its own path in it.
 *
 * @param writer              Writer every page is written through, by every worker at once. Must
 *                            not be `NULL`.
 * @param cache               Output cache of the previous build, which `output_writer_produce`
 *                            reads. Must not be `NULL`.
 * @param site_config         Site configuration supplying the default content template. Must not be
 *                            `NULL`.
 * @param template_set        Templates every page renders from, read by every worker at once. Must
 *                            not be `NULL`.
 * @param source_entries      Slot array filled by `entry_renderer_parse_entries`, one slot per
 *                            source path, where a draft or a failed source left `NULL`. Read-only
 *                            during the run. May be `NULL` only when `source_entry_count` is 0.
 * @param source_entry_count  Number of slots in `source_entries`, and therefore of jobs.
 * @param content_entries     Non-draft entries, sorted newest-first, visible to every page. May be
 *                            `NULL` only when `content_entry_count` is 0.
 * @param content_entry_count Number of entries in `content_entries`.
 * @param worker_count        Requested worker threads, as for `pool_run`.
 * @param worker_arenas       One `JobArenas` per requested worker, as for `job_run`. Must not be
 *                            `NULL`.
 * @param is_verbose          Whether progress is printed to `stderr`.
 * @param os                  Host progress is printed on, by every worker at once. Must not be
 *                            `NULL`.
 * @param scratch             Arena for the phase's own bookkeeping, as for `job_run`.
 * @param log                 Log that receives the collected render errors. Its arena must be none
 *                            of the others. Must not be `NULL`.
 * @return `0` when every job succeeded, or `-1` when a page failed to render or write or the worker
 *         pool could not start.
 */
int page_renderer_render_pages(const struct OutputWriter* writer,
                               const struct OutputCache* cache,
                               const struct SiteConfig* site_config,
                               const struct TemplateSet* template_set,
                               const struct ContentEntry* const* source_entries,
                               ptrdiff_t source_entry_count,
                               const struct ContentEntry* const* content_entries,
                               ptrdiff_t content_entry_count,
                               ptrdiff_t worker_count,
                               struct JobArenas* worker_arenas,
                               bool is_verbose,
                               struct Os* os,
                               struct Arena scratch,
                               struct Log* log) __attribute__((nonnull(1, 2, 3, 4, 10, 12, 14)));

#endif
