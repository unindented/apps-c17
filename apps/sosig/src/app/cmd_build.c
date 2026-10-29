#include "app/cmd_build.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "build/entry_renderer.h"
#include "build/output_manifest.h"
#include "build/page_renderer.h"
#include "build/site_writer.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/path.h"
#include "core/path_list.h"
#include "core/str.h"
#include "core/text.h"
#include "domain/content_entry.h"
#include "domain/site_config.h"
#include "markup/highlight.h"
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

// Each thread has a perm and a scratch arena of the sizes below, and the build has one log. A size
// costs address space only, because pages are committed as an arena fills, and a build that
// outgrows an arena ends in `os_oom`.
_Static_assert(PTRDIFF_MAX > 0xffffffff, "the arenas below need a 64-bit address space");

/**
 * Bytes of the build's log, and of each worker's share of the job logs, which hold the report: the
 * configuration and output-path errors and every failed job's messages.
 */
#define BUILD_LOG_LEN (256 * BUILD_MIB)

/**
 * Bytes of each thread's perm, which holds the main thread's paths, entry arrays, manifest and
 * output cache, or a worker's parsed entries with their sources and the bodies it converted, beside
 * one render's buffer of up to twice `TEMPLATE_RENDER_OUTPUT_LEN_MAX`.
 */
#define BUILD_PERM_LEN (16 * (ptrdiff_t)TEMPLATE_RENDER_OUTPUT_LEN_MAX)

/**
 * Bytes of each thread's scratch, which holds one job's temporaries: what a converter holds, and
 * the region its code blocks are highlighted in, twice `HIGHLIGHT_MEMORY_LEN_MAX`. The source
 * itself is in the perm. A converter's buffers grow by doubling in an arena they share, so each
 * takes at most four times what it holds. Markdown holds a code block's text: four bytes for each
 * byte of the source. Org holds a source block's text and an inline source's, four bytes for each
 * byte of them, and a source block's code references, a 16-byte record for the eight bytes a
 * `(ref:name)` and its line end take, of which the held text keeps the line end: nine bytes for
 * each. Org's is the larger, with the region 2.5 GiB together. A block made almost wholly of code
 * references whose `-l` label format is shorter takes up to 32 bytes for each byte, and past about
 * 88 MiB of such lines it ends in `os_oom`: that is the one input the limits accept that this does
 * not hold. Or the scratch holds a parse's copy of the frontmatter, or a render in which a partial
 * that includes itself reaches the expansion limit long before the arena is full. Or, once a render
 * has returned, it holds the existing output that `output_writer_produce` reads back to compare
 * with the rendered one, of up to `TEMPLATE_RENDER_OUTPUT_LEN_MAX`, and 64 KiB more for the read's
 * message.
 */
#define BUILD_SCRATCH_LEN (3 * 1024 * BUILD_MIB)

_Static_assert(9 * (ptrdiff_t)CONTENT_FILE_LEN_MAX + 2 * (ptrdiff_t)HIGHLIGHT_MEMORY_LEN_MAX <=
                   BUILD_SCRATCH_LEN,
               "a worker's scratch must hold what its converter holds, and the region its code "
               "blocks are highlighted in");

_Static_assert((ptrdiff_t)TEMPLATE_RENDER_OUTPUT_LEN_MAX + 64 * 1024 <= BUILD_SCRATCH_LEN,
               "a thread's scratch must hold the existing output a produce compares a render with");

/** Mutable state owned by one `cmd_build_execute` invocation. */
struct BuildState {
  /** Host every phase reads from, writes to and prints on. */
  struct Os* os;

  /**
   * The main thread's perm, the caller's. It receives the configuration, the source and static file
   * paths, the entry arrays and the manifest.
   */
  struct Arena perm;

  /**
   * The main thread's scratch, the caller's. Each phase receives a copy, so nothing a phase
   * allocates here outlives it, and a parallel phase's copy holds its job logs.
   */
  struct Arena scratch;

  /**
   * One perm and one scratch per worker, `worker_count` of them. A worker's perm holds the entries
   * it parsed, so it must not be reset before the last render that reads them.
   */
  struct JobArenas* worker_arenas;

  /** Config file the build reads, which it also claims as a build input. */
  struct Str config_path;

  /** Loaded site configuration. Its values live in `perm`. */
  struct SiteConfig site_config;

  /** Compiled templates, released when the build ends, or `NULL` until they load. */
  struct TemplateSet* template_set;

  /**
   * Discovered content entry source paths below `content_dir`, the files that end in `.md` or
   * `.org` in any letter case, sorted by the walk that produced them.
   */
  struct PathList source_paths;

  /**
   * Every other file below `content_dir`, which the build copies as a static file, sorted by the
   * walk that produced them.
   */
  struct PathList asset_paths;

  /** What `os_stat` reported for each of `asset_paths`, parallel to it, in `perm`. */
  struct OsStat* asset_stats;

  /** Discovered static file paths below `static_dir`, left empty when that directory is absent. */
  struct PathList static_paths;

  /** What `os_stat` reported for each of `static_paths`, parallel to it, in `perm`. */
  struct OsStat* static_stats;

  /** Intended output paths for this build, populated before any file is written. */
  struct Manifest manifest;

  /** Writer every change under `output_dir` goes through, which marks `manifest`. */
  struct OutputWriter writer;

  /**
   * One slot per source path, holding the entry parsed from it, so its length is
   * `source_paths.count`. A slot stays `NULL` for a draft and for a source that failed to parse.
   * Each entry lives in the perm of the worker that parsed it.
   */
  struct ContentEntry** source_entries;

  /** One flag per source path, set for a draft, in `perm`. */
  bool* draft_flags;

  /**
   * Rendered non-draft content entries used by templates, sorted newest-first. The pointers are
   * borrowed from `source_entries`.
   */
  struct ContentEntry** content_entries;

  /** Number of content entries in `content_entries`. */
  ptrdiff_t content_entry_count;

  /** Path of the output cache below `output_dir`, in `perm`. */
  struct Str cache_path;

  /**
   * What the output cache of an earlier build recorded, in `perm`, empty when there is none or the
   * build is forced.
   */
  struct OutputCache cache;

  /** Every file below `templates_dir`, sorted, which the manifest claims as inputs. */
  struct PathList template_paths;

  /** Most worker threads a parallel phase runs, each with its arenas in `worker_arenas`. */
  ptrdiff_t worker_count;

  /** Where the build's status lines go, which `--verbose` enables. */
  struct Verbose verbose;
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
 * @brief Loads the config, rejects an `output_dir` inside an input tree, and discovers content
 *        entry sources, the assets next to them, and static files.
 *
 * The `output_dir` check runs before either walk, and both walks leave `output_dir` out, so a file
 * an earlier build generated is never read back as a source or copied as an asset or a static file.
 *
 * A config that was read and parsed but holds a bad key has logged it and kept that key's default,
 * so the checks after it still run against it. A rejected directory, permalink or template list key
 * is the exception: its default is a directory or an output the user never chose, so nothing checks
 * or walks it, and the load stops once it has checked the other read roots.
 *
 * @param state Build state that receives the configuration, content entry source paths, asset
 *              paths, and static file paths. Must not be `NULL`.
 * @param log   Log that receives every error. Must not be `NULL`.
 * @return `0` when there are sources to check, or `-1` on a configuration that cannot be read, a
 *         rejected directory, permalink or template list key, a directory, output directory, or
 *         listing failure.
 */
static int load_build_inputs(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Splits every file below `content_dir` into the content sources and the assets.
 *
 * A content source ends in `.md` or `.org`, with ASCII letters in any case, so `post.MD` is
 * Markdown, as Emacs opens `x.ORG` in Org mode. Every other file is an asset. Both lists keep the
 * sorted order of `content_paths` and point at its path strings, so no path is copied.
 *
 * @param state         Build state that receives `source_paths` and `asset_paths`, in its perm.
 *                      Must not be `NULL`.
 * @param content_paths Every file below `content_dir`, sorted. Must not be `NULL`.
 */
static void load_build_inputs_split_sources(struct BuildState* state,
                                            const struct PathList* content_paths)
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
 * @brief Lists the static files below `static_dir`, which a site may leave out.
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
 * user knows what to edit. An optional root that is not there is usable, and a root whose key the
 * config load rejected is not checked, because the load has reported it. Every other root that
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
 * @brief Runs the phases from parsing to the prune, and in a dry run the report.
 *
 * Parses every source, checks every output path, and converts every body even after an error, then
 * stops before the first write when `log` holds any error, an earlier phase's included. Once the
 * output directory is prepared, it writes the output cache whether or not the outputs succeed.
 *
 * @param state Build state whose inputs are loaded. Must not be `NULL`.
 * @param log   Log that receives every error. Must not be `NULL`.
 * @return `0` on a complete build, or `-1` on any error.
 */
static int run_build_phases(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Parses every discovered content entry source, dispatching jobs across worker threads.
 *
 * Allocates `source_entries` and `draft_flags`, one slot per source path, before the parse phase
 * fills them. A site with no sources skips the phase, so it prints no status line and starts no
 * pool. Both then stay `NULL`.
 *
 * @param state Build state holding the sources, whose `source_entries` receives the parsed entries.
 *              Must not be `NULL`.
 * @param log   Log that receives the collected parse errors. Must not be `NULL`.
 * @return `0` when every job succeeded or there are no sources, or `-1` when any parse job failed.
 */
static int parse_content_entries(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Compacts rendered non-draft content entries and orders them.
 *
 * @param state Build state whose `content_entries` array is filled and sorted. Must not be `NULL`.
 */
static void collect_content_entries(struct BuildState* state) __attribute__((nonnull(1)));

/**
 * @brief Compacts every rendered non-draft entry into one contiguous array.
 *
 * Sizes the array in a first pass and fills it in a second, so the two never disagree. Writes both
 * `content_entries` and `content_entry_count` on the state, which the sort in the caller then
 * reads.
 *
 * @param state Build state whose `content_entries` array is allocated and filled. Must not be
 *              `NULL`.
 */
static void collect_content_entries_compact(struct BuildState* state) __attribute__((nonnull(1)));

/**
 * @brief Reads and compiles every template the build renders and every partial, and reports every
 *        one that cannot be read or does not compile.
 *
 * The templates are the aggregate and feed templates, each entry's `template`, and the content
 * template when an entry uses it. A template that only entries name names the first of them, in
 * source order, when it cannot be read.
 *
 * @param state Build state holding the parsed entries, which receives the set. Must not be `NULL`.
 * @param log   Log that receives every error. Must not be `NULL`.
 * @return `0` on success, or `-1` when a template fails.
 */
static int load_templates(struct BuildState* state, struct Log* log) __attribute__((nonnull(1, 2)));

/**
 * @brief Adds `name` to the template names of a load, with the source that names it, unless it is
 *        there already.
 *
 * @param names      Names so far, with room for one more. Must not be `NULL`.
 * @param owners     Sources parallel to `names`. Must not be `NULL`.
 * @param name_count Number of entries in `names`, which this increments when it adds one. Must not
 *                   be `NULL`.
 * @param name       Template name to add.
 * @param owner      Source that names it, or empty for the configuration.
 */
static void load_templates_add(struct Str* names,
                               struct Str* owners,
                               ptrdiff_t* name_count,
                               struct Str name,
                               struct Str owner) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Stats every static file and asset, and records every intended output path, rejecting a
 *        collision before anything is written.
 *
 * @param state Build state whose manifest is populated from the collected entries, and which
 *              receives the stats. Must not be `NULL`.
 * @param log   Log that receives every collision. Must not be `NULL`.
 * @return `0` when every output path is unique and names no build input, or `-1` on a collision or
 *         an input overwrite.
 */
static int populate_output_manifest(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Reads the output cache an earlier build left, unless the build is forced.
 *
 * A missing output cache, one that cannot be read, and one that does not parse are each an empty
 * output cache, never an error: the verbose log says which, and every static file and asset is
 * copied.
 *
 * @param state Build state holding the configuration, which receives the output cache and its path,
 *              and gives the writer `output_dir`. Must not be `NULL`.
 */
static void load_output_cache(struct BuildState* state) __attribute__((nonnull(1)));

/**
 * @brief Converts the body of every parsed entry, dispatching jobs across worker threads, with each
 *        link to another content file pointing at its page or copy.
 *
 * Runs after the entries have their URLs, and before the first write, so a link that names no file
 * or a draft stops the build with the other errors. A site with no sources skips the phase, so it
 * prints no status line and starts no pool.
 *
 * @param state Build state holding the parsed entries and the asset paths. Must not be `NULL`.
 * @param log   Log that receives the collected link errors. Must not be `NULL`.
 * @return `0` when every job succeeded or there are no sources, or `-1` when any link failed.
 */
static int convert_content_bodies(struct BuildState* state, struct Log* log)
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
 * @brief Renders every content entry's page, dispatching jobs across worker threads, and writes
 *        each one that changed.
 *
 * Runs after the entries are collected and sorted, so a content template sees the whole entry set
 * and the site's last-modified timestamp. Runs after the manifest, so no two jobs write one file. A
 * site with no sources skips the phase, so it prints no status line and starts no pool.
 *
 * @param state Build state holding the parsed and sorted entries. Must not be `NULL`.
 * @param log   Log that receives the collected render errors. Must not be `NULL`.
 * @return `0` when every job succeeded or there are no sources, or `-1` when any render job failed.
 */
static int render_content_pages(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Renders the configured aggregate and feed templates, writing each one that changed, and
 *        copies the static files and the assets whose sources changed.
 *
 * @param state Build state holding the collected entries, the static file paths, and the asset
 *              paths. Must not be `NULL`.
 * @param log   Log that receives the failure. Must not be `NULL`.
 * @return `0` when every output was written, or `-1` on the first render or write failure.
 */
static int write_generated_site(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Copies each of `paths` below `output_dir`, at its path below `root_dir`, through
 *        `output_writer_produce`, which keeps a copy whose source the output cache records
 *        unchanged.
 *
 * Each copy names its source to `oom_set_job`, which names no file once every copy succeeds.
 *
 * @param state    Build state holding the output cache and the writer. Must not be `NULL`.
 * @param paths    Files to copy, each below `root_dir`. Must not be `NULL`.
 * @param root_dir Directory the files are below.
 * @param log      Log that receives the first failure. Must not be `NULL`.
 * @return `0` on success, or `-1` on the first copy that fails.
 */
static int write_generated_site_copy(const struct BuildState* state,
                                     const struct PathList* paths,
                                     struct Str root_dir,
                                     struct Log* log) __attribute__((nonnull(1, 2, 4)));

/**
 * @brief Writes the output cache, with a row for every copy this build wrote or kept, as
 *        `output_cache_write` does.
 *
 * @param state Build state holding the writer and the output cache's path. Must not be `NULL`.
 * @param log   Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the write fails.
 */
static int write_output_cache(const struct BuildState* state, struct Log* log)
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
  // something to check, so one run reports a bad config key, every source that does not parse, and
  // every output collision together. Only a failure that leaves nothing to check stops early: a
  // config that cannot be read and a root that cannot be walked.
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
  // zeroed path list and manifest are already their empty state. `site_config_init` does more: it
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
  // A rejected directory, permalink or template list key holds a default the user never chose, so
  // the build stops before the checks and the walks that would report that default as a second
  // error, such as a collision with an output the user's own config does not name.
  const struct SiteConfig* site_config = &state->site_config;
  const int roots_rc = load_build_inputs_require_read_roots(state, log);
  if (site_config->is_content_dir_rejected || site_config->is_output_dir_rejected ||
      site_config->is_templates_dir_rejected || site_config->is_static_dir_rejected ||
      site_config->is_output_key_rejected) {
    return -1;
  }
  // A directory that failed its check still lets the overlap check run, so one run reports both.
  if (output_manifest_check_output_dir(&state->site_config, state->config_path, state->os,
                                       state->scratch, log) != 0 ||
      roots_rc != 0) {
    return -1;
  }

  verbose_printf(&state->verbose, "discovering content");
  // One walk lists every file, so each directory is listed once, and the split sorts each file into
  // a source or an asset. A failure names the directory or entry that failed, which is more precise
  // than the configured root, so the root is not repeated.
  static const struct Str content_suffixes[] = {STR_INIT("")};
  struct PathList content_paths = {0};
  if (walk_files_with_suffixes(&content_paths, state->site_config.content_dir,
                               state->site_config.output_dir, content_suffixes, 1, false, state->os,
                               &state->perm, state->scratch, S("failed to list content files"),
                               log) != 0) {
    return -1;
  }
  load_build_inputs_split_sources(state, &content_paths);
  verbose_printf(&state->verbose, "discovering templates and static files");
  if (load_build_inputs_templates(state, log) != 0 || load_build_inputs_static(state, log) != 0) {
    return -1;
  }
  return 0;
}

static void load_build_inputs_split_sources(struct BuildState* state,
                                            const struct PathList* content_paths) {
  // Each list gets a slot for every file, which spends a few bytes per file to fill both in one
  // pass.
  const ptrdiff_t count = content_paths->count;
  state->source_paths =
      (struct PathList){.items = arena_new(&state->perm, count, struct Str), .capacity = count};
  state->asset_paths =
      (struct PathList){.items = arena_new(&state->perm, count, struct Str), .capacity = count};
  for (ptrdiff_t i = 0; i < count; i++) {
    const struct Str path = content_paths->items[i];
    struct PathList* list =
        text_has_suffix_folded(path, S(".md")) || text_has_suffix_folded(path, S(".org"))
            ? &state->source_paths
            : &state->asset_paths;
    list->items[list->count++] = path;
  }
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
  // An absent `static_dir` leaves `static_paths` empty rather than failing the build. A site that
  // ships no stylesheet is a supported configuration, and the walk stats its root before
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
      {.dir_path = site_config->content_dir,
       .key = S("content_dir"),
       .is_rejected = site_config->is_content_dir_rejected},
      {.dir_path = site_config->templates_dir,
       .key = S("templates_dir"),
       .is_rejected = site_config->is_templates_dir_rejected},
      {.dir_path = site_config->static_dir,
       .key = S("static_dir"),
       .is_optional = true,
       .is_rejected = site_config->is_static_dir_rejected},
  };
  return read_root_require(roots, (ptrdiff_t)(sizeof(roots) / sizeof(roots[0])), state->os, log);
}

static int run_build_phases(struct BuildState* state, struct Log* log) {
  // The order below follows the data: a body's link to another content file needs that file's URL,
  // and a content template can iterate `content_entries`. Parsing every source has to finish before
  // any body is converted, and the entries have to be collected and sorted before any page is
  // rendered. A source that failed to parse leaves its slot empty, and the manifest and the
  // conversion check the rest.
  (void)parse_content_entries(state, log);
  collect_content_entries(state);
  (void)load_templates(state, log);
  (void)populate_output_manifest(state, log);
  load_output_cache(state);
  (void)convert_content_bodies(state, log);
  if (log->errors > 0) {
    return -1;
  }

  // Nothing before this point writes, so a build stopped above leaves the filesystem as it found
  // it. From here on a failure stops the phase it happens in, and the output cache is written
  // either way: it records only the copies this build wrote or kept, so the next build keeps the
  // copies a failed build made.
  if (prepare_output_dir(state, log) != 0) {
    return -1;
  }
  int rc = render_content_pages(state, log);
  if (rc == 0) {
    rc = write_generated_site(state, log);
  }
  if (write_output_cache(state, log) != 0) {
    rc = -1;
  }
  if (rc != 0 || prune_stale_outputs(state, log) != 0) {
    return -1;
  }
  if (state->writer.is_dry_run && report_dry_run(state, log) != 0) {
    return -1;
  }

  verbose_printf(&state->verbose, "build complete");
  return 0;
}

static int parse_content_entries(struct BuildState* state, struct Log* log) {
  if (state->source_paths.count == 0) {
    return 0;
  }
  verbose_printf(&state->verbose, "parsing content, workers: %td", state->worker_count);
  state->source_entries = arena_new(&state->perm, state->source_paths.count, struct ContentEntry*);
  state->draft_flags = arena_new(&state->perm, state->source_paths.count, bool);
  return entry_renderer_parse_entries(
      &state->site_config, &state->source_paths, CONTENT_FILE_LEN_MAX, state->worker_count,
      state->worker_arenas, state->verbose.is_enabled, state->os, state->scratch,
      state->source_entries, state->draft_flags, log);
}

static void collect_content_entries(struct BuildState* state) {
  verbose_printf(&state->verbose, "collecting content entries");
  collect_content_entries_compact(state);

  // Every template render below reads the entries in this order, so the code settles it here rather
  // than per render.
  content_entry_sort(state->content_entries, state->content_entry_count);
}

static void collect_content_entries_compact(struct BuildState* state) {
  for (ptrdiff_t i = 0; i < state->source_paths.count; i++) {
    if (state->source_entries[i] != NULL) {
      state->content_entry_count++;
    }
  }

  state->content_entries =
      arena_new(&state->perm, state->content_entry_count, struct ContentEntry*);
  ptrdiff_t j = 0;
  for (ptrdiff_t i = 0; i < state->source_paths.count; i++) {
    if (state->source_entries[i] != NULL) {
      state->content_entries[j++] = state->source_entries[i];
    }
  }
}

static int load_templates(struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "compiling templates");
  const struct SiteConfig* site_config = &state->site_config;
  const ptrdiff_t name_count_max = 1 + site_config->aggregate_template_count +
                                   site_config->feed_template_count + state->content_entry_count;
  struct Arena scratch = state->scratch;
  struct Str* names = arena_new(&scratch, name_count_max, struct Str);
  struct Str* owners = arena_new(&scratch, name_count_max, struct Str);
  ptrdiff_t name_count = 0;
  bool is_content_template_used = false;
  for (ptrdiff_t i = 0; i < site_config->aggregate_template_count; i++) {
    load_templates_add(names, owners, &name_count, site_config->aggregate_templates[i],
                       (struct Str){0});
  }
  for (ptrdiff_t i = 0; i < site_config->feed_template_count; i++) {
    load_templates_add(names, owners, &name_count, site_config->feed_templates[i], (struct Str){0});
  }
  // The entries go in source order, so a template that only entries name names the first of them. A
  // site whose every entry names its own template needs no content template.
  for (ptrdiff_t i = 0; i < state->source_paths.count; i++) {
    const struct ContentEntry* entry = state->source_entries[i];
    if (entry == NULL) {
      continue;
    }
    if (entry->template.len == 0) {
      is_content_template_used = true;
    } else {
      load_templates_add(names, owners, &name_count, entry->template, entry->source_path);
    }
  }
  if (is_content_template_used) {
    load_templates_add(names, owners, &name_count, site_config->content_template, (struct Str){0});
  }
  return template_set_load(site_config->templates_dir, names, owners, name_count, state->os,
                           &state->perm, scratch, &state->template_set, log);
}

static void load_templates_add(struct Str* names,
                               struct Str* owners,
                               ptrdiff_t* name_count,
                               struct Str name,
                               struct Str owner) {
  for (ptrdiff_t i = 0; i < *name_count; i++) {
    if (str_eq(names[i], name)) {
      return;
    }
  }
  names[*name_count] = name;
  owners[*name_count] = owner;
  (*name_count)++;
}

static int populate_output_manifest(struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "building output manifest");
  // Each copy's attributes are its source's size and modification time. A source that cannot be
  // inspected gets none, so it is copied, and the copy reports why it fails.
  state->static_stats = walk_stat_paths(&state->static_paths, state->os, &state->perm);
  state->asset_stats = walk_stat_paths(&state->asset_paths, state->os, &state->perm);
  // This runs ahead of the page renders, so a duplicate output path fails before rendering is
  // wasted. It also runs ahead of every write, `output_dir` itself included, so an output aimed at
  // one of this build's own inputs is refused before it can destroy the file. C has no implicit
  // qualification conversion for pointer-to-pointer, so handing the entry array to a read-only
  // callee needs this cast spelled out. It recurs at all four such call sites in this file. It only
  // adds `const`, at both levels, and the deep `const` is a thread-safety requirement. A
  // `TemplateContext` with only `const` pointers keeps one render job from writing an entry another
  // job is reading.
  return output_manifest_populate(
      &state->manifest, &state->site_config, state->config_path, &state->source_paths,
      &state->static_paths, state->static_stats, &state->asset_paths, state->asset_stats,
      &state->template_paths, (const struct ContentEntry* const*)state->content_entries,
      state->content_entry_count, state->os, &state->perm, state->scratch, log);
}

static void load_output_cache(struct BuildState* state) {
  const struct Str output_dir = state->site_config.output_dir;
  state->writer.output_dir = output_dir;
  state->cache_path = path_join(output_dir, S(OUTPUT_MANIFEST_CACHE_PATH), &state->perm);
  if (state->writer.is_forced) {
    verbose_printf(&state->verbose, "ignoring output cache, making every output");
    return;
  }
  verbose_printf(&state->verbose, "loading output cache");
  struct Str reason = {0};
  const enum BuildCacheFileState cache_state = output_cache_load(
      state->cache_path, state->os, &state->perm, state->scratch, &state->cache, &reason);
  if (cache_state == BUILD_CACHE_FILE_MISSING) {
    verbose_printf(&state->verbose, "no output cache, copying every file");
  } else if (cache_state == BUILD_CACHE_FILE_UNUSABLE) {
    verbose_printf(&state->verbose, "output cache not used, copying every file: %.*s",
                   STR_FMT(reason));
  }
}

static int convert_content_bodies(struct BuildState* state, struct Log* log) {
  if (state->source_paths.count == 0) {
    return 0;
  }
  verbose_printf(&state->verbose, "converting bodies, workers: %td", state->worker_count);
  return entry_renderer_convert_bodies(
      state->source_entries, state->draft_flags, &state->site_config, &state->source_paths,
      &state->asset_paths, state->worker_count, state->worker_arenas, state->verbose.is_enabled,
      state->os, state->scratch, log);
}

static int prepare_output_dir(const struct BuildState* state, struct Log* log) {
  // A failure names the component that failed, which is more precise than the configured root, so
  // the root is not repeated.
  return output_writer_mkdirs(&state->writer, state->site_config.output_dir,
                              S("failed to prepare output directory"), log);
}

static int render_content_pages(struct BuildState* state, struct Log* log) {
  if (state->source_paths.count == 0) {
    return 0;
  }
  verbose_printf(&state->verbose, "rendering content, workers: %td", state->worker_count);
  return page_renderer_render_pages(
      &state->writer, &state->cache, &state->site_config, state->template_set,
      (const struct ContentEntry* const*)state->source_entries, state->source_paths.count,
      (const struct ContentEntry* const*)state->content_entries, state->content_entry_count,
      state->worker_count, state->worker_arenas, state->verbose.is_enabled, state->os,
      state->scratch, log);
}

static int write_generated_site(struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "rendering aggregate templates");
  const struct SiteConfig* site_config = &state->site_config;
  const struct ContentEntry* const* entries =
      (const struct ContentEntry* const*)state->content_entries;
  if (site_writer_write_templates(
          &state->writer, &state->cache, site_config, state->template_set,
          site_config->aggregate_templates, site_config->aggregate_template_count, entries,
          state->content_entry_count, false, state->perm, state->scratch, log) != 0) {
    return -1;
  }
  verbose_printf(&state->verbose, "rendering feed templates");
  const ptrdiff_t feed_entry_count = state->content_entry_count < site_config->feed_count
                                         ? state->content_entry_count
                                         : site_config->feed_count;
  if (site_writer_write_templates(&state->writer, &state->cache, site_config, state->template_set,
                                  site_config->feed_templates, site_config->feed_template_count,
                                  entries, feed_entry_count, true, state->perm, state->scratch,
                                  log) != 0) {
    return -1;
  }
  verbose_printf(&state->verbose, "copying static files");
  // An asset is a static file whose root is `content_dir`, so it is copied the same way.
  if (write_generated_site_copy(state, &state->static_paths, site_config->static_dir, log) != 0) {
    return -1;
  }
  return write_generated_site_copy(state, &state->asset_paths, site_config->content_dir, log);
}

static int write_generated_site_copy(const struct BuildState* state,
                                     const struct PathList* paths,
                                     struct Str root_dir,
                                     struct Log* log) {
  for (ptrdiff_t i = 0; i < paths->count; i++) {
    const struct Str source_path = paths->items[i];
    oom_set_job(source_path, NULL);
    // Each copy works in its own copy of the scratch, so one file's path is dead before the next.
    struct Arena scratch = state->scratch;
    struct Str relative = {0};
    (void)path_is_below(source_path, root_dir, &relative);
    const struct Str output_path = path_join(state->site_config.output_dir, relative, &scratch);
    // A copy is streamed from its source, so it makes nothing in the perm. The copy names the path
    // that failed, the source or its destination, which mirrors it.
    if (output_writer_produce(&state->writer, output_path, &state->cache, NULL, NULL, state->perm,
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
  const int rc =
      output_cache_write(&state->writer, state->cache_path, NULL, 0, state->scratch, log);
  oom_set_job((struct Str){0}, NULL);
  return rc;
}

static int prune_stale_outputs(struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "removing stale outputs");
  oom_set_job((struct Str){0}, NULL);
  ptrdiff_t removed_count = 0;
  const struct SiteConfig* site_config = &state->site_config;
  const struct Str root_dirs[] = {site_config->content_dir, site_config->templates_dir,
                                  site_config->static_dir};
  const int rc = output_pruner_prune(&state->writer, site_config->output_dir, root_dirs,
                                     (ptrdiff_t)(sizeof(root_dirs) / sizeof(root_dirs[0])),
                                     state->os, state->scratch, &removed_count, log);
  verbose_printf(&state->verbose, "removed stale outputs: %td", removed_count);
  return rc;
}

static int report_dry_run(struct BuildState* state, struct Log* log) {
  return output_writer_report(&state->writer, state->scratch, log);
}
