#ifndef SURGE_EXPORT_WRITER_H
#define SURGE_EXPORT_WRITER_H

#include "core/arena.h"
#include "core/str.h"

struct BuildCacheRow;
struct SiteConfig;
struct Log;
struct ManifestBuilder;
struct Os;
struct OutputWriter;
struct RepoScan;

/**
 * @brief Records the outputs of one repo's clone export: `<r>.git/HEAD`, `info/refs`,
 *        `objects/info/packs`, the pack and its idx, and the pack before it, which stays one build.
 *
 * Nothing a clone needs starts with a dot or an underscore, so a host that hides such files serves
 * every one of them.
 *
 * @param builder Prepared manifest builder. Must not be `NULL`.
 * @param scan    Scanned repo. Must not be `NULL`.
 * @param scratch Arena for the paths.
 * @param log     Log that receives every refusal. Must not be `NULL`.
 * @return `0` when every path was recorded, or `-1` otherwise.
 */
int export_writer_register(struct ManifestBuilder* builder,
                           const struct RepoScan* scan,
                           struct Arena scratch,
                           struct Log* log) __attribute__((nonnull(1, 2, 4)));

/**
 * @brief Writes one repo's clone export into the output, or keeps what is current.
 *
 * A pack the scan wrote below `TMPDIR` is streamed in, so the manifest, a dry run and the prune see
 * it. `HEAD`, `info/refs` and `objects/info/packs` are kept when the output holds their bytes, so a
 * moved default branch writes only `HEAD`.
 *
 * @param writer     Writer of the build. Must not be `NULL`.
 * @param output_dir Output directory.
 * @param scan       Scanned repo. Must not be `NULL`.
 * @param os         Host the existing outputs are read on. Must not be `NULL`.
 * @param scratch    Arena for the paths and the existing outputs.
 * @param log        Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when a write fails.
 */
int export_writer_write(const struct OutputWriter* writer,
                        struct Str output_dir,
                        const struct RepoScan* scan,
                        struct Os* os,
                        struct Arena scratch,
                        struct Log* log) __attribute__((nonnull(1, 3, 4, 6)));

/** Output path of the headers file below `output_dir`, which Cloudflare Pages and Netlify read. */
#define EXPORT_WRITER_HEADERS_PAGE S("_headers")

/**
 * @brief Writes `_headers` into the output, or keeps it when the output holds its bytes.
 *
 * It sets `Cache-Control: no-cache` on every clone export's `HEAD`, `info/refs` and
 * `objects/info/packs`, which change as its refs move, with one rule each, and a year's immutable
 * caching on each repo's packs, which a name that hashes their content never changes under. Each
 * path starts with the path of `base_url`, so a site served below a prefix gets it too. Cloudflare
 * Pages takes 100 rules and one splat in each, so a site of up to 97 repos fits.
 *
 * @param writer      Writer of the build. Must not be `NULL`.
 * @param site_config Loaded configuration. Must not be `NULL`.
 * @param os          Host the existing file is read on. Must not be `NULL`.
 * @param scratch     Arena for the bytes and the existing file.
 * @param log         Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the write fails.
 */
int export_writer_write_headers(const struct OutputWriter* writer,
                                const struct SiteConfig* site_config,
                                struct Os* os,
                                struct Arena scratch,
                                struct Log* log) __attribute__((nonnull(1, 2, 3, 5)));

/**
 * @brief Returns the build cache row of one repo's clone export.
 *
 * @param scan Scanned repo. Must not be `NULL`.
 * @return The row, whose strings point into `scan`.
 */
struct BuildCacheRow export_writer_row(const struct RepoScan* scan) __attribute__((nonnull(1)));

#endif
