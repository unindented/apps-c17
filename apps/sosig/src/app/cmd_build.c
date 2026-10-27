#include "app/cmd_build.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "build/entry_renderer.h"
#include "build/manifest_builder.h"
#include "build/page_renderer.h"
#include "build/site_writer.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/path.h"
#include "core/path_list.h"
#include "core/str.h"
#include "core/text.h"
#include "domain/build_cache.h"
#include "domain/content_entry.h"
#include "domain/site_config.h"
#include "markup/highlight.h"
#include "os/oom.h"
#include "os/os.h"
#include "output/build_cache_file.h"
#include "output/manifest.h"
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
 * Bytes of each thread's perm, which holds the main thread's paths, entry arrays and manifest, or a
 * worker's parsed entries with their sources and the bodies it converted, beside one render's
 * buffer of up to twice `TEMPLATE_RENDER_OUTPUT_LEN_MAX`.
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
 * that includes itself reaches the expansion limit long before the arena is full.
 */
#define BUILD_SCRATCH_LEN (3 * 1024 * BUILD_MIB)

_Static_assert(9 * (ptrdiff_t)CONTENT_FILE_LEN_MAX + 2 * (ptrdiff_t)HIGHLIGHT_MEMORY_LEN_MAX <=
                   BUILD_SCRATCH_LEN,
               "a worker's scratch must hold what its converter holds, and the region its code "
               "blocks are highlighted in");

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

  /** Path of the build cache below `output_dir`, in `perm`. */
  struct Str build_cache_path;

  /** Rows an earlier build left, empty when there are none or the build ignores them. */
  struct BuildCache build_cache;

  /** Every file below `templates_dir`, sorted, which the manifest claims and the cache records. */
  struct PathList template_paths;

  /** What `os_stat` reported for each of `template_paths`, parallel to it, in `perm`. */
  struct OsStat* template_stats;

  /**
   * One slot per source path, holding the build cache row that vouches for its page, or `NULL`
   * where the page must be made, in `perm`.
   */
  const struct BuildCacheRow** fresh_rows;

  /**
   * One slot per source path, which the page phase fills with whether its page read the entry list,
   * in `perm`.
   */
  bool* entry_list_flags;

  /**
   * Whether the build cache vouches for every template and every entry, so an aggregate or feed
   * that exists can be kept.
   */
  bool is_aggregates_fresh;

  /** Most worker threads a parallel phase runs, each with its arenas in `worker_arenas`. */
  ptrdiff_t worker_count;

  /** Where the build's status lines go, which `--verbose` enables. */
  struct Verbose verbose;

  /** Whether the build ignores the build cache, and writes it afresh. */
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
 *        inputs and the build cache records a row for.
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
 * stops before the first write when `log` holds any error, an earlier phase's included.
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
 * @brief Records every intended output path, rejecting a collision before anything is written.
 *
 * @param state Build state whose manifest is populated from the collected entries. Must not be
 *              `NULL`.
 * @param log   Log that receives every collision. Must not be `NULL`.
 * @return `0` when every output path is unique and names no build input, or `-1` on a collision or
 *         an input overwrite.
 */
static int populate_output_manifest(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Reads the build cache an earlier build left, unless the build ignores it, and decides
 *        which outputs it vouches for.
 *
 * Stats every template that the load inputs phase listed either way, for the rows the build writes.
 * A missing build cache, one that cannot be read, and one that does not parse or was written for
 * another page recipe are each an empty build cache, never an error: the verbose log says which,
 * and every page is made.
 *
 * @param state Build state holding the parsed entries and the template paths, which receives the
 *              build cache, the template stats and the fresh rows. Must not be `NULL`.
 */
static void load_build_cache(struct BuildState* state) __attribute__((nonnull(1)));

/**
 * @brief Reads and parses the build cache, reporting why it is not used in the verbose log.
 *
 * @param state Build state that receives the build cache. Must not be `NULL`.
 */
static void load_build_cache_read(struct BuildState* state) __attribute__((nonnull(1)));

/**
 * @brief Parses the text of the build cache into the build state, as `BuildCacheFileParseFn`.
 *
 * @param text     Bytes of the build cache.
 * @param perm     Arena that owns the rows. Must not be `NULL`.
 * @param log      Log that receives the reason the build cache cannot be used. Must not be `NULL`.
 * @param userdata The `struct BuildState`, which receives the build cache.
 * @return `0` on success, or `-1` when the build cache cannot be used.
 */
static int load_build_cache_read_parse(struct Str text,
                                       struct Arena* perm,
                                       struct Log* log,
                                       void* userdata) __attribute__((nonnull(2, 3, 4)));

/**
 * @brief Decides which outputs the build cache vouches for.
 *
 * Every row needs the page recipe to match, which a parsed build cache does, and every template row
 * to match with no template added or removed, because the template dependencies are tracked as a
 * whole. A content page also needs its own row to match its source. One that read the entry list,
 * and every aggregate and feed, also need every entry's row to match with no entry added or
 * removed.
 *
 * @param state Build state holding the build cache, the entries and the template stats, which
 *              receives the fresh rows and `is_aggregates_fresh`. Must not be `NULL`.
 */
static void load_build_cache_find_fresh(struct BuildState* state) __attribute__((nonnull(1)));

/**
 * @brief Reports whether every content entry has a row that matches its source, and no row names
 *        anything else.
 *
 * @param state Build state holding the build cache and the entries. Must not be `NULL`.
 * @return `true` when the content rows match the entries one for one.
 */
static bool is_every_entry_unchanged(const struct BuildState* state) __attribute__((nonnull(1)));

/**
 * @brief Reports whether every template has a row that matches its stat, and no row names anything
 *        else.
 *
 * @param state Build state holding the build cache and the template stats. Must not be `NULL`.
 * @return `true` when the template rows match the templates one for one.
 */
static bool is_every_template_unchanged(const struct BuildState* state) __attribute__((nonnull(1)));

/**
 * @brief Converts the body of every parsed entry, dispatching jobs across worker threads, with each
 *        link to another content file pointing at its page or copy.
 *
 * Runs after the entries have their URLs, and before the first write, so a link that names no file
 * or a draft stops the build with the other errors. A page whose denote link names an asset loses
 * its fresh row, and the aggregates and feeds are made again, because no row tracks the asset. A
 * site with no sources skips the phase, so it prints no status line and starts no pool.
 *
 * @param state Build state holding the parsed entries, the asset paths and the fresh rows. Must not
 *              be `NULL`.
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
 * @brief Empties the build cache before the first page is written.
 *
 * A build that fails or is interrupted from here on leaves an empty build cache, which the next
 * build does not use, so it never keeps an output this build may have cut short or written under
 * other inputs.
 *
 * @param state Build state holding the build cache's path. Must not be `NULL`.
 * @param log   Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the write fails.
 */
static int empty_build_cache(const struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Renders and writes every content entry's page, dispatching jobs across worker threads.
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
 * @brief Renders and writes the configured aggregate and feed templates, and copies the static
 *        files and the assets.
 *
 * @param state Build state holding the collected entries, the static file paths, and the asset
 *              paths. Must not be `NULL`.
 * @param log   Log that receives the failure. Must not be `NULL`.
 * @return `0` when every output was written, or `-1` on the first render or write failure.
 */
static int write_generated_site(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Copies each of `paths` below `output_dir`, or keeps a copy whose row of `kind` matches its
 *        source, as `output_writer_copy_files` does.
 *
 * @param state     Build state holding the build cache and the writer. Must not be `NULL`.
 * @param kind      Kind of the rows that vouch for the sources.
 * @param paths     Files to copy, each below `root_dir`. Must not be `NULL`.
 * @param stats     Stat of each of `paths`, parallel to them. Must not be `NULL` unless there are
 *                  none.
 * @param root_dir  Directory the files are below.
 * @param operation Operation leading the message of a copy that fails.
 * @param log       Log that receives the first failure. Must not be `NULL`.
 * @return `0` on success, or `-1` on the first copy that fails.
 */
static int write_generated_site_copy(const struct BuildState* state,
                                     enum BuildCacheKind kind,
                                     const struct PathList* paths,
                                     const struct OsStat* stats,
                                     struct Str root_dir,
                                     struct Str operation,
                                     struct Log* log) __attribute__((nonnull(1, 3, 7)));

/**
 * @brief Reports whether a source has a row of the copy's kind with its stat, as
 *        `OutputWriterIsUnchangedFn`.
 *
 * @param name        Path of the source below the root of the copies.
 * @param source_stat What `os_stat` reported for the source. Must not be `NULL`.
 * @param userdata    The `struct CopyRows`.
 * @return `true` when the build cache records the source with that stat.
 */
static bool write_generated_site_copy_is_unchanged(struct Str name,
                                                   const struct OsStat* source_stat,
                                                   void* userdata) __attribute__((nonnull(2, 3)));

/**
 * @brief Writes the build cache once every output is written or kept, with a row for every current
 *        entry and template.
 *
 * @param state Build state holding the entries, their entry-list flags and the template stats. Must
 *              not be `NULL`.
 * @param log   Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the write fails.
 */
static int write_build_cache(struct BuildState* state, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Appends a row of `kind` for each of `paths`, named by its path below `root_dir`.
 *
 * @param cache    Cache that receives the rows, with room for them. Must not be `NULL`.
 * @param kind     Kind of the rows.
 * @param paths    Files the rows record. Must not be `NULL`.
 * @param stats    Stat of each of `paths`, parallel to them. Must not be `NULL` unless there are
 *                 none.
 * @param root_dir Directory the files are below.
 */
static void write_build_cache_files(struct BuildCache* cache,
                                    enum BuildCacheKind kind,
                                    const struct PathList* paths,
                                    const struct OsStat* stats,
                                    struct Str root_dir) __attribute__((nonnull(1, 3)));

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
  if (manifest_builder_check_output_dir(&state->site_config, state->config_path, state->os,
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
  load_build_cache(state);
  (void)convert_content_bodies(state, log);
  if (log->errors > 0) {
    return -1;
  }

  // Nothing before this point writes, so a build stopped above leaves the filesystem as it found
  // it. From here on a failure stops the phase it happens in.
  if (prepare_output_dir(state, log) != 0 || empty_build_cache(state, log) != 0) {
    return -1;
  }
  if (render_content_pages(state, log) != 0) {
    return -1;
  }
  if (write_generated_site(state, log) != 0 || write_build_cache(state, log) != 0 ||
      prune_stale_outputs(state, log) != 0) {
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
  // This runs ahead of the page renders, so a duplicate output path fails before rendering is
  // wasted. It also runs ahead of every write, `output_dir` itself included, so an output aimed at
  // one of this build's own inputs is refused before it can destroy the file. C has no implicit
  // qualification conversion for pointer-to-pointer, so handing the entry array to a read-only
  // callee needs this cast spelled out. It recurs at all four such call sites in this file. It only
  // adds `const`, at both levels, and the deep `const` is a thread-safety requirement. A
  // `TemplateContext` with only `const` pointers keeps one render job from writing an entry another
  // job is reading.
  return manifest_builder_populate(
      &state->manifest, &state->site_config, state->config_path, &state->source_paths,
      &state->static_paths, &state->asset_paths, &state->template_paths,
      (const struct ContentEntry* const*)state->content_entries, state->content_entry_count,
      state->os, &state->perm, state->scratch, log);
}

static void load_build_cache(struct BuildState* state) {
  state->build_cache_path = build_cache_path(state->site_config.output_dir, &state->perm);
  if (state->is_forced) {
    verbose_printf(&state->verbose, "ignoring build cache, rendering every page");
  } else {
    load_build_cache_read(state);
  }
  // A template that cannot be inspected keeps a zeroed stat, which no row matches, so every page is
  // made, and its render reports why.
  state->template_stats = walk_stat_paths(&state->template_paths, state->os, &state->perm);
  state->static_stats = walk_stat_paths(&state->static_paths, state->os, &state->perm);
  state->asset_stats = walk_stat_paths(&state->asset_paths, state->os, &state->perm);
  load_build_cache_find_fresh(state);
}

static void load_build_cache_read(struct BuildState* state) {
  verbose_printf(&state->verbose, "loading build cache");
  struct Str reason = {0};
  const enum BuildCacheFileState cache_state = build_cache_file_load(
      state->build_cache_path, BUILD_CACHE_FILE_LEN_MAX, load_build_cache_read_parse, state,
      state->os, &state->perm, state->scratch, &reason);
  if (cache_state == BUILD_CACHE_FILE_MISSING) {
    verbose_printf(&state->verbose, "no build cache, rendering every page");
  } else if (cache_state == BUILD_CACHE_FILE_UNUSABLE) {
    verbose_printf(&state->verbose, "build cache not used, rendering every page: %.*s",
                   STR_FMT(reason));
  }
}

static int load_build_cache_read_parse(struct Str text,
                                       struct Arena* perm,
                                       struct Log* log,
                                       void* userdata) {
  struct BuildState* state = userdata;
  return build_cache_parse(text, &state->site_config, perm, &state->build_cache, log);
}

static void load_build_cache_find_fresh(struct BuildState* state) {
  state->fresh_rows =
      arena_new(&state->perm, state->source_paths.count, const struct BuildCacheRow*);
  state->entry_list_flags = arena_new(&state->perm, state->source_paths.count, bool);
  if (!is_every_template_unchanged(state)) {
    return;
  }
  const bool is_every_entry_fresh = is_every_entry_unchanged(state);
  state->is_aggregates_fresh = is_every_entry_fresh;
  for (ptrdiff_t i = 0; i < state->source_paths.count; i++) {
    const struct ContentEntry* entry = state->source_entries[i];
    if (entry == NULL) {
      continue;
    }
    struct Str relative = {0};
    (void)path_is_below(entry->output_path, state->site_config.output_dir, &relative);
    const struct BuildCacheRow* row = build_cache_find(
        &state->build_cache, BUILD_CACHE_KIND_CONTENT, relative, &entry->source_stat);
    if (row != NULL && (!row->is_entry_list_read || is_every_entry_fresh)) {
      state->fresh_rows[i] = row;
    }
  }
}

static bool is_every_entry_unchanged(const struct BuildState* state) {
  ptrdiff_t row_count = 0;
  for (ptrdiff_t i = 0; i < state->build_cache.count; i++) {
    row_count += state->build_cache.rows[i].kind == BUILD_CACHE_KIND_CONTENT;
  }
  if (row_count != state->content_entry_count) {
    return false;
  }
  for (ptrdiff_t i = 0; i < state->content_entry_count; i++) {
    const struct ContentEntry* entry = state->content_entries[i];
    struct Str relative = {0};
    (void)path_is_below(entry->output_path, state->site_config.output_dir, &relative);
    if (build_cache_find(&state->build_cache, BUILD_CACHE_KIND_CONTENT, relative,
                         &entry->source_stat) == NULL) {
      return false;
    }
  }
  return true;
}

static bool is_every_template_unchanged(const struct BuildState* state) {
  ptrdiff_t row_count = 0;
  for (ptrdiff_t i = 0; i < state->build_cache.count; i++) {
    row_count += state->build_cache.rows[i].kind == BUILD_CACHE_KIND_TEMPLATE;
  }
  // An empty build cache has no template rows, and a site always has its content template, so this
  // also rejects a build cache that was not read.
  if (row_count != state->template_paths.count || row_count == 0) {
    return false;
  }
  for (ptrdiff_t i = 0; i < state->template_paths.count; i++) {
    struct Str relative = {0};
    (void)path_is_below(state->template_paths.items[i], state->site_config.templates_dir,
                        &relative);
    if (build_cache_find(&state->build_cache, BUILD_CACHE_KIND_TEMPLATE, relative,
                         &state->template_stats[i]) == NULL) {
      return false;
    }
  }
  return true;
}

static int convert_content_bodies(struct BuildState* state, struct Log* log) {
  if (state->source_paths.count == 0) {
    return 0;
  }
  verbose_printf(&state->verbose, "converting bodies, workers: %td", state->worker_count);
  const int rc = entry_renderer_convert_bodies(
      state->source_entries, state->draft_flags, &state->site_config, &state->source_paths,
      &state->asset_paths, state->worker_count, state->worker_arenas, state->verbose.is_enabled,
      state->os, state->scratch, log);
  // No row tracks an asset, so a body that links one can change with no row changing, and its page
  // is made again. An aggregate or a feed that shows the body is not: it follows the entry rows as
  // before, and remaking every aggregate in every build for a renamed asset costs more than it is
  // worth.
  for (ptrdiff_t i = 0; i < state->source_paths.count; i++) {
    const struct ContentEntry* entry = state->source_entries[i];
    if (entry != NULL && entry->has_asset_link) {
      state->fresh_rows[i] = NULL;
    }
  }
  return rc;
}

static int prepare_output_dir(const struct BuildState* state, struct Log* log) {
  // A failure names the component that failed, which is more precise than the configured root, so
  // the root is not repeated.
  return output_writer_mkdirs(&state->writer, state->site_config.output_dir,
                              S("failed to prepare output directory"), log);
}

static int empty_build_cache(const struct BuildState* state, struct Log* log) {
  oom_set_job(state->build_cache_path, NULL);
  const int rc = output_writer_write(&state->writer, state->build_cache_path, (struct OsSource){0},
                                     S("failed to empty build cache"), log);
  // The phases that follow name their own files, and the main thread's work between them none.
  oom_set_job((struct Str){0}, NULL);
  return rc;
}

static int render_content_pages(struct BuildState* state, struct Log* log) {
  if (state->source_paths.count == 0) {
    return 0;
  }
  verbose_printf(&state->verbose, "rendering content, workers: %td", state->worker_count);
  return page_renderer_render_pages(
      &state->writer, &state->site_config, state->template_set,
      (const struct ContentEntry* const*)state->source_entries, state->source_paths.count,
      (const struct ContentEntry* const*)state->content_entries, state->content_entry_count,
      (const struct BuildCacheRow* const*)state->fresh_rows, state->worker_count,
      state->worker_arenas, state->verbose.is_enabled, state->os, state->scratch,
      state->entry_list_flags, log);
}

static int write_generated_site(struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "rendering aggregate templates");
  const struct SiteConfig* site_config = &state->site_config;
  const struct ContentEntry* const* entries =
      (const struct ContentEntry* const*)state->content_entries;
  if (site_writer_write_templates(
          &state->writer, site_config, state->template_set, site_config->aggregate_templates,
          site_config->aggregate_template_count, entries, state->content_entry_count, false,
          state->is_aggregates_fresh, state->os, state->perm, state->scratch, log) != 0) {
    return -1;
  }
  verbose_printf(&state->verbose, "rendering feed templates");
  const ptrdiff_t feed_entry_count = state->content_entry_count < site_config->feed_count
                                         ? state->content_entry_count
                                         : site_config->feed_count;
  if (site_writer_write_templates(&state->writer, site_config, state->template_set,
                                  site_config->feed_templates, site_config->feed_template_count,
                                  entries, feed_entry_count, true, state->is_aggregates_fresh,
                                  state->os, state->perm, state->scratch, log) != 0) {
    return -1;
  }
  verbose_printf(&state->verbose, "copying static files");
  // An asset is a static file whose root is `content_dir`, so it is copied the same way.
  if (write_generated_site_copy(state, BUILD_CACHE_KIND_STATIC, &state->static_paths,
                                state->static_stats, site_config->static_dir,
                                S("failed to copy static file"), log) != 0) {
    return -1;
  }
  return write_generated_site_copy(state, BUILD_CACHE_KIND_ASSET, &state->asset_paths,
                                   state->asset_stats, site_config->content_dir,
                                   S("failed to copy static file"), log);
}

static int write_generated_site_copy(const struct BuildState* state,
                                     enum BuildCacheKind kind,
                                     const struct PathList* paths,
                                     const struct OsStat* stats,
                                     struct Str root_dir,
                                     struct Str operation,
                                     struct Log* log) {
  struct CopyRows rows = {.cache = &state->build_cache, .kind = kind};
  return output_writer_copy_files(&state->writer, paths, stats,
                                  write_generated_site_copy_is_unchanged, &rows, root_dir,
                                  state->site_config.output_dir, state->scratch, operation, log);
}

static bool write_generated_site_copy_is_unchanged(struct Str name,
                                                   const struct OsStat* source_stat,
                                                   void* userdata) {
  const struct CopyRows* rows = userdata;
  return build_cache_find(rows->cache, rows->kind, name, source_stat) != NULL;
}

static int write_build_cache(struct BuildState* state, struct Log* log) {
  verbose_printf(&state->verbose, "writing build cache");
  oom_set_job(state->build_cache_path, NULL);
  struct Arena scratch = state->scratch;
  struct BuildCache cache = {
      .rows = arena_new(&scratch,
                        state->content_entry_count + state->template_paths.count +
                            state->static_paths.count + state->asset_paths.count,
                        struct BuildCacheRow),
  };
  for (ptrdiff_t i = 0; i < state->source_paths.count; i++) {
    const struct ContentEntry* entry = state->source_entries[i];
    if (entry == NULL) {
      continue;
    }
    struct Str relative = {0};
    (void)path_is_below(entry->output_path, state->site_config.output_dir, &relative);
    cache.rows[cache.count++] = (struct BuildCacheRow){
        .kind = BUILD_CACHE_KIND_CONTENT,
        .name = relative,
        .len = entry->source_stat.len,
        .mtime_s = entry->source_stat.mtime_s,
        .mtime_ns = entry->source_stat.mtime_ns,
        // A page that links another entry depends on that entry's URL, as a page that read the
        // entry list depends on every entry.
        .is_entry_list_read = state->entry_list_flags[i] || entry->has_entry_link,
    };
  }
  for (ptrdiff_t i = 0; i < state->template_paths.count; i++) {
    struct Str relative = {0};
    (void)path_is_below(state->template_paths.items[i], state->site_config.templates_dir,
                        &relative);
    cache.rows[cache.count++] = (struct BuildCacheRow){
        .kind = BUILD_CACHE_KIND_TEMPLATE,
        .name = relative,
        .len = state->template_stats[i].len,
        .mtime_s = state->template_stats[i].mtime_s,
        .mtime_ns = state->template_stats[i].mtime_ns,
    };
  }
  write_build_cache_files(&cache, BUILD_CACHE_KIND_STATIC, &state->static_paths,
                          state->static_stats, state->site_config.static_dir);
  write_build_cache_files(&cache, BUILD_CACHE_KIND_ASSET, &state->asset_paths, state->asset_stats,
                          state->site_config.content_dir);
  struct Buf text = {.arena = &scratch};
  build_cache_format(&text, &state->site_config, &cache);
  return output_writer_write(&state->writer, state->build_cache_path,
                             (struct OsSource){.bytes = buf_str(&text)},
                             S("failed to write build cache"), log);
}

static void write_build_cache_files(struct BuildCache* cache,
                                    enum BuildCacheKind kind,
                                    const struct PathList* paths,
                                    const struct OsStat* stats,
                                    struct Str root_dir) {
  for (ptrdiff_t i = 0; i < paths->count; i++) {
    struct Str relative = {0};
    (void)path_is_below(paths->items[i], root_dir, &relative);
    cache->rows[cache->count++] = (struct BuildCacheRow){
        .kind = kind,
        .name = relative,
        .len = stats[i].len,
        .mtime_s = stats[i].mtime_s,
        .mtime_ns = stats[i].mtime_ns,
    };
  }
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
