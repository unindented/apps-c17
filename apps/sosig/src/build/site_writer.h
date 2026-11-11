#ifndef SOSIG_SITE_WRITER_H
#define SOSIG_SITE_WRITER_H

#include <stdbool.h>
#include <stddef.h>

#include "core/arena.h"
#include "core/str.h"

struct ContentEntry;
struct Log;
struct OutputCache;
struct OutputWriter;
struct SiteConfig;
struct TemplateSet;

// The manifest phase in `output_manifest` claims every intended output path before this module
// writes anything. Content pages are written earlier, by `page_renderer_render_pages`. Ordering the
// entries happens earlier still, in `content_entry` (`content_entry_sort`).

/**
 * @brief Renders each of `template_names` with `content_entries` visible and hands it to
 *        `output_writer_produce` for its matching path under `output_dir`.
 *
 * `cmd_build` calls this once for the aggregate templates, with every non-draft entry, and once for
 * the feed templates, with the newest `feed_count`. Every output is rendered, and an existing file
 * of the same bytes is kept.
 *
 * @param writer              Writer every output is written through. Must not be `NULL`.
 * @param cache               Output cache of the previous build, which `output_writer_produce`
 *                            reads. Must not be `NULL`.
 * @param site_config         Configuration supplying `output_dir`. Must not be `NULL`.
 * @param template_set        Templates the outputs render from. Must not be `NULL`.
 * @param template_names      Array of `template_count` safe relative template names. May be `NULL`
 *                            only when `template_count` is 0.
 * @param template_count      Number of entries in `template_names`.
 * @param content_entries     Content entries, sorted newest-first, visible to the templates. May be
 *                            `NULL` only when `content_entry_count` is 0.
 * @param content_entry_count Number of entries in `content_entries`.
 * @param is_feed             Whether the templates are feed templates, whose links are absolute.
 * @param output              Arena whose free space holds each rendered output until it is written.
 *                            Nothing in it outlives the call. Must not overlap `scratch`.
 * @param scratch             Arena for each render's temporaries, and then for the read of the
 *                            existing output it is compared with.
 * @param log                 Log that receives the first failure. Its arena must be none of the
 *                            others. Must not be `NULL`.
 * @return `0` on success, including when `template_count` is 0, or `-1` on the first template that
 *         fails to render or write.
 */
int site_writer_write_templates(const struct OutputWriter* writer,
                                const struct OutputCache* cache,
                                const struct SiteConfig* site_config,
                                const struct TemplateSet* template_set,
                                const struct Str* template_names,
                                ptrdiff_t template_count,
                                const struct ContentEntry* const* content_entries,
                                ptrdiff_t content_entry_count,
                                bool is_feed,
                                struct Arena output,
                                struct Arena scratch,
                                struct Log* log) __attribute__((nonnull(1, 2, 3, 4, 12)));

/**
 * @brief Renders `templates.not_found` and hands it to `output_writer_produce` for `not_found_path`
 *        under `output_dir`.
 *
 * The page sees `site`, `generator` and a `path_to_root` that is the root-relative path of
 * `base_url`, from `output_path_not_found_root`, because a host serves it at every depth. It sees
 * no entries. Like every other output, it is rendered, and an existing file of the same bytes is
 * kept.
 *
 * @param writer       Writer the page is written through. Must not be `NULL`.
 * @param cache        Output cache of the previous build, which `output_writer_produce` reads. Must
 *                     not be `NULL`.
 * @param site_config  Configuration supplying `output_dir`, `base_url`, `not_found_path` and the
 *                     template. Must not be `NULL`.
 * @param template_set Templates the page renders from. Must not be `NULL`.
 * @param output       Arena whose free space holds the page until it is written. Nothing in it
 *                     outlives the call. Must not overlap `scratch`.
 * @param scratch      Arena for the render's temporaries, and then for the read of the existing
 *                     output it is compared with.
 * @param log          Log that receives the failure. Its arena must be none of the others. Must not
 *                     be `NULL`.
 * @return `0` on success, or `-1` when the page fails to render or write.
 */
int site_writer_write_not_found(const struct OutputWriter* writer,
                                const struct OutputCache* cache,
                                const struct SiteConfig* site_config,
                                const struct TemplateSet* template_set,
                                struct Arena output,
                                struct Arena scratch,
                                struct Log* log) __attribute__((nonnull(1, 2, 3, 4, 7)));

#endif
