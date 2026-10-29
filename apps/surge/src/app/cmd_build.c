#include "app/cmd_build.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "build/export_writer.h"
#include "build/page_feed.h"
#include "build/page_index.h"
#include "build/page_release.h"
#include "build/release_scan.h"
#include "build/release_writer.h"
#include "build/repo_pages.h"
#include "build/repo_scan.h"
#include "build/template_value.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/path.h"
#include "core/path_list.h"
#include "core/str.h"
#include "core/text.h"
#include "domain/build_cache.h"
#include "domain/repo_model.h"
#include "domain/site_config.h"
#include "os/oom.h"
#include "os/os.h"
#include "output/build_cache_file.h"
#include "output/manifest.h"
#include "output/manifest_builder.h"
#include "output/output_pruner.h"
#include "output/output_writer.h"
#include "runtime/build_run.h"
#include "runtime/job.h"
#include "runtime/read_root.h"
#include "runtime/verbose.h"
#include "runtime/walk.h"
#include "templates/template_set.h"
#include "vcs/repository.h"

/** Bytes in a mebibyte, the unit of the sizes below. */
#define BUILD_MIB ((ptrdiff_t)1024 * 1024)

// Each thread has a perm and a scratch arena of the sizes below, and the build has one log, as in
// sosig. A size costs address space only, because pages are committed as an arena fills.
_Static_assert(PTRDIFF_MAX > 0xffffffff, "the arenas below need a 64-bit address space");

/** Bytes of the build's log, and of each worker's share of the job logs. */
#define BUILD_LOG_LEN (256 * BUILD_MIB)

/**
 * Bytes of each thread's perm, which holds the main thread's config, paths and manifest, or what a
 * worker's scans keep, beside one render's buffer of up to twice `TEMPLATE_RENDER_OUTPUT_LEN_MAX`.
 */
#define BUILD_PERM_LEN (16 * (ptrdiff_t)TEMPLATE_RENDER_OUTPUT_LEN_MAX)

/** Bytes of each thread's scratch, which holds one job's temporaries. */
#define BUILD_SCRATCH_LEN (3 * 1024 * BUILD_MIB)

/** The page templates a build renders, the release templates last. */
static const struct Str BUILD_PAGE_TEMPLATES[] = {
    STR_INIT("index.html"), STR_INIT("404.html"),   REPO_PAGES_TEMPLATES,
    PAGE_FEED_TEMPLATES,    PAGE_RELEASE_TEMPLATES, PAGE_FEED_RELEASE_TEMPLATES,
};

/** The files the build reads below a root, with what `os_stat` reported for each. */
struct BuildInputs {
  /** Paths of the files, sorted. */
  struct PathList paths;

  /** Stat of each of `paths`, parallel to it. A file that cannot be inspected has one that no row
   * matches.
   */
  struct OsStat* stats;
};

/** The build cache rows of one kind that a copy of files checks its sources against. */
struct CopyRows {
  /** Rows an earlier build left. */
  const struct BuildCache* cache;

  /** Kind of the rows the sources have. */
  enum BuildCacheKind kind;
};

/** Mutable state owned by one `cmd_build_execute` invocation. */
struct BuildState {
  /** Host every phase reads from, writes to and prints on. */
  struct Os* os;

  /** The main thread's perm, which receives everything that outlives a phase. */
  struct Arena perm;

  /** The main thread's scratch. Each phase receives a copy. */
  struct Arena scratch;

  /** One perm and one scratch per worker. */
  struct JobArenas* worker_arenas;

  /** Config file the build reads. */
  struct Str config_path;

  /** Loaded configuration. */
  struct SiteConfig site_config;

  /** Compiled templates, released when the build ends, or `NULL` until they load. */
  struct TemplateSet* template_set;

  /** Every file below `templates_dir`, which the build cache records a row for. */
  struct BuildInputs templates;

  /** Every file below `static_dir`, empty when it does not exist. */
  struct BuildInputs statics;

  /** One slot per repo of the config. */
  struct RepoScan* scans;

  /** The input roots no output may enter, which the overlap check, the manifest and the prune
      read. */
  struct ManifestRoot* roots;

  /** Number of entries in `roots`. */
  ptrdiff_t root_count;

  /** Every output path of this build. */
  struct Manifest manifest;

  /** Writer every change under `output_dir` goes through. */
  struct OutputWriter writer;

  /** Path of the build cache below `output_dir`. */
  struct Str build_cache_path;

  /** Rows an earlier build left, empty when there are none or the build ignores them. */
  struct BuildCache build_cache;

  /** Workers a parallel phase runs. */
  ptrdiff_t worker_count;

  /** Where the build's status lines go, which `--verbose` enables. */
  struct Verbose verbose;

  /** Whether the build ignores the build cache. */
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
 * @brief Loads the configuration, sets libgit2 up, opens every repo, rejects an `output_dir` that
 *        overlaps an input, and lists the templates and the static files.
 *
 * @param state Build state that receives the inputs. Must not be `NULL`.
 * @param log   Log that receives every error. Must not be `NULL`.
 * @return `0` when the build has inputs to go on with, or `-1` otherwise.
 */
static int load_build_inputs(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Requires `templates_dir` to be a directory, and `static_dir` and each repo's
 *        `releases_dir` to be one when it exists, and checks no directory whose key was rejected.
 *
 * @param state Build state holding the configuration. Must not be `NULL`.
 * @param log   Log that receives every error. Must not be `NULL`.
 * @return `0` when the roots are usable, or `-1` otherwise.
 */
static int load_build_inputs_require_read_roots(const struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Returns the input roots no output may enter: `templates_dir`, `static_dir`, and each
 *        repo's git directory, work tree and `releases_dir`.
 *
 * @param state     Build state whose repos are located. Must not be `NULL`.
 * @param count_out Receives the number of roots. Must not be `NULL`.
 * @return The roots, in the main thread's perm.
 */
static struct ManifestRoot* load_build_inputs_roots(struct BuildState* state, ptrdiff_t* count_out)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Lists and stats every file below `root_dir`, which may be optional.
 *
 * @param state       Build state, whose perm owns the list. Must not be `NULL`.
 * @param root_dir    Directory to walk.
 * @param is_optional Whether a missing `root_dir` holds no file rather than an error.
 * @param inputs_out  Receives the files and their stats. Must not be `NULL`.
 * @param operation   Operation leading the failure message.
 * @param log         Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the tree cannot be walked.
 */
static int load_build_inputs_walk(struct BuildState* state,
                                  struct Str root_dir,
                                  bool is_optional,
                                  struct BuildInputs* inputs_out,
                                  struct Str operation,
                                  struct Log* log) __attribute__((nonnull(1, 4, 6)));

/**
 * @brief Reports whether a repo of `site_config` has a release project: a `releases_dir`, or a
 *        project with a `tag_prefix`, so the build needs the release templates.
 *
 * @param site_config Loaded configuration. Must not be `NULL`.
 * @return `true` when a repo has a release project.
 */
static bool has_release_projects(const struct SiteConfig* site_config) __attribute__((nonnull(1)));

/**
 * @brief Runs the phases after the inputs are loaded: the templates, the build cache, the scan, the
 *        manifest, and then the writes, the build cache, the prune and a dry run's report.
 *
 * @param state Build state whose inputs are loaded. Must not be `NULL`.
 * @param log   Log that receives every error. Must not be `NULL`.
 * @return `0` on a complete build, or `-1` on any error.
 */
static int run_build_phases(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Reads and compiles every page template the site needs and every partial, and reports every
 *        one that cannot be read or does not compile.
 *
 * @param state Build state that receives the set. Must not be `NULL`.
 * @param log   Log that receives every error. Must not be `NULL`.
 * @return `0` on success, or `-1` when a template fails.
 */
static int load_templates(struct BuildState* state, struct Log* log) __attribute__((nonnull(1, 2)));

/**
 * @brief Returns how many of `BUILD_PAGE_TEMPLATES` a site needs: all of them, or without the
 *        release templates at their end when no repo has a release project.
 *
 * @param site_config Loaded configuration. Must not be `NULL`.
 * @return The number of templates, from the start of `BUILD_PAGE_TEMPLATES`.
 */
static ptrdiff_t load_templates_count(const struct SiteConfig* site_config)
    __attribute__((nonnull(1)));

/**
 * @brief Reads the build cache an earlier build left, unless the build ignores it. A missing or
 *        unusable one is an empty one, with a verbose note.
 *
 * @param state Build state that receives the build cache. Must not be `NULL`.
 */
static void load_build_cache(struct BuildState* state) __attribute__((nonnull(1)));

/**
 * @brief Parses the text of the build cache into the build state, as `BuildCacheFileParseFn`.
 *
 * @param text     Bytes of the build cache.
 * @param perm     Arena that owns the rows. Must not be `NULL`.
 * @param log      Log that receives the reason the build cache cannot be used. Must not be `NULL`.
 * @param userdata The `struct BuildState`, which receives the build cache.
 * @return `0` on success, or `-1` when the build cache cannot be used.
 */
static int load_build_cache_parse(struct Str text,
                                  struct Arena* perm,
                                  struct Log* log,
                                  void* userdata) __attribute__((nonnull(2, 3, 4)));

/**
 * @brief Scans every repo on the worker pool.
 *
 * @param state Build state whose repos are located. Must not be `NULL`.
 * @param log   Log that receives every failure. Must not be `NULL`.
 * @return `0` when every repo scanned, or `-1` otherwise.
 */
static int scan_repos(struct BuildState* state, struct Log* log) __attribute__((nonnull(1, 2)));

/**
 * @brief Records every output path, rejecting a collision before anything is written.
 *
 * @param state Build state whose repos are scanned. Must not be `NULL`.
 * @param log   Log that receives every refusal. Must not be `NULL`.
 * @return `0` when every path was recorded, or `-1` otherwise.
 */
static int populate_output_manifest(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Returns the input files outside the roots that an output could still overwrite through a
 *        symlink in `output_dir`: the config, and the targets of the templates, the static files
 *        and the release assets.
 *
 * @param state Build state holding the inputs. Must not be `NULL`.
 * @return The files, in the main thread's perm.
 */
static struct PathList populate_output_manifest_inputs(struct BuildState* state)
    __attribute__((nonnull(1)));

/**
 * @brief Refuses a top-level static entry whose name, folded, is a repo's or its clone export's,
 *        once per entry, and marks every file below it.
 *
 * @param state            Build state holding the static files. Must not be `NULL`.
 * @param is_colliding_out Receives, for each static file, whether its entry collides, which keeps
 *                         it out of the manifest. Must not be `NULL` unless there are no static
 *                         files.
 * @param log              Log that receives every collision. Must not be `NULL`.
 * @return `0` when no static entry collides, or `-1` otherwise.
 */
static int populate_output_manifest_check_statics(const struct BuildState* state,
                                                  bool* is_colliding_out,
                                                  struct Log* log) __attribute__((nonnull(1, 3)));

/**
 * @brief Creates `output_dir` and empties the build cache, the build's first writes.
 *
 * @param state Build state. Must not be `NULL`.
 * @param log   Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when either write fails.
 */
static int prepare_output_dir(const struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Writes every repo's pages on the worker pool, or keeps each one the build cache vouches
 *        for, and notes what they left out when the build is verbose.
 *
 * @param state Build state whose repos are scanned. Must not be `NULL`.
 * @param log   Log that receives every failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when a page cannot be written.
 */
static int write_repo_pages(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Reports whether the build cache's repo row and ref rows of one repo are exactly what the
 *        scan found, which keeps its home and refs pages.
 *
 * @param state      Build state whose repos are scanned. Must not be `NULL`.
 * @param repo_index Index of the repo in the config.
 * @return `true` when the repo and every ref of it are as the earlier build saw them.
 */
static bool is_repo_unchanged(const struct BuildState* state, ptrdiff_t repo_index)
    __attribute__((nonnull(1)));

/**
 * @brief Writes every repo's clone export, and `_headers` when the config asks for it.
 *
 * @param state Build state whose repos are scanned. Must not be `NULL`.
 * @param log   Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when a write fails.
 */
static int write_clone_exports(const struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Writes every repo's release pages, version lists and assets, or keeps those that are
 *        current.
 *
 * @param state Build state whose repos are scanned. Must not be `NULL`.
 * @param log   Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when a render, a write or a copy fails.
 */
static int write_release_outputs(const struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Reports whether the build cache's releases row of one repo is what the scan found, which
 *        keeps its release outputs.
 *
 * @param state      Build state whose repos are scanned. Must not be `NULL`.
 * @param repo_index Index of the repo in the config.
 * @return `true` when the repo's releases are as the earlier build saw them.
 */
static bool is_releases_unchanged(const struct BuildState* state, ptrdiff_t repo_index)
    __attribute__((nonnull(1)));

/**
 * @brief Reports whether the build cache's repo map row of one repo is what the scan found, which
 *        every page of the repo needs to be kept, because each one shows its `repo` map.
 *
 * @param state      Build state whose repos are scanned. Must not be `NULL`.
 * @param repo_index Index of the repo in the config.
 * @return `true` when the repo's `repo` map is as the earlier build saw it.
 */
static bool is_repo_map_unchanged(const struct BuildState* state, ptrdiff_t repo_index)
    __attribute__((nonnull(1)));

/**
 * @brief Renders and writes the site index and the not-found page, or keeps each one the build
 *        cache vouches for.
 *
 * @param state Build state whose repos are scanned. Must not be `NULL`.
 * @param log   Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when a render or a write fails.
 */
static int write_site_pages(const struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Renders one page of the root into the output, or keeps it.
 *
 * @param state      Build state. Must not be `NULL`.
 * @param name       Name of the page's template, and of its output below `output_dir`.
 * @param value      Data tree of the page. Must not be `NULL`.
 * @param is_vouched Whether the build cache vouches for the page, which is then kept when it
 *                   exists.
 * @param scratch    Arena for the render, past the bytes that hold `value`.
 * @param log        Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the render or the write fails.
 */
static int write_site_page(const struct BuildState* state,
                           struct Str name,
                           const struct TemplateValue* value,
                           bool is_vouched,
                           struct Arena scratch,
                           struct Log* log) __attribute__((nonnull(1, 3, 6)));

/**
 * @brief Reports whether the build cache's rows of `kind` are exactly the files of `inputs`.
 *
 * @param state    Build state holding the build cache. Must not be `NULL`.
 * @param kind     Kind of the rows.
 * @param inputs   Files and their stats. Must not be `NULL`.
 * @param root_dir Directory the files are below.
 * @return `true` when the rows match the files one for one.
 */
static bool is_every_file_unchanged(const struct BuildState* state,
                                    enum BuildCacheKind kind,
                                    const struct BuildInputs* inputs,
                                    struct Str root_dir) __attribute__((nonnull(1, 3)));

/**
 * @brief Reports whether the build cache's repo and ref rows are exactly what the scan found.
 *
 * @param state Build state whose repos are scanned. Must not be `NULL`.
 * @return `true` when every repo and every ref is as the earlier build saw it.
 */
static bool is_every_repo_unchanged(const struct BuildState* state) __attribute__((nonnull(1)));

/**
 * @brief Copies every static file into the output, or keeps each copy whose source did not change.
 *
 * @param state Build state holding the static files. Must not be `NULL`.
 * @param log   Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when a copy fails.
 */
static int copy_static_files(const struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Reports whether a source has a row of the copy's kind with its stat, as
 *        `OutputWriterIsUnchangedFn`.
 *
 * @param name        Path of the source below the root of the copies.
 * @param source_stat What `os_stat` reported for the source. Must not be `NULL`.
 * @param userdata    The `struct CopyRows`.
 * @return `true` when the build cache records the source with that stat.
 */
static bool copy_static_files_is_unchanged(struct Str name,
                                           const struct OsStat* source_stat,
                                           void* userdata) __attribute__((nonnull(2, 3)));

/**
 * @brief Writes the build cache: the page recipe and a row for every input this build read.
 *
 * @param state Build state. Must not be `NULL`.
 * @param log   Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the write fails.
 */
static int write_build_cache(const struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Appends the rows of `kind` for the files of `inputs`, named relative to `root_dir`.
 *
 * @param cache    Cache that receives the rows, with room for them. Must not be `NULL`.
 * @param kind     Kind of the rows.
 * @param inputs   Files and their stats. Must not be `NULL`.
 * @param root_dir Directory the files are below.
 */
static void append_file_rows(struct BuildCache* cache,
                             enum BuildCacheKind kind,
                             const struct BuildInputs* inputs,
                             struct Str root_dir) __attribute__((nonnull(1, 3)));

/**
 * @brief Appends the repo row of each scanned repo and the ref row of each of its refs.
 *
 * @param cache Cache that receives the rows, with room for them. Must not be `NULL`.
 * @param state Build state whose repos are scanned. Must not be `NULL`.
 * @param arena Arena for the rows' names. Must not be `NULL`.
 */
static void append_repo_rows(struct BuildCache* cache,
                             const struct BuildState* state,
                             struct Arena* arena) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Returns the number of rows `append_repo_rows` appends.
 *
 * @param state Build state whose repos are scanned. Must not be `NULL`.
 * @return The number of repo and ref rows.
 */
static ptrdiff_t count_repo_rows(const struct BuildState* state) __attribute__((nonnull(1)));

/**
 * @brief Appends each repo's `repo-map` and `export` rows, and its `releases` row when it has
 *        release projects.
 *
 * @param cache Cache that receives the rows, with room for `count_repo_export_rows`. Must not be
 *              `NULL`.
 * @param state Build state whose repos are scanned. Must not be `NULL`.
 */
static void append_repo_export_rows(struct BuildCache* cache, const struct BuildState* state)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Counts the rows `append_repo_export_rows` appends.
 *
 * @param state Build state whose repos are scanned. Must not be `NULL`.
 * @return The number of rows.
 */
static ptrdiff_t count_repo_export_rows(const struct BuildState* state) __attribute__((nonnull(1)));

/**
 * @brief Removes every directory entry below `output_dir` that the build did not write.
 *
 * @param state Build state. Must not be `NULL`.
 * @param log   Log that receives every failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when an entry cannot be listed or removed.
 */
static int prune_stale_outputs(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

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

  int rc = -1;
  if (load_build_inputs(&state, log) == 0) {
    rc = run_build_phases(&state, log);
  }
  // The packs the scan wrote below `TMPDIR` go on every path, as fram's video frames do.
  if (state.scans != NULL) {
    repo_scan_remove_packs(state.scans, state.site_config.repo_count);
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
  // zeroed lists and manifest are already their empty state. `site_config_init` does more: it
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
  const struct SiteConfig* site_config = &state->site_config;
  const bool has_config_errors = log->errors > 0;
  const int roots_rc = load_build_inputs_require_read_roots(state, log);
  // A rejected directory holds a default the user never chose, so nothing checks or walks it.
  if (site_config->is_output_dir_rejected || site_config->is_templates_dir_rejected ||
      site_config->is_static_dir_rejected) {
    return -1;
  }
  if (repository_setup(log) != 0) {
    return -1;
  }
  // A directory that failed its check, and any other config error, still let the repos open and the
  // overlap check run, so one run reports them together, and stop the build before the walks. Only
  // a repo that does not open stops the build first: without its directories there is nothing to
  // check the output against.
  state->scans = arena_new(&state->perm, site_config->repo_count, struct RepoScan);
  if (repo_scan_locate(site_config, &state->perm, state->scans, log) != 0) {
    return -1;
  }
  state->roots = load_build_inputs_roots(state, &state->root_count);
  if (manifest_builder_check_output_dir(site_config->output_dir, state->roots, state->root_count,
                                        state->config_path, state->os, state->scratch, log) != 0 ||
      roots_rc != 0 || has_config_errors) {
    return -1;
  }
  verbose_printf(&state->verbose, "discovering templates and static files");
  if (load_build_inputs_walk(state, site_config->templates_dir, false, &state->templates,
                             S("failed to list templates"), log) != 0 ||
      load_build_inputs_walk(state, site_config->static_dir, true, &state->statics,
                             S("failed to list static files"), log) != 0) {
    return -1;
  }
  return 0;
}

static int load_build_inputs_require_read_roots(const struct BuildState* state, struct Log* log) {
  const struct SiteConfig* site_config = &state->site_config;
  struct Arena scratch = state->scratch;
  struct ReadRoot* roots = arena_new(&scratch, 2 + site_config->repo_count, struct ReadRoot);
  ptrdiff_t root_count = 0;
  roots[root_count++] = (struct ReadRoot){.dir_path = site_config->templates_dir,
                                          .key = S("templates_dir"),
                                          .is_rejected = site_config->is_templates_dir_rejected};
  roots[root_count++] = (struct ReadRoot){.dir_path = site_config->static_dir,
                                          .key = S("static_dir"),
                                          .is_optional = true,
                                          .is_rejected = site_config->is_static_dir_rejected};
  // A repo's `releases_dir` is filled by CI, so it may not exist before the first release.
  for (ptrdiff_t i = 0; i < site_config->repo_count; i++) {
    const struct Str releases_dir = site_config->repos[i].releases_dir;
    if (releases_dir.len == 0) {
      continue;
    }
    struct Buf key = {.arena = &scratch};
    buf_printf(&key, "repo[%td].releases_dir", i);
    roots[root_count++] =
        (struct ReadRoot){.dir_path = releases_dir, .key = buf_str(&key), .is_optional = true};
  }
  return read_root_require(roots, root_count, state->os, log);
}

static struct ManifestRoot* load_build_inputs_roots(struct BuildState* state,
                                                    ptrdiff_t* count_out) {
  const struct SiteConfig* site_config = &state->site_config;
  struct ManifestRoot* roots =
      arena_new(&state->perm, 2 + 3 * site_config->repo_count, struct ManifestRoot);
  ptrdiff_t count = 0;
  roots[count++] =
      (struct ManifestRoot){.label = S("'templates_dir'"), .dir_path = site_config->templates_dir};
  roots[count++] =
      (struct ManifestRoot){.label = S("'static_dir'"), .dir_path = site_config->static_dir};
  for (ptrdiff_t i = 0; i < site_config->repo_count; i++) {
    const struct RepoModel* model = &state->scans[i].model;
    const struct Str name = model->config->name;
    const struct Str dirs[] = {model->git_dir, model->work_dir, model->config->releases_dir};
    const char* kinds[] = {"git directory", "work tree", "'releases_dir'"};
    for (ptrdiff_t j = 0; j < 3; j++) {
      if (dirs[j].len == 0) {
        continue;
      }
      struct Buf label = {.arena = &state->perm};
      buf_printf(&label, "the %s of repo '%.*s'", kinds[j], STR_FMT(name));
      roots[count++] = (struct ManifestRoot){.label = buf_str(&label), .dir_path = dirs[j]};
    }
  }
  *count_out = count;
  return roots;
}

static int load_build_inputs_walk(struct BuildState* state,
                                  struct Str root_dir,
                                  bool is_optional,
                                  struct BuildInputs* inputs_out,
                                  struct Str operation,
                                  struct Log* log) {
  struct OsStat st;
  if (is_optional && os_stat(state->os, root_dir, &st) != 0 && errno == ENOENT) {
    return 0;
  }
  static const struct Str all_suffixes[] = {STR_INIT("")};
  if (walk_files_with_suffixes(&inputs_out->paths, root_dir, state->site_config.output_dir,
                               all_suffixes, 1, false, state->os, &state->perm, state->scratch,
                               operation, log) != 0) {
    return -1;
  }
  inputs_out->stats = walk_stat_paths(&inputs_out->paths, state->os, &state->perm);
  return 0;
}

static bool has_release_projects(const struct SiteConfig* site_config) {
  for (ptrdiff_t i = 0; i < site_config->repo_count; i++) {
    if (release_scan_has_projects(&site_config->repos[i])) {
      return true;
    }
  }
  return false;
}

static int run_build_phases(struct BuildState* state, struct Log* log) {
  // A template that fails to load still lets the scan and the manifest run, so one run reports
  // their errors too, and the check of the log before the first write stops the build.
  (void)load_templates(state, log);
  // The build cache loads before the scan, because the scan decides whether a clone export's pack
  // is current, and it writes a new pack before the manifest, which needs the pack's name.
  load_build_cache(state);
  if (scan_repos(state, log) != 0) {
    return -1;
  }
  (void)populate_output_manifest(state, log);
  if (log->errors > 0) {
    return -1;
  }
  // Nothing before this point writes below `output_dir`.
  if (prepare_output_dir(state, log) != 0 || write_repo_pages(state, log) != 0 ||
      write_clone_exports(state, log) != 0 || write_release_outputs(state, log) != 0 ||
      write_site_pages(state, log) != 0 || copy_static_files(state, log) != 0 ||
      write_build_cache(state, log) != 0 || prune_stale_outputs(state, log) != 0) {
    return -1;
  }
  if (state->writer.is_dry_run && output_writer_report(&state->writer, state->scratch, log) != 0) {
    return -1;
  }
  verbose_printf(&state->verbose, "build complete");
  return 0;
}

static int load_templates(struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "compiling templates");
  return template_set_load(state->site_config.templates_dir, BUILD_PAGE_TEMPLATES, NULL,
                           load_templates_count(&state->site_config), state->os, &state->perm,
                           state->scratch, &state->template_set, log);
}

static ptrdiff_t load_templates_count(const struct SiteConfig* site_config) {
  const ptrdiff_t count =
      (ptrdiff_t)(sizeof(BUILD_PAGE_TEMPLATES) / sizeof(BUILD_PAGE_TEMPLATES[0]));
  const ptrdiff_t release_count =
      (ptrdiff_t)(sizeof((struct Str[]){PAGE_RELEASE_TEMPLATES, PAGE_FEED_RELEASE_TEMPLATES}) /
                  sizeof(struct Str));
  return has_release_projects(site_config) ? count : count - release_count;
}

static void load_build_cache(struct BuildState* state) {
  state->build_cache_path = build_cache_path(state->site_config.output_dir, &state->perm);
  if (state->is_forced) {
    verbose_printf(&state->verbose, "ignoring build cache, making every output");
    return;
  }
  verbose_printf(&state->verbose, "loading build cache");
  struct Str reason = {0};
  const enum BuildCacheFileState cache_state = build_cache_file_load(
      state->build_cache_path, BUILD_CACHE_FILE_LEN_MAX, load_build_cache_parse, state, state->os,
      &state->perm, state->scratch, &reason);
  if (cache_state == BUILD_CACHE_FILE_MISSING) {
    verbose_printf(&state->verbose, "no build cache, making every output");
  } else if (cache_state == BUILD_CACHE_FILE_UNUSABLE) {
    verbose_printf(&state->verbose, "build cache not used, making every output: %.*s",
                   STR_FMT(reason));
  }
}

static int load_build_cache_parse(struct Str text,
                                  struct Arena* perm,
                                  struct Log* log,
                                  void* userdata) {
  struct BuildState* state = userdata;
  return build_cache_parse(text, &state->site_config, perm, &state->build_cache, log);
}

static int scan_repos(struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "scanning repos, workers: %td", state->worker_count);
  return repo_scan_run(&state->site_config, &state->build_cache, state->worker_count,
                       state->worker_arenas, state->verbose.is_enabled, state->os, state->scratch,
                       state->scans, log);
}

static int populate_output_manifest(struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "building output manifest");
  const struct SiteConfig* site_config = &state->site_config;
  const struct PathList inputs = populate_output_manifest_inputs(state);
  struct ManifestBuilder builder;
  manifest_builder_init(&builder, &state->manifest, site_config->output_dir, state->roots,
                        state->root_count, &inputs, state->os, &state->perm);

  bool* is_colliding = arena_new(&state->perm, state->statics.paths.count, bool);
  int rc = populate_output_manifest_check_statics(state, is_colliding, log);
  for (ptrdiff_t i = 0; i < site_config->repo_count; i++) {
    if (export_writer_register(&builder, &state->scans[i], state->scratch, log) != 0) {
      rc = -1;
    }
  }
  if (repo_pages_register(&builder, site_config, state->scans, state->scratch, log) != 0) {
    rc = -1;
  }
  if (site_config->has_headers_file &&
      manifest_builder_add(&builder, EXPORT_WRITER_HEADERS_PAGE, S("headers file"), state->scratch,
                           log) != 0) {
    rc = -1;
  }
  for (ptrdiff_t i = 0; i < site_config->repo_count; i++) {
    if (release_writer_register(&builder, &state->scans[i].model, state->scratch, log) != 0) {
      rc = -1;
    }
  }
  const struct Str pages[] = {PAGE_INDEX_TEMPLATE, PAGE_INDEX_NOT_FOUND_TEMPLATE};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(pages) / sizeof(pages[0])); i++) {
    if (manifest_builder_add(&builder, pages[i], pages[i], state->scratch, log) != 0) {
      rc = -1;
    }
  }
  // A colliding file has been reported, and its path would only collide again.
  if (manifest_builder_add_files(&builder, &state->statics.paths, state->site_config.static_dir,
                                 is_colliding, state->scratch, log) != 0) {
    rc = -1;
  }
  // The build cache is recorded last, so a dry run lists it last.
  struct Str cache_relative = {0};
  (void)path_is_below(state->build_cache_path, site_config->output_dir, &cache_relative);
  if (manifest_builder_add(&builder, cache_relative, S("build cache"), state->scratch, log) != 0) {
    rc = -1;
  }
  if (manifest_builder_finish(&builder, log) != 0) {
    rc = -1;
  }
  return rc;
}

static struct PathList populate_output_manifest_inputs(struct BuildState* state) {
  struct PathList inputs = {0};
  path_list_push(&inputs, state->config_path, &state->perm);
  for (ptrdiff_t i = 0; i < state->templates.paths.count; i++) {
    path_list_push(&inputs, state->templates.paths.items[i], &state->perm);
  }
  for (ptrdiff_t i = 0; i < state->statics.paths.count; i++) {
    path_list_push(&inputs, state->statics.paths.items[i], &state->perm);
  }
  for (ptrdiff_t i = 0; i < state->site_config.repo_count; i++) {
    const struct RepoModel* model = &state->scans[i].model;
    for (ptrdiff_t j = 0; j < model->release_count; j++) {
      const struct Release* release = &model->releases[j];
      for (ptrdiff_t k = 0; k < release->asset_count; k++) {
        path_list_push(&inputs, release->assets[k].source_path, &state->perm);
      }
    }
  }
  return inputs;
}

static int populate_output_manifest_check_statics(const struct BuildState* state,
                                                  bool* is_colliding_out,
                                                  struct Log* log) {
  struct Arena scratch = state->scratch;
  const ptrdiff_t count = state->statics.paths.count;
  // The entries reported so far, each one once, however many files are below it.
  struct Str* reported = arena_new(&scratch, count, struct Str);
  ptrdiff_t reported_count = 0;
  int rc = 0;
  for (ptrdiff_t i = 0; i < count; i++) {
    is_colliding_out[i] = false;
    struct Str relative = {0};
    (void)path_is_below(state->statics.paths.items[i], state->site_config.static_dir, &relative);
    const struct Str entry = str_cut(relative, '/').head;
    const struct Str stem =
        text_has_suffix_folded(entry, S(".git")) ? str_head(entry, entry.len - 4) : entry;
    for (ptrdiff_t j = 0; j < state->site_config.repo_count && !is_colliding_out[i]; j++) {
      // A static entry named after a repo, or after its clone export, would share its directory on
      // a case-insensitive host.
      const struct Str name = state->site_config.repos[j].name;
      if (!str_eq_folded(stem, name)) {
        continue;
      }
      is_colliding_out[i] = true;
      bool is_reported = false;
      for (ptrdiff_t k = 0; k < reported_count && !is_reported; k++) {
        is_reported = str_eq(reported[k], entry);
      }
      if (!is_reported) {
        reported[reported_count++] = entry;
        const struct Str entry_path = path_join(state->site_config.static_dir, entry, &scratch);
        rc = log_error(log, "static entry collides with repo '%.*s' after case folding: '%.*s'",
                       STR_FMT(name), STR_FMT(entry_path));
      }
    }
  }
  return rc;
}

static int prepare_output_dir(const struct BuildState* state, struct Log* log) {
  if (output_writer_mkdirs(&state->writer, state->site_config.output_dir,
                           S("failed to prepare output directory"), log) != 0) {
    return -1;
  }
  // A build that fails or is interrupted from here on leaves an empty build cache, which the next
  // build does not use, so it never keeps an output this build may have cut short.
  oom_set_job(state->build_cache_path, NULL);
  const int rc = output_writer_write(&state->writer, state->build_cache_path, (struct OsSource){0},
                                     S("failed to empty build cache"), log);
  // The phases that follow name their own files, and the main thread's work between them none.
  oom_set_job((struct Str){0}, NULL);
  return rc;
}

static int write_repo_pages(struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "rendering repo pages, workers: %td", state->worker_count);
  const ptrdiff_t repo_count = state->site_config.repo_count;
  bool* is_repo_fresh = arena_new(&state->perm, repo_count, bool);
  bool* is_releases_fresh = arena_new(&state->perm, repo_count, bool);
  bool* is_repo_map_fresh = arena_new(&state->perm, repo_count, bool);
  for (ptrdiff_t i = 0; i < repo_count; i++) {
    is_repo_fresh[i] = is_repo_unchanged(state, i);
    is_releases_fresh[i] = is_releases_unchanged(state, i);
    is_repo_map_fresh[i] = is_repo_map_unchanged(state, i);
  }
  const struct RepoPagesInputs inputs = {
      .site_config = &state->site_config,
      .scans = state->scans,
      .template_set = state->template_set,
      .cache = &state->build_cache,
      .is_templates_fresh = is_every_file_unchanged(
          state, BUILD_CACHE_KIND_TEMPLATE, &state->templates, state->site_config.templates_dir),
      .is_repo_fresh = is_repo_fresh,
      .is_releases_fresh = is_releases_fresh,
      .is_repo_map_fresh = is_repo_map_fresh,
  };
  struct RepoPagesNotes* notes = NULL;
  if (repo_pages_write(&inputs, &state->writer, state->worker_count, state->worker_arenas,
                       state->verbose.is_enabled, state->os, &state->perm, state->scratch, &notes,
                       log) != 0) {
    return -1;
  }
  for (ptrdiff_t i = 0; i < repo_count; i++) {
    const struct Str name = state->site_config.repos[i].name;
    if (notes[i].missing_link_count > 0) {
      verbose_printf(&state->verbose,
                     "links to missing paths on pages rendered this build, left as written: %td "
                     "(in repo '%.*s')",
                     notes[i].missing_link_count, STR_FMT(name));
    }
    if (notes[i].raw_skipped_count > 0) {
      verbose_printf(
          &state->verbose,
          "blobs over config key 'limits.raw_len_max', without raw copies: %td (in repo '%.*s')",
          notes[i].raw_skipped_count, STR_FMT(name));
    }
  }
  return 0;
}

static bool is_repo_unchanged(const struct BuildState* state, ptrdiff_t repo_index) {
  const struct RepoScan* scan = &state->scans[repo_index];
  const struct Str repo_name = scan->model.config->name;
  const struct BuildCacheRow* repo_row =
      build_cache_find(&state->build_cache, BUILD_CACHE_KIND_REPO, repo_name);
  if (repo_row == NULL || !str_eq(repo_row->id, scan->repo_id)) {
    return false;
  }
  struct Arena scratch = state->scratch;
  for (ptrdiff_t i = 0; i < scan->model.refset.count; i++) {
    const struct Ref* ref = &scan->model.refset.refs[i];
    const struct BuildCacheRow* row =
        build_cache_find(&state->build_cache, BUILD_CACHE_KIND_REF,
                         build_cache_ref_name(repo_name, ref->name, &scratch));
    if (row == NULL || !str_eq(row->id, ref->oid)) {
      return false;
    }
  }
  // A ref the earlier build saw and this one does not changes the refs pages too. A repo name holds
  // no `/`, so `<repo>/` starts exactly this repo's ref rows.
  const struct Str prefix = str_concat(repo_name, S("/"), &scratch);
  ptrdiff_t cached_count = 0;
  for (ptrdiff_t i = 0; i < state->build_cache.count; i++) {
    const struct BuildCacheRow* row = &state->build_cache.rows[i];
    cached_count += row->kind == BUILD_CACHE_KIND_REF && str_startswith(row->name, prefix);
  }
  return cached_count == scan->model.refset.count;
}

static int write_clone_exports(const struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "writing clone exports");
  for (ptrdiff_t i = 0; i < state->site_config.repo_count; i++) {
    oom_set_job(state->site_config.repos[i].name, NULL);
    if (export_writer_write(&state->writer, state->site_config.output_dir, &state->scans[i],
                            state->os, state->scratch, log) != 0) {
      return -1;
    }
  }
  if (state->site_config.has_headers_file) {
    oom_set_job(EXPORT_WRITER_HEADERS_PAGE, NULL);
    return export_writer_write_headers(&state->writer, &state->site_config, state->os,
                                       state->scratch, log);
  }
  return 0;
}

static int write_release_outputs(const struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "writing releases");
  const bool is_templates_fresh = is_every_file_unchanged(
      state, BUILD_CACHE_KIND_TEMPLATE, &state->templates, state->site_config.templates_dir);
  int rc = 0;
  for (ptrdiff_t i = 0; i < state->site_config.repo_count; i++) {
    oom_set_job(state->site_config.repos[i].name, NULL);
    const struct ReleaseWriterInputs inputs = {
        .site_config = &state->site_config,
        .template_set = state->template_set,
        .repo = &state->scans[i].model,
        .is_templates_fresh = is_templates_fresh,
        .is_releases_fresh = is_releases_unchanged(state, i),
        .is_repo_map_fresh = is_repo_map_unchanged(state, i),
    };
    if (release_writer_write(&inputs, &state->writer, state->os, state->scratch, log) != 0) {
      rc = -1;
    }
  }
  return rc;
}

static bool is_releases_unchanged(const struct BuildState* state, ptrdiff_t repo_index) {
  const struct RepoScan* scan = &state->scans[repo_index];
  const struct BuildCacheRow* row =
      build_cache_find(&state->build_cache, BUILD_CACHE_KIND_RELEASES, scan->model.config->name);
  // A repo without release projects has no row, and is fresh when the earlier build left none.
  if (scan->model.release_project_count == 0) {
    return row == NULL && state->build_cache.count > 0;
  }
  return row != NULL && str_eq(row->id, scan->releases_id);
}

static bool is_repo_map_unchanged(const struct BuildState* state, ptrdiff_t repo_index) {
  const struct RepoScan* scan = &state->scans[repo_index];
  const struct BuildCacheRow* row =
      build_cache_find(&state->build_cache, BUILD_CACHE_KIND_REPO_MAP, scan->model.config->name);
  return row != NULL && str_eq(row->id, scan->repo_map_id);
}

static int write_site_pages(const struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "rendering site index");
  const bool is_templates_fresh = is_every_file_unchanged(
      state, BUILD_CACHE_KIND_TEMPLATE, &state->templates, state->site_config.templates_dir);
  const bool is_index_fresh = is_templates_fresh && is_every_repo_unchanged(state);
  // Each value tree lives in a copy of the scratch, and its render gets the rest of that copy.
  struct Arena scratch = state->scratch;
  struct RepoModel* models = arena_new(&scratch, state->site_config.repo_count, struct RepoModel);
  for (ptrdiff_t i = 0; i < state->site_config.repo_count; i++) {
    models[i] = state->scans[i].model;
  }
  const struct TemplateValue* index_value =
      page_index_value(&state->site_config, models, state->site_config.repo_count, &scratch);
  if (write_site_page(state, PAGE_INDEX_TEMPLATE, index_value, is_index_fresh, scratch, log) != 0) {
    return -1;
  }
  scratch = state->scratch;
  const struct TemplateValue* not_found_value =
      page_index_not_found_value(&state->site_config, &scratch);
  return write_site_page(state, PAGE_INDEX_NOT_FOUND_TEMPLATE, not_found_value, is_templates_fresh,
                         scratch, log);
}

static int write_site_page(const struct BuildState* state,
                           struct Str name,
                           const struct TemplateValue* value,
                           bool is_vouched,
                           struct Arena scratch,
                           struct Log* log) {
  oom_set_job(name, NULL);
  const struct Str output_path = path_join(state->site_config.output_dir, name, &scratch);
  struct OsStat st;
  if (is_vouched && os_stat(state->os, output_path, &st) == 0 && !st.is_dir) {
    return output_writer_keep(&state->writer, output_path, log);
  }
  // The page grows in a perm of its own, which the render's scratch does not overlap, so it grows
  // in place. Each gets half of what is left, up to the region's limit, so a halved reservation
  // halves both.
  const char* region_end = scratch.limit != NULL ? scratch.limit : scratch.end;
  struct Arena render_scratch = arena_split(&scratch, (region_end - scratch.beg) / 2);
  struct Str html = {0};
  if (template_value_render(state->template_set, name, value, name, &scratch, render_scratch, &html,
                            log) != 0) {
    return -1;
  }
  return output_writer_write(&state->writer, output_path, (struct OsSource){.bytes = html},
                             S("failed to write page"), log);
}

static bool is_every_file_unchanged(const struct BuildState* state,
                                    enum BuildCacheKind kind,
                                    const struct BuildInputs* inputs,
                                    struct Str root_dir) {
  struct Arena scratch = state->scratch;
  struct BuildCache rows = {.rows = arena_new(&scratch, inputs->paths.count, struct BuildCacheRow)};
  append_file_rows(&rows, kind, inputs, root_dir);
  // An empty build cache has no rows, and a site always has its templates, so this also rejects a
  // build cache that was not read.
  return state->build_cache.count > 0 &&
         build_cache_has_rows(&state->build_cache, kind, rows.rows, rows.count);
}

static bool is_every_repo_unchanged(const struct BuildState* state) {
  struct Arena scratch = state->scratch;
  struct BuildCache rows = {
      .rows = arena_new(&scratch, count_repo_rows(state), struct BuildCacheRow),
  };
  append_repo_rows(&rows, state, &scratch);
  if (rows.count > 0) {
    qsort(rows.rows, (size_t)rows.count, sizeof(*rows.rows), build_cache_compare_rows);
  }
  // The repo rows sort before the ref rows, so each kind is one contiguous run.
  ptrdiff_t repo_count = 0;
  while (repo_count < rows.count && rows.rows[repo_count].kind == BUILD_CACHE_KIND_REPO) {
    repo_count++;
  }
  return build_cache_has_rows(&state->build_cache, BUILD_CACHE_KIND_REPO, rows.rows, repo_count) &&
         build_cache_has_rows(&state->build_cache, BUILD_CACHE_KIND_REF, rows.rows + repo_count,
                              rows.count - repo_count);
}

static int copy_static_files(const struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "copying static files");
  struct CopyRows rows = {.cache = &state->build_cache, .kind = BUILD_CACHE_KIND_STATIC};
  return output_writer_copy_files(&state->writer, &state->statics.paths, state->statics.stats,
                                  copy_static_files_is_unchanged, &rows,
                                  state->site_config.static_dir, state->site_config.output_dir,
                                  state->scratch, S("failed to copy static file"), log);
}

static bool copy_static_files_is_unchanged(struct Str name,
                                           const struct OsStat* source_stat,
                                           void* userdata) {
  const struct CopyRows* rows = userdata;
  return build_cache_has_stat(rows->cache, rows->kind, name, source_stat);
}

static int write_build_cache(const struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "writing build cache");
  oom_set_job(state->build_cache_path, NULL);
  struct Arena scratch = state->scratch;
  const ptrdiff_t row_count = state->templates.paths.count + state->statics.paths.count +
                              count_repo_rows(state) + count_repo_export_rows(state) +
                              repo_pages_count_rows(&state->site_config, state->scans);
  struct BuildCache cache = {.rows = arena_new(&scratch, row_count, struct BuildCacheRow)};
  append_file_rows(&cache, BUILD_CACHE_KIND_TEMPLATE, &state->templates,
                   state->site_config.templates_dir);
  append_file_rows(&cache, BUILD_CACHE_KIND_STATIC, &state->statics, state->site_config.static_dir);
  append_repo_rows(&cache, state, &scratch);
  append_repo_export_rows(&cache, state);
  repo_pages_append_rows(&cache, &state->site_config, state->scans, &scratch);
  struct Buf text = {.arena = &scratch};
  build_cache_format(&text, &state->site_config, &cache);
  return output_writer_write(&state->writer, state->build_cache_path,
                             (struct OsSource){.bytes = buf_str(&text)},
                             S("failed to write build cache"), log);
}

static void append_file_rows(struct BuildCache* cache,
                             enum BuildCacheKind kind,
                             const struct BuildInputs* inputs,
                             struct Str root_dir) {
  for (ptrdiff_t i = 0; i < inputs->paths.count; i++) {
    struct Str relative = {0};
    (void)path_is_below(inputs->paths.items[i], root_dir, &relative);
    cache->rows[cache->count++] = (struct BuildCacheRow){
        .kind = kind,
        .name = relative,
        .len = inputs->stats[i].len,
        .mtime_s = inputs->stats[i].mtime_s,
        .mtime_ns = inputs->stats[i].mtime_ns,
    };
  }
}

static void append_repo_rows(struct BuildCache* cache,
                             const struct BuildState* state,
                             struct Arena* arena) {
  for (ptrdiff_t i = 0; i < state->site_config.repo_count; i++) {
    const struct RepoScan* scan = &state->scans[i];
    const struct Str repo_name = scan->model.config->name;
    cache->rows[cache->count++] = (struct BuildCacheRow){
        .kind = BUILD_CACHE_KIND_REPO,
        .name = repo_name,
        .id = scan->repo_id,
    };
    for (ptrdiff_t j = 0; j < scan->model.refset.count; j++) {
      const struct Ref* ref = &scan->model.refset.refs[j];
      cache->rows[cache->count++] = (struct BuildCacheRow){
          .kind = BUILD_CACHE_KIND_REF,
          .name = build_cache_ref_name(repo_name, ref->name, arena),
          .id = ref->oid,
      };
    }
  }
}

static ptrdiff_t count_repo_rows(const struct BuildState* state) {
  ptrdiff_t count = state->site_config.repo_count;
  for (ptrdiff_t i = 0; i < state->site_config.repo_count; i++) {
    count += state->scans[i].model.refset.count;
  }
  return count;
}

static void append_repo_export_rows(struct BuildCache* cache, const struct BuildState* state) {
  for (ptrdiff_t i = 0; i < state->site_config.repo_count; i++) {
    const struct RepoScan* scan = &state->scans[i];
    cache->rows[cache->count++] = (struct BuildCacheRow){
        .kind = BUILD_CACHE_KIND_REPO_MAP,
        .name = scan->model.config->name,
        .id = scan->repo_map_id,
    };
    cache->rows[cache->count++] = export_writer_row(scan);
    if (scan->model.release_project_count > 0) {
      cache->rows[cache->count++] = (struct BuildCacheRow){
          .kind = BUILD_CACHE_KIND_RELEASES,
          .name = scan->model.config->name,
          .id = scan->releases_id,
      };
    }
  }
}

static ptrdiff_t count_repo_export_rows(const struct BuildState* state) {
  ptrdiff_t count = 0;
  for (ptrdiff_t i = 0; i < state->site_config.repo_count; i++) {
    count += 2 + (state->scans[i].model.release_project_count > 0);
  }
  return count;
}

static int prune_stale_outputs(struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "removing stale outputs");
  oom_set_job((struct Str){0}, NULL);
  struct Str* root_dirs = arena_new(&state->perm, state->root_count, struct Str);
  for (ptrdiff_t i = 0; i < state->root_count; i++) {
    root_dirs[i] = state->roots[i].dir_path;
  }
  ptrdiff_t removed_count = 0;
  const int rc =
      output_pruner_prune(&state->writer, state->site_config.output_dir, root_dirs,
                          state->root_count, state->os, state->scratch, &removed_count, log);
  verbose_printf(&state->verbose, "removed stale outputs: %td", removed_count);
  return rc;
}
