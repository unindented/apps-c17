#ifndef SURGE_CONTRIBUTION_WRITER_H
#define SURGE_CONTRIBUTION_WRITER_H

#include <stddef.h>
#include <stdint.h>

#include "core/arena.h"
#include "core/str.h"

struct Log;
struct ManifestBuilder;
struct OutputCache;
struct OutputWriter;
struct RepoModel;

/**
 * @brief Returns the output path of a year's contributions file below `output_dir`,
 *        `contributions/<year>.json`, with four digits of year.
 *
 * @param year  The year, from 0000 to 9999.
 * @param arena Arena that owns the path. Must not be `NULL`.
 * @return The path.
 */
struct Str contribution_writer_path(int64_t year, struct Arena* arena) __attribute__((nonnull(2)));

/**
 * @brief Records the contributions file of each calendar year with commits.
 *
 * A file declares the page recipe, the commits of its year, each with its day and LLM flag, and the
 * year's last counted day, which moves when the year stops being the newest.
 *
 * @param builder    Prepared manifest builder. Must not be `NULL`.
 * @param repos      What the scan found, each with its `contributions`. Must not be `NULL` unless
 *                   `repo_count` is `0`.
 * @param repo_count Number of entries in `repos`.
 * @param recipe_id  Id of the page recipe.
 * @param scratch    Arena for the commits, the paths and the attributes.
 * @param log        Log that receives every refusal. Must not be `NULL`.
 * @return `0` when every path was recorded, or `-1` otherwise.
 */
int contribution_writer_register(struct ManifestBuilder* builder,
                                 const struct RepoModel* repos,
                                 ptrdiff_t repo_count,
                                 struct Str recipe_id,
                                 struct Arena scratch,
                                 struct Log* log) __attribute__((nonnull(1, 6)));

/**
 * @brief Writes the contributions file of each calendar year with commits through
 *        `output_writer_produce`, oldest year first, or keeps those that are current.
 *
 * A file is one line of JSON and a newline: the year's calendar, with the keys of the site index's
 * `contributions` map and `year`, its numbers as numbers and its flags as `true` or `false`.
 *
 * @param writer     Writer of the build. Must not be `NULL`.
 * @param output_dir Directory the outputs are below.
 * @param repos      What the scan found, each with its `contributions`. Must not be `NULL` unless
 *                   `repo_count` is `0`.
 * @param repo_count Number of entries in `repos`.
 * @param cache      Output cache of the previous build. Must not be `NULL`.
 * @param scratch    Arena for the commits and the files.
 * @param log        Log that receives every failure. Must not be `NULL`.
 * @return `0` when every file was written or kept, or `-1` otherwise.
 */
int contribution_writer_write(const struct OutputWriter* writer,
                              struct Str output_dir,
                              const struct RepoModel* repos,
                              ptrdiff_t repo_count,
                              const struct OutputCache* cache,
                              struct Arena scratch,
                              struct Log* log) __attribute__((nonnull(1, 5, 7)));

#endif
