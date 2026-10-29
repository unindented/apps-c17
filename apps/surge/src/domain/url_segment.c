#include "domain/url_segment.h"

#include <stdbool.h>
#include <stddef.h>

#include "core/ascii.h"
#include "core/buf.h"
#include "core/str.h"

/**
 * @brief Reports whether `c` is in RFC 3986's unreserved set.
 *
 * @param c Byte to check.
 * @return `true` for `A-Z`, `a-z`, `0-9`, `-`, `.`, `_` and `~`.
 */
static bool is_unreserved(unsigned char c);

void url_segment_append(struct Buf* buffer, struct Str segment) {
  static const char HEX_DIGITS[] = "0123456789ABCDEF";
  for (ptrdiff_t i = 0; i < segment.len; i++) {
    const unsigned char c = (unsigned char)segment.data[i];
    if (is_unreserved(c)) {
      buf_append_char(buffer, (char)c);
    } else {
      buf_append_char(buffer, '%');
      buf_append_char(buffer, HEX_DIGITS[c >> 4]);
      buf_append_char(buffer, HEX_DIGITS[c & 0xF]);
    }
  }
}

void url_segment_append_path(struct Buf* buffer, struct Str path) {
  struct StrCut cut = {.tail = path, .is_found = true};
  bool is_first = true;
  while (cut.is_found) {
    cut = str_cut(cut.tail, '/');
    if (!is_first) {
      buf_append_char(buffer, '/');
    }
    url_segment_append(buffer, cut.head);
    is_first = false;
  }
}

static bool is_unreserved(unsigned char c) {
  return ascii_is_alphanumeric(c) || c == '-' || c == '.' || c == '_' || c == '~';
}
