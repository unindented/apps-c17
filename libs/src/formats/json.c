#include "formats/json.h"

#include <stddef.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/str.h"
#include "core/utf8.h"

void json_string_append(struct Buf* buffer, struct Str text, struct Arena scratch) {
  const struct Str repaired = utf8_repair_for_xml(text, &scratch);
  buf_append_char(buffer, '"');
  for (ptrdiff_t i = 0; i < repaired.len; i++) {
    const char c = repaired.data[i];
    switch (c) {
      case '"':
        buf_append(buffer, S("\\\""));
        break;
      case '\\':
        buf_append(buffer, S("\\\\"));
        break;
      case '\t':
        buf_append(buffer, S("\\t"));
        break;
      case '\n':
        buf_append(buffer, S("\\n"));
        break;
      case '\r':
        buf_append(buffer, S("\\r"));
        break;
      default:
        buf_append_char(buffer, c);
        break;
    }
  }
  buf_append_char(buffer, '"');
}
