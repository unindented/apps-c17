#ifndef APPS_CORE_URL_H
#define APPS_CORE_URL_H

#include <stdbool.h>

#include "core/str.h"

/**
 * @brief Reports whether a URL starts with `http://` or `https://` and then a host.
 *
 * The scheme is matched in any letter case, because schemes are case-insensitive. The host must be
 * present, and nothing more is judged about it, because ruling beyond "not empty" would mean ruling
 * on what DNS and punycode allow. A host is empty when the path, query, fragment or port starts
 * right after the scheme, as in `https:///path`, `https://?q`, `https://#f` or `https://:8080`.
 *
 * @param url Bytes to inspect.
 * @return `true` when the URL has an HTTP(S) scheme and a non-empty host, or `false` otherwise.
 */
bool url_is_http_with_host(struct Str url);

/**
 * @brief Reports whether a URL holds a byte that breaks a path joined after it.
 *
 * A link that appends a path to a base URL needs the base to end in its path. A `?` or a `#` starts
 * a query or a fragment that would swallow the appended path, and a space or a control byte is not
 * valid anywhere in a URL. The bytes are `?`, `#`, every byte up to and including the space, and
 * 0x7F. A byte above 0x7F passes, so a base URL can name a host or a path in UTF-8.
 *
 * @param url Bytes to inspect.
 * @return `true` when any byte of the URL is one of these, or `false` otherwise.
 */
bool url_has_path_breaking_byte(struct Str url);

#endif
