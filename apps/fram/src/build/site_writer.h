#ifndef FRAM_SITE_WRITER_H
#define FRAM_SITE_WRITER_H

#include <stddef.h>

#include "core/arena.h"
#include "core/str.h"

struct Album;
struct Log;
struct SiteConfig;
struct OutputWriter;
struct RenderedPage;
struct TemplateSet;

// These are declared in the order `cmd_build` runs them. The manifest phase in `output_manifest`
// claims every intended output path before this module writes anything.

/**
 * @brief Writes each already-rendered album page through `writer` to its planned output path.
 *
 * Call this only once `page_renderer_render_pages` returns `0`, which guarantees that every
 * `rendered_pages[i].html` is filled. An unfilled slot is a caller error, not a skipped album.
 *
 * @param writer         Writer every page is written through. Must not be `NULL`.
 * @param albums         Albums whose `output_path` receives the matching page. Must not be `NULL`.
 * @param album_count    Number of albums in `albums`, and of slots in `rendered_pages`.
 * @param rendered_pages Rendered page per album, parallel to `albums`. Every `html` must be filled.
 *                       Borrowed. Must not be `NULL`.
 * @param scratch        Arena for the failure message's operation.
 * @param log            Log that receives the first failure, which names the album and the path
 *                       that could not be written. Must not be `NULL`.
 * @return `0` on success, or `-1` on the first write failure.
 */
int site_writer_write_album_pages(const struct OutputWriter* writer,
                                  const struct Album* const* albums,
                                  ptrdiff_t album_count,
                                  const struct RenderedPage* rendered_pages,
                                  struct Arena scratch,
                                  struct Log* log) __attribute__((nonnull(1, 2, 4, 6)));

/**
 * @brief Renders each configured aggregate template and writes it through `writer` below the output
 *        directory.
 *
 * Each aggregate renders with the root album's data as `album_current`, but with `path_to_root`
 * rebased onto the aggregate's own output depth, so a page-relative URL resolves from the directory
 * the aggregate is written to.
 *
 * @param writer       Writer every output is written through. Must not be `NULL`.
 * @param site_config  Configuration supplying `output_dir` and the aggregate template list. Must
 *                     not be `NULL`.
 * @param template_set Templates the aggregates render from. Must not be `NULL`.
 * @param root_album   Root album whose data every aggregate renders against. Must not be `NULL`.
 * @param output       Arena whose free space holds each rendered output until it is written.
 *                     Nothing in it outlives the call. Must not overlap `scratch`.
 * @param scratch      Arena for each render's temporaries.
 * @param log          Log that receives the first failure. Its arena must be none of the others.
 *                     Must not be `NULL`.
 * @return `0` on success, including when no aggregate is configured, or `-1` on the first template
 *         that fails to render or write.
 */
int site_writer_write_aggregates(const struct OutputWriter* writer,
                                 const struct SiteConfig* site_config,
                                 const struct TemplateSet* template_set,
                                 const struct Album* root_album,
                                 struct Arena output,
                                 struct Arena scratch,
                                 struct Log* log) __attribute__((nonnull(1, 2, 3, 4, 7)));

#endif
