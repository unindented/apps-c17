#include "build/site_writer.h"

#include <stddef.h>

#include "build/page_renderer.h"
#include "build/template_context.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/path.h"
#include "core/path_list.h"
#include "core/str.h"
#include "domain/album.h"
#include "domain/site_config.h"
#include "os/oom.h"
#include "os/os.h"
#include "output/output_path.h"
#include "output/output_writer.h"

// This module's three exported writers are unit-tested in `src/build/test_site_writer.c`, and
// CTest's golden test suite exercises them end to end by building every fixture gallery and
// comparing each result with its `tests/expected/<gallery>` tree. Collision checks live in
// `manifest_builder`, unit-tested in `src/build/test_manifest_builder.c`.

/**
 * @brief Renders one configured template against the root album and writes it to its matching
 *        output path.
 *
 * @param writer        Writer the output is written through. Must not be `NULL`.
 * @param site_config   Configuration supplying `output_dir`. Must not be `NULL`.
 * @param template_set  Templates the output renders from. Must not be `NULL`.
 * @param root_album    Root album whose data the template renders against. Must not be `NULL`.
 * @param template_name Safe relative template name to render. `manifest_builder_populate` already
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
                                   const struct SiteConfig* site_config,
                                   const struct TemplateSet* template_set,
                                   const struct Album* root_album,
                                   struct Str template_name,
                                   struct Arena output,
                                   struct Arena scratch,
                                   struct Log* log) __attribute__((nonnull(1, 2, 3, 4, 8)));

int site_writer_write_album_pages(const struct OutputWriter* writer,
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
    if (output_writer_write(writer, albums[i]->output_path,
                            (struct OsSource){.bytes = rendered_pages[i].html}, buf_str(&operation),
                            log) != 0) {
      return -1;
    }
  }
  return 0;
}

int site_writer_write_aggregates(const struct OutputWriter* writer,
                                 const struct SiteConfig* site_config,
                                 const struct TemplateSet* template_set,
                                 const struct Album* root_album,
                                 struct Arena output,
                                 struct Arena scratch,
                                 struct Log* log) {
  for (ptrdiff_t i = 0; i < site_config->aggregate_template_count; i++) {
    if (write_rendered_template(writer, site_config, template_set, root_album,
                                site_config->aggregate_templates[i], output, scratch, log) != 0) {
      return -1;
    }
  }
  return 0;
}

int site_writer_copy_static_files(const struct OutputWriter* writer,
                                  const struct SiteConfig* site_config,
                                  const struct PathList* static_paths,
                                  struct Str root_dir,
                                  struct Arena scratch,
                                  struct Log* log) {
  for (ptrdiff_t i = 0; i < static_paths->count; i++) {
    oom_set_job(static_paths->items[i], NULL);
    // Each copy works in its own copy of `scratch`, so one file's path is dead before the next.
    struct Arena copy_scratch = scratch;
    struct Str relative = {0};
    (void)path_is_below(static_paths->items[i], root_dir, &relative);
    const struct Str output_path = path_join(site_config->output_dir, relative, &copy_scratch);
    // The copy names the path that failed, the static file or its destination, which mirrors it.
    const struct OsSource source = {.kind = OS_SOURCE_FILE, .file_path = static_paths->items[i]};
    if (output_writer_write(writer, output_path, source, S("failed to copy static file"), log) !=
        0) {
      return -1;
    }
  }
  return 0;
}

static int write_rendered_template(const struct OutputWriter* writer,
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
  const struct TemplateContext context = {
      .site_config = site_config,
      .album_current = &aggregate_album,
  };
  struct Str rendered_html = {0};
  if (template_context_render(template_set, template_name, &context, template_name, &output,
                              scratch, &rendered_html, log) != 0) {
    return -1;
  }
  // The render has returned, so its temporaries in `scratch` are dead and the path can reuse them.
  const struct Str output_path = path_join(site_config->output_dir, template_name, &scratch);
  return output_writer_write(writer, output_path, (struct OsSource){.bytes = rendered_html},
                             S("failed to write page"), log);
}
