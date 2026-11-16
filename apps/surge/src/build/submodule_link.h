#ifndef SURGE_SUBMODULE_LINK_H
#define SURGE_SUBMODULE_LINK_H

#include "core/str.h"

struct Arena;
struct SiteConfig;

/**
 * @brief Returns a submodule's URL as a page shows it: without the whole userinfo of an `http://`
 *        or `https://` URL, which can hold a token, and without a `user:password@` of any other.
 *
 * @param url   The URL as `.gitmodules` writes it.
 * @param arena Arena that owns the URL. Must not be `NULL`.
 * @return The URL, or `url` itself when it has no credentials.
 */
struct Str submodule_link_shown_url(struct Str url, struct Arena* arena)
    __attribute__((nonnull(2)));

/**
 * @brief Returns the link to the commit of a submodule.
 *
 * A URL that names a repo of the site, by the repo's clone URL or as a URL that starts with `./` or
 * `../` and resolves as git resolves it against the clone URL of `repo_name`, links that repo's
 * commit page. An `https://<host>/<path>`, `git@<host>:<path>` or `ssh://[<user>@]<host>/<path>`
 * URL of `github.com`, `gitlab.com` or `codeberg.org`, in any letter case, whose path has at least
 * two segments, links the commit on that host, without the path's trailing `/` and `.git`. The
 * credentials come off first, as `submodule_link_shown_url` takes them.
 *
 * @param url          The submodule's URL as `.gitmodules` writes it.
 * @param commit_oid   Hex id of the submodule's commit.
 * @param site_config  Configuration of the build, whose repos and `base_url` name the site's clone
 *                     URLs. Must not be `NULL`.
 * @param repo_name    Name of the repo whose tree holds the submodule.
 * @param path_to_root Prefix from the page back to the output root.
 * @param arena        Arena that owns the link. Must not be `NULL`.
 * @return The link, or empty for any other URL.
 */
struct Str submodule_link_commit_url(struct Str url,
                                     struct Str commit_oid,
                                     const struct SiteConfig* site_config,
                                     struct Str repo_name,
                                     struct Str path_to_root,
                                     struct Arena* arena) __attribute__((nonnull(3, 6)));

#endif
