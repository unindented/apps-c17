#include "templates/template_set.h"

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/error.h"
#include "core/log.h"
#include "core/path.h"
#include "core/str.h"
#include "core/text.h"
#include "core/utf8.h"
#include "formats/html.h"
#include "mustache.h"
#include "os/os.h"

// Bounds on one render. The expansion count stops a partial that includes itself, and the output
// length bounds the output buffer. Both bound a single render, and the renders of a phase run once
// per worker at a time, so several renders' costs can be live at once.
//
// Neither bounds the render's total memory. Templates are trusted input, like the config. The data
// can make a node for every name that it resolves, and a section nested inside another resolves its
// names once per iteration of every enclosing section, without touching the expansion counter.
//
// Termination: mustache4c expands partials through its own stack and exposes no nesting depth, but
// it calls `node_get_partial` once per expansion. Counting expansions stops a partial that includes
// itself. A legitimate render costs one expansion per partial reference per item, so an aggregate
// over 10,000 entries that references ten partials each costs 100,000, and
// `TEMPLATE_RENDER_EXPANSION_COUNT_MAX` sits three times above that. The margin is three rather
// than ten because a runaway render's memory is its expansions times the names that each expansion
// resolves. At this bound a whole build with one worker measured 10 MB of peak resident memory for
// a bare `{{>loop}}`, 53 MB at three names and 111 MB at nine.
//
// Output: escaping turns a byte into as much as a six-byte entity, and the repair turns one into
// three, so `check_output_size` bounds the accumulated output after each append. It is the only
// bound that a legitimate site can reach. An aggregate that embeds the full body of 10,000 entries
// of about 6.7 KB each costs 64 MB, so `TEMPLATE_RENDER_OUTPUT_LEN_MAX` sits four times above that.
// The buffer doubles in place, so a render that reaches the bound reserves up to twice it before it
// fails, and the test that asserts the diagnostic passes a smaller bound.

/** One compiled template of a set. */
struct TemplateFile {
  /** Path relative to `templates_dir`, which a diagnostic names, such as `partials/card.html`. */
  struct Str name;

  /** Name a template refers to a partial by, such as `card`, or empty for a page template. */
  struct Str partial_name;

  /** Compiled template, mustache4c's own allocation, or `NULL` when it failed to compile. */
  MUSTACHE_TEMPLATE* compiled;
};

struct TemplateSet {
  /** Page templates, in the order `template_set_load` was given them. */
  struct TemplateFile* pages;

  /** Number of entries in `pages`. */
  ptrdiff_t page_count;

  /** Partials, sorted bytewise by `partial_name`. */
  struct TemplateFile* partials;

  /** Number of entries in `partials`. */
  ptrdiff_t partial_count;
};

/** State of one compile, which mustache4c's parse-error callback reports into. */
struct CompileState {
  /** File being compiled, relative to `templates_dir`. */
  struct Str file_name;

  /** Whether a syntax error was recorded, so only the first of one file is. */
  bool has_failed;

  /** Log that receives the syntax error. */
  struct Log* log;
};

/** State of one render, threaded through mustache4c's callbacks. */
struct RenderState {
  /** Set the render resolves partials in. */
  const struct TemplateSet* set;

  /** Page template being rendered. */
  const struct TemplateFile* page;

  /** Data the names resolve against. */
  const struct TemplateData* data;

  /** Buffer accumulating the output, in the caller's `perm`. */
  struct Buf* buffer;

  /** Largest accumulated length `buffer` may reach, in bytes. */
  ptrdiff_t output_len_max;

  /** Partial expansions resolved so far, bounded by `TEMPLATE_RENDER_EXPANSION_COUNT_MAX`. */
  ptrdiff_t expansion_count;

  /** Arena of the render, for the nodes that `data` makes and the failure message. */
  struct Arena* arena;

  /** Log of this render alone, which receives its first failure. */
  struct Log* log;

  /** Whether a callback failed, which `mustache_process` does not always report. */
  bool has_failed;
};

/**
 * @brief Reads and compiles one template file.
 *
 * @param file          File to compile, whose `name` is set, which receives `compiled`. Must not be
 *                      `NULL`.
 * @param templates_dir Directory of the templates.
 * @param owner         Source that names the file, which a read failure names, or empty.
 * @param operation     Operation that a read failure starts with.
 * @param os            Host the file is read from. Must not be `NULL`.
 * @param scratch       Arena for the file bytes.
 * @param log           Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the file cannot be read or does not compile.
 */
static int template_set_load_compile(struct TemplateFile* file,
                                     struct Str templates_dir,
                                     struct Str owner,
                                     struct Str operation,
                                     struct Os* os,
                                     struct Arena scratch,
                                     struct Log* log) __attribute__((nonnull(1, 5, 7)));

/**
 * @brief Lists the partials below `templates_dir`, sorted by name, without compiling them.
 *
 * @param set           Set that receives the partials. Must not be `NULL`.
 * @param templates_dir Directory of the templates.
 * @param os            Host the directory is listed on. Must not be `NULL`.
 * @param perm          Arena that owns the partials. Must not be `NULL`.
 * @param scratch       Arena for the listing.
 * @param log           Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, including when there is no `partials/`, or `-1` when it cannot be listed
 *         or holds too many partials.
 */
static int template_set_load_partials(struct TemplateSet* set,
                                      struct Str templates_dir,
                                      struct Os* os,
                                      struct Arena* perm,
                                      struct Arena scratch,
                                      struct Log* log) __attribute__((nonnull(1, 3, 4, 6)));

/**
 * @brief Orders two partials bytewise by name, for `qsort` and `bsearch`.
 *
 * @param a First `struct TemplateFile`. Must not be `NULL`.
 * @param b Second `struct TemplateFile`. Must not be `NULL`.
 * @return Negative, zero or positive as `a` sorts before, with, or after `b`.
 */
static int compare_partials(const void* a, const void* b) __attribute__((nonnull(1, 2)));

/**
 * @brief Records a template syntax error with its line and column, once per file.
 *
 * Installed as mustache4c's parse-error callback.
 *
 * @param err_code    mustache4c `MUSTACHE_ERR_*` code. Unused, since `msg` names the cause.
 * @param msg         Library message for `err_code`. Must not be `NULL`.
 * @param line        One-based line of the offending tag.
 * @param column      One-based column of the offending tag.
 * @param parser_data Pointer to the compile's `struct CompileState`. Must not be `NULL`.
 */
static void record_parse_error(int err_code,
                               const char* msg,
                               unsigned line,
                               unsigned column,
                               void* parser_data) __attribute__((nonnull(2, 5)));

/**
 * @brief Appends output text to the render buffer unescaped, repaired with `utf8_repair_for_xml`
 *        first, as mustache4c's unescaped-output callback.
 *
 * `node_dump` writes only HTML through it, so the rest of what reaches it is literal template text.
 *
 * @param output        Rendered bytes.
 * @param output_len    Number of bytes in `output`.
 * @param renderer_data Pointer to the render's `struct RenderState`.
 * @return `0` on success, or `-1` past the output bound.
 */
static int out_verbatim(const char* output, size_t output_len, void* renderer_data);

/**
 * @brief Appends output text to the render buffer HTML-escaped, repaired with `utf8_repair_for_xml`
 *        first, as mustache4c's escaped-output callback.
 *
 * @param output        Rendered bytes.
 * @param output_len    Number of bytes in `output`.
 * @param renderer_data Pointer to the render's `struct RenderState`.
 * @return `0` on success, or `-1` past the output bound.
 */
static int out_escaped(const char* output, size_t output_len, void* renderer_data);

/**
 * @brief Fails the render once its output has passed its bound.
 *
 * @param state Render whose buffer is checked. Must not be `NULL`.
 * @return `0` when the render may go on, or `-1` once the bound is passed.
 */
static int check_output_size(struct RenderState* state) __attribute__((nonnull(1)));

/**
 * @brief Writes a node's text through `out_fn`, as mustache4c's dump callback, rejecting unescaped
 *        interpolation of a node whose text is not HTML.
 *
 * A node that the renderer asked to write unescaped fails the render unless its text is HTML that
 * the app made itself, so no template can put unescaped text that the app did not make in an
 * output.
 *
 * @param node_ptr          Node to write.
 * @param out_fn            Output callback of the renderer.
 * @param renderer_data     Renderer state forwarded to `out_fn`.
 * @param provider_data_ptr Pointer to the render's `struct RenderState`, which receives the
 *                          diagnostic for an unescaped interpolation.
 * @return The result of `out_fn`, `0` for a node that writes nothing, or `-1` for an unescaped
 *         interpolation of a node whose text is not HTML.
 */
static int node_dump(void* node_ptr,
                     int (*out_fn)(const char*, size_t, void*),
                     void* renderer_data,
                     void* provider_data_ptr);

/**
 * @brief Returns the root node of the data, as mustache4c's root callback.
 *
 * @param provider_data_ptr Pointer to the render's `struct RenderState`.
 * @return The root node.
 */
static void* node_get_root(void* provider_data_ptr);

/**
 * @brief Returns the handle mustache4c carries for `node`.
 *
 * mustache4c takes its nodes as `void*` and only passes them back to these callbacks, and the data
 * only reads them, so a node stays read-only. The round trip through `uintptr_t` says that the
 * `const` is dropped on purpose.
 *
 * @param node Node of the data, or `NULL`.
 * @return The handle, `NULL` for `NULL`.
 */
static void* node_handle(const void* node);

/**
 * @brief Resolves a named child, as mustache4c's by-name callback.
 *
 * @param node_ptr          Parent node.
 * @param name              Child name. Not `NUL`-terminated.
 * @param name_len          Length of `name`.
 * @param provider_data_ptr Pointer to the render's `struct RenderState`.
 * @return The child, or `NULL` when the parent has no such child.
 */
static void* node_get_child_by_name(void* node_ptr,
                                    const char* name,
                                    size_t name_len,
                                    void* provider_data_ptr);

/**
 * @brief Resolves the item at `index`, as mustache4c's by-index callback, which also decides
 *        whether a section enters.
 *
 * @param node_ptr          Parent node.
 * @param index             Zero-based index.
 * @param provider_data_ptr Pointer to the render's `struct RenderState`.
 * @return The item, or `NULL`.
 */
static void* node_get_child_by_index(void* node_ptr, unsigned index, void* provider_data_ptr);

/**
 * @brief Returns the compiled partial `name`, as mustache4c's partial callback, counting the
 *        expansion.
 *
 * @param name              Partial name. Not `NUL`-terminated.
 * @param name_len          Length of `name`.
 * @param provider_data_ptr Pointer to the render's `struct RenderState`.
 * @return The compiled partial, or `NULL` after recording a failure.
 */
static MUSTACHE_TEMPLATE* node_get_partial(const char* name,
                                           size_t name_len,
                                           void* provider_data_ptr);

/**
 * @brief Names the template whose `{{>...}}` reference passed `name` to `node_get_partial`.
 *
 * mustache4c passes a pointer into the compiled template that holds the reference, and each
 * compiled template is one allocation, so the holder is the compiled template with the highest
 * start at or below `name`. Its interface does not say which template a reference comes from, so
 * this rests on reading `mustache.c`.
 *
 * @param state Render whose set holds the compiled templates. Must not be `NULL`.
 * @param name  Partial name exactly as mustache4c passed it. Must not be `NULL`.
 * @return The holder's name relative to `templates_dir`.
 */
static struct Str node_get_partial_holder(const struct RenderState* state, const char* name)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Records the render's first failure, found in `file`.
 *
 * @param state  Render that failed. Must not be `NULL`.
 * @param file   Template the failure is in.
 * @param format `printf` format of the failure. Must not be `NULL`.
 * @param ...    Arguments that the format converts.
 */
static void render_fail(struct RenderState* state, struct Str file, const char* format, ...)
    __attribute__((format(printf, 3, 4), nonnull(1, 3)));

/**
 * @brief Relays the one failure of `source` to `log`, with `suffix` and `name` after it, as
 *        `<failure><suffix><name>')`.
 *
 * @param source Log holding at most one failure. Must not be `NULL`.
 * @param suffix Text that starts the attribution, such as `TEMPLATE_RENDER_ATTRIBUTION`. Must not
 *               be `NULL`.
 * @param name   Name the attribution gives, or empty to relay the failure as it is.
 * @param log    Log that receives the failure. Must not be `NULL`.
 */
static void relay_attributed(const struct Log* source,
                             const char* suffix,
                             struct Str name,
                             struct Log* log) __attribute__((nonnull(1, 2, 4)));

/** Renderer dispatch table, `const` so that it stays read-only while workers render at once. */
static const MUSTACHE_RENDERER renderer = {out_verbatim, out_escaped};

/** Data-provider dispatch table. */
static const MUSTACHE_DATAPROVIDER provider = {node_dump, node_get_root, node_get_child_by_name,
                                               node_get_child_by_index, node_get_partial};

/** Parser dispatch table. */
static const MUSTACHE_PARSER parser = {record_parse_error};

int template_set_load(struct Str templates_dir,
                      const struct Str* page_names,
                      const struct Str* page_owners,
                      ptrdiff_t page_count,
                      struct Os* os,
                      struct Arena* perm,
                      struct Arena scratch,
                      struct TemplateSet** set_out,
                      struct Log* log) {
  assert(page_count >= 0);
  struct TemplateSet* set = arena_new(perm, 1, struct TemplateSet);
  set->pages = arena_new(perm, page_count, struct TemplateFile);
  set->page_count = page_count;
  int rc = 0;
  for (ptrdiff_t i = 0; i < page_count; i++) {
    assert(path_is_safe_relative(page_names[i]));
    set->pages[i].name = page_names[i];
    const struct Str owner = page_owners != NULL ? page_owners[i] : (struct Str){0};
    if (template_set_load_compile(&set->pages[i], templates_dir, owner,
                                  S("failed to read template"), os, scratch, log) != 0) {
      rc = -1;
    }
  }
  if (template_set_load_partials(set, templates_dir, os, perm, scratch, log) != 0) {
    rc = -1;
  }
  for (ptrdiff_t i = 0; i < set->partial_count; i++) {
    if (template_set_load_compile(&set->partials[i], templates_dir, (struct Str){0},
                                  S("failed to read partial"), os, scratch, log) != 0) {
      rc = -1;
    }
  }
  if (rc != 0) {
    template_set_release(set);
    return -1;
  }
  *set_out = set;
  return 0;
}

void template_set_release(struct TemplateSet* set) {
  if (set == NULL) {
    return;
  }
  for (ptrdiff_t i = 0; i < set->page_count; i++) {
    if (set->pages[i].compiled != NULL) {
      mustache_release(set->pages[i].compiled);
      set->pages[i].compiled = NULL;
    }
  }
  for (ptrdiff_t i = 0; i < set->partial_count; i++) {
    if (set->partials[i].compiled != NULL) {
      mustache_release(set->partials[i].compiled);
      set->partials[i].compiled = NULL;
    }
  }
}

int template_set_render(const struct TemplateSet* set,
                        struct Str page_name,
                        const struct TemplateData* data,
                        struct Str output_name,
                        ptrdiff_t output_len_max,
                        struct Arena* perm,
                        struct Arena scratch,
                        struct Str* output_out,
                        struct Log* log) {
  const struct TemplateFile* page = NULL;
  for (ptrdiff_t i = 0; i < set->page_count && page == NULL; i++) {
    if (str_eq(set->pages[i].name, page_name)) {
      page = &set->pages[i];
    }
  }
  // Every page a build renders was named when the set was loaded, and a set that loaded compiled
  // every page, so a missing one is a bug in the caller, for which no message would help.
  if (page == NULL || page->compiled == NULL) {
    abort();
  }

  // The render logs into a log of its own, in the caller's log's arena, so the relay below can name
  // the output after its one failure.
  struct Log render_log = {.buf = {.arena = log->buf.arena}};
  struct Buf buffer = {.arena = perm};
  struct RenderState state = {
      .set = set,
      .page = page,
      .data = data,
      .buffer = &buffer,
      .output_len_max = output_len_max,
      .arena = &scratch,
      .log = &render_log,
  };
  int rc = -1;
  // `mustache_process` reports a callback that returned non-zero, but a `NULL` partial means absent
  // to it, so `has_failed` is the second signal. A failure with neither means mustache4c's own
  // allocation failed.
  if (mustache_process(page->compiled, &renderer, &state, &provider, &state) != 0 ||
      state.has_failed) {
    if (!state.has_failed) {
      os_oom();
    }
  } else {
    *output_out = buf_str(&buffer);
    rc = 0;
  }
  relay_attributed(&render_log, TEMPLATE_RENDER_ATTRIBUTION, output_name, log);
  return rc;
}

static int template_set_load_compile(struct TemplateFile* file,
                                     struct Str templates_dir,
                                     struct Str owner,
                                     struct Str operation,
                                     struct Os* os,
                                     struct Arena scratch,
                                     struct Log* log) {
  const struct Str path = path_join(templates_dir, file->name, &scratch);
  struct Str data = {0};
  // The read logs into a log of its own, so a failure can name the source that named the file.
  struct Log read_log = {.buf = {.arena = log->buf.arena}};
  const int read_rc =
      os_read(os, path, TEMPLATE_FILE_LEN_MAX, OS_READ_TEXT, &scratch, &data, operation, &read_log);
  relay_attributed(&read_log, " (for '", owner, log);
  if (read_rc != 0) {
    return -1;
  }
  // mustache4c copies every literal run and tag name into the instructions it returns, so the bytes
  // can go with `scratch`. Its interface does not say so, so this rests on reading `mustache.c`.
  struct CompileState compile = {.file_name = file->name, .log = log};
  file->compiled = mustache_compile(data.data, (size_t)data.len, &parser, &compile, 0);
  if (file->compiled == NULL) {
    // mustache4c fails a compile without reporting a syntax error only when its own allocation
    // fails.
    if (!compile.has_failed) {
      os_oom();
    }
    return -1;
  }
  return 0;
}

static int template_set_load_partials(struct TemplateSet* set,
                                      struct Str templates_dir,
                                      struct Os* os,
                                      struct Arena* perm,
                                      struct Arena scratch,
                                      struct Log* log) {
  const struct Str partials_dir = path_join(templates_dir, S("partials"), &scratch);
  struct OsStat st;
  if (os_stat(os, partials_dir, &st) != 0 && errno == ENOENT) {
    return 0;
  }
  struct OsEntry* entries = NULL;
  ptrdiff_t entry_count = 0;
  if (os_list(os, partials_dir, &scratch, &entries, &entry_count, S("failed to list partials"),
              log) != 0) {
    return -1;
  }
  struct TemplateFile* partials = arena_new(perm, entry_count, struct TemplateFile);
  ptrdiff_t partial_count = 0;
  int rc = 0;
  for (ptrdiff_t i = 0; i < entry_count; i++) {
    if (!str_endswith(entries[i].name, S(".html"))) {
      continue;
    }
    const struct Str partial_name = str_head(entries[i].name, entries[i].name.len - 5);
    if (!text_is_safe_identifier(partial_name)) {
      continue;
    }
    // A partial that cannot be inspected would fail a later reference as missing, so the load
    // reports it as it is.
    if (entries[i].kind == OS_ENTRY_FAILED) {
      const struct Str partial_path = path_join(partials_dir, entries[i].name, &scratch);
      char reason[ERROR_SYSTEM_MESSAGE_SIZE];
      rc = log_error(log, "failed to list partials: cannot inspect entry: %s ('%.*s')",
                     error_system_message(reason, sizeof(reason), entries[i].error_number),
                     STR_FMT(partial_path));
      continue;
    }
    if (entries[i].kind != OS_ENTRY_FILE) {
      continue;
    }
    partials[partial_count++] = (struct TemplateFile){
        .name = str_concat(S("partials/"), str_clone(entries[i].name, perm), perm),
        .partial_name = str_clone(partial_name, perm),
    };
  }
  if (partial_count > 0) {
    qsort(partials, (size_t)partial_count, sizeof(*partials), compare_partials);
  }
  // The limit is checked in name order, because `os_list` lists in no fixed order, so the partial
  // named is the first past the limit on every host.
  if (partial_count > TEMPLATE_PARTIAL_COUNT_MAX) {
    const struct Str partial_path =
        path_join(templates_dir, partials[TEMPLATE_PARTIAL_COUNT_MAX].name, &scratch);
    return log_error(log, "templates exceed max partial count (%d): '%.*s'",
                     TEMPLATE_PARTIAL_COUNT_MAX, STR_FMT(partial_path));
  }
  set->partials = partials;
  set->partial_count = partial_count;
  return rc;
}

static int compare_partials(const void* a, const void* b) {
  const struct TemplateFile* partial_a = a;
  const struct TemplateFile* partial_b = b;
  return str_compare(partial_a->partial_name, partial_b->partial_name);
}

static void record_parse_error(int err_code,
                               const char* msg,
                               unsigned line,
                               unsigned column,
                               void* parser_data) {
  (void)err_code;
  struct CompileState* compile = parser_data;
  // mustache4c can report a secondary note after the error, such as where an unclosed section
  // opened, so only the first message of a file is kept.
  if (!compile->has_failed) {
    (void)log_error_in(compile->log, compile->file_name, "%s at line %u, column %u", msg, line,
                       column);
  }
  compile->has_failed = true;
}

static int out_verbatim(const char* output, size_t output_len, void* renderer_data) {
  struct RenderState* state = renderer_data;
  // mustache4c cuts literal text only beside an ASCII byte, at a tag or a line start, and hands
  // over each node's text whole, so a repair never splits a character. The repaired copy goes with
  // this callback's scratch.
  struct Arena scratch = *state->arena;
  const struct Str text = {output, (ptrdiff_t)output_len};
  buf_append(state->buffer, utf8_repair_for_xml(text, &scratch));
  return check_output_size(state);
}

static int out_escaped(const char* output, size_t output_len, void* renderer_data) {
  struct RenderState* state = renderer_data;
  struct Arena scratch = *state->arena;
  const struct Str text = {output, (ptrdiff_t)output_len};
  html_escape_append(state->buffer, utf8_repair_for_xml(text, &scratch));
  return check_output_size(state);
}

static int check_output_size(struct RenderState* state) {
  const ptrdiff_t output_len = state->buffer->len;
  if (output_len <= state->output_len_max) {
    return 0;
  }
  render_fail(state, state->page->name,
              "render exceeds max rendered output (%td bytes) at %td bytes", state->output_len_max,
              output_len);
  return -1;
}

static int node_dump(void* node_ptr,
                     int (*out_fn)(const char*, size_t, void*),
                     void* renderer_data,
                     void* provider_data_ptr) {
  struct RenderState* state = provider_data_ptr;
  const struct TemplateText text = state->data->text(node_ptr, state->data->userdata);
  // The callback identity is what distinguishes `{{{value}}}` from `{{value}}`. mustache4c passes
  // the renderer's unescaped-output callback for a triple-brace or ampersand tag and names the tag
  // kind nowhere else. Only HTML that the app made itself, under a key that ends in `_html`, is
  // written unescaped, so a value it did not make, such as a title, cannot put markup in an output.
  // The check comes before the text is written, so a tag fails whatever node it names, and empty
  // text too.
  if (out_fn != out_escaped && !text.is_html) {
    render_fail(
        state, state->page->name,
        "unescaped Mustache interpolation is supported only for a key that ends in '_html'");
    return -1;
  }
  if (text.text.len == 0) {
    return 0;
  }
  return out_fn(text.text.data, (size_t)text.text.len, renderer_data);
}

static void* node_get_root(void* provider_data_ptr) {
  const struct RenderState* state = provider_data_ptr;
  return node_handle(state->data->root(state->arena, state->data->userdata));
}

static void* node_handle(const void* node) {
  return (void*)(uintptr_t)node;
}

static void* node_get_child_by_name(void* node_ptr,
                                    const char* name,
                                    size_t name_len,
                                    void* provider_data_ptr) {
  const struct RenderState* state = provider_data_ptr;
  const struct Str name_text = {name, (ptrdiff_t)name_len};
  return node_handle(
      state->data->child_by_name(node_ptr, name_text, state->arena, state->data->userdata));
}

static void* node_get_child_by_index(void* node_ptr, unsigned index, void* provider_data_ptr) {
  const struct RenderState* state = provider_data_ptr;
  return node_handle(
      state->data->child_by_index(node_ptr, (ptrdiff_t)index, state->arena, state->data->userdata));
}

static MUSTACHE_TEMPLATE* node_get_partial(const char* name,
                                           size_t name_len,
                                           void* provider_data_ptr) {
  struct RenderState* state = provider_data_ptr;
  // mustache4c renders on past a callback's failure, so a failed render resolves no more partials,
  // and its first failure stays the one it reports.
  if (state->has_failed) {
    return NULL;
  }
  // The count is the render's only termination guard against a partial that includes itself.
  state->expansion_count++;
  if (state->expansion_count > TEMPLATE_RENDER_EXPANSION_COUNT_MAX) {
    render_fail(state, state->page->name,
                "render exceeds max partial expansions (%d); check for a partial that includes "
                "itself",
                TEMPLATE_RENDER_EXPANSION_COUNT_MAX);
    return NULL;
  }
  const struct Str name_text = {name, (ptrdiff_t)name_len};
  // The set holds only partials with a valid name, so an invalid name is reported as one, rather
  // than as a missing partial.
  if (!text_is_safe_identifier(name_text)) {
    render_fail(state, node_get_partial_holder(state, name),
                "partial name must contain only letters, digits, '_' and '-': '%.*s'",
                STR_FMT(name_text));
    return NULL;
  }
  const struct TemplateFile key = {.partial_name = name_text};
  const struct TemplateFile* partial =
      state->set->partial_count > 0
          ? bsearch(&key, state->set->partials, (size_t)state->set->partial_count,
                    sizeof(*state->set->partials), compare_partials)
          : NULL;
  if (partial == NULL) {
    render_fail(state, node_get_partial_holder(state, name),
                "partial '%.*s' is not in the templates ('partials/%.*s.html')", STR_FMT(name_text),
                STR_FMT(name_text));
    return NULL;
  }
  return partial->compiled;
}

static struct Str node_get_partial_holder(const struct RenderState* state, const char* name) {
  // The addresses compare as integers, because `<` between pointers into different objects is
  // undefined.
  const uintptr_t name_address = (uintptr_t)name;
  const struct TemplateFile* holder = state->page;
  uintptr_t holder_address = (uintptr_t)state->page->compiled;
  for (ptrdiff_t i = 0; i < state->set->partial_count; i++) {
    const uintptr_t partial_address = (uintptr_t)state->set->partials[i].compiled;
    if (partial_address <= name_address &&
        (holder_address > name_address || partial_address > holder_address)) {
      holder_address = partial_address;
      holder = &state->set->partials[i];
    }
  }
  return holder->name;
}

static void render_fail(struct RenderState* state, struct Str file, const char* format, ...) {
  if (!state->has_failed) {
    // The first pass measures the message and the second writes it, as `buf_printf` does. Starting
    // the list twice needs no `va_copy`, and forwarding it to another formatter instead makes
    // Clang's analyzer report a false `va_end` on AArch64 Linux.
    va_list args;
    va_start(args, format);
    const int message_len = vsnprintf(NULL, 0, format, args);
    va_end(args);
    assert(message_len >= 0);
    char* message = arena_new(state->arena, (ptrdiff_t)message_len + 1, char);
    va_start(args, format);
    (void)vsnprintf(message, (size_t)message_len + 1, format, args);
    va_end(args);
    (void)log_error_in(state->log, file, "%.*s", message_len, message);
  }
  state->has_failed = true;
}

static void relay_attributed(const struct Log* source,
                             const char* suffix,
                             struct Str name,
                             struct Log* log) {
  if (source->errors == 0 || name.len == 0) {
    log_append(log, source);
    return;
  }
  // The source records one failure, a line ending in `\n`, which the attribution goes after.
  const struct Str failure = buf_str(&source->buf);
  const struct Str failure_line = str_head(failure, failure.len - 1);
  (void)log_error(log, "%.*s%s%.*s')", STR_FMT(failure_line), suffix, STR_FMT(name));
}
