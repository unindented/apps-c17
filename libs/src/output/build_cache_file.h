#ifndef APPS_OUTPUT_BUILD_CACHE_FILE_H
#define APPS_OUTPUT_BUILD_CACHE_FILE_H

#include <stddef.h>

#include "core/arena.h"
#include "core/str.h"

struct Log;
struct Os;

/**
 * @brief Prepares for the rows of a build cache, before the first row is parsed, so the app can
 *        size the array its rows go in.
 *
 * @param row_count Number of rows the file holds.
 * @param perm      Arena that owns the rows. Must not be `NULL`.
 * @param userdata  Opaque pointer the parse was given.
 */
typedef void (*BuildCacheFileBeginFn)(ptrdiff_t row_count, struct Arena* perm, void* userdata);

/**
 * @brief Parses one row of a build cache into the app's own row type.
 *
 * @param line     The row, without its newline.
 * @param index    Zero-based index of the row, below the count that `BuildCacheFileBeginFn` got.
 * @param perm     Arena that owns what the row keeps, such as a copy of its name. Must not be
 *                 `NULL`.
 * @param userdata Opaque pointer the parse was given.
 * @return `0` on success, or `-1` when the row does not parse or does not sort after the row before
 *         it.
 */
typedef int (*BuildCacheFileRowFn)(struct Str line,
                                   ptrdiff_t index,
                                   struct Arena* perm,
                                   void* userdata);

/**
 * @brief Parses the text of a build cache whose first line must be `header`, handing each row to
 *        `parse_row`.
 *
 * A build writes its cache in place, so an interrupted write leaves the file empty or cut short: an
 * empty file is reported as such, and every row must end in a newline. A failed parse leaves `perm`
 * as it found it.
 *
 * The reason a cache cannot be used is
 * `build cache is empty, as a build that did not finish leaves it`, the app's `header_mismatch`, or
 * `build cache does not parse at line <n>`, counting the header's lines.
 *
 * @param text            Bytes of the build cache.
 * @param header          The first line that this build writes, with its newline.
 * @param header_mismatch Reason given when the text does not start with `header`, which names what
 *                        the header records, such as the app's version.
 * @param begin           Called once with the row count, before any row. Must not be `NULL`.
 * @param parse_row       Called for each row in order. Must not be `NULL`.
 * @param userdata        Opaque pointer passed to `begin` and `parse_row`.
 * @param perm            Arena that owns the rows, which the callbacks get. Must not be `NULL`.
 * @param log             Log that receives the reason the cache cannot be used. Must not be `NULL`.
 * @return `0` on success, or `-1` when the file is empty, its header differs, or a row does not
 *         parse.
 */
int build_cache_file_parse(struct Str text,
                           struct Str header,
                           struct Str header_mismatch,
                           BuildCacheFileBeginFn begin,
                           BuildCacheFileRowFn parse_row,
                           void* userdata,
                           struct Arena* perm,
                           struct Log* log) __attribute__((nonnull(4, 5, 7, 8)));

/**
 * @brief Parses the read text of a build cache, as the app's own `build_cache_parse` does.
 *
 * @param text     Bytes of the build cache.
 * @param perm     Arena that owns the parsed cache. Must not be `NULL`.
 * @param log      Log that receives the reason the cache cannot be used, which a `-1` must write.
 *                 Must not be `NULL`.
 * @param userdata Opaque pointer the load was given.
 * @return `0` on success, or `-1` when the cache cannot be used.
 */
typedef int (*BuildCacheFileParseFn)(struct Str text,
                                     struct Arena* perm,
                                     struct Log* log,
                                     void* userdata);

/** What a load found at the build cache's path. */
enum BuildCacheFileState {
  /** No file, the usual case of a first build. */
  BUILD_CACHE_FILE_MISSING,

  /** A file that cannot be read or does not parse, which the build does without. */
  BUILD_CACHE_FILE_UNUSABLE,

  /** A file that parsed. */
  BUILD_CACHE_FILE_LOADED,
};

/**
 * @brief Reads and parses the build cache at `path`, and says why it is not used when it is not.
 *
 * A build cache that cannot be used is a note for the verbose log and never a build error, so the
 * reason does not go to the build's log. A file that cannot be read gives the read's message, as
 * `failed to read build cache: <cause> ('<path>')`, and one that does not parse gives the parse's.
 *
 * @param path         Path of the build cache.
 * @param file_len_max Largest file read, in bytes. A larger one is unusable.
 * @param parse        Parses the text into the app's cache. Must not be `NULL`.
 * @param userdata     Opaque pointer passed to `parse`.
 * @param os           Host the file is read on. Must not be `NULL`.
 * @param perm         Arena that owns the text, the parsed cache and the reason. Must not be
 *                     `NULL`.
 * @param scratch      Arena for the reason while it is written. Must not overlap `perm`.
 * @param reason_out   Receives, for an unusable file, why it is not used, without a newline. Must
 *                     not be `NULL`.
 * @return What the load found.
 */
enum BuildCacheFileState build_cache_file_load(struct Str path,
                                               ptrdiff_t file_len_max,
                                               BuildCacheFileParseFn parse,
                                               void* userdata,
                                               struct Os* os,
                                               struct Arena* perm,
                                               struct Arena scratch,
                                               struct Str* reason_out)
    __attribute__((nonnull(3, 5, 6, 8)));

#endif
