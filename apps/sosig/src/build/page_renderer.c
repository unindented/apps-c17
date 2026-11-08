#include "build/page_renderer.h"

#include <stdbool.h>
#include <stddef.h>

#include "build/template_context.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/str.h"
#include "domain/content_entry.h"
#include "domain/site_config.h"
#include "os/oom.h"
#include "os/os.h"
#include "output/output_path.h"
#include "output/output_writer.h"
#include "runtime/job.h"
#include "templates/template_set.h"

/**
 * Worker context shared by all content entry page render jobs. Every field is read-only to a job,
 * apart from the mark each job leaves on its own path in the writer's manifest.
 */
struct PageRenderContext {
  /**
   * Template context every page render starts from, fully populated here so a job varies only
   * `content_entry_current` and `path_to_root`. That every pointer in `TemplateContext` is `const`
   * is a thread-safety invariant. During this phase every worker holds pointers to every entry, and
   * the `const` keeps any job from writing what another job reads. Do not relax it to add a mutable
   * field.
   */
  struct TemplateContext base_context;

  /** Templates every job renders from, read by every worker at once. */
  const struct TemplateSet* template_set;

  /** Writer every job writes its page through. */
  const struct OutputWriter* writer;

  /** Output cache of the previous build, which every job's produce reads. */
  const struct OutputCache* cache;

  /** One entry slot per source path, or `NULL` where no entry was parsed. No entry is written. */
  const struct ContentEntry* const* source_entries;
};

/** One job's page, which `render_content_page_make` receives as its userdata. */
struct PageRender {
  /** Templates the page renders from. */
  const struct TemplateSet* template_set;

  /** Template context of the page, whose `content_entry_current` is the entry to render. */
  struct TemplateContext context;

  /** The rendered page, once the make has run, or empty. */
  struct Str html;

  /** The arena the page was rendered into, as the render left it, past the page. */
  struct Arena perm;
};

/**
 * @brief Renders one parsed content entry's content template and writes its page.
 *
 * This is the `JobFn` of the page phase, so it runs concurrently with other indexes and writes only
 * its own page and its own log. The HTML goes into a copy of the worker's `perm`, past the entries
 * it holds, and is dropped once `output_writer_produce` has kept or written it. A slot left `NULL`
 * by a draft or by a source that was never parsed is skipped.
 *
 * @param index    Entry slot index this job handles. Must be below the entry slot count.
 * @param arenas   Arenas of the worker running the job. Must not be `NULL`.
 * @param log      Log of the job, which receives the failure. Must not be `NULL`.
 * @param userdata `struct PageRenderContext*` shared by every worker. Must not be `NULL`.
 * @return `0` on success, or a skipped slot, or `-1` after recording the failure.
 */
static int render_content_page_job(ptrdiff_t index,
                                   struct JobArenas* arenas,
                                   struct Log* log,
                                   void* userdata) __attribute__((nonnull(2, 3, 4)));

/**
 * @brief Renders one content entry through its configured content template, as
 *        `OutputWriterMakeFn`.
 *
 * @param perm      Arena that owns the HTML. Must not be `NULL`.
 * @param scratch   Arena for the render's temporaries.
 * @param bytes_out Receives the HTML owned by `perm` on success. Must not be `NULL`.
 * @param log       Log of the job, which receives the failure. Must not be `NULL`.
 * @param userdata  The job's `struct PageRender`, which receives the HTML and `perm` past it.
 * @return `0` on success, or `-1` on render failure.
 */
static int render_content_page_make(struct Arena* perm,
                                    struct Arena scratch,
                                    struct Str* bytes_out,
                                    struct Log* log,
                                    void* userdata) __attribute__((nonnull(1, 3, 4, 5)));

int page_renderer_render_pages(const struct OutputWriter* writer,
                               const struct OutputCache* cache,
                               const struct SiteConfig* site_config,
                               const struct TemplateSet* template_set,
                               const struct ContentEntry* const* source_entries,
                               ptrdiff_t source_entry_count,
                               const struct ContentEntry* const* content_entries,
                               ptrdiff_t content_entry_count,
                               ptrdiff_t worker_count,
                               struct JobArenas* worker_arenas,
                               bool is_verbose,
                               struct Os* os,
                               struct Arena scratch,
                               struct Log* log) {
  struct PageRenderContext context = {
      .base_context =
          {
              .site_config = site_config,
              .content_entry_current = NULL,
              .content_entries = content_entries,
              .content_entry_count = content_entry_count,
          },
      .template_set = template_set,
      .writer = writer,
      .cache = cache,
      .source_entries = source_entries,
  };
  return job_run(source_entry_count, worker_count, worker_arenas, render_content_page_job, &context,
                 S("rendering content"), S(TEMPLATE_RENDER_ATTRIBUTION), is_verbose, os, scratch,
                 log);
}

static int render_content_page_job(ptrdiff_t index,
                                   struct JobArenas* arenas,
                                   struct Log* log,
                                   void* userdata) {
  const struct PageRenderContext* context = userdata;
  const struct ContentEntry* entry = context->source_entries[index];
  if (entry == NULL) {
    return 0;
  }
  // A worker of the parse phase wrote `entry`. Reading it from a different thread here is not a
  // race. That phase's `pool_run` joined its workers before returning, and this phase's
  // `pthread_create` happens after, so the write is already published to this thread.
  oom_set_job(entry->source_path, log);

  // Copy the shared context before setting `content_entry_current` and `path_to_root`. Every page
  // job reads `base_context` concurrently, so mutating it in place would be a data race, which is
  // undefined behavior. The copy lives on this job's stack, so no other job can observe a change to
  // it.
  struct PageRender render = {.template_set = context->template_set,
                              .context = context->base_context};
  render.context.content_entry_current = entry;
  // The scratch is a copy, so the page's temporaries are dropped when this job returns. The page's
  // path below `output_dir` is its URL without the leading `/`, so the permalink decides how deep
  // the page is.
  struct Arena scratch = arenas->scratch;
  render.context.path_to_root = output_path_to_root(str_tail(entry->url_path, 1), &scratch);
  // The operation names the source, and the write names the output path or the directory it could
  // not create, so a failure reads `failed to write page for '<source>': <cause> ('<path>')`.
  // `os_write` creates missing parent directories, and a sibling job creating the same parent at
  // the same moment is not a failure.
  struct Buf operation = {.arena = &scratch};
  buf_printf(&operation, "failed to write page for '%.*s'", STR_FMT(entry->source_path));
  // The page lands in a copy of the worker's perm, past the entries this worker parsed, which the
  // copy never touches.
  const int rc = output_writer_produce(context->writer, entry->output_path, context->cache,
                                       render_content_page_make, &render, arenas->perm, scratch,
                                       buf_str(&operation), log);
  // The page is kept or written, so its pages are given back rather than kept resident in the
  // worker's region. A large page costs far more to render than to fault in again.
  if (render.html.len > 0) {
    arena_discard(&render.perm, render.html.data, render.html.len);
  }
  return rc;
}

static int render_content_page_make(struct Arena* perm,
                                    struct Arena scratch,
                                    struct Str* bytes_out,
                                    struct Log* log,
                                    void* userdata) {
  struct PageRender* render = userdata;
  const struct ContentEntry* entry = render->context.content_entry_current;
  const struct Str template_name =
      entry->template.len > 0 ? entry->template : render->context.site_config->templates.content;
  // The page's path below `output_dir` is its URL without the leading `/`, which a failure names.
  if (template_context_render(render->template_set, template_name, &render->context,
                              str_tail(entry->url_path, 1), perm, scratch, bytes_out, log) != 0) {
    return -1;
  }
  render->html = *bytes_out;
  render->perm = *perm;
  return 0;
}
