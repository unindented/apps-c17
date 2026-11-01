#include "app/cmd_build.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
#include "domain/repo_model.h"
#include "domain/site_config.h"
#include "domain/surge_version.h"
#include "formats/tsv.h"
#include "markup/highlight.h"
#include "os/oom.h"
#include "os/os.h"
#include "output/build_cache_file.h"
#include "output/manifest.h"
#include "output/manifest_builder.h"
#include "output/output_cache.h"
#include "output/output_pruner.h"
#include "output/output_writer.h"
#include "runtime/build_run.h"
#include "runtime/job.h"
#include "runtime/read_root.h"
#include "runtime/verbose.h"
#include "runtime/walk.h"
#include "templates/template_set.h"
#include "vcs/repository.h"

/** Path of the output cache below `output_dir`. */
#define OUTPUT_CACHE_PATH S("_surge/output-cache.tsv")

/** Hex digits of the page recipe's id that every page declares. */
enum { BUILD_RECIPE_ID_LEN = 16 };

/** Bytes in a mebibyte, the unit of the sizes below. */
#define BUILD_MIB ((ptrdiff_t)1024 * 1024)

_Static_assert(PTRDIFF_MAX > 0xffffffff, "the arenas below need a 64-bit address space");

/** Bytes of the build's log, and of each worker's share of the job logs. */
#define BUILD_LOG_LEN (256 * BUILD_MIB)

/** Bytes of each thread's perm, which holds what its scans keep beside one render's buffer. */
#define BUILD_PERM_LEN (16 * (ptrdiff_t)TEMPLATE_RENDER_OUTPUT_LEN_MAX)

/** Bytes of each thread's scratch, whose largest use is a raw copy's blob beside its output. */
#define BUILD_SCRATCH_LEN (3 * 1024 * BUILD_MIB)
_Static_assert(2 * SURGE_RAW_LEN_MAX_LIMIT + BUILD_MIB <= BUILD_SCRATCH_LEN,
               "the scratch must hold a raw copy's comparison");

/** The page templates a build renders, the release templates last. */
static const struct Str BUILD_PAGE_TEMPLATES[] = {
    STR_INIT("index.html"), STR_INIT("404.html"),   REPO_PAGES_TEMPLATES,
    PAGE_FEED_TEMPLATES,    PAGE_RELEASE_TEMPLATES, PAGE_FEED_RELEASE_TEMPLATES,
};

/** The files the build reads below a root, with what `os_stat` reported for each. */
struct BuildInputs {
  /** Paths of the files, sorted. */
  struct PathList paths;

  /**
   * Stat of each of `paths`, parallel to it. A file that cannot be inspected has a negative `len`,
   * as `walk_stat_paths` marks it.
   */
  struct OsStat* stats;
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

  /** Every file below `templates_dir`, which the page recipe names. */
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

  /** Path of the output cache below `output_dir`. */
  struct Str cache_path;

  /** What an earlier build left, empty when there is none or the build ignores it. */
  struct OutputCache cache;

  /** Id of the page recipe, which every page declares. */
  struct Str recipe_id;

  /** Workers a parallel phase runs. */
  ptrdiff_t worker_count;

  /** Where the build's status lines go, which `--verbose` enables. */
  struct Verbose verbose;
};

/** What makes a page of the root, for `output_writer_produce`. */
struct SitePageMake {
  /** Build state. */
  const struct BuildState* state;

  /** Name of the page's template, and of its output below `output_dir`. */
  struct Str name;
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
 * @brief Loads the configuration, checks the read roots, sets libgit2 up, opens every repo, rejects
 *        an `output_dir` that overlaps an input, and lists the templates and the static files.
 *
 * Each step stops the load when it fails.
 *
 * @param state Build state that receives the inputs. Must not be `NULL`.
 * @param log   Log that receives the errors of the step that failed. Must not be `NULL`.
 * @return `0` when the build has inputs to go on with, or `-1` otherwise.
 */
static int load_build_inputs(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Requires `templates_dir` to be a directory, and `static_dir` and each repo's
 *        `releases_dir` to be one when it exists.
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
 * @brief Runs the phases after the inputs are loaded: the templates, the page recipe, the build
 *        cache, the scan, the manifest, and then the writes, the output cache, the prune and a dry
 *        run's report.
 *
 * Once `output_dir` is prepared, the output cache is written even when a write fails, as it records
 * only the outputs this build wrote or kept.
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
 * @brief Returns the id of the page recipe: what every page depends on besides its own inputs.
 *
 * The recipe is the generator string with the version, the grammars the build links, the site keys
 * and the limits of the config but `pack_len_max`, and the path, size and modification time of
 * every file below `templates_dir`, each text with its `\`, tab and newline escaped, one
 * `name=value` line each.
 *
 * @param state Build state whose inputs are loaded. Must not be `NULL`.
 * @return The id, in the main thread's perm.
 */
static struct Str load_recipe_id(struct BuildState* state) __attribute__((nonnull(1)));

/**
 * @brief Reads the output cache an earlier build left, unless the build ignores it. A missing or
 *        unusable one is an empty one, with a verbose note.
 *
 * @param state Build state that receives the output cache. Must not be `NULL`.
 */
static void load_output_cache(struct BuildState* state) __attribute__((nonnull(1)));

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
 * @brief Records `index.html` and `404.html`: the site index declares the page recipe and every
 *        repo and ref, and the not-found page the recipe.
 *
 * @param state   Build state whose repos are scanned. Must not be `NULL`.
 * @param builder Prepared manifest builder. Must not be `NULL`.
 * @param log     Log that receives every refusal. Must not be `NULL`.
 * @return `0` when both paths were recorded, or `-1` otherwise.
 */
static int populate_output_manifest_site_pages(const struct BuildState* state,
                                               struct ManifestBuilder* builder,
                                               struct Log* log) __attribute__((nonnull(1, 2, 3)));

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
 * @brief Creates `output_dir`, the build's first write.
 *
 * @param state Build state. Must not be `NULL`.
 * @param log   Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when a component of `output_dir` cannot be created.
 */
static int prepare_output_dir(const struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Writes every output, or keeps each one the output cache vouches for, phase by phase, and
 *        stops at the first phase that fails.
 *
 * @param state Build state whose manifest is complete. Must not be `NULL`.
 * @param log   Log that receives every failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when an output cannot be made or written.
 */
static int write_outputs(struct BuildState* state, struct Log* log) __attribute__((nonnull(1, 2)));

/**
 * @brief Keeps the pack before each repo's new clone pack, first, so a build that fails before it
 *        writes the new pack still records the old one for the next build.
 *
 * @param state Build state whose repos are scanned. Must not be `NULL`.
 * @param log   Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when a keep fails.
 */
static int keep_previous_packs(const struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Writes every repo's pages on the worker pool, or keeps each one the output cache vouches
 *        for, and notes what they left out when the build is verbose.
 *
 * @param state Build state whose repos are scanned. Must not be `NULL`.
 * @param log   Log that receives every failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when a page cannot be written.
 */
static int write_repo_pages(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

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
 * @brief Makes one page of the root, or keeps it.
 *
 * @param state   Build state. Must not be `NULL`.
 * @param name    Name of the page's template, and of its output below `output_dir`.
 * @param scratch Arena for the page and its render.
 * @param log     Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the render or the write fails.
 */
static int write_site_page(const struct BuildState* state,
                           struct Str name,
                           struct Arena scratch,
                           struct Log* log) __attribute__((nonnull(1, 4)));

/**
 * @brief Renders one page of the root into `perm`, its data tree first, as `OutputWriterMakeFn`.
 *
 * @param perm      Arena that owns the data tree and the page. Must not be `NULL`.
 * @param scratch   Arena for the render's temporaries.
 * @param bytes_out Receives the page. Must not be `NULL`.
 * @param log       Log that receives the failure. Must not be `NULL`.
 * @param userdata  The `struct SitePageMake`.
 * @return `0` on success, or `-1` when the render fails.
 */
static int write_site_page_make(struct Arena* perm,
                                struct Arena scratch,
                                struct Str* bytes_out,
                                struct Log* log,
                                void* userdata) __attribute__((nonnull(1, 3, 4, 5)));

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
 * @brief Writes the output cache: a row for every output this build wrote or kept that declares
 *        attributes, and the fact of each clone export's pack.
 *
 * @param state Build state. Must not be `NULL`.
 * @param log   Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the write fails.
 */
static int write_output_cache(const struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

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
              .is_forced = options->is_forced,
              .perm = &state->perm,
          },
      .worker_count = options->worker_count,
      .verbose = {.os = os, .is_enabled = options->is_verbose, .scratch = &state->scratch},
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
  state->writer.output_dir = site_config->output_dir;
  if (load_build_inputs_require_read_roots(state, log) != 0 || repository_setup(log) != 0) {
    return -1;
  }
  state->scans = arena_new(&state->perm, site_config->repo_count, struct RepoScan);
  if (repo_scan_locate(site_config, &state->perm, state->scans, log) != 0) {
    return -1;
  }
  state->roots = load_build_inputs_roots(state, &state->root_count);
  if (manifest_builder_check_output_dir(site_config->output_dir, state->roots, state->root_count,
                                        state->config_path, state->os, state->scratch, log) != 0) {
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
  roots[root_count++] =
      (struct ReadRoot){.dir_path = site_config->templates_dir, .key = S("templates_dir")};
  roots[root_count++] = (struct ReadRoot){
      .dir_path = site_config->static_dir, .key = S("static_dir"), .is_optional = true};
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
  state->recipe_id = load_recipe_id(state);
  // The output cache loads before the scan, because the scan decides whether a clone export's pack
  // is current, and it writes a new pack before the manifest, which needs the pack's name.
  load_output_cache(state);
  if (scan_repos(state, log) != 0) {
    return -1;
  }
  (void)populate_output_manifest(state, log);
  if (log->errors > 0) {
    return -1;
  }
  // Nothing before this point writes below `output_dir`.
  if (prepare_output_dir(state, log) != 0) {
    return -1;
  }
  const int write_rc = write_outputs(state, log);
  if (write_output_cache(state, log) != 0 || write_rc != 0 ||
      prune_stale_outputs(state, log) != 0) {
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

static struct Str load_recipe_id(struct BuildState* state) {
  const struct SiteConfig* site_config = &state->site_config;
  const struct SurgeLimits* limits = &site_config->limits;
  struct Arena scratch = state->scratch;
  struct Buf text = {.arena = &scratch};
  const struct Str generator = surge_generator_string();
  const struct Str grammars = highlight_grammar_list();
  buf_printf(&text, "generator=%.*s\ngrammars=%.*s\n", STR_FMT(generator), STR_FMT(grammars));
  const struct {
    const char* name;
    struct Str value;
  } texts[] = {
      {"title", site_config->title},
      {"description", site_config->description},
      {"author", site_config->author},
      {"base_url", site_config->base_url},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(texts) / sizeof(texts[0])); i++) {
    buf_printf(&text, "%s=", texts[i].name);
    tsv_escape_append(&text, texts[i].value);
    buf_append_char(&text, '\n');
  }
  buf_printf(&text,
             "log_page_size=%td\nfeed_count=%td\nblob_len_max=%td\nraw_len_max=%td\n"
             "diff_file_count_max=%td\ndiff_line_count_max=%td\ndiff_len_max=%td\n"
             "image_len_max=%td\n",
             site_config->log_page_size, site_config->feed_count, limits->blob_len_max,
             limits->raw_len_max, limits->diff_file_count_max, limits->diff_line_count_max,
             limits->diff_len_max, limits->image_len_max);
  for (ptrdiff_t i = 0; i < state->templates.paths.count; i++) {
    const struct OsStat* stat = &state->templates.stats[i];
    struct Str relative = {0};
    (void)path_is_below(state->templates.paths.items[i], site_config->templates_dir, &relative);
    buf_append(&text, S("template="));
    tsv_escape_append(&text, relative);
    buf_printf(&text, " len=%td mtime=%" PRId64 ".%09" PRId64 "\n", stat->len, stat->mtime_s,
               stat->mtime_ns);
  }
  // Every page declares the id, so a prefix of it, which two recipes share by chance once in 2^64,
  // keeps the output cache short.
  return str_head(repository_text_id(buf_str(&text), &state->perm), BUILD_RECIPE_ID_LEN);
}

static void load_output_cache(struct BuildState* state) {
  state->cache_path = path_join(state->site_config.output_dir, OUTPUT_CACHE_PATH, &state->perm);
  if (state->writer.is_forced) {
    verbose_printf(&state->verbose, "ignoring output cache, making every output");
    return;
  }
  verbose_printf(&state->verbose, "loading output cache");
  struct Str reason = {0};
  const enum BuildCacheFileState cache_state = output_cache_load(
      state->cache_path, state->os, &state->perm, state->scratch, &state->cache, &reason);
  if (cache_state == BUILD_CACHE_FILE_MISSING) {
    verbose_printf(&state->verbose, "no output cache, making every output");
  } else if (cache_state == BUILD_CACHE_FILE_UNUSABLE) {
    verbose_printf(&state->verbose, "output cache not used, making every output: %.*s",
                   STR_FMT(reason));
  }
}

static int scan_repos(struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "scanning repos, workers: %td", state->worker_count);
  return repo_scan_run(&state->site_config, &state->cache, state->writer.is_dry_run,
                       state->worker_count, state->worker_arenas, state->verbose.is_enabled,
                       state->os, state->scratch, state->scans, log);
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
  if (repo_pages_register(&builder, site_config, state->scans, state->recipe_id, state->scratch,
                          log) != 0) {
    rc = -1;
  }
  if (site_config->has_headers_file &&
      manifest_builder_add(&builder, EXPORT_WRITER_HEADERS_PAGE, S("headers file"), state->scratch,
                           log) != 0) {
    rc = -1;
  }
  for (ptrdiff_t i = 0; i < site_config->repo_count; i++) {
    if (release_writer_register(&builder, &state->scans[i], state->recipe_id, state->scratch,
                                log) != 0) {
      rc = -1;
    }
  }
  if (populate_output_manifest_site_pages(state, &builder, log) != 0) {
    rc = -1;
  }
  // A colliding file has been reported, and its path would only collide again.
  if (manifest_builder_add_copies(&builder, &state->statics.paths, state->statics.stats,
                                  state->site_config.static_dir, is_colliding, state->scratch,
                                  log) != 0) {
    rc = -1;
  }
  // The output cache is recorded last, so a dry run lists it last.
  if (manifest_builder_add(&builder, OUTPUT_CACHE_PATH, S("output cache"), state->scratch, log) !=
      0) {
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

static int populate_output_manifest_site_pages(const struct BuildState* state,
                                               struct ManifestBuilder* builder,
                                               struct Log* log) {
  // The site index shows every repo and its refs, as the home and the refs pages do.
  struct Arena scratch = state->scratch;
  struct Buf repos = {.arena = &scratch};
  for (ptrdiff_t i = 0; i < state->site_config.repo_count; i++) {
    const struct RepoScan* scan = &state->scans[i];
    buf_printf(&repos, "%.*s %.*s %.*s\n", STR_FMT(scan->model.config->name),
               STR_FMT(scan->repo_id), STR_FMT(scan->export.info_refs_id));
  }
  const struct Str repos_id = repository_text_id(buf_str(&repos), &scratch);
  struct Buf index = {.arena = &scratch};
  buf_printf(&index, "recipe=%.*s repos=%.*s", STR_FMT(state->recipe_id), STR_FMT(repos_id));
  const struct Str index_attributes = buf_str(&index);
  const struct Str not_found_attributes = str_concat(S("recipe="), state->recipe_id, &scratch);
  const struct {
    struct Str name;
    struct Str attributes;
  } pages[] = {
      {PAGE_INDEX_TEMPLATE, index_attributes},
      {PAGE_INDEX_NOT_FOUND_TEMPLATE, not_found_attributes},
  };
  int rc = 0;
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(pages) / sizeof(pages[0])); i++) {
    if (manifest_builder_add_output(builder, pages[i].name, pages[i].name, pages[i].attributes,
                                    scratch, log) != 0) {
      rc = -1;
    }
  }
  return rc;
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
  return output_writer_mkdirs(&state->writer, state->site_config.output_dir,
                              S("failed to prepare output directory"), log);
}

static int write_outputs(struct BuildState* state, struct Log* log) {
  return keep_previous_packs(state, log) != 0 || write_repo_pages(state, log) != 0 ||
                 write_clone_exports(state, log) != 0 || write_release_outputs(state, log) != 0 ||
                 write_site_pages(state, log) != 0 || copy_static_files(state, log) != 0
             ? -1
             : 0;
}

static int keep_previous_packs(const struct BuildState* state, struct Log* log) {
  for (ptrdiff_t i = 0; i < state->site_config.repo_count; i++) {
    if (export_writer_keep_previous(&state->writer, state->site_config.output_dir, &state->scans[i],
                                    state->scratch, log) != 0) {
      return -1;
    }
  }
  return 0;
}

static int write_repo_pages(struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "rendering repo pages, workers: %td", state->worker_count);
  const ptrdiff_t repo_count = state->site_config.repo_count;
  const struct RepoPagesInputs inputs = {
      .site_config = &state->site_config,
      .scans = state->scans,
      .template_set = state->template_set,
      .cache = &state->cache,
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

static int write_clone_exports(const struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "writing clone exports");
  for (ptrdiff_t i = 0; i < state->site_config.repo_count; i++) {
    oom_set_job(state->site_config.repos[i].name, NULL);
    if (export_writer_write(&state->writer, state->site_config.output_dir, &state->scans[i],
                            &state->cache, state->scratch, log) != 0) {
      return -1;
    }
  }
  if (state->site_config.has_headers_file) {
    oom_set_job(EXPORT_WRITER_HEADERS_PAGE, NULL);
    return export_writer_write_headers(&state->writer, &state->site_config, &state->cache,
                                       state->scratch, log);
  }
  return 0;
}

static int write_release_outputs(const struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "writing releases");
  int rc = 0;
  for (ptrdiff_t i = 0; i < state->site_config.repo_count; i++) {
    oom_set_job(state->site_config.repos[i].name, NULL);
    const struct ReleaseWriterInputs inputs = {
        .site_config = &state->site_config,
        .template_set = state->template_set,
        .repo = &state->scans[i].model,
        .cache = &state->cache,
    };
    if (release_writer_write(&inputs, &state->writer, state->scratch, log) != 0) {
      rc = -1;
    }
  }
  return rc;
}

static int write_site_pages(const struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "rendering site index");
  if (write_site_page(state, PAGE_INDEX_TEMPLATE, state->scratch, log) != 0) {
    return -1;
  }
  return write_site_page(state, PAGE_INDEX_NOT_FOUND_TEMPLATE, state->scratch, log);
}

static int write_site_page(const struct BuildState* state,
                           struct Str name,
                           struct Arena scratch,
                           struct Log* log) {
  oom_set_job(name, NULL);
  const struct Str output_path = path_join(state->site_config.output_dir, name, &scratch);
  // The page grows in a perm of its own, after its data tree, which the render's scratch does not
  // overlap, so it grows in place. Each gets half of what is left, up to the region's limit, so a
  // halved reservation halves both, and the scratch holds the read of the page in the output too.
  const char* region_end = scratch.limit != NULL ? scratch.limit : scratch.end;
  struct Arena page_arena = arena_split(&scratch, (region_end - scratch.beg) / 2);
  struct SitePageMake make = {.state = state, .name = name};
  return output_writer_produce(&state->writer, output_path, &state->cache, write_site_page_make,
                               &make, page_arena, scratch, S("failed to write page"), log);
}

static int write_site_page_make(struct Arena* perm,
                                struct Arena scratch,
                                struct Str* bytes_out,
                                struct Log* log,
                                void* userdata) {
  const struct SitePageMake* make = userdata;
  const struct BuildState* state = make->state;
  const struct TemplateValue* value = NULL;
  if (str_eq(make->name, PAGE_INDEX_TEMPLATE)) {
    struct RepoModel* models = arena_new(perm, state->site_config.repo_count, struct RepoModel);
    for (ptrdiff_t i = 0; i < state->site_config.repo_count; i++) {
      models[i] = state->scans[i].model;
    }
    value = page_index_value(&state->site_config, models, state->site_config.repo_count, perm);
  } else {
    value = page_index_not_found_value(&state->site_config, perm);
  }
  return template_value_render(state->template_set, make->name, value, make->name, perm, scratch,
                               bytes_out, log);
}

static int copy_static_files(const struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "copying static files");
  for (ptrdiff_t i = 0; i < state->statics.paths.count; i++) {
    const struct Str source_path = state->statics.paths.items[i];
    oom_set_job(source_path, NULL);
    // Each copy works in its own copy of the scratch, so one file's path is dead before the next.
    struct Arena scratch = state->scratch;
    struct Str relative = {0};
    (void)path_is_below(source_path, state->site_config.static_dir, &relative);
    const struct Str output_path = path_join(state->site_config.output_dir, relative, &scratch);
    // A copy is streamed, never made, so it needs no make.
    if (output_writer_produce(&state->writer, output_path, &state->cache, NULL, NULL, scratch,
                              scratch, S("failed to copy static file"), log) != 0) {
      return -1;
    }
  }
  oom_set_job((struct Str){0}, NULL);
  return 0;
}

static int write_output_cache(const struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "writing output cache");
  oom_set_job(state->cache_path, NULL);
  struct Arena scratch = state->scratch;
  struct OutputCacheFact* facts =
      arena_new(&scratch, state->site_config.repo_count, struct OutputCacheFact);
  ptrdiff_t fact_count = 0;
  for (ptrdiff_t i = 0; i < state->site_config.repo_count; i++) {
    const struct OutputCacheFact fact = repo_scan_export_fact(
        &state->scans[i], &state->manifest, state->site_config.output_dir, &scratch);
    if (fact.key.len > 0) {
      facts[fact_count++] = fact;
    }
  }
  return output_cache_write(&state->writer, state->cache_path, facts, fact_count, scratch, log);
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
