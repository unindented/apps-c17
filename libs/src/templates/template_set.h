#ifndef APPS_TEMPLATES_TEMPLATE_SET_H
#define APPS_TEMPLATES_TEMPLATE_SET_H

#include <stdbool.h>
#include <stddef.h>

#include "core/arena.h"
#include "core/str.h"

struct Log;
struct Os;

/**
 * Largest template or partial file read, in bytes. A hand-written template with inline styles,
 * scripts and SVG is tens of kilobytes, so this sits two orders of magnitude above one. The read
 * checks it before it loads the file, so an oversize template is never resident.
 */
enum { TEMPLATE_FILE_LEN_MAX = 4 * 1024 * 1024 };

/**
 * Most partials a template set holds, the `.html` files of `partials/` with a valid name. A
 * hand-written theme has a handful. With `TEMPLATE_FILE_LEN_MAX` it bounds the compiled partials of
 * a set at 256 MiB.
 */
enum { TEMPLATE_PARTIAL_COUNT_MAX = 64 };

/**
 * Largest output of one render, in bytes. `template_set.c` explains why the bound sits where it
 * does. An app also sizes its arenas from it, because a render's output lives in the caller's
 * `perm`.
 */
enum { TEMPLATE_RENDER_OUTPUT_LEN_MAX = 256 * 1024 * 1024 };

/**
 * Most partial expansions of one render, which stops a partial that includes itself.
 * `template_set.c` explains why the bound sits where it does.
 */
enum { TEMPLATE_RENDER_EXPANSION_COUNT_MAX = 300000 };

/**
 * Text that starts a render's attribution, which ends its failure message as
 * `(while rendering '<output>')`. A parallel phase passes it to `job_run`, so one template failure
 * that every page repeats is reported once.
 */
#define TEMPLATE_RENDER_ATTRIBUTION " (while rendering '"

/**
 * Every page template and every partial that a build renders with, each compiled once before the
 * first render. The set is read-only once loaded, so every worker renders from it at once:
 * mustache4c only reads a compiled template while it processes one. Defined in `template_set.c`.
 */
struct TemplateSet;

/** The text of a node of a render's data, which a tag writes. */
struct TemplateText {
  /** Text the tag writes, empty for a node that writes nothing, such as a list. */
  struct Str text;

  /**
   * Whether the text is HTML that the app made itself, under a key that ends in `_html`, which a
   * triple mustache or an ampersand tag may write unescaped. Such a tag of any other node fails the
   * render.
   */
  bool is_html;
};

/**
 * @brief Returns the root node of a render's data.
 *
 * @param arena    Arena of the render, for a node that the callback makes. Must not be `NULL`.
 * @param userdata Opaque pointer from `struct TemplateData`.
 * @return The root node. Must not be `NULL`.
 */
typedef const void* (*TemplateRootFn)(struct Arena* arena, void* userdata);

/**
 * @brief Returns the child of `node` named `name`.
 *
 * Mustache looks a name that a node does not have up in the enclosing nodes, so a node gives every
 * key of its schema, with empty text where a value is absent.
 *
 * @param node     Node to look in.
 * @param name     Name of the child.
 * @param arena    Arena of the render, for a node that the callback makes. Must not be `NULL`.
 * @param userdata Opaque pointer from `struct TemplateData`.
 * @return The child, or `NULL` when `node` has none of that name.
 */
typedef const void* (*TemplateChildByNameFn)(const void* node,
                                             struct Str name,
                                             struct Arena* arena,
                                             void* userdata);

/**
 * @brief Returns the item of `node` at `index`, which also decides whether a section enters.
 *
 * A list gives its items. Per the Mustache specification a single value is iterable once, so any
 * other node gives itself at index `0` when it is true, and `NULL` when it is false or empty.
 *
 * @param node     Node to look in.
 * @param index    Zero-based index.
 * @param arena    Arena of the render, for a node that the callback makes. Must not be `NULL`.
 * @param userdata Opaque pointer from `struct TemplateData`.
 * @return The item, or `NULL` past the last one.
 */
typedef const void* (*TemplateChildByIndexFn)(const void* node,
                                              ptrdiff_t index,
                                              struct Arena* arena,
                                              void* userdata);

/**
 * @brief Returns what a tag of `node` writes.
 *
 * @param node     Node that a tag names.
 * @param userdata Opaque pointer from `struct TemplateData`.
 * @return The node's text, and whether it is HTML.
 */
typedef struct TemplateText (*TemplateTextFn)(const void* node, void* userdata);

/**
 * The data that a render reads, through callbacks that the app gives. Mustache4c asks that each
 * node be told apart by its pointer, so a callback that makes a node makes a new one for each call.
 */
struct TemplateData {
  /** Returns the root node. Must not be `NULL`. */
  TemplateRootFn root;

  /** Returns a named child. Must not be `NULL`. */
  TemplateChildByNameFn child_by_name;

  /** Returns an indexed item. Must not be `NULL`. */
  TemplateChildByIndexFn child_by_index;

  /** Returns a node's text. Must not be `NULL`. */
  TemplateTextFn text;

  /** Opaque pointer passed to every callback. */
  void* userdata;
};

/**
 * @brief Reads and compiles the page templates `page_names` and every `partials/<name>.html` below
 *        `templates_dir`.
 *
 * A partial's name is its file name without `.html`, and only a name of letters, digits, `_` and
 * `-` is one, so a template refers to `partials/card.html` as `{{>card}}`. Other files of
 * `partials/`, and its subdirectories, are not partials. Every page template must exist. Every file
 * is read and compiled even after one fails, so one load reports every broken template.
 *
 * @param templates_dir Directory of the templates.
 * @param page_names    Names of the page templates, relative to `templates_dir`, `page_count` of
 *                      them, each a safe relative path, and no two the same. Must not be `NULL`
 *                      unless `page_count` is `0`.
 * @param page_owners   For each page template, the source that names it, which a failure to read
 *                      the template names as `(for '<owner>')`, or empty for none. `NULL` gives
 *                      every page template none.
 * @param page_count    Number of entries in `page_names`.
 * @param os            Host the templates are read from. Must not be `NULL`.
 * @param perm          Arena that owns the set. Must not be `NULL`.
 * @param scratch       Arena for the file bytes.
 * @param set_out       Receives the set on success, which `template_set_release` releases. Must not
 *                      be `NULL`.
 * @param log           Log that receives every failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when a template cannot be read or compiled, or there are more
 *         partials than `TEMPLATE_PARTIAL_COUNT_MAX`.
 */
int template_set_load(struct Str templates_dir,
                      const struct Str* page_names,
                      const struct Str* page_owners,
                      ptrdiff_t page_count,
                      struct Os* os,
                      struct Arena* perm,
                      struct Arena scratch,
                      struct TemplateSet** set_out,
                      struct Log* log) __attribute__((nonnull(5, 6, 8, 9)));

/**
 * @brief Releases what mustache4c allocated for the compiled templates of `set`.
 *
 * @param set Set to release, or `NULL`, which releases nothing.
 */
void template_set_release(struct TemplateSet* set);

/**
 * @brief Renders the page template `page_name` of `set` with `data`.
 *
 * Interpolation escapes its text for HTML. A triple mustache or an ampersand tag writes text as it
 * is only for a node whose text is HTML, and fails the render for any other node. Every text the
 * render writes, literal template text included, is repaired with `utf8_repair_for_xml` first, so
 * an output is valid UTF-8 without the characters that XML 1.0 forbids, whatever bytes a node
 * holds. A partial resolves by name within the set. The partial expansions of one render are
 * counted, so a partial that includes itself fails the render.
 *
 * Templates are trusted input. The limits stop a partial that includes itself and bound the output,
 * but not the render's total memory: a template's own nested sections ask `data` for nodes in
 * proportion to the iterations they ask for.
 *
 * @param set            Loaded templates. Must not be `NULL`.
 * @param page_name      Name of a page template of `set`.
 * @param data           Data the names resolve against. Must not be `NULL`.
 * @param output_name    Output the render is for, relative to the output directory, which a failure
 *                       names after `TEMPLATE_RENDER_ATTRIBUTION`.
 * @param output_len_max Largest output accepted, in bytes, `TEMPLATE_RENDER_OUTPUT_LEN_MAX` outside
 *                       the tests.
 * @param perm           Arena that owns the output, where nothing else of the render allocates, so
 *                       the output grows in place. A failed render may leave its partial output
 *                       there. Must not be `NULL`.
 * @param scratch        Arena for the render's temporaries and the nodes that `data` makes. Must
 *                       not overlap `perm`.
 * @param output_out     Receives the output on success. Must not be `NULL`.
 * @param log            Log that receives the render's first failure. Its arena must not overlap
 *                       `perm` or `scratch`. Must not be `NULL`.
 * @return `0` on success, or `-1` on failure.
 */
int template_set_render(const struct TemplateSet* set,
                        struct Str page_name,
                        const struct TemplateData* data,
                        struct Str output_name,
                        ptrdiff_t output_len_max,
                        struct Arena* perm,
                        struct Arena scratch,
                        struct Str* output_out,
                        struct Log* log) __attribute__((nonnull(1, 3, 6, 8, 9)));

#endif
