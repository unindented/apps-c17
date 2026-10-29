#ifndef FRAM_DERIVATIVE_RENDERER_H
#define FRAM_DERIVATIVE_RENDERER_H

#include <stdbool.h>
#include <stddef.h>

#include "core/arena.h"
#include "media/image.h"

struct SiteConfig;
struct JobArenas;
struct Log;
struct MediaItem;
struct Os;
struct OutputCache;
struct OutputWriter;
struct VideoFrame;

/**
 * @brief Produces every derivative and original copy in parallel.
 *
 * This is the second of the three parallel phases. When `media_count` is 0, it runs no job and
 * returns `0`. One job owns each media item. `video_frames[index]` supplies the auto-rotated JPEG
 * the probe extracted for a video. A video whose probe fact answered for it has no frame yet, and
 * its job extracts one only when a derivative is made. Every video frame is removed after its job,
 * including on failure, best effort: a frame whose unlink fails stays marked for the caller's final
 * cleanup. A job reads its source into a copy of its worker's scratch, holds each encoded
 * derivative in a copy of its worker's perm, and allocates its decoded pixels with `malloc`, under
 * a budget shared by every worker.
 *
 * Every output goes through `output_writer_produce`, which keeps a derivative or an original copy
 * that `cache` vouches for and makes or copies any other. A source is decoded once for all of the
 * derivatives the writer makes, and not at all when it makes none, as in a dry run.
 *
 * Call this only after `output_manifest_populate` accepts every output path. Jobs write
 * concurrently, and the manifest is what guarantees that no two of them target the same file, so
 * each job marks only its own item's paths in it.
 *
 * Each finished job reports progress when `is_verbose`. Failing jobs' errors go to `log` in media
 * order, as `job_run` collects them.
 *
 * @param writer        Writer every output is written through, by every worker at once. Must not be
 *                      `NULL`.
 * @param cache         Cache of the previous build, read by every worker at once. Must not be
 *                      `NULL`.
 * @param site_config   Configured derivative sizes, crops, and qualities shared by every job. Must
 *                      not be `NULL`.
 * @param media         Probed media items, one job per item. Must not be `NULL`.
 * @param media_count   Number of items in `media`.
 * @param video_frames  Frame slots, one per item in `media`, filled by the probe for a video it
 *                      probed, and by the job for one whose probe fact answered for it. Must not be
 *                      `NULL`.
 * @param worker_count  Requested worker threads, as for `pool_run`.
 * @param worker_arenas One `JobArenas` per requested worker, as for `job_run`. Must not be `NULL`.
 * @param is_verbose    Whether progress is printed to `stderr`.
 * @param os            Host the sources, frames and outputs are read from and progress printed on,
 *                      by every worker at once. Must not be `NULL`.
 * @param scratch       Arena for the phase's own bookkeeping, as for `job_run`.
 * @param log           Log that receives the collected derivative errors. Its arena must be none of
 *                      the others. Must not be `NULL`.
 * @return `0` when every job succeeded, or `-1` when a derivative job failed or the worker pool
 *         could not start.
 */
int derivative_renderer_generate(const struct OutputWriter* writer,
                                 const struct OutputCache* cache,
                                 const struct SiteConfig* site_config,
                                 struct MediaItem* const* media,
                                 ptrdiff_t media_count,
                                 struct VideoFrame* video_frames,
                                 ptrdiff_t worker_count,
                                 struct JobArenas* worker_arenas,
                                 bool is_verbose,
                                 struct Os* os,
                                 struct Arena scratch,
                                 struct Log* log)
    __attribute__((nonnull(1, 2, 3, 4, 6, 8, 10, 12)));

/**
 * @brief Returns the scale a source is decoded at to write its derivatives.
 *
 * This is the smallest eighth at which every derivative's source region, cropped as configured, is
 * still at least twice the derivative in each dimension, so the resizer, and not the decoder's DCT
 * reduction, does the final reduction. A source too small for that at any reduced scale decodes at
 * full size. The scale counts every derivative, whichever the writer makes, so a derivative's bytes
 * do not depend on which of the others were kept.
 *
 * @param site_config Configured derivatives. Must not be `NULL`.
 * @param item        Media item, with one derivative per configured one. Must not be `NULL`.
 * @param width_px    Stored width of the source, as its JPEG header gives it.
 * @param height_px   Stored height of the source, as its JPEG header gives it.
 * @param orientation Orientation the decoded source is given.
 * @return The decode scale, in eighths, from 1 through `IMAGE_SCALE_FULL`.
 */
ptrdiff_t derivative_renderer_decode_scale(const struct SiteConfig* site_config,
                                           const struct MediaItem* item,
                                           ptrdiff_t width_px,
                                           ptrdiff_t height_px,
                                           enum ImageOrientation orientation)
    __attribute__((nonnull(1, 2)));

#endif
