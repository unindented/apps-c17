#include "build/submodule_link.h"

#include <stdbool.h>
#include <stddef.h>

#include "build/page_commit.h"
#include "build/page_context.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/str.h"
#include "domain/site_config.h"

/** A host whose commits a submodule links, and the path of a commit below a repository there. */
struct SubmoduleForge {
  /** Host name, compared after ASCII case folding. */
  struct Str host;

  /** What goes between the repository's path and the commit id, such as `/tree/`. */
  struct Str commit_infix;
};

/** The hosts whose commits a submodule links. */
static const struct SubmoduleForge SUBMODULE_FORGES[] = {
    {STR_INIT("github.com"), STR_INIT("/tree/")},
    {STR_INIT("gitlab.com"), STR_INIT("/-/tree/")},
    {STR_INIT("codeberg.org"), STR_INIT("/src/commit/")},
};

/** Number of entries in `SUBMODULE_FORGES`. */
enum {
  SUBMODULE_FORGE_COUNT = (int)(sizeof(SUBMODULE_FORGES) / sizeof(SUBMODULE_FORGES[0])),
};

/**
 * @brief Resolves a URL that starts with `./` or `../` against a repo's clone URL as git resolves a
 *        relative submodule URL: each `../` drops the last segment of the clone URL's path, each
 *        `./` drops nothing, and the rest follows a `/`.
 *
 * @param url       The relative URL.
 * @param clone_url Clone URL of the repo that holds the submodule.
 * @param arena     Arena that owns the URL. Must not be `NULL`.
 * @param url_out   Receives the absolute URL. Must not be `NULL`.
 * @return `true` when the URL resolves, `false` when a `../` would leave the host.
 */
static bool submodule_link_commit_url_resolve(struct Str url,
                                              struct Str clone_url,
                                              struct Arena* arena,
                                              struct Str* url_out) __attribute__((nonnull(3, 4)));

/**
 * @brief Finds the forge and the repository path of a URL of `SUBMODULE_FORGES`.
 *
 * @param url       URL without credentials.
 * @param forge_out Receives the index of the forge in `SUBMODULE_FORGES`. Must not be `NULL`.
 * @param path_out  Receives the repository's path, without a trailing `/` or `.git`. Must not be
 *                  `NULL`.
 * @return `true` for a URL of a forge whose path has at least two segments.
 */
static bool submodule_link_commit_url_forge(struct Str url,
                                            ptrdiff_t* forge_out,
                                            struct Str* path_out) __attribute__((nonnull(2, 3)));

/**
 * @brief Reports whether `text` starts with `prefix`, after ASCII case folding.
 *
 * @param text   Text to check.
 * @param prefix Prefix to look for.
 * @return `true` when it does.
 */
static bool has_prefix_folded(struct Str text, struct Str prefix);

/**
 * @brief Returns the index of the first `byte` in `text` at or after `start`.
 *
 * @param text  Text to search.
 * @param start Index the search starts at.
 * @param byte  Byte to find.
 * @return The index, or `text.len` when there is none.
 */
static ptrdiff_t find_byte(struct Str text, ptrdiff_t start, char byte);

struct Str submodule_link_shown_url(struct Str url, struct Arena* arena) {
  const ptrdiff_t colon = find_byte(url, 0, ':');
  if (!str_startswith(str_tail(url, colon), S("://"))) {
    return url;
  }
  const ptrdiff_t authority = colon + 3;
  const ptrdiff_t authority_end = find_byte(url, authority, '/');
  ptrdiff_t at = authority_end - 1;
  while (at >= authority && url.data[at] != '@') {
    at--;
  }
  if (at < authority) {
    return url;
  }
  const struct Str userinfo = str_head(str_tail(url, authority), at - authority);
  const struct Str scheme = str_head(url, colon);
  const bool is_http = str_eq_folded(scheme, S("http")) || str_eq_folded(scheme, S("https"));
  // A user name alone is no secret outside HTTP, such as the `git@` of an `ssh://` URL.
  if (!is_http && find_byte(userinfo, 0, ':') == userinfo.len) {
    return url;
  }
  return str_concat(str_head(url, authority), str_tail(url, at + 1), arena);
}

struct Str submodule_link_commit_url(struct Str url,
                                     struct Str commit_oid,
                                     const struct SiteConfig* site_config,
                                     struct Str repo_name,
                                     struct Str path_to_root,
                                     struct Arena* arena) {
  struct Str target = submodule_link_shown_url(url, arena);
  const bool is_relative = str_startswith(target, S("./")) || str_startswith(target, S("../"));
  if (is_relative &&
      !submodule_link_commit_url_resolve(
          target, page_context_clone_url(site_config, repo_name, arena), arena, &target)) {
    return (struct Str){0};
  }
  for (ptrdiff_t i = 0; i < site_config->repo_count; i++) {
    const struct Str name = site_config->repos[i].name;
    struct Arena scratch = *arena;
    if (str_eq(target, page_context_clone_url(site_config, name, &scratch))) {
      return page_commit_link(path_to_root, name, commit_oid, arena);
    }
  }
  ptrdiff_t forge = 0;
  struct Str path = {0};
  if (is_relative || !submodule_link_commit_url_forge(target, &forge, &path)) {
    return (struct Str){0};
  }
  struct Buf link = {.arena = arena};
  buf_printf(&link, "https://%.*s/%.*s%.*s%.*s", STR_FMT(SUBMODULE_FORGES[forge].host),
             STR_FMT(path), STR_FMT(SUBMODULE_FORGES[forge].commit_infix), STR_FMT(commit_oid));
  return buf_str(&link);
}

static bool submodule_link_commit_url_resolve(struct Str url,
                                              struct Str clone_url,
                                              struct Arena* arena,
                                              struct Str* url_out) {
  // The clone URL is `<scheme>://<host>/<path>`, and its path is where a `../` may cut.
  const ptrdiff_t path_start = find_byte(clone_url, find_byte(clone_url, 0, ':') + 3, '/');
  struct Str base = clone_url;
  struct Str rest = url;
  for (;;) {
    if (str_startswith(rest, S("./"))) {
      rest = str_tail(rest, 2);
    } else if (str_startswith(rest, S("../"))) {
      ptrdiff_t slash = base.len - 1;
      while (slash >= path_start && base.data[slash] != '/') {
        slash--;
      }
      if (slash < path_start) {
        return false;
      }
      base = str_head(base, slash);
      rest = str_tail(rest, 3);
    } else {
      break;
    }
  }
  struct Buf resolved = {.arena = arena};
  buf_printf(&resolved, "%.*s/%.*s", STR_FMT(base), STR_FMT(rest));
  *url_out = buf_str(&resolved);
  return true;
}

static bool submodule_link_commit_url_forge(struct Str url,
                                            ptrdiff_t* forge_out,
                                            struct Str* path_out) {
  struct Str host = {0};
  struct Str path = {0};
  if (has_prefix_folded(url, S("https://")) || has_prefix_folded(url, S("ssh://"))) {
    const struct Str rest = str_tail(url, find_byte(url, 0, ':') + 3);
    const ptrdiff_t slash = find_byte(rest, 0, '/');
    host = str_head(rest, slash);
    path = slash < rest.len ? str_tail(rest, slash + 1) : (struct Str){0};
    // An `ssh://` URL names its user before the host, and an `https://` one has lost it.
    const ptrdiff_t at = find_byte(host, 0, '@');
    host = at < host.len ? str_tail(host, at + 1) : host;
  } else if (str_startswith(url, S("git@"))) {
    // git reads `git@<host>:<path>` as `ssh://git@<host>/<path>`.
    const struct Str rest = str_tail(url, 4);
    const ptrdiff_t colon = find_byte(rest, 0, ':');
    if (colon == rest.len || find_byte(str_head(rest, colon), 0, '/') < colon) {
      return false;
    }
    host = str_head(rest, colon);
    path = str_tail(rest, colon + 1);
  } else {
    return false;
  }
  *forge_out = -1;
  for (ptrdiff_t i = 0; i < SUBMODULE_FORGE_COUNT; i++) {
    if (str_eq_folded(host, SUBMODULE_FORGES[i].host)) {
      *forge_out = i;
    }
  }
  while (str_endswith(path, S("/"))) {
    path = str_head(path, path.len - 1);
  }
  if (str_endswith(path, S(".git"))) {
    path = str_head(path, path.len - 4);
  }
  // A repository's path is at least an owner and a name, and no segment of it is empty.
  ptrdiff_t segment_count = 1;
  bool has_empty_segment = path.len == 0 || path.data[0] == '/' || path.data[path.len - 1] == '/';
  for (ptrdiff_t i = 0; i < path.len; i++) {
    if (path.data[i] == '/') {
      segment_count++;
      has_empty_segment = has_empty_segment || (i > 0 && path.data[i - 1] == '/');
    }
  }
  *path_out = path;
  return *forge_out >= 0 && segment_count >= 2 && !has_empty_segment;
}

static bool has_prefix_folded(struct Str text, struct Str prefix) {
  return text.len >= prefix.len && str_eq_folded(str_head(text, prefix.len), prefix);
}

static ptrdiff_t find_byte(struct Str text, ptrdiff_t start, char byte) {
  ptrdiff_t i = start;
  while (i < text.len && text.data[i] != byte) {
    i++;
  }
  return i;
}
