#include "build/derivative_renderer.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/error.h"
#include "core/log.h"
#include "core/str.h"
#include "domain/media_item.h"
#include "domain/site_config.h"
#include "media/exif.h"
#include "media/image.h"
#include "media/video.h"
#include "os/oom.h"
#include "os/os.h"
#include "output/output_writer.h"
#include "runtime/job.h"

/**
 * How many times each dimension of a derivative its source region is, at least, after a scaled
 * decode. Reducing straight to a derivative's size leaves the DCT's box-like reduction all of the
 * work, which visibly softens it.
 */
enum { DECODE_SCALE_HEADROOM = 2 };

/**
 * Worker context shared by all derivative jobs. Every field is read-only to a job, apart from the
 * marks each job leaves on its own item's paths in the writer's manifest and its own frame slot,
 * which it creates when the probe did not and then removes.
 */
struct DerivativeContext {
  /** Configured derivative sizes, crops, and qualities. Read-only during the run. */
  const struct SiteConfig* site_config;

  /**
   * Media items to render, one per job index. Read-only during the run: this phase reads the paths
   * and dimensions the probe recorded and writes nothing back into an item.
   */
  struct MediaItem* const* media;

  /**
   * Video frames, one slot per job index. A job reads its own frame, extracting it first when the
   * probe did not, and removes it, which clears the slot's `is_created`, so the job writes only its
   * own slot.
   */
  struct VideoFrame* video_frames;

  /** Host every job reads its source from. */
  struct Os* os;

  /** Writer every job produces its outputs through. */
  const struct OutputWriter* writer;

  /** Cache of the previous build, which tells the writer which outputs it can keep. */
  const struct OutputCache* cache;
};

/**
 * One item's source as its derivatives are made: decoded by the first derivative that the writer
 * makes, and shared by the others, so a source is decoded at most once per job.
 */
struct DerivativeSource {
  /** Shared derivative-phase state. */
  const struct DerivativeContext* context;

  /** Media index of the item. */
  ptrdiff_t index;

  /** Oriented source image at its decode scale, once `is_decoded`. Released by the job. */
  struct Image image;

  /** Whether `image` holds the decoded source. */
  bool is_decoded;

  /** Whether the decode failed, so no other derivative of the item is made. */
  bool is_failed;
};

/** The `userdata` of one derivative's `make_derivative`. */
struct DerivativeMake {
  /** Source the derivative is made from, shared by the item's derivatives. */
  struct DerivativeSource* source;

  /** Index of the derivative in the item and in the configuration. */
  ptrdiff_t derivative_index;

  /** Operation leading the failure message, which names the derivative. */
  struct Str operation;
};

/**
 * @brief Produces one media item's derivatives and its original copy.
 *
 * This is the `JobFn` of the derivative phase, so it runs concurrently with other indexes and
 * writes only the outputs belonging to `index` and its own frame slot. A video's extracted frame is
 * removed before returning on every path. Each encoded derivative is held in a copy of its worker's
 * perm, which the page phase only fills later, and every other allocation goes into a copy of its
 * worker's scratch, except the decode's memory, which stays on `malloc`.
 *
 * @param index    Media index this job handles. Must be below the media count.
 * @param arenas   Arenas of the worker running the job. Must not be `NULL`.
 * @param log      Log of the job, which receives every failure. Must not be `NULL`.
 * @param userdata `struct DerivativeContext*` shared by every worker. Must not be `NULL`.
 * @return `0` on success, or `-1` after recording a failure.
 */
static int render_derivatives_job(ptrdiff_t index,
                                  struct JobArenas* arenas,
                                  struct Log* log,
                                  void* userdata) __attribute__((nonnull(2, 3, 4)));

/**
 * @brief Produces each derivative of one media item and its original copy through the writer.
 *
 * The writer keeps an output its cache vouches for and makes any other, so the source is read,
 * decoded, and oriented only when a derivative is made, and then once for the whole item. A dry run
 * makes no derivative, so it decodes nothing. A failed decode stops the item's derivatives, and any
 * other failure is logged and the job carries on, so a failing derivative does not hide a failing
 * original copy.
 *
 * @param context Shared derivative-phase state. Must not be `NULL`.
 * @param index   Media index to render.
 * @param output  Arena whose free space holds each encoded derivative until it is written.
 * @param scratch Arena for the source bytes and the read of an existing derivative.
 * @param log     Log of the job, which receives every failure. Must not be `NULL`.
 * @return `0` when nothing failed, or `-1` after recording at least one failure.
 */
static int render_derivatives_outputs(const struct DerivativeContext* context,
                                      ptrdiff_t index,
                                      struct Arena output,
                                      struct Arena scratch,
                                      struct Log* log) __attribute__((nonnull(1, 5)));

/**
 * @brief Inspects one media source before its outputs are produced from it.
 *
 * A source that became a directory after the walk is reported here, once, rather than by both the
 * decode and the copy of the original.
 *
 * @param source_path Media source to inspect.
 * @param os          Host the source is on. Must not be `NULL`.
 * @param log         Log of the job, which receives the failure. Must not be `NULL`.
 * @return `0` when the source is a file, or `-1` after recording why it cannot be used.
 */
static int render_derivatives_outputs_inspect(struct Str source_path,
                                              struct Os* os,
                                              struct Log* log) __attribute__((nonnull(2, 3)));

/**
 * @brief Makes one derivative's WebP bytes, as `OutputWriterMakeFn`, decoding the item's source
 *        first when no derivative of the item has.
 *
 * The region is selected inside the oriented source rather than copied out first, and the resized
 * image is released before returning, on success and on failure alike.
 *
 * @param perm      Arena the encoded WebP is held in until it is written. Must not be `NULL`.
 * @param scratch   Arena for the source bytes.
 * @param bytes_out Receives the WebP. Must not be `NULL`.
 * @param log       Log of the job, which receives the failure. Must not be `NULL`.
 * @param userdata  The `struct DerivativeMake`. Must not be `NULL`.
 * @return `0` on success, or `-1` on a decode, resize, or encode failure.
 */
static int make_derivative(struct Arena* perm,
                           struct Arena scratch,
                           struct Str* bytes_out,
                           struct Log* log,
                           void* userdata) __attribute__((nonnull(1, 3, 4, 5)));

/**
 * @brief Reads, decodes, and orients one item's source for all of its derivatives.
 *
 * A video whose frame the probe did not extract, because its probe fact answered for it, has its
 * frame extracted first. The source is decoded at the smallest scale all of its derivatives allow,
 * whichever of them the writer makes, so a derivative's bytes never depend on which others it kept.
 *
 * @param source  Source to decode, which receives the image. Must not be `NULL`.
 * @param scratch Arena for the source bytes.
 * @param log     Log of the job, which receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` after recording a failure.
 */
static int make_derivative_decode(struct DerivativeSource* source,
                                  struct Arena scratch,
                                  struct Log* log) __attribute__((nonnull(1, 3)));

/**
 * @brief Decodes a JPEG source at a scale and applies its orientation.
 *
 * Once decoded, the pixels hold all the derivatives need, so a source of
 * `JOB_SOURCE_DISCARD_LEN_MIN` or more gives its pages back before `image_orient`, which may hold
 * two buffers of the decoded size.
 *
 * @param data                   JPEG bytes of the source image or video frame.
 * @param scale_eighths          Decode scale, from 1 through `IMAGE_SCALE_FULL`.
 * @param orientation            Orientation to apply to the decoded pixels.
 * @param scratch                Arena `data` was read into. Must not be `NULL`.
 * @param image_out              Receives the oriented image, which the caller releases with
 *                               `image_free` on success and failure alike. Must not be `NULL`.
 * @param derivative_source_path Path the JPEG bytes were read from, for diagnostics.
 * @param log                    Log of the job, which receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` after recording a decode failure.
 */
static int decode_oriented(struct Str data,
                           ptrdiff_t scale_eighths,
                           enum ImageOrientation orientation,
                           const struct Arena* scratch,
                           struct Image* image_out,
                           struct Str derivative_source_path,
                           struct Log* log) __attribute__((nonnull(4, 5, 7)));

/**
 * @brief Selects the part of a source that a derivative is resized from.
 *
 * That is the whole source, or for a cropped derivative the largest centered rectangle of the
 * derivative's aspect ratio.
 *
 * @param config_derivative Configured derivative. Must not be `NULL`.
 * @param width_px          Source width in pixels.
 * @param height_px         Source height in pixels.
 * @param x_out             Receives the region's left edge. Must not be `NULL`.
 * @param y_out             Receives the region's top edge. Must not be `NULL`.
 * @param width_out         Receives the region's width. Must not be `NULL`.
 * @param height_out        Receives the region's height. Must not be `NULL`.
 */
static void derivative_source_region(const struct GalleryDerivative* config_derivative,
                                     ptrdiff_t width_px,
                                     ptrdiff_t height_px,
                                     ptrdiff_t* x_out,
                                     ptrdiff_t* y_out,
                                     ptrdiff_t* width_out,
                                     ptrdiff_t* height_out) __attribute__((nonnull(1, 4, 5, 6, 7)));

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
                                 struct Log* log) {
  struct DerivativeContext context = {
      .site_config = site_config,
      .media = media,
      .video_frames = video_frames,
      .os = os,
      .writer = writer,
      .cache = cache,
  };
  return job_run(media_count, worker_count, worker_arenas, render_derivatives_job, &context,
                 S("generating derivatives"), (struct Str){0}, is_verbose, os, scratch, log);
}

ptrdiff_t derivative_renderer_decode_scale(const struct SiteConfig* site_config,
                                           const struct MediaItem* item,
                                           ptrdiff_t width_px,
                                           ptrdiff_t height_px,
                                           enum ImageOrientation orientation) {
  for (ptrdiff_t scale_eighths = 1; scale_eighths < IMAGE_SCALE_FULL; scale_eighths++) {
    ptrdiff_t width_stored = 0;
    ptrdiff_t height_stored = 0;
    image_scale_size(width_px, height_px, scale_eighths, &width_stored, &height_stored);
    ptrdiff_t width = 0;
    ptrdiff_t height = 0;
    image_orient_size(orientation, width_stored, height_stored, &width, &height);
    bool is_enough = true;
    for (ptrdiff_t i = 0; i < item->derivative_count && is_enough; i++) {
      ptrdiff_t region_x = 0;
      ptrdiff_t region_y = 0;
      ptrdiff_t region_width = 0;
      ptrdiff_t region_height = 0;
      derivative_source_region(&site_config->derivatives[i], width, height, &region_x, &region_y,
                               &region_width, &region_height);
      is_enough = region_width >= DECODE_SCALE_HEADROOM * item->derivatives[i].width_px &&
                  region_height >= DECODE_SCALE_HEADROOM * item->derivatives[i].height_px;
    }
    if (is_enough) {
      return scale_eighths;
    }
  }
  return IMAGE_SCALE_FULL;
}

static int render_derivatives_job(ptrdiff_t index,
                                  struct JobArenas* arenas,
                                  struct Log* log,
                                  void* userdata) {
  const struct DerivativeContext* context = userdata;
  oom_set_job(context->media[index]->source_path, log);
  const int rc = render_derivatives_outputs(context, index, arenas->perm, arenas->scratch, log);
  // Best effort is deliberate. A frame whose unlink fails stays marked, so the final cleanup tries
  // again, and a transient unlink failure cannot replace the job's useful first diagnostic.
  video_frame_remove(&context->video_frames[index]);
  return rc;
}

static int render_derivatives_outputs(const struct DerivativeContext* context,
                                      ptrdiff_t index,
                                      struct Arena output,
                                      struct Arena scratch,
                                      struct Log* log) {
  const struct MediaItem* item = context->media[index];
  if (render_derivatives_outputs_inspect(item->source_path, context->os, log) != 0) {
    return -1;
  }

  // `add_media` gives the item one `MediaDerivative` per configured derivative in the same order,
  // which is what lets `make_derivative` read both arrays at the same index.
  struct DerivativeSource source = {.context = context, .index = index};
  int rc = 0;
  for (ptrdiff_t i = 0; i < item->derivative_count && !source.is_failed; i++) {
    // Each derivative's operation lives in a copy of `scratch`, which the next one reuses.
    struct Arena derivative_scratch = scratch;
    struct Buf operation = {.arena = &derivative_scratch};
    buf_printf(&operation, "failed to write derivative '%.*s'", STR_FMT(item->derivatives[i].name));
    struct DerivativeMake make = {
        .source = &source, .derivative_index = i, .operation = buf_str(&operation)};
    if (output_writer_produce(context->writer, item->derivatives[i].path, context->cache,
                              make_derivative, &make, output, derivative_scratch, make.operation,
                              log) != 0) {
      rc = -1;
    }
  }
  if (source.is_decoded) {
    image_free(&source.image);
  }
  if (output_writer_produce(context->writer, item->original_path, context->cache, NULL, NULL,
                            output, scratch, S("failed to copy original"), log) != 0) {
    rc = -1;
  }
  return rc;
}

static int render_derivatives_outputs_inspect(struct Str source_path,
                                              struct Os* os,
                                              struct Log* log) {
  struct OsStat source_stat;
  if (os_stat(os, source_path, &source_stat) != 0) {
    char message[ERROR_SYSTEM_MESSAGE_SIZE];
    return log_error(log, "failed to inspect media source: %s ('%.*s')",
                     error_system_message(message, sizeof(message), errno), STR_FMT(source_path));
  }
  if (source_stat.is_dir) {
    return log_error(log, "failed to inspect media source: not a regular file ('%.*s')",
                     STR_FMT(source_path));
  }
  return 0;
}

static int make_derivative(struct Arena* perm,
                           struct Arena scratch,
                           struct Str* bytes_out,
                           struct Log* log,
                           void* userdata) {
  const struct DerivativeMake* make = userdata;
  struct DerivativeSource* source = make->source;
  if (!source->is_decoded && make_derivative_decode(source, scratch, log) != 0) {
    source->is_failed = true;
    return -1;
  }
  const struct DerivativeContext* context = source->context;
  const struct MediaDerivative* derivative =
      &context->media[source->index]->derivatives[make->derivative_index];
  const struct GalleryDerivative* config_derivative =
      &context->site_config->derivatives[make->derivative_index];
  ptrdiff_t crop_x = 0;
  ptrdiff_t crop_y = 0;
  ptrdiff_t crop_width = 0;
  ptrdiff_t crop_height = 0;
  derivative_source_region(config_derivative, source->image.width_px, source->image.height_px,
                           &crop_x, &crop_y, &crop_width, &crop_height);
  struct Image resized = {0};
  if (image_resize_region(&source->image, crop_x, crop_y, crop_width, crop_height,
                          derivative->width_px, derivative->height_px, &resized, make->operation,
                          derivative->path, log) != 0) {
    return -1;
  }
  const int rc = image_encode_webp(&resized, config_derivative->quality, perm, bytes_out,
                                   make->operation, derivative->path, log);
  image_free(&resized);
  return rc;
}

static int make_derivative_decode(struct DerivativeSource* source,
                                  struct Arena scratch,
                                  struct Log* log) {
  const struct DerivativeContext* context = source->context;
  const struct MediaItem* item = context->media[source->index];
  struct VideoFrame* frame = &context->video_frames[source->index];
  // A video whose probe fact answered for it was not probed, so its frame is extracted here, once a
  // derivative needs it. The job removes it like a probed one.
  if (item->kind == MEDIA_KIND_VIDEO && !frame->is_created &&
      video_extract_frame(
          frame, item->source_path,
          video_frame_position_ms(context->site_config->video_frame_seconds, item->duration_ms),
          scratch, S("failed to extract video frame"), log) != 0) {
    return -1;
  }
  const struct Str derivative_source_path =
      item->kind == MEDIA_KIND_VIDEO ? str_from_cstr(frame->path) : item->source_path;
  // The bytes stay in `scratch` until the decode, which gives a large source's pages back. The
  // decode allocates nothing in an arena.
  struct Str data = {0};
  if (os_read(context->os, derivative_source_path, IMAGE_INPUT_LEN_MAX, OS_READ_BYTES, &scratch,
              &data, S("failed to read derivative source"), log) != 0) {
    return -1;
  }
  struct ImageHeader header;
  if (image_probe(data, &header, S("failed to decode derivative source"), derivative_source_path,
                  log) != 0) {
    return -1;
  }

  // An image source is oriented by its EXIF tag. A video frame is already upright, because it was
  // extracted auto-rotated.
  const enum ImageOrientation orientation =
      item->kind == MEDIA_KIND_IMAGE ? exif_read_orientation(data) : IMAGE_ORIENTATION_TOP_LEFT;
  const ptrdiff_t scale_eighths = derivative_renderer_decode_scale(
      context->site_config, item, header.width_px, header.height_px, orientation);
  if (decode_oriented(data, scale_eighths, orientation, &scratch, &source->image,
                      derivative_source_path, log) != 0) {
    image_free(&source->image);
    return -1;
  }
  source->is_decoded = true;
  return 0;
}

static int decode_oriented(struct Str data,
                           ptrdiff_t scale_eighths,
                           enum ImageOrientation orientation,
                           const struct Arena* scratch,
                           struct Image* image_out,
                           struct Str derivative_source_path,
                           struct Log* log) {
  if (image_decode(data, scale_eighths, image_out, S("failed to decode derivative source"),
                   derivative_source_path, log) != 0) {
    return -1;
  }
  // The decode was the last read of the source. The worker's scratch outlives the job, so a large
  // source's pages would otherwise stay resident beside other workers' decodes and beside a rotated
  // image's second buffer.
  if (data.len >= JOB_SOURCE_DISCARD_LEN_MIN) {
    arena_discard(scratch, data.data, data.len);
  }
  image_orient(image_out, orientation);
  return 0;
}

static void derivative_source_region(const struct GalleryDerivative* config_derivative,
                                     ptrdiff_t width_px,
                                     ptrdiff_t height_px,
                                     ptrdiff_t* x_out,
                                     ptrdiff_t* y_out,
                                     ptrdiff_t* width_out,
                                     ptrdiff_t* height_out) {
  *x_out = 0;
  *y_out = 0;
  *width_out = width_px;
  *height_out = height_px;
  if (config_derivative->is_crop) {
    image_center_crop(width_px, height_px, config_derivative->width_px,
                      config_derivative->height_px, x_out, y_out, width_out, height_out);
  }
}
