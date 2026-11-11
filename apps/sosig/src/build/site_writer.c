#include "build/site_writer.h"

#include <stddef.h>

#include "build/template_context.h"
#include "core/arena.h"
#include "core/path.h"
#include "core/str.h"
#include "domain/content_entry.h"
#include "domain/site_config.h"
#include "os/oom.h"
#include "output/output_path.h"
#include "output/output_writer.h"

// This module's writers are unit-tested in `src/build/test_site_writer.c`, and CTest's golden test
// suite exercises them end to end by building every fixture site and comparing each result with its
// `tests/expected/<site>` tree. Collision checks live in `output_manifest`, unit-tested in
// `src/build/test_output_manifest.c`.

/**
 * One configured template's render, which `write_rendered_template_make` receives as its userdata.
 */
struct TemplateRender {
  /** Templates the output renders from. */
  const struct TemplateSet* template_set;

  /** Safe relative template name. */
  struct Str template_name;

  /** Output path below `output_dir`, which a failure names. */
  struct Str output_name;

  /** Template context of the render, with the `path_to_root` of the output. */
  struct TemplateContext context;
};

/**
 * @brief Renders one configured template and hands it to `output_writer_produce` for its output
 *        path.
 *
 * @param writer        Writer the output is written or kept through. Must not be `NULL`.
 * @param cache         Output cache of the previous build. Must not be `NULL`.
 * @param template_set  Templates the output renders from. Must not be `NULL`.
 * @param output_dir    Output directory the output name is rooted under.
 * @param template_name Safe relative template name to render.
 * @param output_name   Output path below `output_dir`: the template name of an aggregate or a feed,
 *                      or `not_found_path`. `output_manifest_populate` already checked it against
 *                      the output-path limits, because every call site runs the manifest phase over
 *                      the same configuration first. This function does not re-check, so an
 *                      overlong name reaching here fails as an opaque write error from the OS
 *                      rather than with a diagnostic naming the limit.
 * @param context       Template context for this render, with its `path_to_root` set. Must not be
 *                      `NULL`.
 * @param output        Arena that holds the rendered output until it is written.
 * @param scratch       Arena for the render's temporaries, the output path, and the read of the
 *                      existing output.
 * @param log           Log that receives the first failure. Its arena must be none of the others.
 *                      Must not be `NULL`.
 * @return `0` on success, or `-1` on a render or write failure.
 */
static int write_rendered_template(const struct OutputWriter* writer,
                                   const struct OutputCache* cache,
                                   const struct TemplateSet* template_set,
                                   struct Str output_dir,
                                   struct Str template_name,
                                   struct Str output_name,
                                   const struct TemplateContext* context,
                                   struct Arena output,
                                   struct Arena scratch,
                                   struct Log* log) __attribute__((nonnull(1, 2, 3, 7, 10)));

/**
 * @brief Renders one configured template, as `OutputWriterMakeFn`.
 *
 * @param perm      Arena that owns the output. Must not be `NULL`.
 * @param scratch   Arena for the render's temporaries.
 * @param bytes_out Receives the output owned by `perm` on success. Must not be `NULL`.
 * @param log       Log that receives the failure. Must not be `NULL`.
 * @param userdata  The `struct TemplateRender`.
 * @return `0` on success, or `-1` on render failure.
 */
static int write_rendered_template_make(struct Arena* perm,
                                        struct Arena scratch,
                                        struct Str* bytes_out,
                                        struct Log* log,
                                        void* userdata) __attribute__((nonnull(1, 3, 4, 5)));

int site_writer_write_templates(const struct OutputWriter* writer,
                                const struct OutputCache* cache,
                                const struct SiteConfig* site_config,
                                const struct TemplateSet* template_set,
                                const struct Str* template_names,
                                ptrdiff_t template_count,
                                const struct ContentEntry* const* content_entries,
                                ptrdiff_t content_entry_count,
                                bool is_feed,
                                struct Arena output,
                                struct Arena scratch,
                                struct Log* log) {
  for (ptrdiff_t i = 0; i < template_count; i++) {
    // The template name is also the output path below `output_dir`, so a name with a `/`, such as
    // `feeds/atom.xml`, renders a page that is below the top of `output_dir`. Each render works in
    // its own copy of the scratch, so one prefix is dead before the next.
    struct Arena template_scratch = scratch;
    const struct TemplateContext context = {
        .site_config = site_config,
        .content_entry_current = NULL,
        .content_entries = content_entries,
        .content_entry_count = content_entry_count,
        .path_to_root = output_path_to_root(template_names[i], &template_scratch),
        .is_feed = is_feed,
    };
    if (write_rendered_template(writer, cache, template_set, site_config->output_dir,
                                template_names[i], template_names[i], &context, output,
                                template_scratch, log) != 0) {
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
  return write_rendered_template(writer, cache, template_set, site_config->output_dir,
                                 site_config->templates.not_found, site_config->not_found_path,
                                 &context, output, scratch, log);
}

static int write_rendered_template(const struct OutputWriter* writer,
                                   const struct OutputCache* cache,
                                   const struct TemplateSet* template_set,
                                   struct Str output_dir,
                                   struct Str template_name,
                                   struct Str output_name,
                                   const struct TemplateContext* context,
                                   struct Arena output,
                                   struct Arena scratch,
                                   struct Log* log) {
  oom_set_job(output_name, NULL);
  const struct Str output_path = path_join(output_dir, output_name, &scratch);
  struct TemplateRender render = {
      .template_set = template_set,
      .template_name = template_name,
      .output_name = output_name,
      .context = *context,
  };
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
