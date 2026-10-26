#ifndef FRAM_TEMPLATE_CONTEXT_H
#define FRAM_TEMPLATE_CONTEXT_H

#include <stdbool.h>
#include <stddef.h>

#include "core/arena.h"
#include "core/str.h"

struct Album;
struct SiteConfig;
struct Log;
struct TemplateSet;

/**
 * Borrowed data visible to templates during one render call. Every pointer is `const`, and that is
 * a thread-safety invariant. During the page phase every worker reaches other albums through
 * `sub_albums` and `parent`, and the `const` stops one job writing an album another job is reading.
 * Do not relax it to add a mutable field.
 */
struct TemplateContext {
  /** Gallery metadata and configured paths. Must not be `NULL`. */
  const struct SiteConfig* site_config;

  /** Album whose page-relative URL prefix applies to this render. Must not be `NULL`. */
  const struct Album* album_current;

  /**
   * Whether this render is `album_current`'s own page, whose last breadcrumb is the album itself,
   * rather than an aggregate rendered against the root album.
   */
  bool is_album_page;
};

/**
 * @brief Renders the page template `template_name` of `set` with `context`.
 *
 * The render is `template_set_render`'s, under `TEMPLATE_RENDER_OUTPUT_LEN_MAX`, and resolves its
 * names lazily against `context`: `site.*`, `album`, `sub_albums`, `media`, `breadcrumbs`,
 * `generator`, `path_to_root`, and the fields of the current album and of each listed album, media
 * item and derivative. fram makes no HTML, so an unescaped tag of any value fails the render.
 *
 * @param set           Loaded templates. Must not be `NULL`.
 * @param template_name Name of a page template of `set`.
 * @param context       Borrowed data visible to the template during this render. Must not be
 *                      `NULL`.
 * @param output_name   Output the render is for, relative to `output_dir`, which a failure names.
 * @param perm          Arena that owns the returned HTML. Nothing else in the render allocates
 *                      there, so the output grows in place. A failed render may leave its partial
 *                      output there, so a caller that must not keep it passes a copy. Must not be
 *                      `NULL`.
 * @param scratch       Arena for the render's temporaries, released when it returns. Must not
 *                      overlap `perm`.
 * @param html_out      Receives the HTML, owned by `perm`, on success. Must not be `NULL`.
 * @param log           Log that receives the render's first failure. The log's arena must not
 *                      overlap `perm` or `scratch`. Must not be `NULL`.
 * @return `0` on success, or `-1` on failure.
 */
int template_context_render(const struct TemplateSet* set,
                            struct Str template_name,
                            const struct TemplateContext* context,
                            struct Str output_name,
                            struct Arena* perm,
                            struct Arena scratch,
                            struct Str* html_out,
                            struct Log* log) __attribute__((nonnull(1, 3, 5, 7, 8)));

#endif
