#ifndef APPS_OUTPUT_OUTPUT_CACHE_H
#define APPS_OUTPUT_OUTPUT_CACHE_H

#include <stddef.h>

#include "core/arena.h"
#include "core/str.h"
#include "output/build_cache_file.h"

struct Log;
struct Os;
struct OutputWriter;

/** Largest output cache a build reads, in bytes. A larger file is an empty cache. */
#define OUTPUT_CACHE_FILE_LEN_MAX ((ptrdiff_t)256 * 1024 * 1024)

/** What an output cache recorded for one output that a build wrote or kept. */
struct OutputCacheRow {
  /** Path of the output relative to `output_dir`. */
  struct Str relative_path;

  /** Attributes the manifest declared for the output, which are never empty. */
  struct Str attributes;

  /** Size of the output in bytes when the build had written or kept it. */
  ptrdiff_t len;
};

/** One fact that a build recorded about an input, such as what a probe of a source read. */
struct OutputCacheFact {
  /** What the fact is about, such as a source path. */
  struct Str key;

  /** The fact. */
  struct Str value;
};

/**
 * The rows and facts of one output cache, each sorted bytewise, by path and by key. A zeroed cache
 * is empty. Its strings are views of the file's text.
 */
struct OutputCache {
  /** Output rows, sorted by `relative_path`, with no path twice. */
  struct OutputCacheRow* rows;

  /** Number of entries in `rows`. */
  ptrdiff_t row_count;

  /** Facts, sorted by `key`, with no key twice. */
  struct OutputCacheFact* facts;

  /** Number of entries in `facts`. */
  ptrdiff_t fact_count;
};

/**
 * @brief Reads the output cache at `cache_path`, and says why it is not used when it is not.
 *
 * The first line names the format and its version, `output cache v1`, so a file of another version
 * is unusable, as is one that `build_cache_file_parse` rejects. Each row after it is
 * `output<TAB><path><TAB><attributes><TAB><len>` or `fact<TAB><key><TAB><value>`, and every output
 * row comes before every fact. The reason a file is not used is the one `build_cache_file_load`
 * gives, or `output cache is not in the current format`.
 *
 * @param cache_path Path of the output cache.
 * @param os         Host the file is read on. Must not be `NULL`.
 * @param perm       Arena that owns the text, the cache and the reason. Must not be `NULL`.
 * @param scratch    Arena for the reason while it is written. Must not overlap `perm`.
 * @param cache_out  Receives the cache, or an empty one when the file is missing or unusable. Must
 *                   not be `NULL`.
 * @param reason_out Receives, for an unusable file, why it is not used. Must not be `NULL`.
 * @return What the load found.
 */
enum BuildCacheFileState output_cache_load(struct Str cache_path,
                                           struct Os* os,
                                           struct Arena* perm,
                                           struct Arena scratch,
                                           struct OutputCache* cache_out,
                                           struct Str* reason_out)
    __attribute__((nonnull(2, 3, 5, 6)));

/**
 * @brief Finds the row of the output at `relative_path`.
 *
 * @param cache         Cache to search. Must not be `NULL`.
 * @param relative_path Path of the output relative to `output_dir`.
 * @return The row, or `NULL` when the cache has none for the path.
 */
const struct OutputCacheRow* output_cache_find(const struct OutputCache* cache,
                                               struct Str relative_path)
    __attribute__((nonnull(1)));

/**
 * @brief Finds the fact recorded under `key`.
 *
 * @param cache Cache to search. Must not be `NULL`.
 * @param key   Key of the fact.
 * @return The fact, or `NULL` when the cache has none under `key`.
 */
const struct OutputCacheFact* output_cache_find_fact(const struct OutputCache* cache,
                                                     struct Str key) __attribute__((nonnull(1)));

/**
 * @brief Writes the output cache of a build from its manifest and `facts`.
 *
 * A build calls it after its last output, also when the build failed: only what this build wrote or
 * kept gets a row, so a failed output has none and the next build makes it again. Until then the
 * previous cache stays, so an interrupted build leaves it: a row keeps only an output whose
 * attributes match and whose file has the recorded size, and a file that the build cut short has
 * another size, while one that it rewrote whole has new attributes. Each output that the manifest
 * marks written or kept and that declares attributes gets a row, with the size the output has now:
 * the size its mark recorded, which `output_writer_produce` gives every output it handles, or else
 * the size a stat of the output gives. An output without a recorded size that is missing or a
 * directory gets none. A path, an attribute text, a key or a value that holds a tab or a newline
 * gets no row either, so no field needs escaping and what the row would vouch for is made on every
 * build. A dry run writes no cache and looks at no output, and only marks `cache_path` written.
 *
 * @param writer     Writer of the build, whose manifest names `cache_path`. Must not be `NULL`.
 * @param cache_path Path of the output cache.
 * @param facts      Facts to record, each key once, in any order. Must not be `NULL` unless
 *                   `fact_count` is `0`.
 * @param fact_count Number of entries in `facts`.
 * @param scratch    Arena for the text.
 * @param log        Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the write fails.
 */
int output_cache_write(const struct OutputWriter* writer,
                       struct Str cache_path,
                       const struct OutputCacheFact* facts,
                       ptrdiff_t fact_count,
                       struct Arena scratch,
                       struct Log* log) __attribute__((nonnull(1, 6)));

#endif
