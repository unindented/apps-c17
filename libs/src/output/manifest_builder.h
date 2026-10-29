#ifndef APPS_OUTPUT_MANIFEST_BUILDER_H
#define APPS_OUTPUT_MANIFEST_BUILDER_H

#include <stdbool.h>
#include <stddef.h>

#include "core/arena.h"
#include "core/str.h"
#include "os/os.h"

struct Log;
struct Manifest;
struct PathList;

/** One input directory tree that no output may enter, and that `output_dir` may not hold. */
struct ManifestRoot {
  /**
   * What a diagnostic names the root by: a quoted config key, such as `'templates_dir'`, or a
   * phrase, such as `the git directory of repo 'x'`.
   */
  struct Str label;

  /** Path of the directory. */
  struct Str dir_path;

  /** Identity of the directory, when `is_present`. Filled by `manifest_builder_init`. */
  struct OsIdentity identity;

  /** Whether the directory exists. A missing root holds nothing to protect. */
  bool is_present;
};

/** One output that `manifest_builder_add` reported inside an input root. */
struct ManifestRootReport {
  /** Existing directory below `output_dir` through which the output reached the root. */
  struct Str dir_path;

  /** What produced the output. */
  struct Str source_label;
};

/**
 * What every output path is checked against before the manifest records it: the input roots, and
 * the identities of the input files outside them, which a link could still place an output on.
 */
struct ManifestBuilder {
  /** Manifest that receives the paths. */
  struct Manifest* manifest;

  /** Output directory every path is joined onto. */
  struct Str output_dir;

  /** Input roots, with their identities. */
  struct ManifestRoot* roots;

  /** Number of entries in `roots`. */
  ptrdiff_t root_count;

  /** Identities of the input files, sorted for `bsearch`. */
  struct OsIdentity* input_items;

  /** Number of entries in `input_items`. */
  ptrdiff_t input_count;

  /** Host the roots and the outputs are on. */
  struct Os* os;

  /** Arena that owns the manifest's entries. */
  struct Arena* perm;

  /**
   * Outputs reported inside a root so far, so a later output of the same cause fails without a line
   * of its own. Grows in `perm`.
   */
  struct ManifestRootReport* reports;

  /** Number of entries in `reports`. */
  ptrdiff_t report_count;

  /** Number of entries `reports` has room for. */
  ptrdiff_t report_cap;
};

/**
 * @brief Rejects an `output_dir` at or below an input root, whether or not it exists yet, and one
 *        that holds an input root or the configuration file.
 *
 * Each root is compared by `(device, inode)` with the nearest existing ancestor of `output_dir` and
 * each of its physical ancestors, so an alternate spelling or a symlink is caught and a sibling is
 * not, and the other way round for each root and the config's directory. This rejects the layout
 * `/srv/git/foo.git` with `output_dir = "/srv/git"`. A missing root is skipped.
 *
 * @param output_dir  Output directory.
 * @param roots       Input roots, `root_count` of them, with `label` and `dir_path` set. Must not
 *                    be `NULL` unless `root_count` is `0`.
 * @param root_count  Number of entries in `roots`.
 * @param config_path Path the configuration was loaded from.
 * @param os          Host the directories are on. Must not be `NULL`.
 * @param scratch     Arena for the ancestor paths.
 * @param log         Log that receives the first overlap. Must not be `NULL`.
 * @return `0` when `output_dir` and the inputs are apart, or `-1` when one holds the other.
 */
int manifest_builder_check_output_dir(struct Str output_dir,
                                      const struct ManifestRoot* roots,
                                      ptrdiff_t root_count,
                                      struct Str config_path,
                                      struct Os* os,
                                      struct Arena scratch,
                                      struct Log* log) __attribute__((nonnull(5, 7)));

/**
 * @brief Prepares a builder: the identities of the roots and of every input file.
 *
 * @param builder     Builder to prepare. Must not be `NULL`.
 * @param manifest    Manifest that receives the paths. Must not be `NULL`.
 * @param output_dir  Output directory.
 * @param roots       Input roots, which the builder fills with identities and keeps. Must not be
 *                    `NULL` unless `root_count` is `0`.
 * @param root_count  Number of entries in `roots`.
 * @param input_paths Input files outside the roots that no output may overwrite. Must not be
 *                    `NULL`.
 * @param os          Host the inputs are on. Must not be `NULL`.
 * @param perm        Arena that owns the identities and the manifest's entries. Must not be `NULL`.
 */
void manifest_builder_init(struct ManifestBuilder* builder,
                           struct Manifest* manifest,
                           struct Str output_dir,
                           struct ManifestRoot* roots,
                           ptrdiff_t root_count,
                           const struct PathList* input_paths,
                           struct Os* os,
                           struct Arena* perm) __attribute__((nonnull(1, 2, 6, 7, 8)));

/**
 * @brief Records the output `relative_path` below `output_dir`, unless it exceeds an output-path
 *        limit, lands at or below an input root, names an input file, or is claimed already.
 *
 * An output inside a root that shares the cause of one reported already, reaching the root through
 * the same directory, or one below it, from the same source, or one below it, is refused without a
 * line of its own, so one symlink into a root is reported once and not once per output.
 *
 * @param builder       Prepared builder. Must not be `NULL`.
 * @param relative_path Output path relative to `output_dir`.
 * @param source_label  What produces the output, which a diagnostic names.
 * @param scratch       Arena for the joined path.
 * @param log           Log that receives the failure. Must not be `NULL`.
 * @return `0` when the path was recorded, or `-1` when it was refused.
 */
int manifest_builder_add(struct ManifestBuilder* builder,
                         struct Str relative_path,
                         struct Str source_label,
                         struct Arena scratch,
                         struct Log* log) __attribute__((nonnull(1, 5)));

/**
 * @brief Records the output `relative_path` below `output_dir` with the attributes that name its
 *        inputs, refusing it as `manifest_builder_add` does.
 *
 * @param builder       Prepared builder. Must not be `NULL`.
 * @param relative_path Output path relative to `output_dir`.
 * @param source_label  What produces the output, which a diagnostic names.
 * @param attributes    Text that names every input of the output, with no tab or newline, as
 *                      `ManifestOutput.attributes` describes, or empty when it declares none.
 * @param scratch       Arena for the joined path.
 * @param log           Log that receives the failure. Must not be `NULL`.
 * @return `0` when the path was recorded, or `-1` when it was refused.
 */
int manifest_builder_add_output(struct ManifestBuilder* builder,
                                struct Str relative_path,
                                struct Str source_label,
                                struct Str attributes,
                                struct Arena scratch,
                                struct Log* log) __attribute__((nonnull(1, 6)));

/**
 * @brief Records `relative_path` below `output_dir` as a copy of `source_path` that the output
 *        writer streams, refused as `manifest_builder_add` refuses, labelled by `source_path`.
 *
 * Its attributes are those of `manifest_builder_add_copies`, from `source_name` and `source_stat`,
 * and the output writer records `source_stat`'s size as the size of the copy it streams. A copy
 * whose path below the output is not its source's path below a root, such as fram's original under
 * a slug, records itself with this.
 *
 * @param builder       Prepared builder. Must not be `NULL`.
 * @param relative_path Output path relative to `output_dir`.
 * @param source_path   File the output is a copy of.
 * @param source_name   Path of `source_path` relative to its root, such as `media_dir`, so that
 *                      moving the whole site keeps the attributes.
 * @param source_stat   What `os_stat` reported for `source_path`. Must not be `NULL`.
 * @param scratch       Arena for the joined path and the attributes.
 * @param log           Log that receives the refusal. Must not be `NULL`.
 * @return `0` when the copy was recorded, or `-1` when it was refused.
 */
int manifest_builder_add_copy(struct ManifestBuilder* builder,
                              struct Str relative_path,
                              struct Str source_path,
                              struct Str source_name,
                              const struct OsStat* source_stat,
                              struct Arena scratch,
                              struct Log* log) __attribute__((nonnull(1, 5, 7)));

/**
 * @brief Records each of `paths` at the path it has below `root_dir`, labelled by the file itself,
 *        as a copy of its source that the output writer streams, with the source's path below
 *        `root_dir`, size and modification time as its attributes.
 *
 * Each path is refused as `manifest_builder_add` refuses, and every path is checked even after one
 * is refused, so each refusal is reported in one run.
 *
 * The attributes are `src=<path below root_dir> len=<bytes> mtime=<seconds>.<nanoseconds>`, so the
 * output writer keeps a copy whose source has the name, size and modification time that the build
 * cache recorded, and records the source's size as the size of the copy it streams. A source whose
 * stat has a negative `len`, as `walk_stat_paths` gives a file it could not inspect, declares no
 * attributes, so it is copied every build and the copy reports why it fails.
 *
 * @param builder    Prepared builder. Must not be `NULL`.
 * @param paths      Files below `root_dir`, such as a build's static files. Must not be `NULL`.
 * @param stats      What `os_stat` reported for each of `paths`, parallel to them. Must not be
 *                   `NULL` unless `paths` is empty.
 * @param root_dir   Directory the files are below.
 * @param is_skipped For each of `paths`, whether to leave it out, or `NULL` to record every one.
 * @param scratch    Arena for each joined path and its attributes.
 * @param log        Log that receives every refusal. Must not be `NULL`.
 * @return `0` when every path that was not skipped was recorded, or `-1` otherwise.
 */
int manifest_builder_add_copies(struct ManifestBuilder* builder,
                                const struct PathList* paths,
                                const struct OsStat* stats,
                                struct Str root_dir,
                                const bool* is_skipped,
                                struct Arena scratch,
                                struct Log* log) __attribute__((nonnull(1, 2, 7)));

/**
 * @brief Records each of `names`, a config list of templates whose names are their output paths,
 *        labelled `<list_key>[<index>]`.
 *
 * Every name is checked even after one is refused, so each refusal is reported in one run.
 *
 * @param builder  Prepared builder. Must not be `NULL`.
 * @param list_key Config key of the list, such as `aggregate_templates`.
 * @param names    Template names, each a safe relative path. Must not be `NULL` unless `count` is
 *                 `0`.
 * @param count    Number of entries in `names`.
 * @param scratch  Arena for each label and joined path.
 * @param log      Log that receives every refusal. Must not be `NULL`.
 * @return `0` when every name was recorded, or `-1` otherwise.
 */
int manifest_builder_add_template_list(struct ManifestBuilder* builder,
                                       struct Str list_key,
                                       const struct Str* names,
                                       ptrdiff_t count,
                                       struct Arena scratch,
                                       struct Log* log) __attribute__((nonnull(1, 6)));

/**
 * @brief Reports every recorded path that nests under another, once every path is recorded.
 *
 * @param builder Builder whose manifest is complete. Must not be `NULL`.
 * @param log     Log that receives every nesting. Must not be `NULL`.
 * @return `0` when every path can coexist on disk, or `-1` otherwise.
 */
int manifest_builder_finish(const struct ManifestBuilder* builder, struct Log* log)
    __attribute__((nonnull(1, 2)));

#endif
