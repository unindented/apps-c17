#include "core/utf8.h"

#include <stdbool.h>
#include <stddef.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/str.h"

/** U+FFFD REPLACEMENT CHARACTER in UTF-8. */
#define UTF8_REPLACEMENT S("\xEF\xBF\xBD")

/**
 * @brief Returns the length of the valid UTF-8 sequence at the start of `text`, or `0` when the
 *        first byte does not start one.
 *
 * @param text Bytes to inspect, not empty.
 * @return The sequence's length, `1` to `4`, or `0`.
 */
static ptrdiff_t utf8_sequence_len(struct Str text);

/**
 * @brief Returns the length of the maximal subpart of an invalid sequence at the start of `text`:
 *        the bytes that start a sequence and continue it validly before it breaks.
 *
 * @param text Bytes whose first byte does not start a valid sequence, not empty.
 * @return The length to replace with one U+FFFD, at least `1`.
 */
static ptrdiff_t utf8_invalid_len(struct Str text);

/**
 * @brief Reports whether a valid UTF-8 sequence is a character that XML 1.0 forbids.
 *
 * @param sequence One valid UTF-8 sequence.
 * @return `true` for a C0 control character but tab, line feed and carriage return, and for the
 *         noncharacters U+FFFE and U+FFFF.
 */
static bool is_xml_forbidden(struct Str sequence);

bool utf8_is_valid(struct Str text) {
  ptrdiff_t i = 0;
  while (i < text.len) {
    const ptrdiff_t len = utf8_sequence_len(str_tail(text, i));
    if (len == 0) {
      return false;
    }
    i += len;
  }
  return true;
}

struct Str utf8_repair_for_xml(struct Str text, struct Arena* arena) {
  // The first pass finds whether anything changes, so valid text costs no copy.
  ptrdiff_t i = 0;
  while (i < text.len) {
    const ptrdiff_t len = utf8_sequence_len(str_tail(text, i));
    if (len == 0 || is_xml_forbidden(str_head(str_tail(text, i), len))) {
      break;
    }
    i += len;
  }
  if (i == text.len) {
    return text;
  }
  struct Buf repaired = {.arena = arena};
  buf_append(&repaired, str_head(text, i));
  while (i < text.len) {
    const struct Str rest = str_tail(text, i);
    const ptrdiff_t len = utf8_sequence_len(rest);
    if (len == 0) {
      buf_append(&repaired, UTF8_REPLACEMENT);
      i += utf8_invalid_len(rest);
    } else {
      if (!is_xml_forbidden(str_head(rest, len))) {
        buf_append(&repaired, str_head(rest, len));
      }
      i += len;
    }
  }
  return buf_str(&repaired);
}

static ptrdiff_t utf8_sequence_len(struct Str text) {
  const unsigned char lead = (unsigned char)text.data[0];
  if (lead < 0x80) {
    return 1;
  }
  ptrdiff_t len = 0;
  unsigned char low = 0x80;
  unsigned char high = 0xBF;
  if (lead >= 0xC2 && lead <= 0xDF) {
    len = 2;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    len = 3;
    // The second byte excludes the overlong forms below U+0800 and the surrogates.
    low = lead == 0xE0 ? 0xA0 : 0x80;
    high = lead == 0xED ? 0x9F : 0xBF;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    len = 4;
    // The second byte excludes the overlong forms below U+10000 and code points past U+10FFFF.
    low = lead == 0xF0 ? 0x90 : 0x80;
    high = lead == 0xF4 ? 0x8F : 0xBF;
  } else {
    return 0;
  }
  if (text.len < len) {
    return 0;
  }
  const unsigned char second = (unsigned char)text.data[1];
  if (second < low || second > high) {
    return 0;
  }
  for (ptrdiff_t i = 2; i < len; i++) {
    const unsigned char next = (unsigned char)text.data[i];
    if (next < 0x80 || next > 0xBF) {
      return 0;
    }
  }
  return len;
}

static ptrdiff_t utf8_invalid_len(struct Str text) {
  // A lead byte followed by a valid but truncated run of continuation bytes is one maximal subpart.
  // Every other invalid byte is a subpart of its own.
  for (ptrdiff_t len = 3; len >= 2; len--) {
    if (text.len < len) {
      continue;
    }
    // A prefix of a valid sequence, padded with continuation bytes, would be valid at full length.
    // Testing the prefix against a lead that expects more is enough: pad with the smallest byte the
    // next position takes, which `utf8_sequence_len` then judges.
    char padded[4] = {0};
    for (ptrdiff_t i = 0; i < len; i++) {
      padded[i] = text.data[i];
    }
    for (ptrdiff_t i = len; i < 4; i++) {
      padded[i] = (char)0x80;
    }
    const ptrdiff_t full = utf8_sequence_len((struct Str){padded, 4});
    if (full > len) {
      return len;
    }
  }
  return 1;
}

static bool is_xml_forbidden(struct Str sequence) {
  const unsigned char c = (unsigned char)sequence.data[0];
  if (sequence.len == 1) {
    return c < 0x20 && c != '\t' && c != '\n' && c != '\r';
  }
  // U+FFFE and U+FFFF are `EF BF BE` and `EF BF BF`.
  return sequence.len == 3 && c == 0xEF && (unsigned char)sequence.data[1] == 0xBF &&
         ((unsigned char)sequence.data[2] & 0xFE) == 0xBE;
}
