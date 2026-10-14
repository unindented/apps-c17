#include "formats/tsv.h"

#include <stddef.h>

#include "core/buf.h"
#include "core/str.h"

void tsv_escape_append(struct Buf* buffer, struct Str text) {
  for (ptrdiff_t i = 0; i < text.len; i++) {
    const char c = text.data[i];
    if (c == '\\') {
      buf_append(buffer, S("\\\\"));
    } else if (c == '\t') {
      buf_append(buffer, S("\\t"));
    } else if (c == '\n') {
      buf_append(buffer, S("\\n"));
    } else {
      buf_append_char(buffer, c);
    }
  }
}
