#ifndef APPS_OUTPUT_OUTPUT_PATH_H
#define APPS_OUTPUT_OUTPUT_PATH_H

#include <stddef.h>

#include "core/str.h"

struct Arena;
struct Log;

/**
 * @brief Rejects a generated output path that exceeds either output-path limit.
 *
 * Every producer of an output path reports a limit failure through this one function, so the paths
 * of every kind of output share one wording. The limits themselves are applied by
 * `path_check_output_limits`.
 *
 * @param relative_path Output path relative to the output directory.
 * @param source_label  Producer the path was generated for, such as a source file or a config list
 *                      entry, named in the diagnostic.
 * @param log           Log that receives an error naming the limit that was exceeded. Must not be
 *                      `NULL`.
 * @return `0` when both limits hold, or `-1` when one is exceeded.
 */
int output_path_check_limits(struct Str relative_path, struct Str source_label, struct Log* log)
    __attribute__((nonnull(3)));

/**
 * @brief Returns the `../` prefix that leads from an output file's directory back to the output
 *        root.
 *
 * @param relative_path Output file path relative to the output directory, already accepted by
 *                      `output_path_check_limits`.
 * @param arena         Arena that owns the returned prefix. Must not be `NULL`.
 * @return One `../` per `/` in `relative_path`, or an empty string for a top-level file, owned by
 *         `arena`.
 */
struct Str output_path_to_root(struct Str relative_path, struct Arena* arena)
    __attribute__((nonnull(2)));

/**
 * @brief Rejects a `not_found_path` value that is not a safe relative path ending in `.html`.
 *
 * A host serves the not-found page from a path of its own, such as `404.html` or
 * `bunnycdn_errors/404.html`, so the value is a path below the output directory that follows the
 * template-name rules: only letters, digits, `_`, `-`, `.` and `/`, and no empty, `.` or `..`
 * segment. A rejection is
 * `config key 'not_found_path' must be a safe relative path ending in '.html': '<value>'`, an empty
 * value included.
 *
 * @param not_found_path Value of the `not_found_path` config key.
 * @param log            Log that receives the rejection, which names the key and the value. Must
 *                       not be `NULL`.
 * @return `0` when the value is accepted, or `-1` when it is not.
 */
int output_path_check_not_found(struct Str not_found_path, struct Log* log)
    __attribute__((nonnull(2)));

/**
 * @brief Returns the `path_to_root` of the not-found page: the path of `base_url` followed by `/`.
 *
 * A host serves the not-found page at every depth, so a `../` prefix would be right at one depth
 * only. The path of `base_url` is the same at every depth, and is root-relative rather than an
 * absolute URL, so the page also works on a host other than `base_url`'s, such as a preview.
 *
 * @param base_url Absolute `http://` or `https://` URL of the site, as `toml_copy_base_url` accepts
 *                 it, or empty for a site with none, which is at the host's root.
 * @param arena    Arena that owns the returned prefix. Must not be `NULL`.
 * @return `/` for an empty `base_url` or one without a path, such as `https://example.com`, or the
 *         path with one trailing `/`, such as `/blog/` for `https://example.com/blog`, owned by
 *         `arena`.
 */
struct Str output_path_not_found_root(struct Str base_url, struct Arena* arena)
    __attribute__((nonnull(2)));

#endif
