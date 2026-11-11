#ifndef SOSIG_TEMPLATE_CONTEXT_H
#define SOSIG_TEMPLATE_CONTEXT_H

#include <stdbool.h>
#include <stddef.h>

#include "core/arena.h"
#include "core/str.h"

struct ContentEntry;
struct Log;
struct SiteConfig;
struct TemplateSet;

/**
 * Borrowed data visible to templates during one render call. Every pointer is `const`, and that is
 * a thread-safety invariant. During the page phase every worker holds pointers to every entry, and
 * the `const` stops one job writing an entry another job is reading. Do not relax it to add a
 * mutable field.
 */
struct TemplateContext {
  /** Site configuration available to site-level template variables. Must not be `NULL`. */
  const struct SiteConfig* site_config;

  /** Content entries available to `{{#content_entries}}` section blocks. */
  const struct ContentEntry* const* content_entries;

  /** Number of entries in `content_entries`. */
  ptrdiff_t content_entry_count;

  /**
   * Current entry for content templates, or `NULL` for aggregate, feed and not-found templates.
   */
  const struct ContentEntry* content_entry_current;

  /**
   * Prefix from the page being rendered to `output_dir`, exposed as `path_to_root`: one `../` for
   * each directory the page's output path is below, and empty for a page at the top of
   * `output_dir`. The not-found page, which a host serves at every depth, has the root-relative
   * path of `base_url` instead.
   */
  struct Str path_to_root;

  /**
   * Whether the render is of a feed template, whose links are absolute below `base_url`. A link in
   * any other render is relative to the page, behind `path_to_root`.
   */
  bool is_feed;
};

/**
 * @brief Renders the page template `template_name` of `set` with `context`.
 *
 * The render is `template_set_render`'s, under `TEMPLATE_RENDER_OUTPUT_LEN_MAX`, and resolves its
 * names lazily against `context`: `site.*`, `feed.*` in a feed, `content_entries`, `generator`,
 * `path_to_root`, and the fields of the current entry and of each listed one. Only `body_html` is
 * HTML, which a triple mustache writes as it is.
 *
 * @param set           Loaded templates. Must not be `NULL`.
 * @param template_name Name of a page template of `set`.
 * @param context       Borrowed data visible to the template during this render. Must not be
 *                      `NULL`.
 * @param output_name   Output the render is for, relative to `output_dir`, which a failure names,
 *                      and which a feed's `feed.url` gives below `base_url`.
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
