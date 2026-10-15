#include "core/url.h"

#include <stddef.h>

#include "core/ascii.h"

/**
 * @brief Reports whether a URL starts with a scheme, in any letter case.
 *
 * @param url    Bytes to inspect.
 * @param scheme Lowercase scheme with its `://`.
 * @return `true` when the URL starts with the scheme, or `false` otherwise.
 */
static bool has_scheme(struct Str url, struct Str scheme);

bool url_is_http_with_host(struct Str url) {
  static const struct Str URL_SCHEMES[] = {STR_INIT("http://"), STR_INIT("https://")};

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(URL_SCHEMES) / sizeof(URL_SCHEMES[0])); i++) {
    if (has_scheme(url, URL_SCHEMES[i])) {
      const struct Str host = str_tail(url, URL_SCHEMES[i].len);
      return host.len > 0 && host.data[0] != '/' && host.data[0] != '?' && host.data[0] != '#' &&
             host.data[0] != ':';
    }
  }
  return false;
}

bool url_has_path_breaking_byte(struct Str url) {
  for (ptrdiff_t i = 0; i < url.len; i++) {
    const unsigned char c = (unsigned char)url.data[i];
    if (c <= 0x20 || c == 0x7F || c == '?' || c == '#') {
      return true;
    }
  }
  return false;
}

static bool has_scheme(struct Str url, struct Str scheme) {
  if (url.len < scheme.len) {
    return false;
  }
  for (ptrdiff_t i = 0; i < scheme.len; i++) {
    if (ascii_to_lower((unsigned char)url.data[i]) != (unsigned char)scheme.data[i]) {
      return false;
    }
  }
  return true;
}
