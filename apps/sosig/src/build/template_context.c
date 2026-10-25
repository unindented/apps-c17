#include "build/template_context.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "core/arena.h"
#include "core/path.h"
#include "core/str.h"
#include "domain/content_entry.h"
#include "domain/site_config.h"
#include "domain/sosig_version.h"
#include "templates/template_set.h"

/** Kind of a node in the data that a render walks. */
enum NodeKind {
  /** Top-level lookup context: `site.*`, `content_entries`, and current entry fields. */
  NODE_ROOT,

  /** Site metadata subtree resolving `site.*` names. */
  NODE_SITE,

  /** Feed metadata subtree resolving `feed.*` names, in a feed template only. */
  NODE_FEED,

  /** One content entry, resolving bare field names. */
  NODE_ENTRY,

  /** The `content_entries` list, iterated by index. */
  NODE_ENTRY_LIST,

  /** A content entry's tag list, iterated by index. */
  NODE_TAG_LIST,

  /** A terminal string value. */
  NODE_SCALAR,
};

/** A node in the data. Nodes are arena-owned and live for one render. */
struct Node {
  /** Selects which of the fields below carry meaning for this node. */
  enum NodeKind kind;

  /** Backing content entry for `NODE_ENTRY` and `NODE_TAG_LIST`. */
  const struct ContentEntry* entry;

  /**
   * Borrowed text for `NODE_SCALAR`, empty for an unset optional field or an empty tag.
   * `template_context_render_child_by_index` makes an empty scalar false in a section.
   */
  struct Str scalar;

  /**
   * Set for a `NODE_SCALAR` under a key that ends in `_html`, which a template may write unescaped.
   * The text is HTML that sosig made itself, and a render fails a `{{{x}}}` or `{{& x}}` of any
   * other node, so a value sosig did not make, such as a title or a tag, cannot put markup in a
   * page.
   */
  bool is_html;
};

/** State of one render, which every callback receives as its `userdata`. */
struct ContextRender {
  /** Borrowed values visible to template variables. */
  const struct TemplateContext* context;

  /** Output the render is for, relative to `output_dir`, which `feed.url` gives. */
  struct Str output_name;

  /**
   * Whether the render resolved `content_entries`, whose value depends on every entry. A partial's
   * lookups count too, because they share this state.
   */
  bool is_entry_list_read;
};

/**
 * @brief Returns the root node, as `TemplateRootFn`.
 *
 * @param arena    Arena that owns the node. Must not be `NULL`.
 * @param userdata The render's `struct ContextRender`.
 * @return The root node.
 */
static const void* template_context_render_root(struct Arena* arena, void* userdata);

/**
 * @brief Resolves a named child of a node, as `TemplateChildByNameFn`.
 *
 * At the root a name selects the `site.*` subtree, in a feed the `feed.*` subtree, the
 * `content_entries` list, the `generator` string that names this tool and its version, the page's
 * `path_to_root`, or otherwise a field of the entry being rendered, such as `title`.
 *
 * @param node     Parent `struct Node`.
 * @param name     Requested child name.
 * @param arena    Arena that owns the child. Must not be `NULL`.
 * @param userdata The render's `struct ContextRender`.
 * @return The child node, or `NULL` when the name is not present.
 */
static const void* template_context_render_child_by_name(const void* node,
                                                         struct Str name,
                                                         struct Arena* arena,
                                                         void* userdata);

/**
 * @brief Resolves an indexed child of a node, as `TemplateChildByIndexFn`.
 *
 * Iterates content-entry and tag lists by index. Any other value is iterable once at index `0`,
 * unless it is an empty scalar.
 *
 * @param node     Parent `struct Node`.
 * @param index    Zero-based child index.
 * @param arena    Arena that owns the child. Must not be `NULL`.
 * @param userdata The render's `struct ContextRender`.
 * @return The child node, or `NULL` when the index is out of range.
 */
static const void* template_context_render_child_by_index(const void* node,
                                                          ptrdiff_t index,
                                                          struct Arena* arena,
                                                          void* userdata);

/**
 * @brief Returns what a tag of a node writes, as `TemplateTextFn`: a scalar's text, and nothing for
 *        another node.
 *
 * @param node     The `struct Node`.
 * @param userdata Unused.
 * @return The node's text, and whether it is HTML.
 */
static struct TemplateText template_context_render_text(const void* node, void* userdata);

/**
 * @brief Allocates an arena-owned node of the given kind for the current render.
 *
 * Zeroes every field other than `kind`, so a caller sets only the fields its kind carries.
 *
 * @param arena Arena that owns the node. Must not be `NULL`.
 * @param kind  Node kind to assign.
 * @return The new node.
 */
static struct Node* node_alloc(struct Arena* arena, enum NodeKind kind)
    __attribute__((nonnull(1), returns_nonnull));

/**
 * @brief Returns a scalar node wrapping `value`, which may be empty.
 *
 * @param arena Arena that owns the node. Must not be `NULL`.
 * @param value String to wrap, borrowed for the rest of the render.
 * @return A scalar node.
 */
static struct Node* node_scalar(struct Arena* arena, struct Str value)
    __attribute__((nonnull(1), returns_nonnull));

/**
 * @brief Returns a scalar node wrapping HTML that sosig made itself, which a template may write
 *        unescaped.
 *
 * @param arena Arena that owns the node. Must not be `NULL`.
 * @param html  HTML to wrap, which may be empty, borrowed for the rest of the render.
 * @return A scalar node with `is_html` set.
 */
static struct Node* node_scalar_html(struct Arena* arena, struct Str html)
    __attribute__((nonnull(1), returns_nonnull));

/**
 * @brief Returns a scalar node holding an output-root-relative URL rebased onto the current page.
 *
 * Joins the URL behind the page's `path_to_root`, so a link resolves from the page's own directory
 * rather than only from the output root. In a feed, every link is absolute instead, as
 * `node_scalar_url_absolute` makes it.
 *
 * @param render            Render supplying the context. Must not be `NULL`.
 * @param root_relative_url Output-root-relative URL, built from the permalink.
 * @param arena             Arena that owns the node and the URL. Must not be `NULL`.
 * @return A scalar node.
 */
static struct Node* node_scalar_url(const struct ContextRender* render,
                                    struct Str root_relative_url,
                                    struct Arena* arena)
    __attribute__((nonnull(1, 3), returns_nonnull));

/**
 * @brief Returns a scalar node holding an output-root-relative URL joined behind `base_url`.
 *
 * @param render            Render supplying the site configuration. Must not be `NULL`.
 * @param root_relative_url Output-root-relative URL, built from the permalink.
 * @param arena             Arena that owns the node and the URL. Must not be `NULL`.
 * @return A scalar node.
 */
static struct Node* node_scalar_url_absolute(const struct ContextRender* render,
                                             struct Str root_relative_url,
                                             struct Arena* arena)
    __attribute__((nonnull(1, 3), returns_nonnull));

/**
 * @brief Resolves a `site.*` name against the render's site configuration.
 *
 * @param render Render holding the site configuration. Must not be `NULL`.
 * @param name   Requested field name.
 * @param arena  Arena that owns the node. Must not be `NULL`.
 * @return The resolved node, or `NULL` when the name is unmatched.
 */
static struct Node* resolve_site(const struct ContextRender* render,
                                 struct Str name,
                                 struct Arena* arena) __attribute__((nonnull(1, 3)));

/**
 * @brief Resolves a `feed.*` name against the feed being rendered and its entries.
 *
 * `id` and `url` are the feed's own absolute URL, `html_url` is the site's home, and `modified` is
 * the newest change among the feed's entries, so an edit to an entry outside the feed does not
 * change it.
 *
 * @param render Render holding the output name and the feed's entries. Must not be `NULL`.
 * @param name   Requested field name.
 * @param arena  Arena that owns the node. Must not be `NULL`.
 * @return The resolved node, or `NULL` when the name is unmatched.
 */
static struct Node* resolve_feed(const struct ContextRender* render,
                                 struct Str name,
                                 struct Arena* arena) __attribute__((nonnull(1, 3)));

/**
 * @brief Resolves a bare field name against a content entry.
 *
 * @param render Render. Must not be `NULL`.
 * @param entry  Content entry to resolve against, or `NULL`.
 * @param name   Requested field name.
 * @param arena  Arena that owns the node. Must not be `NULL`.
 * @return The resolved node, which is empty for an unset field, or `NULL` when the name is
 *         unmatched or `entry` is `NULL`.
 */
static struct Node* resolve_entry(const struct ContextRender* render,
                                  const struct ContentEntry* entry,
                                  struct Str name,
                                  struct Arena* arena) __attribute__((nonnull(1, 4)));

int template_context_render(const struct TemplateSet* set,
                            struct Str template_name,
                            const struct TemplateContext* context,
                            struct Str output_name,
                            struct Arena* perm,
                            struct Arena scratch,
                            struct Str* html_out,
                            bool* is_entry_list_read_out,
                            struct Log* log) {
  // Nothing in this file may allocate into an arena reached through `context`, because renders run
  // concurrently in pool jobs. Every node goes into the render's scratch, which the callbacks get.
  struct ContextRender render = {.context = context, .output_name = output_name};
  const struct TemplateData data = {
      .root = template_context_render_root,
      .child_by_name = template_context_render_child_by_name,
      .child_by_index = template_context_render_child_by_index,
      .text = template_context_render_text,
      .userdata = &render,
  };
  if (template_set_render(set, template_name, &data, output_name, TEMPLATE_RENDER_OUTPUT_LEN_MAX,
                          perm, scratch, html_out, log) != 0) {
    return -1;
  }
  if (is_entry_list_read_out != NULL) {
    *is_entry_list_read_out = render.is_entry_list_read;
  }
  return 0;
}

static const void* template_context_render_root(struct Arena* arena, void* userdata) {
  (void)userdata;
  return node_alloc(arena, NODE_ROOT);
}

static const void* template_context_render_child_by_name(const void* node,
                                                         struct Str name,
                                                         struct Arena* arena,
                                                         void* userdata) {
  struct ContextRender* render = userdata;
  const struct Node* parent = node;
  switch (parent->kind) {
    case NODE_ROOT:
      if (str_eq(name, S("site"))) {
        return node_alloc(arena, NODE_SITE);
      }
      if (str_eq(name, S("feed")) && render->context->is_feed) {
        return node_alloc(arena, NODE_FEED);
      }
      if (str_eq(name, S("content_entries"))) {
        render->is_entry_list_read = true;
        return node_alloc(arena, NODE_ENTRY_LIST);
      }
      if (str_eq(name, S("generator"))) {
        return node_scalar(arena, sosig_generator_string());
      }
      if (str_eq(name, S("path_to_root"))) {
        return node_scalar(arena, render->context->path_to_root);
      }
      return resolve_entry(render, render->context->content_entry_current, name, arena);
    case NODE_SITE:
      return resolve_site(render, name, arena);
    case NODE_FEED:
      return resolve_feed(render, name, arena);
    case NODE_ENTRY:
      return resolve_entry(render, parent->entry, name, arena);
    case NODE_ENTRY_LIST:
    case NODE_TAG_LIST:
    case NODE_SCALAR:
      return NULL;
  }
  // Unreachable: the switch covers every `enum NodeKind`, and omits `default:` so that `-Wswitch`
  // fails the build when a kind is added without a case here.
  abort();
}

static const void* template_context_render_child_by_index(const void* node,
                                                          ptrdiff_t index,
                                                          struct Arena* arena,
                                                          void* userdata) {
  const struct ContextRender* render = userdata;
  const struct Node* parent = node;
  if (parent->kind == NODE_ENTRY_LIST) {
    if (index >= render->context->content_entry_count) {
      return NULL;
    }
    struct Node* child = node_alloc(arena, NODE_ENTRY);
    child->entry = render->context->content_entries[index];
    return child;
  }
  if (parent->kind == NODE_TAG_LIST) {
    const struct ContentEntry* entry = parent->entry;
    if (index >= entry->tag_count) {
      return NULL;
    }
    // A tag list carries an explicit count, so every index below it yields a node, whatever the tag
    // holds, and `NULL` only ends the list.
    return node_scalar(arena, entry->tags[index]);
  }
  // An empty scalar is false, so `{{#field}}` skips an unset optional field and `{{^field}}` fires,
  // as for an empty tag list.
  if (parent->kind == NODE_SCALAR && parent->scalar.len == 0) {
    return NULL;
  }
  // Per the Mustache specification a single value is iterable too, so this returns the node itself
  // for index 0 and `NULL` for every other index. Section truthiness uses the same call.
  return index == 0 ? node : NULL;
}

static struct TemplateText template_context_render_text(const void* node, void* userdata) {
  (void)userdata;
  const struct Node* text_node = node;
  // Every node answers whether it is HTML, so an unescaped tag fails for any node that is not
  // `body_html`.
  if (text_node->kind != NODE_SCALAR) {
    return (struct TemplateText){0};
  }
  return (struct TemplateText){.text = text_node->scalar, .is_html = text_node->is_html};
}

static struct Node* node_alloc(struct Arena* arena, enum NodeKind kind) {
  // This allocates a fresh node per resolution, never a shared singleton, even for the kinds that
  // carry no per-instance state. mustache4c requires each node of the hierarchy to be uniquely
  // identified by its pointer. Today's `mustache.c` only pushes and pops them, so sharing would
  // work, but that is an implementation detail a vendor bump can change without a word. The
  // allocation reads as an unnoticed inefficiency, so do not replace it with a shared node.
  struct Node* node = arena_new(arena, 1, struct Node);
  node->kind = kind;
  return node;
}

static struct Node* node_scalar(struct Arena* arena, struct Str value) {
  // An empty value is a present field, not a missing name. mustache4c looks a missing name up in
  // the enclosing contexts, so inside `{{#content_entries}}` in a content template, a listed entry
  // with no `modified` would show the current page's. A present empty node stops the lookup at the
  // listed entry, and `template_context_render_child_by_index` makes it false, so `{{#field}}`
  // skips it and `{{^field}}` fires.
  struct Node* node = node_alloc(arena, NODE_SCALAR);
  node->scalar = value;
  return node;
}

static struct Node* node_scalar_html(struct Arena* arena, struct Str html) {
  struct Node* node = node_scalar(arena, html);
  node->is_html = true;
  return node;
}

static struct Node* node_scalar_url(const struct ContextRender* render,
                                    struct Str root_relative_url,
                                    struct Arena* arena) {
  const struct TemplateContext* context = render->context;
  if (context->is_feed) {
    return node_scalar_url_absolute(render, root_relative_url, arena);
  }
  return node_scalar(arena, path_join(context->path_to_root, root_relative_url, arena));
}

static struct Node* node_scalar_url_absolute(const struct ContextRender* render,
                                             struct Str root_relative_url,
                                             struct Arena* arena) {
  return node_scalar(arena,
                     path_join(render->context->site_config->base_url, root_relative_url, arena));
}

static struct Node* resolve_site(const struct ContextRender* render,
                                 struct Str name,
                                 struct Arena* arena) {
  const struct SiteConfig* site_config = render->context->site_config;
  if (str_eq(name, S("title"))) {
    return node_scalar(arena, site_config->title);
  }
  if (str_eq(name, S("description"))) {
    return node_scalar(arena, site_config->description);
  }
  if (str_eq(name, S("author"))) {
    return node_scalar(arena, site_config->author);
  }
  if (str_eq(name, S("base_url"))) {
    return node_scalar(arena, site_config->base_url);
  }
  return NULL;
}

static struct Node* resolve_feed(const struct ContextRender* render,
                                 struct Str name,
                                 struct Arena* arena) {
  if (str_eq(name, S("id")) || str_eq(name, S("url"))) {
    return node_scalar_url_absolute(render, render->output_name, arena);
  }
  if (str_eq(name, S("html_url"))) {
    return node_scalar_url_absolute(render, S(""), arena);
  }
  if (str_eq(name, S("modified"))) {
    const struct TemplateContext* context = render->context;
    return node_scalar(arena, content_entry_modified_latest(context->content_entries,
                                                            context->content_entry_count));
  }
  return NULL;
}

static struct Node* resolve_entry(const struct ContextRender* render,
                                  const struct ContentEntry* entry,
                                  struct Str name,
                                  struct Arena* arena) {
  if (entry == NULL) {
    return NULL;
  }
  if (str_eq(name, S("title"))) {
    return node_scalar(arena, entry->title);
  }
  if (str_eq(name, S("date"))) {
    return node_scalar(arena, entry->date);
  }
  if (str_eq(name, S("modified"))) {
    // Every entry of a feed needs the date of its last change, as Atom's `updated` does, so in a
    // feed an entry that sets no `modified` gives its `date`. On a page it stays empty, so a
    // template can show it only for an entry that was changed.
    if (render->context->is_feed && entry->modified.len == 0) {
      return node_scalar(arena, entry->date);
    }
    return node_scalar(arena, entry->modified);
  }
  if (str_eq(name, S("description"))) {
    return node_scalar(arena, entry->description);
  }
  if (str_eq(name, S("slug"))) {
    return node_scalar(arena, entry->slug);
  }
  if (str_eq(name, S("url"))) {
    return node_scalar_url(render, str_tail(entry->url_path, 1), arena);
  }
  if (str_eq(name, S("absolute_url"))) {
    return node_scalar_url_absolute(render, str_tail(entry->url_path, 1), arena);
  }
  if (str_eq(name, S("body_html"))) {
    return node_scalar_html(arena, entry->body_html);
  }
  if (str_eq(name, S("tags"))) {
    // An empty list is a present field too, so the lookup stops here, and the list is false because
    // `template_context_render_child_by_index` has no child 0 for it.
    struct Node* tags = node_alloc(arena, NODE_TAG_LIST);
    tags->entry = entry;
    return tags;
  }
  return NULL;
}
