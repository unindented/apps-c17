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

// This module's exported writers are unit-tested in `src/build/test_site_writer.c`, and CTest's
// golden test suite exercises them end to end by building every fixture gallery and comparing each
// result with its `tests/expected/<gallery>` tree. Collision checks live in `output_manifest`,
// unit-tested in `src/build/test_output_manifest.c`.

/** What `write_rendered_template_make` renders one aggregate or the not-found page from. */
struct TemplateRender {
  /** Templates the output renders from. */
  const struct TemplateSet* template_set;

  /** Template name. */
  struct Str template_name;

  /** Output path below `output_dir`, which a failure names. */
  struct Str output_name;

  /** Template context of the render, with the `path_to_root` of the output. */
  struct TemplateContext context;
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
 * @brief Renders one configured template and produces it at its output path, which an unchanged
 *        file keeps.
 *
 * @param writer        Writer the output is written through. Must not be `NULL`.
 * @param cache         Cache of the previous build. Must not be `NULL`.
 * @param output_dir    Output directory the output name is rooted under.
 * @param template_set  Templates the output renders from. Must not be `NULL`.
 * @param template_name Safe relative template name to render.
 * @param output_name   Output path below `output_dir`: the template name of an aggregate, or
 *                      `not_found_path`. `output_manifest_populate` already checked it against the
 *                      output-path limits, because every call site runs the manifest phase over the
 *                      same configuration first. This function does not re-check, so an overlong
 *                      name reaching here fails as an opaque write error from the OS rather than
 *                      with a diagnostic naming the limit.
 * @param context       Template context for this render, with its `path_to_root` set. Must not be
 *                      `NULL`.
 * @param output        Arena that holds the rendered output until it is written.
 * @param scratch       Arena for the render's temporaries and the output path.
 * @param log           Log that receives the first failure. Its arena must be none of the others.
 *                      Must not be `NULL`.
 * @return `0` on success, or `-1` on a render or write failure.
 */
static int write_rendered_template(const struct OutputWriter* writer,
                                   const struct OutputCache* cache,
                                   struct Str output_dir,
                                   const struct TemplateSet* template_set,
                                   struct Str template_name,
                                   struct Str output_name,
                                   const struct TemplateContext* context,
                                   struct Arena output,
                                   struct Arena scratch,
                                   struct Log* log) __attribute__((nonnull(1, 2, 4, 7, 10)));

/**
 * @brief Renders one aggregate or the not-found page, as `OutputWriterMakeFn`.
 *
 * @param perm      Arena that holds the rendered output until it is written. Must not be `NULL`.
 * @param scratch   Arena for the render's temporaries.
 * @param bytes_out Receives the rendered output. Must not be `NULL`.
 * @param log       Log that receives the render failure. Must not be `NULL`.
 * @param userdata  The `struct TemplateRender`. Must not be `NULL`.
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
  for (ptrdiff_t i = 0; i < site_config->templates.aggregate_template_count; i++) {
    // An aggregate renders against the root album, whose own `path_to_root` is empty, so a nested
    // aggregate such as `feeds/map.xml` needs its own prefix for page-relative URLs to resolve.
    // Each render works in its own copy of the scratch, so one prefix is dead before the next.
    const struct Str template_name = site_config->templates.aggregates[i];
    struct Arena template_scratch = scratch;
    const struct TemplateContext context = {
        .site_config = site_config,
        .album_current = root_album,
        .path_to_root = output_path_to_root(template_name, &template_scratch),
    };
    if (write_rendered_template(writer, cache, site_config->output_dir, template_set, template_name,
                                template_name, &context, output, template_scratch, log) != 0) {
      return -1;
    }
  }
  return 0;
}

int site_writer_write_not_found(const struct OutputWriter* writer,
                                const struct OutputCache* cache,
                                const struct SiteConfig* site_config,
                                const struct TemplateSet* template_set,
                                struct Arena output,
                                struct Arena scratch,
                                struct Log* log) {
  const struct TemplateContext context = {
      .site_config = site_config,
      .path_to_root = output_path_not_found_root(site_config->base_url, &scratch),
  };
  return write_rendered_template(writer, cache, site_config->output_dir, template_set,
                                 site_config->templates.not_found, site_config->not_found_path,
                                 &context, output, scratch, log);
}

static int write_rendered_template(const struct OutputWriter* writer,
                                   const struct OutputCache* cache,
                                   struct Str output_dir,
                                   const struct TemplateSet* template_set,
                                   struct Str template_name,
                                   struct Str output_name,
                                   const struct TemplateContext* context,
                                   struct Arena output,
                                   struct Arena scratch,
                                   struct Log* log) {
  oom_set_job(output_name, NULL);
  struct TemplateRender render = {
      .template_set = template_set,
      .template_name = template_name,
      .output_name = output_name,
      .context = *context,
  };
  const struct Str output_path = path_join(output_dir, output_name, &scratch);
  return output_writer_produce(writer, output_path, cache, write_rendered_template_make, &render,
                               output, scratch, S("failed to write page"), log);
}

static int write_rendered_template_make(struct Arena* perm,
                                        struct Arena scratch,
                                        struct Str* bytes_out,
                                        struct Log* log,
                                        void* userdata) {
  const struct TemplateRender* render = userdata;
  return template_context_render(render->template_set, render->template_name, &render->context,
                                 render->output_name, perm, scratch, bytes_out, log);
}
