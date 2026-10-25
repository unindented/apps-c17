#ifndef APPS_CORE_UTF8_H
#define APPS_CORE_UTF8_H

#include <stdbool.h>

#include "core/str.h"

struct Arena;

/**
 * @brief Reports whether `text` is valid UTF-8.
 *
 * An overlong form, a surrogate, a code point past U+10FFFF and a truncated sequence are invalid.
 *
 * @param text Bytes to check.
 * @return `true` when every byte belongs to a valid sequence, or `false` otherwise.
 */
bool utf8_is_valid(struct Str text);

/**
 * @brief Returns `text` as valid UTF-8 that XML 1.0 accepts.
 *
 * Each maximal invalid subpart of a sequence becomes one U+FFFD, as the Unicode standard
 * recommends. The C0 control characters but tab, line feed and carriage return, and the
 * noncharacters U+FFFE and U+FFFF, which XML 1.0 forbids, are dropped. HTML treats each of them but
 * form feed as a parse error, so an app that renders templates repairs every render with this, a
 * page as well as a feed, and a page loses its form feeds too. Text that needs no repair comes back
 * as it is, without a copy.
 *
 * @param text  Bytes to repair.
 * @param arena Arena that owns the repaired copy, when one is needed. Must not be `NULL`.
 * @return `text` itself when it needs no repair, or a repaired copy in `arena`.
 */
struct Str utf8_repair_for_xml(struct Str text, struct Arena* arena) __attribute__((nonnull(2)));

#endif
