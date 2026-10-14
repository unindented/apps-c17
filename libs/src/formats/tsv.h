#ifndef APPS_FORMATS_TSV_H
#define APPS_FORMATS_TSV_H

#include "core/str.h"

struct Buf;

/**
 * @brief Appends `text` to `buffer` as a field of a tab-separated line, with each `\`, tab and
 *        newline written as `\\`, `\t` and `\n`.
 *
 * The field then holds no tab and no newline, so it cannot split its line or its row, and two
 * different texts never append the same bytes. Every other byte is copied verbatim.
 *
 * @param buffer Destination buffer to append to. Must not be `NULL`.
 * @param text   Source bytes to escape and append.
 */
void tsv_escape_append(struct Buf* buffer, struct Str text) __attribute__((nonnull(1)));

#endif
