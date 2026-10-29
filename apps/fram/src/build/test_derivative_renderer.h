#ifndef FRAM_TEST_DERIVATIVE_RENDERER_H
#define FRAM_TEST_DERIVATIVE_RENDERER_H

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>

#include "build/derivative_renderer.h"
#include "build/test_job_arenas.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/path.h"
#include "core/str.h"
#include "domain/media_item.h"
#include "domain/site_config.h"
#include "os/os.h"
#include "output/manifest.h"
#include "output/output_cache.h"
#include "output/output_writer.h"
#include "testing/test_support.h"

// This is header-only and `static inline` so that the image and the video tests of the derivative
// phase, two executables, share one configuration, one manifest and one run without a separate test
// library to link.

/** How a test runs the derivative phase, as the build's options would set its writer. */
struct TestDerivativeMode {
  /** Whether the writer withholds its writes. */
  bool is_dry_run;

  /** Whether the writer keeps nothing. */
  bool is_forced;
};

/**
 * @brief Initializes a configuration with the three derivatives each test's `init_item` plans.
 *
 * `s` and `m` crop, and `l` fits. The derivative array is static, so the configuration needs no
 * release and only one may be live at a time.
 *
 * @param site_config  Configuration to initialize. Must not be `NULL`.
 * @param small_width  Width of the `s` derivative in pixels.
 * @param small_height Height of the `s` derivative in pixels.
 * @param large_width  Width bound of the `l` derivative in pixels.
 * @param large_height Height bound of the `l` derivative in pixels.
 * @param quality      WebP quality shared by every derivative, from 1 through 100.
 */
static inline void test_derivative_renderer_init_config(struct SiteConfig* site_config,
                                                        ptrdiff_t small_width,
                                                        ptrdiff_t small_height,
                                                        ptrdiff_t large_width,
                                                        ptrdiff_t large_height,
                                                        ptrdiff_t quality)
    __attribute__((nonnull(1)));

static inline void test_derivative_renderer_init_config(struct SiteConfig* site_config,
                                                        ptrdiff_t small_width,
                                                        ptrdiff_t small_height,
                                                        ptrdiff_t large_width,
                                                        ptrdiff_t large_height,
                                                        ptrdiff_t quality) {
  static struct GalleryDerivative derivatives[3];
  derivatives[0] = (struct GalleryDerivative){.name = S("s"),
                                              .width_px = small_width,
                                              .height_px = small_height,
                                              .quality = quality,
                                              .is_crop = true};
  derivatives[1] = (struct GalleryDerivative){
      .name = S("m"), .width_px = 8, .height_px = 4, .quality = quality, .is_crop = true};
  derivatives[2] = (struct GalleryDerivative){.name = S("l"),
                                              .width_px = large_width,
                                              .height_px = large_height,
                                              .quality = quality,
                                              .is_crop = false};
  *site_config = (struct SiteConfig){
      .derivatives = derivatives,
      .derivative_count = 3,
  };
}

/**
 * @brief Returns the path of the output cache below `output_dir`, as a build places it.
 *
 * @param output_dir Output directory of the test.
 * @param arena      Arena that owns the path. Must not be `NULL`.
 * @return The path.
 */
static inline struct Str test_derivative_renderer_cache_path(struct Str output_dir,
                                                             struct Arena* arena)
    __attribute__((nonnull(2)));

static inline struct Str test_derivative_renderer_cache_path(struct Str output_dir,
                                                             struct Arena* arena) {
  return path_join(output_dir, S("_fram/output-cache.tsv"), arena);
}

/**
 * @brief Returns the attributes a build declares for an output made or copied from a source:
 *        `len=<n> mtime=<s>.<ns>`.
 *
 * @param source_stat What `os_stat` reported for the source. Must not be `NULL`.
 * @param arena       Arena that owns the text. Must not be `NULL`.
 * @return The attributes.
 */
static inline struct Str test_derivative_renderer_attributes(const struct OsStat* source_stat,
                                                             struct Arena* arena)
    __attribute__((nonnull(1, 2)));

static inline struct Str test_derivative_renderer_attributes(const struct OsStat* source_stat,
                                                             struct Arena* arena) {
  struct Buf attributes = {.arena = arena};
  buf_printf(&attributes, "len=%td mtime=%" PRId64 ".%09" PRId64, source_stat->len,
             source_stat->mtime_s, source_stat->mtime_ns);
  return buf_str(&attributes);
}

/**
 * @brief Records every derivative and original path of `media` in `manifest`, as the build's
 *        manifest records them, so the phase's writer accepts each one, and the output cache.
 *
 * A derivative declares its source's size and modification time, and an original is a copy of its
 * source. Items that share a path record it once.
 *
 * @param manifest    Manifest that receives the paths. Must not be `NULL`.
 * @param media       Media items whose outputs are recorded, each with its `source_stat`. Must not
 *                    be `NULL`.
 * @param media_count Number of items in `media`.
 * @param cache_path  Path of the output cache.
 * @param arena       Arena that owns the manifest's entries. Must not be `NULL`.
 */
static inline void test_derivative_renderer_record_outputs(struct Manifest* manifest,
                                                           struct MediaItem* const* media,
                                                           ptrdiff_t media_count,
                                                           struct Str cache_path,
                                                           struct Arena* arena)
    __attribute__((nonnull(1, 2, 5)));

static inline void test_derivative_renderer_record_outputs(struct Manifest* manifest,
                                                           struct MediaItem* const* media,
                                                           ptrdiff_t media_count,
                                                           struct Str cache_path,
                                                           struct Arena* arena) {
  for (ptrdiff_t i = 0; i < media_count; i++) {
    const struct Str attributes =
        test_derivative_renderer_attributes(&media[i]->source_stat, arena);
    const struct ManifestOutput derivative = {.attributes = attributes};
    for (ptrdiff_t j = 0; j < media[i]->derivative_count; j++) {
      (void)manifest_add_output(manifest, media[i]->derivatives[j].path, media[i]->source_path,
                                &derivative, arena, NULL);
    }
    const struct ManifestOutput original = {.attributes = attributes,
                                            .copy_source_path = media[i]->source_path};
    (void)manifest_add_output(manifest, media[i]->original_path, media[i]->source_path, &original,
                              arena, NULL);
  }
  (void)manifest_add(manifest, cache_path, S("output cache"), arena, NULL);
}

/**
 * @brief Runs one build's derivative phase below `output_dir`: stats each source, reads the output
 *        cache an earlier run left, runs the phase over fresh worker arenas, and writes the cache
 *        again, as a build does.
 *
 * @param site_config  Configured derivatives shared by every job. Must not be `NULL`.
 * @param media        Media items, one job per item, whose `source_stat` this fills. Must not be
 *                     `NULL`.
 * @param media_count  Number of items in `media`.
 * @param video_frames Video frame slots, one per item in `media`. Must not be `NULL`.
 * @param worker_count Number of worker threads to request.
 * @param is_verbose   Whether each finished job prints a progress line.
 * @param mode         How the writer runs. Must not be `NULL`.
 * @param output_dir   Directory every output of `media` is below.
 * @param os           Host holding the sources, which receives the outputs. Must not be `NULL`.
 * @param manifest     Empty manifest that receives every output path and the marks. Must not be
 *                     `NULL`.
 * @param log          Log that receives derivative errors. Must not be `NULL`.
 * @return The renderer result.
 */
static inline int test_derivative_renderer_run(const struct SiteConfig* site_config,
                                               struct MediaItem* const* media,
                                               ptrdiff_t media_count,
                                               struct VideoFrame* video_frames,
                                               ptrdiff_t worker_count,
                                               bool is_verbose,
                                               const struct TestDerivativeMode* mode,
                                               struct Str output_dir,
                                               struct Os* os,
                                               struct Manifest* manifest,
                                               struct Log* log)
    __attribute__((nonnull(1, 2, 4, 7, 9, 10, 11)));

static inline int test_derivative_renderer_run(const struct SiteConfig* site_config,
                                               struct MediaItem* const* media,
                                               ptrdiff_t media_count,
                                               struct VideoFrame* video_frames,
                                               ptrdiff_t worker_count,
                                               bool is_verbose,
                                               const struct TestDerivativeMode* mode,
                                               struct Str output_dir,
                                               struct Os* os,
                                               struct Manifest* manifest,
                                               struct Log* log) {
  struct Arena perm = test_arena();
  for (ptrdiff_t i = 0; i < media_count; i++) {
    if (os_stat(os, media[i]->source_path, &media[i]->source_stat) != 0) {
      media[i]->source_stat = (struct OsStat){.len = -1};
    }
  }
  const struct Str cache_path = test_derivative_renderer_cache_path(output_dir, &perm);
  test_derivative_renderer_record_outputs(manifest, media, media_count, cache_path, &perm);
  struct OutputCache cache = {0};
  struct Str reason = {0};
  if (!mode->is_forced) {
    (void)output_cache_load(cache_path, os, &perm, test_arena(), &cache, &reason);
  }
  const struct OutputWriter writer = {
      .manifest = manifest,
      .os = os,
      .output_dir = output_dir,
      .is_dry_run = mode->is_dry_run,
      .is_forced = mode->is_forced,
  };
  const int rc = derivative_renderer_generate(
      &writer, &cache, site_config, media, media_count, video_frames, worker_count,
      test_job_arenas(worker_count, &perm), is_verbose, os, test_arena(), log);
  // A build writes the cache after a failed phase too, so the next run keeps what this one made.
  struct Log cache_log = test_log();
  (void)output_cache_write(&writer, cache_path, NULL, 0, test_arena(), &cache_log);
  return rc;
}

#endif
