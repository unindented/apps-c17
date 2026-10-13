#ifndef APPS_FORMATS_HTML_H
#define APPS_FORMATS_HTML_H

#include "core/str.h"

struct Buf;

/**
 * @brief Appends `text` to `buffer`, HTML-escaped.
 *
 * Replaces `&`, `<`, `>`, `"`, and `'` with their character references and copies every other byte
 * verbatim.
 *
 * This is the escaping boundary of the apps against markup injection. Every `{{ ... }}`
 * interpolation in a template reaches it through `out_escaped`, including untrusted text that an
 * app reads from its inputs, such as metadata and file names. The five bytes are sufficient for the
 * two contexts that templates use:
 * - element text
 * - a quoted attribute value in HTML or XML, with either quote style
 *
 * They are not sufficient anywhere else. Do not place the result in:
 * - an unquoted attribute value (whitespace, `=` and a backtick pass through, so the value can end
 *   early and inject an attribute)
 * - a raw-text element such as `<script>` or `<style>` (character references are not decoded there,
 *   so escaping corrupts the text instead of protecting it)
 * - a URL-valued attribute such as `href` (a `javascript:` scheme survives escaping untouched)
 *
 * Escaping is also not XML validation. A control byte that XML 1.0 forbids passes through verbatim.
 *
 * @param buffer Destination buffer to append to. Must not be `NULL`.
 * @param text   Source bytes to escape and append.
 */
void html_escape_append(struct Buf* buffer, struct Str text) __attribute__((nonnull(1)));

#endif
