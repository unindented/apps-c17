#ifndef SOSIG_OUTPUT_MANIFEST_H
#define SOSIG_OUTPUT_MANIFEST_H

#include <stddef.h>

#include "core/arena.h"
#include "core/str.h"

struct ContentEntry;
struct Log;
struct Manifest;
struct Os;
struct OsStat;
struct PathList;
struct SiteConfig;

/**
 * Path of the output cache below `output_dir`, in the output cache format of
 * `output/output_cache.h`. The name holds no version: the file's first line names its format, so a
 * file of another version is unusable rather than misread.
 */
#define OUTPUT_MANIFEST_CACHE_PATH "_sosig/output-cache.tsv"

/**
 * @brief Rejects an `output_dir` at or below `content_dir`, `templates_dir`, or `static_dir`,
 *        whether or not it exists yet, and one that holds any of them or the configuration file.
 *
 * A build runs this before it walks an input tree or creates anything. An `output_dir` inside a
 * root puts every output inside it, where the next build would read them back as inputs, so this
 * one answer covers every output at once. Each root is compared by `(device, inode)` against the
 * nearest existing ancestor of `output_dir` and each of that directory's physical ancestors, so an
 * alternate spelling of a root, a symlinked root, and a symlink on the way to `output_dir` are all
 * caught, and a sibling such as `contentx` is not. The nearest existing ancestor is `output_dir`
 * itself once it exists, so the answer does not depend on whether an earlier build created it. A
 * root that does not exist holds nothing to protect and is skipped.
 *
 * An `output_dir` that holds a root or the configuration file, such as `output_dir = "."`, mixes
 * the project's own files into the tree the build owns, where deleting `output_dir` to rebuild it
 * deletes them too. Each root, and the directory the configuration file is in, is compared with
 * `output_dir` the same way, along its own physical ancestors. An `output_dir` that does not exist
 * yet holds nothing and passes. One `output_dir` can hold every root and the configuration file at
 * once, which is one mistake, so only the first held input is reported.
 *
 * @param site_config Configuration supplying `content_dir`, `output_dir`, `templates_dir`, and
 *                    `static_dir`. Must not be `NULL`.
 * @param config_path Path the configuration was loaded from.
 * @param os          Host the directories are on. Must not be `NULL`.
 * @param scratch     Arena for the ancestor paths the check builds.
 * @param log         Log that receives the error. Must not be `NULL`.
 * @return `0` when `output_dir` and the inputs are apart, or `-1` when one holds the other.
 */
int output_manifest_check_output_dir(const struct SiteConfig* site_config,
                                     struct Str config_path,
                                     struct Os* os,
                                     struct Arena scratch,
                                     struct Log* log) __attribute__((nonnull(1, 3, 5)));

/**
 * @brief Records every intended output path in the build manifest, rejecting duplicates and any
 *        output that would land inside a build input tree or overwrite a build input file.
 *
 * Registers content entry, template, and static file output paths before writing any file, and the
 * output cache's path last, so it catches a collision between two producers up front. A static file
 * at the output cache's path is a duplicate rather than a silent overwrite, and being recorded is
 * what keeps the output cache through the stale-output prune. Each static file and asset is a copy
 * whose attributes are its source's size and modification time, so the output writer keeps a copy
 * whose source is unchanged. It rejects four kinds of collision:
 * - an output path at or below `content_dir`, `templates_dir`, or `static_dir`
 * - an output path that names a build input
 * - two producers that claim one output path, compared after folding ASCII case
 * - an output path that is a `/`-delimited directory prefix of another output path
 *
 * The first case catches an output that would be read back as an input without overwriting one: a
 * new content source file below `content_dir` is parsed as a source by the next build and one below
 * `static_dir` is copied as a static file by the next build, and a new file below `templates_dir`
 * can be read as a partial by the render of this same build. The caller has already run
 * `output_manifest_check_output_dir`, so this compares each root by `(device, inode)` against each
 * existing directory between `output_dir` and the output. That catches a symlink inside
 * `output_dir` that leads into a root. A root that does not exist holds nothing to protect and is
 * skipped.
 *
 * The last case requires one path to be both a file and a directory.
 * `manifest_find_prefix_collision` checks for it after all paths are recorded. Both path checks
 * fold ASCII `A-Z` to `a-z`, so `About/index.html` and `about/index.html` are one output, as they
 * are on a case-insensitive filesystem such as the macOS default. Every other byte compares
 * exactly.
 *
 * The input-file check compares filesystem identity, not path text, because the two spellings need
 * not match: `output_dir = "."` with a template named `sosig.toml` produces `./sosig.toml` against
 * the config path `sosig.toml`. Comparing `(device, inode)` also rejects a collision created by a
 * symlink, a hard link, or a case-insensitive filesystem. It claims the config, every discovered
 * source and static file, and every file below `templates_dir`, so it still protects an input that
 * a link places outside all three roots. Claiming the whole template tree covers every partial, and
 * every file below `partials/` that is not one. The caller lists the template tree as the build
 * does, leaving out `output_dir`, so a generated file that a symlink inside `templates_dir` reaches
 * is not mistaken for a template.
 *
 * @param manifest            Manifest that receives the output paths. Must not be `NULL`.
 * @param site_config         Configuration supplying `content_dir`, `output_dir`, `templates_dir`,
 *                            `static_dir`, and the template lists. Must not be `NULL`.
 * @param config_path         Path the configuration itself was loaded from. Claimed as an input
 *                            like any other, so a build cannot overwrite the file that configured
 *                            it.
 * @param source_paths        Every discovered content source path, drafts included, whose files
 *                            must not be overwritten. Must not be `NULL`.
 * @param static_paths        Every enumerated file below `static_dir`, each claimed as an input and
 *                            registered as a copy at its path relative to `static_dir`, below
 *                            `output_dir`. Must not be `NULL`.
 * @param static_stats        What `walk_stat_paths` reported for each of `static_paths`, parallel
 *                            to them, which give each copy its attributes. Must not be `NULL`
 *                            unless `static_paths` is empty.
 * @param asset_paths         Every file below `content_dir` that is not a content source, each
 *                            claimed and registered as a copy whose root is `content_dir`. Must not
 *                            be `NULL`.
 * @param asset_stats         What `walk_stat_paths` reported for each of `asset_paths`, as for
 *                            `static_stats`. Must not be `NULL` unless `asset_paths` is empty.
 * @param template_paths      Every file below `templates_dir`, partials included, each claimed as
 *                            an input. Must not be `NULL`.
 * @param content_entries     Rendered non-draft content entries whose output paths, each joined
 *                            onto `output_dir`, are recorded. May be `NULL` only when
 *                            `content_entry_count` is 0.
 * @param content_entry_count Number of entries in `content_entries`.
 * @param os                  Host the inputs and outputs are on. Must not be `NULL`.
 * @param perm                Arena that owns the manifest's entries, for as long as the manifest is
 *                            used. Must not be `NULL`.
 * @param scratch             Arena for the per-output checks.
 * @param log                 Log that receives every error. Every output is checked even after one
 *                            fails, so each collision is reported in one run. Must not be `NULL`.
 * @return `0` when every output path is unique, stays out of every input tree, overwrites no input,
 *         and nests under no other, or `-1` on an output inside an input tree, a duplicate, a
 *         prefix collision, an input overwrite, or an oversize path.
 */
int output_manifest_populate(struct Manifest* manifest,
                             const struct SiteConfig* site_config,
                             struct Str config_path,
                             const struct PathList* source_paths,
                             const struct PathList* static_paths,
                             const struct OsStat* static_stats,
                             const struct PathList* asset_paths,
                             const struct OsStat* asset_stats,
                             const struct PathList* template_paths,
                             const struct ContentEntry* const* content_entries,
                             ptrdiff_t content_entry_count,
                             struct Os* os,
                             struct Arena* perm,
                             struct Arena scratch,
                             struct Log* log)
    __attribute__((nonnull(1, 2, 4, 5, 7, 9, 12, 13, 15)));

#endif
