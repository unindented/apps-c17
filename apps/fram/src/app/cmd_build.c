#include "app/cmd_build.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "build/album_scan.h"
#include "build/derivative_renderer.h"
#include "build/output_manifest.h"
#include "build/page_renderer.h"
#include "build/site_writer.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/path.h"
#include "core/path_list.h"
#include "core/str.h"
#include "domain/album.h"
#include "domain/media_item.h"
#include "domain/site_config.h"
#include "media/image.h"
#include "media/proc.h"
#include "media/video.h"
#include "os/oom.h"
#include "os/os.h"
#include "output/build_cache_file.h"
#include "output/manifest.h"
#include "output/output_cache.h"
#include "output/output_pruner.h"
#include "output/output_writer.h"
#include "runtime/build_run.h"
#include "runtime/job.h"
#include "runtime/read_root.h"
#include "runtime/verbose.h"
#include "runtime/walk.h"
#include "templates/template_set.h"

/** Bytes in a mebibyte, the unit of the sizes below. */
#define BUILD_MIB ((ptrdiff_t)1024 * 1024)

_Static_assert(PTRDIFF_MAX > 0xffffffff, "the arenas below need a 64-bit address space");

/** Bytes of the build's log, and of each worker's share of the job logs. */
#define BUILD_LOG_LEN (256 * BUILD_MIB)

/** Bytes of each thread's perm, which holds its paths, an encoded derivative or rendered pages. */
#define BUILD_PERM_LEN (16 * (ptrdiff_t)TEMPLATE_RENDER_OUTPUT_LEN_MAX)
_Static_assert(BUILD_PERM_LEN > (ptrdiff_t)IMAGE_ENCODED_LEN_MAX,
               "a worker's perm must hold an encoded derivative");

/** Bytes of each thread's scratch, which holds one job's source beside its encoded derivative. */
#define BUILD_SCRATCH_LEN (3 * 1024 * BUILD_MIB)
_Static_assert(BUILD_SCRATCH_LEN > (ptrdiff_t)IMAGE_INPUT_LEN_MAX + IMAGE_ENCODED_LEN_MAX,
               "a worker's scratch must hold a source beside its encoded derivative");

/** Mutable state owned by one `cmd_build_execute` invocation. */
struct BuildState {
  /** Host every phase reads from, writes to and prints on. */
  struct Os* os;

  /**
   * The main thread's perm, the caller's. It receives the configuration, the source and static
   * paths, the skeleton, the frame slots and the manifest.
   */
  struct Arena perm;

  /**
   * The main thread's scratch, the caller's. Each phase receives a copy, so nothing a phase
   * allocates here outlives it, and a parallel phase's copy holds its job logs.
   */
  struct Arena scratch;

  /**
   * One perm and one scratch per worker, `worker_count` of them. A worker's perm holds the album
   * pages it rendered, so it must not be reset before the write phase reads them.
   */
  struct JobArenas* worker_arenas;

  /** Config file the build reads. */
  struct Str config_path;

  /** Loaded gallery configuration. Its values live in `perm`. */
  struct SiteConfig site_config;

  /** Compiled templates, released when the build ends, or `NULL` until they load. */
  struct TemplateSet* template_set;

  /** Discovered media source paths below `media_dir`, sorted by the walk that produced them. */
  struct PathList source_paths;

  /** Discovered static file paths below `static_dir`, left empty when that directory is absent. */
  struct PathList static_paths;

  /**
   * What `walk_stat_paths` reported for each of `static_paths`, parallel to it, in `perm`, which
   * the copies' attributes name.
   */
  struct OsStat* static_stats;

  /** Every file below `templates_dir`, sorted, which the manifest claims as inputs. */
  struct PathList template_paths;

  /** Intended output paths for this build, populated before any file is written. */
  struct Manifest manifest;

  /** Writer every change under `output_dir` goes through, which marks `manifest`. */
  struct OutputWriter writer;

  /** Scanned albums sorted by `source_dir`, with the root album at index `0`, in `perm`. */
  struct Album** albums;

  /** Number of albums in `albums`, and therefore the number of `rendered_pages` slots. */
  ptrdiff_t album_count;

  /** Every media item of every album, in source order, in `perm`. */
  struct MediaItem** media;

  /** Number of media items in `media`, and therefore the number of `video_frames` slots. */
  ptrdiff_t media_count;

  /**
   * One frame slot per media item, whose path the build allocates in `perm` for a video before the
   * probe phase. The probe creates the frame, or the derivative phase does for a video whose probe
   * fact answered for it. The derivative phase removes it, and the build removes any it left behind
   * at its end.
   */
  struct VideoFrame* video_frames;

  /**
   * Output cache an earlier build left, in `perm`, empty when there is none or the build is forced.
   * Its rows tell the writer which outputs it can keep, and its facts answer the probe.
   */
  struct OutputCache output_cache;

  /** Path of the output cache below `output_dir`, in `perm`. */
  struct Str output_cache_path;

  /**
   * One slot per album, holding that album's rendered page. A slot's `html` stays `NULL` until the
   * page render fills it, in the perm of the worker that rendered it.
   */
  struct RenderedPage* rendered_pages;

  /** Number of video sources among `source_paths`. */
  ptrdiff_t video_count;

  /** Most worker threads a parallel phase runs, each with its arenas in `worker_arenas`. */
  ptrdiff_t worker_count;

  /** Where the build's status lines go, which `--verbose` enables. */
  struct Verbose verbose;

  /** Whether the build ignores the output cache, and writes every output afresh. */
  bool is_forced;
};

/**
 * @brief Runs the build in the arenas `build_run` reserved, as `BuildRunExecuteFn`.
 *
 * @param worker_count  Workers there are arenas for.
 * @param worker_arenas One perm and one scratch per worker. Must not be `NULL`.
 * @param os            Host the build runs on. Must not be `NULL`.
 * @param perm          The main thread's perm.
 * @param scratch       The main thread's scratch.
 * @param log           Log that receives every error. Must not be `NULL`.
 * @param userdata      The `struct BuildOptions` of the build, which receives the worker count.
 * @return `0` on a complete build, or `-1` on any error.
 */
static int cmd_build_run_execute(ptrdiff_t worker_count,
                                 struct JobArenas* worker_arenas,
                                 struct Os* os,
                                 struct Arena perm,
                                 struct Arena scratch,
                                 struct Log* log,
                                 void* userdata) __attribute__((nonnull(2, 3, 6, 7)));

/**
 * @brief Initializes a build state over the caller's arenas.
 *
 * @param state         Build state to prepare. Must not be `NULL`.
 * @param options       Build knobs, whose worker count is positive. Must not be `NULL`.
 * @param worker_arenas One perm and one scratch per worker. Must not be `NULL`.
 * @param os            Host the build runs against. Must not be `NULL`.
 * @param perm          The main thread's perm.
 * @param scratch       The main thread's scratch.
 */
static void build_state_init(struct BuildState* state,
                             const struct BuildOptions* options,
                             struct JobArenas* worker_arenas,
                             struct Os* os,
                             struct Arena perm,
                             struct Arena scratch) __attribute__((nonnull(1, 2, 3, 4)));

/**
 * @brief Loads the config, rejects an `output_dir` inside an input tree, and discovers media and
 *        static files.
 *
 * The `output_dir` check runs before either walk, and both walks leave `output_dir` out, so a file
 * an earlier build generated is never read back as a source.
 *
 * It stops at the first step that fails.
 *
 * @param state Build state that receives the configuration, media source paths, and static file
 *              paths. Must not be `NULL`.
 * @param log   Log that receives every error. Must not be `NULL`.
 * @return `0` when there are sources to check, or `-1` on an invalid configuration, or a directory,
 *         output directory, listing, or missing-tool failure.
 */
static int load_build_inputs(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Lists the media sources below `media_dir` and counts the videos.
 *
 * One walk lists every media suffix.
 *
 * @param state Build state that receives the source paths and the video count. Must not be `NULL`.
 * @param log   Log that receives the failure, which names the path the walk failed on. Must not be
 *              `NULL`.
 * @return `0` on success, or `-1` on a directory or entry failure.
 */
static int load_build_inputs_media(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Lists every file below `templates_dir`, partials included, which the manifest claims as
 *        inputs.
 *
 * @param state Build state that receives the template paths. Must not be `NULL`.
 * @param log   Log that receives the failure, which names the path the walk failed on. Must not be
 *              `NULL`.
 * @return `0` on success, or `-1` on a directory or entry failure.
 */
static int load_build_inputs_templates(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Lists the static files below `static_dir`, which a gallery may leave out.
 *
 * @param state Build state that receives the static file paths. Must not be `NULL`.
 * @param log   Log that receives the failure, which names the path the walk failed on. Must not be
 *              `NULL`.
 * @return `0` when `static_dir` is absent or was listed, or `-1` on a directory or entry failure.
 */
static int load_build_inputs_static(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Requires every configured read root to be a usable directory.
 *
 * `read_root_require` checks each root under its config key, which each diagnostic names, so the
 * user knows what to edit. An optional root that is not there is usable. Every other root that
 * cannot be resolved is reported with its cause.
 *
 * @param state Build state supplying the configured read roots. Must not be `NULL`.
 * @param log   Log that receives every error. Must not be `NULL`.
 * @return `0` when every present root is a usable directory, or `-1` when one cannot be resolved or
 *         is not a directory.
 */
static int load_build_inputs_require_read_roots(const struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Requires `ffmpeg` and `ffprobe` to be runnable when any source is a video.
 *
 * A gallery of images alone needs neither tool, so the probe runs only once a video source has been
 * discovered. Running both here reports a missing tool once, before the parallel probe and
 * derivative phases would report it per video. Both tools are checked.
 *
 * @param state Build state supplying the video count and the scratch arena. Must not be `NULL`.
 * @param log   Log that receives every error. Must not be `NULL`.
 * @return `0` when no source is a video or both tools ran, or `-1` when either could not be run.
 */
static int load_build_inputs_require_video_tools(const struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Runs the phases from the skeleton to the prune, and in a dry run the report.
 *
 * Probes every media item and checks every output path even after a probe fails, then stops before
 * the first write when `log` holds any error, an earlier phase's included. A skeleton that cannot
 * be planned, a path-limit failure included, stops the build at once, because the later phases read
 * it. Once `output_dir` is prepared, the output cache is written when the outputs are done, even
 * when one of them failed. Removes every video frame the phases left behind before it returns, on
 * success and on failure.
 *
 * @param state Build state whose inputs are loaded. Must not be `NULL`.
 * @param log   Log that receives every error. Must not be `NULL`.
 * @return `0` on a complete build, or `-1` on any error.
 */
static int run_build_phases(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Reads and compiles the album template, every aggregate template and every partial, and
 *        reports every one that cannot be read or does not compile.
 *
 * @param state Build state that receives the set. Must not be `NULL`.
 * @param log   Log that receives every error. Must not be `NULL`.
 * @return `0` on success, or `-1` when a template fails.
 */
static int load_templates(struct BuildState* state, struct Log* log) __attribute__((nonnull(1, 2)));

/**
 * @brief Adds `name` to the template names of a load, unless it is there already, as when an
 *        aggregate is also the album template.
 *
 * @param names      Names so far, with room for one more. Must not be `NULL`.
 * @param name_count Number of entries in `names`, which this increments when it adds one. Must not
 *                   be `NULL`.
 * @param name       Template name to add.
 */
static void load_templates_add(struct Str* names, ptrdiff_t* name_count, struct Str name)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Builds the album and media skeleton and allocates one result slot per media item and per
 *        album.
 *
 * @param state Build state that receives the albums, the flat media array, the video frame slots,
 *              and the rendered page slots. Must not be `NULL`.
 * @param log   Log that receives every error. Must not be `NULL`.
 * @return `0` on success, or `-1` on a scan or path-limit failure.
 */
static int build_album_skeleton(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Stats the static files, and reads the output cache an earlier build left unless the build
 *        is forced.
 *
 * A missing output cache, one that cannot be read, and one in another format are each an empty
 * cache, never an error. The verbose log says which, and the build then probes every source and
 * makes every output.
 *
 * @param state Build state that receives the static stats, the output cache and its path. Must not
 *              be `NULL`.
 */
static void load_output_cache(struct BuildState* state) __attribute__((nonnull(1)));

/**
 * @brief Probes every media item's metadata, dispatching jobs across worker threads.
 *
 * A gallery with no media skips the phase, so it prints no status line and starts no pool.
 *
 * @param state Build state holding the media items, the output cache, and the frame slots. Must not
 *              be `NULL`.
 * @param log   Log that receives the collected probe errors. Must not be `NULL`.
 * @return `0` when every job succeeded or there is no media, or `-1` when any probe job failed.
 */
static int probe_media(struct BuildState* state, struct Log* log) __attribute__((nonnull(1, 2)));

/**
 * @brief Records every intended output path, rejecting a collision before anything is written.
 *
 * @param state Build state whose manifest is populated from the configuration, the discovered
 *              inputs, and the scanned albums. Must not be `NULL`.
 * @param log   Log that receives every collision. Must not be `NULL`.
 * @return `0` when every output path is unique and names no build input, or `-1` on a collision or
 *         an input overwrite.
 */
static int populate_output_manifest(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Creates the output directory once the manifest has accepted every output path.
 *
 * Nothing before this phase writes, so a build the manifest or an earlier phase refuses leaves the
 * filesystem as it found it, without even an empty `output_dir`. A dry run creates nothing.
 *
 * @param state Build state supplying `output_dir`. Must not be `NULL`.
 * @param log   Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when a component of `output_dir` cannot be created.
 */
static int prepare_output_dir(const struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Copies every static file into the output, or keeps each copy whose source did not change,
 *        as `output_writer_produce` decides.
 *
 * Each copy names its source to `oom_set_job`, which names no file once every copy succeeds.
 *
 * @param state Build state holding the static files and the output cache. Must not be `NULL`.
 * @param log   Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when a copy fails.
 */
static int copy_static_files(const struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Produces every media item's derivatives and original, dispatching jobs across worker
 *        threads.
 *
 * A gallery with no media skips the phase, so it prints no status line and starts no pool.
 *
 * @param state Build state holding the probed media items, the video frame slots and the output
 *              cache. Must not be `NULL`.
 * @param log   Log that receives the collected derivative errors. Must not be `NULL`.
 * @return `0` when every job succeeded or there is no media, or `-1` when any derivative job
 *         failed.
 */
static int generate_derivatives(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Renders every album's page, dispatching jobs across worker threads.
 *
 * A gallery always has its root album, so this phase never runs empty.
 *
 * @param state Build state holding the scanned albums and rendered page slots. Must not be `NULL`.
 * @param log   Log that receives the collected render errors. Must not be `NULL`.
 * @return `0` when every job succeeded, or `-1` when any render job failed.
 */
static int render_album_pages(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Produces the album pages and the aggregate template outputs, each written only when its
 *        bytes changed.
 *
 * @param state Build state holding the rendered pages and the scanned albums. Must not be `NULL`.
 * @param log   Log that receives the failure. Must not be `NULL`.
 * @return `0` when every output was written, or `-1` on the first render or write failure.
 */
static int write_generated_site(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Writes the output cache, with a row for every output this build wrote or kept that
 *        declares attributes, and a probe fact for every media item.
 *
 * It runs once the outputs are done, even when one failed: a row names only an output this build
 * wrote or kept, and a write that failed leaves its output unmarked, so the cache vouches for no
 * output the build did not finish, and every probe fact stays correct. An interrupted build leaves
 * the previous cache, whose rows vouch for no output that the build cut short, as
 * `output_cache_write` explains, so its probe facts survive too.
 *
 * @param state Build state holding the manifest and the probed media items. Must not be `NULL`.
 * @param log   Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the write fails.
 */
static int write_output_cache(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Removes every entry below `output_dir` that this build did not write.
 *
 * Run it only once every output is written, so a failed build removes nothing.
 *
 * @param state Build state holding the manifest and the configuration. Must not be `NULL`.
 * @param log   Log that receives every failure. Must not be `NULL`.
 * @return `0` when every stale entry was removed, or `-1` when one could not be listed or removed.
 */
static int prune_stale_outputs(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Prints to `stdout` what a dry run would have written, kept, and removed, as
 *        `output_writer_report` lists it.
 *
 * @param state Build state whose writer has run the dry run. Must not be `NULL`.
 * @param log   Log that receives every output path no site marked, or the failed print. Must not be
 *              `NULL`.
 * @return `0` once the listing is printed, or `-1` when an output path was neither written nor kept
 *         or the print fails.
 */
static int report_dry_run(struct BuildState* state, struct Log* log) __attribute__((nonnull(1, 2)));

enum ExitCode cmd_build_run(const struct BuildOptions* options, struct Os* os) {
  const struct BuildRunLens lens = {
      .log_len = BUILD_LOG_LEN,
      .perm_len = BUILD_PERM_LEN,
      .scratch_len = BUILD_SCRATCH_LEN,
  };
  // Every option is carried as it was asked for, except the worker count the memory allows.
  struct BuildOptions resolved = *options;
  return build_run(&lens, options->worker_count, cmd_build_run_execute, &resolved, os);
}

int cmd_build_execute(const struct BuildOptions* options,
                      struct JobArenas* worker_arenas,
                      struct Os* os,
                      struct Arena perm,
                      struct Arena scratch,
                      struct Log* log) {
  struct BuildState state;
  build_state_init(&state, options, worker_arenas, os, perm, scratch);

  // Until the first write, a phase that fails still lets the next one run whenever that one has
  // something to check, so one run reports every path-limit failure in the skeleton, or, once the
  // skeleton is planned, every source that cannot be probed and every output collision. Only a
  // failure that leaves nothing to check stops early: an invalid config, a root that cannot be
  // walked, a missing video tool, and a skeleton error, a path-limit failure included, because the
  // later phases read the skeleton.
  int rc = -1;
  if (load_build_inputs(&state, log) == 0) {
    rc = run_build_phases(&state, log);
  }
  template_set_release(state.template_set);
  return rc;
}

static int cmd_build_run_execute(ptrdiff_t worker_count,
                                 struct JobArenas* worker_arenas,
                                 struct Os* os,
                                 struct Arena perm,
                                 struct Arena scratch,
                                 struct Log* log,
                                 void* userdata) {
  struct BuildOptions* resolved = userdata;
  resolved->worker_count = worker_count;
  return cmd_build_execute(resolved, worker_arenas, os, perm, scratch, log);
}

static void build_state_init(struct BuildState* state,
                             const struct BuildOptions* options,
                             struct JobArenas* worker_arenas,
                             struct Os* os,
                             struct Arena perm,
                             struct Arena scratch) {
  // This is one literal, so a field added to `BuildState` cannot be left out of its defaults. The
  // zeroed path lists and manifest are already their empty state. `site_config_init` does more: it
  // installs the optional-key defaults, which `site_config_load` leaves untouched when a key is
  // absent.
  *state = (struct BuildState){
      .os = os,
      .perm = perm,
      .scratch = scratch,
      .worker_arenas = worker_arenas,
      .config_path = options->config_path,
      .writer =
          {
              .manifest = &state->manifest,
              .os = os,
              .is_dry_run = options->is_dry_run,
              .is_forced = options->is_forced,
              .perm = &state->perm,
          },
      .worker_count = options->worker_count,
      .verbose = {.os = os, .is_enabled = options->is_verbose, .scratch = &state->scratch},
      .is_forced = options->is_forced,
  };
  site_config_init(&state->site_config);
}

static int load_build_inputs(struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "loading config");
  if (site_config_load(&state->site_config, state->config_path, state->os, &state->perm,
                       state->scratch, log) != 0) {
    return -1;
  }
  state->writer.output_dir = state->site_config.output_dir;
  if (load_build_inputs_require_read_roots(state, log) != 0 ||
      output_manifest_check_output_dir(&state->site_config, state->config_path, state->os,
                                       state->scratch, log) != 0) {
    return -1;
  }

  verbose_printf(&state->verbose, "discovering media");
  if (load_build_inputs_media(state, log) != 0) {
    return -1;
  }
  verbose_printf(&state->verbose, "discovering templates and static files");
  if (load_build_inputs_templates(state, log) != 0 || load_build_inputs_static(state, log) != 0) {
    return -1;
  }
  return load_build_inputs_require_video_tools(state, log);
}

static int load_build_inputs_media(struct BuildState* state, struct Log* log) {
  static const struct Str media_suffixes[] = {STR_INIT(".jpg"), STR_INIT(".jpeg"),
                                              STR_INIT(".mp4")};
  // A failure names the directory or entry that failed, which is more precise than the configured
  // root, so the root is not repeated.
  if (walk_files_with_suffixes(
          &state->source_paths, state->site_config.media_dir, state->site_config.output_dir,
          media_suffixes, (ptrdiff_t)(sizeof(media_suffixes) / sizeof(media_suffixes[0])), true,
          state->os, &state->perm, state->scratch, S("failed to list source media"), log) != 0) {
    return -1;
  }
  for (ptrdiff_t i = 0; i < state->source_paths.count; i++) {
    state->video_count += media_item_is_video_path(state->source_paths.items[i]);
  }
  return 0;
}

static int load_build_inputs_templates(struct BuildState* state, struct Log* log) {
  // An empty suffix matches every file, so the walk lists the whole tree. `templates_dir` passed
  // its check, so the walk fails only on a directory or entry below it, which it names.
  static const struct Str template_suffixes[] = {STR_INIT("")};
  return walk_files_with_suffixes(&state->template_paths, state->site_config.templates_dir,
                                  state->site_config.output_dir, template_suffixes, 1, false,
                                  state->os, &state->perm, state->scratch,
                                  S("failed to list templates"), log);
}

static int load_build_inputs_static(struct BuildState* state, struct Log* log) {
  // An absent `static_dir` leaves `static_paths` empty rather than failing the build. A gallery
  // that ships no stylesheet is a supported configuration, and the walk stats its root before
  // descending, so calling it here would turn "no static files" into a failed build.
  // `load_build_inputs_require_read_roots` skips the same directory on the same condition. A
  // failure names the directory or entry that failed, which is more precise than the configured
  // root, so the root is not repeated.
  struct OsStat st;
  if (os_stat(state->os, state->site_config.static_dir, &st) != 0 && errno == ENOENT) {
    return 0;
  }
  static const struct Str static_suffixes[] = {STR_INIT("")};
  return walk_files_with_suffixes(&state->static_paths, state->site_config.static_dir,
                                  state->site_config.output_dir, static_suffixes, 1, false,
                                  state->os, &state->perm, state->scratch,
                                  S("failed to list static files"), log);
}

static int load_build_inputs_require_read_roots(const struct BuildState* state, struct Log* log) {
  const struct SiteConfig* site_config = &state->site_config;
  const struct ReadRoot roots[] = {
      {.dir_path = site_config->media_dir, .key = S("media_dir")},
      {.dir_path = site_config->templates_dir, .key = S("templates_dir")},
      {.dir_path = site_config->static_dir, .key = S("static_dir"), .is_optional = true},
  };
  return read_root_require(roots, (ptrdiff_t)(sizeof(roots) / sizeof(roots[0])), state->os, log);
}

static int load_build_inputs_require_video_tools(const struct BuildState* state, struct Log* log) {
  if (state->video_count == 0) {
    return 0;
  }

  const char* const ffmpeg_argv[] = {"ffmpeg", "-version", NULL};
  const char* const ffprobe_argv[] = {"ffprobe", "-version", NULL};
  // Each run works in its own copy of the scratch arena, which the next one reuses.
  struct Arena ffmpeg_scratch = state->scratch;
  int rc = proc_run(ffmpeg_argv, &ffmpeg_scratch, NULL, S("video sources require ffmpeg"),
                    (struct Str){0}, log);
  struct Arena ffprobe_scratch = state->scratch;
  if (proc_run(ffprobe_argv, &ffprobe_scratch, NULL, S("video sources require ffprobe"),
               (struct Str){0}, log) != 0) {
    rc = -1;
  }
  return rc;
}

static int run_build_phases(struct BuildState* state, struct Log* log) {
  // The order below follows the data: the probe records each item's derivative dimensions, which
  // the derivative and page phases read, from its probe fact or from the source, and extracts each
  // probed video's still frame, which the derivative phase reads. Every source has to be probed
  // before any derivative is generated or any page is rendered. The manifest reads each source's
  // stat, which the probe takes, for the attributes of its outputs. Output paths do not depend on
  // probed dimensions, because a derivative path names the recipe and not the size, so the manifest
  // checks every output even after a probe failed.
  (void)load_templates(state, log);
  if (build_album_skeleton(state, log) != 0) {
    return -1;
  }
  int rc = -1;
  int phase_rc = -1;
  load_output_cache(state);
  (void)probe_media(state, log);
  (void)populate_output_manifest(state, log);
  if (log->errors > 0) {
    goto cleanup;
  }

  // Nothing before this point writes, so a build stopped above leaves the filesystem as it found
  // it. From here on a failure stops the phase it happens in.
  if (prepare_output_dir(state, log) != 0) {
    goto cleanup;
  }
  phase_rc = copy_static_files(state, log) != 0 || generate_derivatives(state, log) != 0 ||
                     render_album_pages(state, log) != 0 || write_generated_site(state, log) != 0
                 ? -1
                 : 0;
  // A failed phase still lets the cache keep what this build wrote or kept, and the probe facts.
  if (write_output_cache(state, log) != 0 || phase_rc != 0) {
    goto cleanup;
  }
  if (prune_stale_outputs(state, log) != 0) {
    goto cleanup;
  }
  if (state->writer.is_dry_run && report_dry_run(state, log) != 0) {
    goto cleanup;
  }

  verbose_printf(&state->verbose, "build complete");
  rc = 0;

cleanup:
  album_scan_remove_video_frames(state->video_frames, state->media_count);
  return rc;
}

static int load_templates(struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "compiling templates");
  const struct SiteConfig* site_config = &state->site_config;
  const ptrdiff_t name_count_max = 1 + site_config->aggregate_template_count;
  struct Arena scratch = state->scratch;
  struct Str* names = arena_new(&scratch, name_count_max, struct Str);
  ptrdiff_t name_count = 0;
  load_templates_add(names, &name_count, site_config->album_template);
  for (ptrdiff_t i = 0; i < site_config->aggregate_template_count; i++) {
    load_templates_add(names, &name_count, site_config->aggregate_templates[i]);
  }
  return template_set_load(site_config->templates_dir, names, NULL, name_count, state->os,
                           &state->perm, scratch, &state->template_set, log);
}

static void load_templates_add(struct Str* names, ptrdiff_t* name_count, struct Str name) {
  for (ptrdiff_t i = 0; i < *name_count; i++) {
    if (str_eq(names[i], name)) {
      return;
    }
  }
  names[*name_count] = name;
  (*name_count)++;
}

static int build_album_skeleton(struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "planning albums");
  if (album_scan_build_skeleton(&state->site_config, &state->source_paths, &state->perm,
                                state->scratch, &state->albums, &state->album_count, &state->media,
                                &state->media_count, log) != 0) {
    return -1;
  }
  // The slots are allocated here, on the main thread, before either phase that uses them, so a job
  // only fills in its own slot and never allocates in the main thread's arenas.
  state->video_frames = arena_new(&state->perm, state->media_count, struct VideoFrame);
  for (ptrdiff_t i = 0; i < state->media_count; i++) {
    if (state->media[i]->kind == MEDIA_KIND_VIDEO) {
      video_frame_init(&state->video_frames[i], &state->perm);
    }
  }
  state->rendered_pages = arena_new(&state->perm, state->album_count, struct RenderedPage);
  return 0;
}

static void load_output_cache(struct BuildState* state) {
  state->output_cache_path =
      output_manifest_cache_path(state->site_config.output_dir, &state->perm);
  // The copies' attributes name the static files' stats whether or not the cache is read.
  state->static_stats = walk_stat_paths(&state->static_paths, state->os, &state->perm);
  if (state->is_forced) {
    verbose_printf(&state->verbose, "ignoring output cache, probing every source");
    return;
  }
  verbose_printf(&state->verbose, "loading output cache");
  struct Str reason = {0};
  const enum BuildCacheFileState cache_state =
      output_cache_load(state->output_cache_path, state->os, &state->perm, state->scratch,
                        &state->output_cache, &reason);
  if (cache_state == BUILD_CACHE_FILE_MISSING) {
    verbose_printf(&state->verbose, "no output cache, probing every source");
  } else if (cache_state == BUILD_CACHE_FILE_UNUSABLE) {
    verbose_printf(&state->verbose, "output cache not used, probing every source: %.*s",
                   STR_FMT(reason));
  }
}

static int probe_media(struct BuildState* state, struct Log* log) {
  if (state->media_count == 0) {
    return 0;
  }
  verbose_printf(&state->verbose, "probing media, workers: %td", state->worker_count);
  return album_scan_probe_media(&state->site_config, state->media, state->media_count,
                                &state->output_cache, state->video_frames, state->worker_count,
                                state->worker_arenas, state->verbose.is_enabled, state->os,
                                state->scratch, log);
}

static int populate_output_manifest(struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "building output manifest");
  // This runs ahead of the derivative and page renders, so a duplicate output path fails before
  // rendering is wasted. It also runs ahead of every write, `output_dir` itself included, so an
  // output aimed at one of this build's own inputs is refused before it can destroy the file.
  //
  // C has no implicit qualification conversion for pointer-to-pointer, so handing the album array
  // to a read-only callee needs this cast spelled out. It recurs at all three such call sites in
  // this file. It only adds `const`, at both levels, and the deep `const` is a thread-safety
  // requirement. A `TemplateContext` with only `const` pointers keeps one render job from writing
  // an album another job is reading.
  return output_manifest_populate(&state->manifest, &state->site_config, state->config_path,
                                  &state->source_paths, &state->static_paths, state->static_stats,
                                  &state->template_paths, (const struct Album* const*)state->albums,
                                  state->album_count, state->os, &state->perm, state->scratch, log);
}

static int prepare_output_dir(const struct BuildState* state, struct Log* log) {
  // A failure names the component that failed, which is more precise than the configured root, so
  // the root is not repeated.
  return output_writer_mkdirs(&state->writer, state->site_config.output_dir,
                              S("failed to prepare output directory"), log);
}

static int copy_static_files(const struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "copying static files");
  for (ptrdiff_t i = 0; i < state->static_paths.count; i++) {
    const struct Str source_path = state->static_paths.items[i];
    oom_set_job(source_path, NULL);
    // Each copy works in its own copy of the scratch, so one file's path is dead before the next.
    struct Arena copy_scratch = state->scratch;
    struct Str relative = {0};
    (void)path_is_below(source_path, state->site_config.static_dir, &relative);
    const struct Str output_path =
        path_join(state->site_config.output_dir, relative, &copy_scratch);
    // The copy names the path that failed, the source or its destination, which mirrors it.
    if (output_writer_produce(&state->writer, output_path, &state->output_cache, NULL, NULL,
                              (struct Arena){0}, copy_scratch, S("failed to copy static file"),
                              log) != 0) {
      return -1;
    }
  }
  oom_set_job((struct Str){0}, NULL);
  return 0;
}

static int generate_derivatives(struct BuildState* state, struct Log* log) {
  if (state->media_count == 0) {
    return 0;
  }
  verbose_printf(&state->verbose, "generating derivatives, workers: %td", state->worker_count);
  return derivative_renderer_generate(&state->writer, &state->output_cache, &state->site_config,
                                      state->media, state->media_count, state->video_frames,
                                      state->worker_count, state->worker_arenas,
                                      state->verbose.is_enabled, state->os, state->scratch, log);
}

static int render_album_pages(struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "rendering albums, workers: %td", state->worker_count);
  return page_renderer_render_pages(
      &state->site_config, state->template_set, (const struct Album* const*)state->albums,
      state->album_count, state->worker_count, state->worker_arenas, state->verbose.is_enabled,
      state->os, state->scratch, state->rendered_pages, log);
}

static int write_generated_site(struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "writing album pages");
  if (site_writer_write_album_pages(&state->writer, &state->output_cache,
                                    (const struct Album* const*)state->albums, state->album_count,
                                    state->rendered_pages, state->scratch, log) != 0) {
    return -1;
  }
  verbose_printf(&state->verbose, "rendering aggregate templates");
  return site_writer_write_aggregates(&state->writer, &state->output_cache, &state->site_config,
                                      state->template_set, state->albums[0], state->perm,
                                      state->scratch, log);
}

static int write_output_cache(struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "writing output cache");
  oom_set_job(state->output_cache_path, NULL);
  struct Arena scratch = state->scratch;
  struct OutputCacheFact* facts = arena_new(&scratch, state->media_count, struct OutputCacheFact);
  for (ptrdiff_t i = 0; i < state->media_count; i++) {
    facts[i] = album_scan_probe_fact(state->media[i], state->site_config.media_dir, &scratch);
  }
  return output_cache_write(&state->writer, state->output_cache_path, facts, state->media_count,
                            scratch, log);
}

static int prune_stale_outputs(struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "removing stale outputs");
  oom_set_job((struct Str){0}, NULL);
  ptrdiff_t removed_count = 0;
  const int rc = output_pruner_prune(&state->writer, state->site_config.output_dir, state->os,
                                     state->scratch, &removed_count, log);
  verbose_printf(&state->verbose, "removed stale outputs: %td", removed_count);
  return rc;
}

static int report_dry_run(struct BuildState* state, struct Log* log) {
  return output_writer_report(&state->writer, state->scratch, log);
}
