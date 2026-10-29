#ifndef SURGE_BUILD_CACHE_H
#define SURGE_BUILD_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/str.h"

struct Arena;
struct Buf;
struct Log;
struct OsStat;
struct SiteConfig;

/**
 * Format of the build cache's rows, in its file name. Bump it whenever the row format or the
 * meaning of a field changes, so a build never opens a file written in another format, and the
 * prune removes the old one.
 */
#define BUILD_CACHE_VERSION 1

/**
 * Version of how an output is made from its inputs, in the page recipe. Bump it whenever a change
 * to the code, including the libraries in `libs/` and the vendored libgit2, md4c, org4c,
 * mustache4c, tomlc17 and tree-sitter, means that an output its inputs still match must be made
 * again.
 */
#define PAGE_RECIPE_VERSION 1

/** Largest build cache a build reads, in bytes. A larger file is an empty build cache. */
#define BUILD_CACHE_FILE_LEN_MAX ((ptrdiff_t)256 * 1024 * 1024)

/** What a build cache row records. Rows sort by kind first, in this order. */
enum BuildCacheKind {
  /** A file below `templates_dir`, named by its path relative to it: its size and mtime. */
  BUILD_CACHE_KIND_TEMPLATE,

  /** A file below `static_dir`, named by its path relative to it: its size and mtime. */
  BUILD_CACHE_KIND_STATIC,

  /**
   * What a repo's pages read from the config and the repository besides its refs, named by the
   * repo: the id of that text, so a changed description, or a repo moved in the config, makes them
   * again.
   */
  BUILD_CACHE_KIND_REPO,

  /**
   * What the `repo` map of every page of a repo shows besides the repo's name, named by the repo:
   * the id of `page_context_repo_text`, so a new default branch or a first release project makes
   * every page of the repo again, its commit pages included.
   */
  BUILD_CACHE_KIND_REPO_MAP,

  /** A ref of a repo, named `<repo>/<refname>`: the oid it points at. */
  BUILD_CACHE_KIND_REF,

  /**
   * The clone export of a repo, named by the repo: its pack, the pack's and the idx's sizes, the id
   * of its `info/refs` bytes, and the pack it replaced.
   */
  BUILD_CACHE_KIND_EXPORT,

  /**
   * The releases of a repo, named by the repo: the id of its release projects, and of each
   * release's tag, version and assets with their sizes and mtimes, which its release outputs read.
   */
  BUILD_CACHE_KIND_RELEASES,

  /**
   * A tree page, blob page or raw copy of a branch, named by its path below `output_dir`: the ids
   * of what it reads, joined by `,`, such as its blob, its mode, and the branch's root tree when
   * its links resolve against every path of the branch.
   */
  BUILD_CACHE_KIND_PAGE,
};

/** One row of a build cache, as the build that wrote it saw its input. */
struct BuildCacheRow {
  /** What the row records, which selects the fields below that carry meaning. */
  enum BuildCacheKind kind;

  /** Name of the row, which holds no tab or newline. */
  struct Str name;

  /** For a template or static row, the size of the file in bytes. */
  ptrdiff_t len;

  /** For a template or static row, the modification time, in whole seconds. */
  int64_t mtime_s;

  /** For a template or static row, the nanoseconds within `mtime_s`. */
  int64_t mtime_ns;

  /**
   * For a repo row, the id of what its pages read. For a repo map row, the id of what its `repo`
   * map shows. For a ref row, the oid it points at. For an export row, the name of the pack, the
   * hex id in `pack-<id>.pack`, or empty when the repo has nothing to export. For a page row, the
   * ids of what the output reads.
   */
  struct Str id;

  /** For an export row, the size of the pack in bytes. */
  ptrdiff_t pack_len;

  /** For an export row, the size of the pack's idx in bytes. */
  ptrdiff_t idx_len;

  /** For an export row, the id of the `info/refs` bytes. */
  struct Str info_refs_id;

  /** For an export row, the pack this one replaced, which stays one build, or empty. */
  struct Str previous_pack_id;
};

/** The rows of one build cache, sorted by kind and then bytewise by name. A zeroed one is empty. */
struct BuildCache {
  /** Rows, sorted by `kind` and then bytewise by `name`, with no row twice. */
  struct BuildCacheRow* rows;

  /** Number of rows in `rows`. */
  ptrdiff_t count;
};

/**
 * @brief Returns the path of the build cache below `output_dir`, `_surge/v<n>-build-cache.tsv`.
 *
 * @param output_dir Output directory the build cache lives in.
 * @param arena      Arena that owns the path. Must not be `NULL`.
 * @return The path, owned by `arena`.
 */
struct Str build_cache_path(struct Str output_dir, struct Arena* arena) __attribute__((nonnull(2)));

/**
 * @brief Returns the name of a ref row, `<repo>/<refname>`.
 *
 * @param repo_name Name of the repo.
 * @param ref_name  Full name of the ref, such as `refs/heads/main`.
 * @param arena     Arena that owns the name. Must not be `NULL`.
 * @return The name.
 */
struct Str build_cache_ref_name(struct Str repo_name, struct Str ref_name, struct Arena* arena)
    __attribute__((nonnull(3)));

/**
 * @brief Parses a build cache with `build_cache_file_parse`, rejecting a first line other than the
 *        page recipe of `site_config`.
 *
 * The page recipe is what every output depends on besides its own inputs: the site keys and the
 * limits of the config, the generator string, the grammars the build links, and
 * `PAGE_RECIPE_VERSION`, each as `name=value`, joined by tabs, with each `\`, tab and newline of
 * the title, the description, the author and `base_url` written as `\\`, `\t` and `\n`. Every row
 * ends in a newline, and the rows are sorted by kind and then by name, with none twice. An empty
 * file, which a build leaves until it has written every output, is reported as such.
 *
 * @param text        Bytes of the build cache.
 * @param site_config Configuration of this build, for its page recipe. Must not be `NULL`.
 * @param perm        Arena that owns the rows. Must not be `NULL`.
 * @param cache_out   Receives the rows on success. Written only on success. Must not be `NULL`.
 * @param log         Log that receives the reason the build cache cannot be used. Must not be
 *                    `NULL`.
 * @return `0` on success, or `-1` when the file is empty, the first line differs, or a row does not
 *         parse.
 */
int build_cache_parse(struct Str text,
                      const struct SiteConfig* site_config,
                      struct Arena* perm,
                      struct BuildCache* cache_out,
                      struct Log* log) __attribute__((nonnull(2, 3, 4, 5)));

/**
 * @brief Finds the row of `kind` named `name`.
 *
 * @param cache Build cache to search. Must not be `NULL`.
 * @param kind  Kind of the row.
 * @param name  Name of the row.
 * @return The row, or `NULL` when there is none.
 */
const struct BuildCacheRow* build_cache_find(const struct BuildCache* cache,
                                             enum BuildCacheKind kind,
                                             struct Str name) __attribute__((nonnull(1)));

/**
 * @brief Reports whether the row of `kind` named `name` records the size and modification time that
 *        `stat` reports now.
 *
 * @param cache Build cache to search. Must not be `NULL`.
 * @param kind  Kind of the row, a template or a static row.
 * @param name  Name of the row.
 * @param stat  What `os_stat` reported for the file now. Must not be `NULL`.
 * @return `true` when the row exists and matches.
 */
bool build_cache_has_stat(const struct BuildCache* cache,
                          enum BuildCacheKind kind,
                          struct Str name,
                          const struct OsStat* stat) __attribute__((nonnull(1, 4)));

/**
 * @brief Reports whether the rows of `kind` are exactly `rows`, with every field equal.
 *
 * @param cache Build cache to search. Must not be `NULL`.
 * @param kind  Kind of the rows to compare.
 * @param rows  Rows of `kind` this build has, `count` of them, sorted by name. Must not be `NULL`
 *              unless `count` is `0`.
 * @param count Number of entries in `rows`.
 * @return `true` when the cache holds the same rows of `kind`, no more and no fewer.
 */
bool build_cache_has_rows(const struct BuildCache* cache,
                          enum BuildCacheKind kind,
                          const struct BuildCacheRow* rows,
                          ptrdiff_t count) __attribute__((nonnull(1)));

/**
 * @brief Appends a whole build cache: the page recipe, then the rows, sorted in place first.
 *
 * A row whose name holds a tab or a newline is left out, so what depends on its input is made on
 * every build, and no name needs escaping.
 *
 * @param buffer      Buffer that receives the text. Must not be `NULL`.
 * @param site_config Configuration of the build, for the page recipe. Must not be `NULL`.
 * @param cache       Rows to append, each name once per kind, which this sorts. Must not be `NULL`.
 */
void build_cache_format(struct Buf* buffer,
                        const struct SiteConfig* site_config,
                        struct BuildCache* cache) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Orders two rows by kind and then bytewise by name, for `qsort` and `bsearch`.
 *
 * @param a First `struct BuildCacheRow`. Must not be `NULL`.
 * @param b Second `struct BuildCacheRow`. Must not be `NULL`.
 * @return Negative, zero or positive as `a` sorts before, with, or after `b`.
 */
int build_cache_compare_rows(const void* a, const void* b) __attribute__((nonnull(1, 2)));

#endif
