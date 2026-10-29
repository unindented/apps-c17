#include "build/site_writer.h"

#include <stddef.h>

#include "build/page_renderer.h"
#include "build/template_context.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/path.h"
#include "core/str.h"
#include "domain/album.h"
#include "domain/site_config.h"
#include "os/oom.h"
#include "os/os.h"
#include "output/output_path.h"
#include "output/output_writer.h"

// This module's two exported writers are unit-tested in `src/build/test_site_writer.c`, and CTest's
// golden test suite exercises them end to end by building every fixture gallery and comparing each
// result with its `tests/expected/<gallery>` tree. Collision checks live in `output_manifest`,
// unit-tested in `src/build/test_output_manifest.c`.

/** What `write_rendered_template_make` renders one aggregate from. */
struct AggregateRender {
  /** Configuration the template renders against. */
  const struct SiteConfig* site_config;

  /** Templates the aggregate renders from. */
  const struct TemplateSet* template_set;

  /** Root album, its `path_to_root` rebased onto the aggregate's output depth. */
  const struct Album* album;

  /** Template name, which is also the aggregate's path below `output_dir`. */
  struct Str template_name;
};

/**
 * @brief Hands the writer an album page the page phase rendered, as `OutputWriterMakeFn`.
 *
 * @param perm      Unused, because the page is in the perm of the worker that rendered it.
 * @param scratch   Unused.
 * @param bytes_out Receives the page. Must not be `NULL`.
 * @param log       Unused.
 * @param userdata  The page's `struct Str`. Must not be `NULL`.
 * @return `0`.
 */
static int site_writer_write_album_pages_make(struct Arena* perm,
                                              struct Arena scratch,
                                              struct Str* bytes_out,
                                              struct Log* log,
                                              void* userdata) __attribute__((nonnull(3, 5)));

/**
 * @brief Renders one configured template against the root album and produces it at its matching
 *        output path, which an unchanged file keeps.
 *
 * @param writer        Writer the output is written through. Must not be `NULL`.
 * @param cache         Cache of the previous build. Must not be `NULL`.
 * @param site_config   Configuration supplying `output_dir`. Must not be `NULL`.
 * @param template_set  Templates the output renders from. Must not be `NULL`.
 * @param root_album    Root album whose data the template renders against. Must not be `NULL`.
 * @param template_name Safe relative template name to render. `output_manifest_populate` already
 *                      checked it against the output-path limits, because every call site runs the
 *                      manifest phase over the same configured list first. This function does not
 *                      re-check, so an overlong name reaching here fails as an opaque write error
 *                      from the OS rather than with a diagnostic naming the limit.
 * @param output        Arena that holds the rendered output until it is written.
 * @param scratch       Arena for the render's temporaries, the URL context and the output path.
 * @param log           Log that receives the first failure. Its arena must be none of the others.
 *                      Must not be `NULL`.
 * @return `0` on success, or `-1` on a render or write failure.
 */
static int write_rendered_template(const struct OutputWriter* writer,
                                   const struct OutputCache* cache,
                                   const struct SiteConfig* site_config,
                                   const struct TemplateSet* template_set,
                                   const struct Album* root_album,
                                   struct Str template_name,
                                   struct Arena output,
                                   struct Arena scratch,
                                   struct Log* log) __attribute__((nonnull(1, 2, 3, 4, 5, 9)));

/**
 * @brief Renders one aggregate, as `OutputWriterMakeFn`.
 *
 * @param perm      Arena that holds the rendered output until it is written. Must not be `NULL`.
 * @param scratch   Arena for the render's temporaries.
 * @param bytes_out Receives the rendered output. Must not be `NULL`.
 * @param log       Log that receives the render failure. Must not be `NULL`.
 * @param userdata  The `struct AggregateRender`. Must not be `NULL`.
 * @return `0` on success, or `-1` when the template fails to render.
 */
static int write_rendered_template_make(struct Arena* perm,
                                        struct Arena scratch,
                                        struct Str* bytes_out,
                                        struct Log* log,
                                        void* userdata) __attribute__((nonnull(1, 3, 4, 5)));

int site_writer_write_album_pages(const struct OutputWriter* writer,
                                  const struct OutputCache* cache,
                                  const struct Album* const* albums,
                                  ptrdiff_t album_count,
                                  const struct RenderedPage* rendered_pages,
                                  struct Arena scratch,
                                  struct Log* log) {
  for (ptrdiff_t i = 0; i < album_count; i++) {
    oom_set_job(albums[i]->output_path, NULL);
    // The operation names the album, and the write names the output path or the directory it could
    // not create, so a failure reads `failed to write page for '<album>': <cause> ('<path>')`. Each
    // page's operation lives in a copy of `scratch`, which the next page reuses.
    struct Arena page_scratch = scratch;
    struct Buf operation = {.arena = &page_scratch};
    if (album_is_root(albums[i])) {
      buf_printf(&operation, "failed to write page for the root album");
    } else {
      buf_printf(&operation, "failed to write page for '%.*s'", STR_FMT(albums[i]->source_dir));
    }
    // The page is made already, so the make allocates nothing in the output arena it is given.
    struct Str html = rendered_pages[i].html;
    if (output_writer_produce(writer, albums[i]->output_path, cache,
                              site_writer_write_album_pages_make, &html, (struct Arena){0},
                              page_scratch, buf_str(&operation), log) != 0) {
      return -1;
    }
  }
  return 0;
}

static int site_writer_write_album_pages_make(struct Arena* perm,
                                              struct Arena scratch,
                                              struct Str* bytes_out,
                                              struct Log* log,
                                              void* userdata) {
  (void)perm;
  (void)scratch;
  (void)log;
  *bytes_out = *(const struct Str*)userdata;
  return 0;
}

int site_writer_write_aggregates(const struct OutputWriter* writer,
                                 const struct OutputCache* cache,
                                 const struct SiteConfig* site_config,
                                 const struct TemplateSet* template_set,
                                 const struct Album* root_album,
                                 struct Arena output,
                                 struct Arena scratch,
                                 struct Log* log) {
  for (ptrdiff_t i = 0; i < site_config->aggregate_template_count; i++) {
    if (write_rendered_template(writer, cache, site_config, template_set, root_album,
                                site_config->aggregate_templates[i], output, scratch, log) != 0) {
      return -1;
    }
  }
  return 0;
}

static int write_rendered_template(const struct OutputWriter* writer,
                                   const struct OutputCache* cache,
                                   const struct SiteConfig* site_config,
                                   const struct TemplateSet* template_set,
                                   const struct Album* root_album,
                                   struct Str template_name,
                                   struct Arena output,
                                   struct Arena scratch,
                                   struct Log* log) {
  oom_set_job(template_name, NULL);
  // An aggregate renders against the root album, whose own `path_to_root` is empty, so a nested
  // aggregate such as `feeds/map.xml` needs its own prefix for page-relative URLs to resolve.
  struct Album aggregate_album = *root_album;
  aggregate_album.path_to_root = output_path_to_root(template_name, &scratch);
  struct AggregateRender render = {
      .site_config = site_config,
      .template_set = template_set,
      .album = &aggregate_album,
      .template_name = template_name,
  };
  const struct Str output_path = path_join(site_config->output_dir, template_name, &scratch);
  return output_writer_produce(writer, output_path, cache, write_rendered_template_make, &render,
                               output, scratch, S("failed to write page"), log);
}

static int write_rendered_template_make(struct Arena* perm,
                                        struct Arena scratch,
                                        struct Str* bytes_out,
                                        struct Log* log,
                                        void* userdata) {
  const struct AggregateRender* render = userdata;
  const struct TemplateContext context = {
      .site_config = render->site_config,
      .album_current = render->album,
  };
  return template_context_render(render->template_set, render->template_name, &context,
                                 render->template_name, perm, scratch, bytes_out, log);
}
