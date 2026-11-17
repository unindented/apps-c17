#ifndef APPS_FORMATS_JSON_H
#define APPS_FORMATS_JSON_H

#include "core/arena.h"
#include "core/str.h"

struct Buf;

/**
 * @brief Appends `text` to `buffer` as a JSON string, between double quotes.
 *
 * The text is repaired first as `utf8_repair_for_xml` repairs it, so each invalid UTF-8 sequence
 * becomes U+FFFD and the control characters that XML 1.0 forbids are dropped, as from every page
 * the apps render. Each `"` and `\` then gets a `\` before it, and each tab, line feed and carriage
 * return is written `\t`, `\n` and `\r`. Every other byte is copied verbatim.
 *
 * @param buffer  Destination buffer to append to. Must not be `NULL`.
 * @param text    Source bytes, in any encoding.
 * @param scratch Arena for the repaired copy, apart from the buffer's.
 */
void json_string_append(struct Buf* buffer, struct Str text, struct Arena scratch)
    __attribute__((nonnull(1)));

#endif
