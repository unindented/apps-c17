#include "build/template_context.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "core/arena.h"
#include "core/path.h"
#include "core/str.h"
#include "domain/album.h"
#include "domain/fram_version.h"
#include "domain/media_item.h"
#include "domain/site_config.h"
#include "templates/template_set.h"

/** Size of the stack buffer `node_scalar_number` formats one decimal `ptrdiff_t` into. */
enum { SCALAR_NUMBER_SIZE = 32 };

// A decimal `ptrdiff_t` at its widest, sign included, so a number formatted in `node_scalar_number`
// cannot truncate.
_Static_assert(SCALAR_NUMBER_SIZE > sizeof("-9223372036854775808") - 1,
               "scalar number buffer must hold the widest ptrdiff_t");

/** Name of each media kind as a template sees it, indexed by `enum MediaKind`. */
static const struct Str MEDIA_KIND_NAMES[] = {
    [MEDIA_KIND_IMAGE] = STR_INIT("image"),
    [MEDIA_KIND_VIDEO] = STR_INIT("video"),
};

/** Kind of a node in the data that a render walks. */
enum NodeKind {
  /** Top-level lookup context: `site.*`, the page's lists, and the current album's fields. */
  NODE_ROOT,

  /** Site metadata subtree resolving `site.*` names, from the gallery configuration. */
  NODE_SITE,

  /** One album, resolving bare album field names. */
  NODE_ALBUM,

  /** The rendered album's `sub_albums`, iterated by index. */
  NODE_ALBUM_LIST,

  /** One media item, resolving bare media field names and derivative names. */
  NODE_MEDIA,

  /** One derivative of a media item, resolving its URL and presented dimensions. */
  NODE_DERIVATIVE,

  /** The rendered album's `media`, iterated by index. */
  NODE_MEDIA_LIST,

  /** The rendered album's ancestors from the root down to itself, iterated by index. */
  NODE_BREADCRUMB_LIST,

  /** One album of `breadcrumbs`, resolving `is_last` and its `url`, and its other album fields. */
  NODE_BREADCRUMB,

  /** A terminal string value. */
  NODE_SCALAR,
};

/** A node in the data. Nodes are arena-owned and live for one render. */
struct Node {
  /** Selects which of the fields below carry meaning for this node. */
  enum NodeKind kind;

  /** Album a `NODE_ALBUM` or a `NODE_BREADCRUMB` presents. */
  const struct Album* album;

  /** Backing media item for `NODE_MEDIA`. */
  const struct MediaItem* media;

  /** Backing derivative for `NODE_DERIVATIVE`. */
  const struct MediaDerivative* derivative;

  /**
   * Borrowed text for `NODE_SCALAR`, empty for an absent value or a false flag.
   * `template_context_render_child_by_index` makes an empty scalar false in a section.
   */
  struct Str scalar;
};

/** State of one render, which every callback receives as its `userdata`. */
struct ContextRender {
  /** Borrowed values visible to template variables. */
  const struct TemplateContext* context;
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
 * At the root a name selects the `site.*` subtree, the current `album`, the `sub_albums`, `media`
 * and `breadcrumbs` lists, the `generator` string that names this tool and its version, the page's
 * `path_to_root`, or otherwise a field of the current album.
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
 * Iterates the album, media and breadcrumb lists by index. Any other value is iterable once at
 * index `0`, unless it is an empty scalar.
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
 * @return The node's text. fram makes no HTML, so no text is HTML.
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
 * @brief Returns a scalar node holding `value` in decimal.
 *
 * Copies the formatted digits into the render arena, because a scalar node borrows its text for the
 * rest of the render.
 *
 * @param arena Arena that owns the node. Must not be `NULL`.
 * @param value Number to format.
 * @return A scalar node.
 */
static struct Node* node_scalar_number(struct Arena* arena, ptrdiff_t value)
    __attribute__((nonnull(1), returns_nonnull));

/**
 * @brief Returns a scalar node holding an output-root-relative URL rebased onto the current page.
 *
 * Joins the URL behind the rendered album's `path_to_root`, so a link resolves from the page's own
 * directory rather than only from the output root.
 *
 * @param render            Render supplying the rendered album. Must not be `NULL`.
 * @param root_relative_url Output-root-relative URL, already URL-safe because it is built from
 *                          slugs.
 * @param arena             Arena that owns the node and the URL. Must not be `NULL`.
 * @return A scalar node.
 */
static struct Node* node_scalar_url(const struct ContextRender* render,
                                    struct Str root_relative_url,
                                    struct Arena* arena)
    __attribute__((nonnull(1, 3), returns_nonnull));

/**
 * @brief Returns a scalar node holding an output-root-relative URL joined behind `base_url`, or
 *        empty text when the gallery sets no `base_url`.
 *
 * @param render            Render supplying the gallery configuration. Must not be `NULL`.
 * @param root_relative_url Output-root-relative URL, already URL-safe because it is built from
 *                          slugs.
 * @param arena             Arena that owns the node and the URL. Must not be `NULL`.
 * @return A scalar node.
 */
static struct Node* node_scalar_url_absolute(const struct ContextRender* render,
                                             struct Str root_relative_url,
                                             struct Arena* arena)
    __attribute__((nonnull(1, 3), returns_nonnull));

/**
 * @brief Resolves a `site.*` name against the render's gallery configuration.
 *
 * @param render Render holding the gallery configuration. Must not be `NULL`.
 * @param name   Requested field name.
 * @param arena  Arena that owns the node. Must not be `NULL`.
 * @return The resolved node, or `NULL` when the name is unmatched.
 */
static struct Node* resolve_site(const struct ContextRender* render,
                                 struct Str name,
                                 struct Arena* arena) __attribute__((nonnull(1, 3)));

/**
 * @brief Resolves a bare field name against an album.
 *
 * `has_media` and `has_sub_albums` resolve to a truthy scalar only when the album holds that
 * content, so a section over either renders exactly when it is non-empty. A URL resolves already
 * rebased onto the page being rendered. An absent `cover` or `parent` resolves to an empty scalar,
 * so the name does not fall through to an enclosing album.
 *
 * @param render Render. Must not be `NULL`.
 * @param album  Album to resolve against. Must not be `NULL`.
 * @param name   Requested field name.
 * @param arena  Arena that owns the node. Must not be `NULL`.
 * @return The resolved node, or `NULL` when the name is unmatched.
 */
static struct Node* resolve_album(const struct ContextRender* render,
                                  const struct Album* album,
                                  struct Str name,
                                  struct Arena* arena) __attribute__((nonnull(1, 2, 4)));

/**
 * @brief Resolves a bare field name against an album of `breadcrumbs`.
 *
 * `is_last` is truthy only for the rendered album on its own page, the trail's last crumb, and that
 * crumb's `url` is empty, so a template writes the page it is on without a link to itself, as
 * surge's breadcrumbs do. Every other name resolves as for the album.
 *
 * @param render Render. Must not be `NULL`.
 * @param album  Album of the crumb. Must not be `NULL`.
 * @param name   Requested field name.
 * @param arena  Arena that owns the node. Must not be `NULL`.
 * @return The resolved node, or `NULL` when the name is unmatched.
 */
static struct Node* resolve_breadcrumb(const struct ContextRender* render,
                                       const struct Album* album,
                                       struct Str name,
                                       struct Arena* arena) __attribute__((nonnull(1, 2, 4)));

/**
 * @brief Resolves a bare field name against a media item, including its derivative names.
 *
 * A name matching no media field is matched against the item's configured derivative names, so
 * `{{m.url}}` reaches the derivative named `m`. A derivative name is one of the fixed sizes, which
 * no other key of the data uses, so it hides none. `url` is the copied original's. `kind` names the
 * source kind, as surge names the kind of a tree entry, and `is_image` and `is_video` resolve to a
 * truthy scalar only for that kind, so a section over either renders exactly for it. `duration` is
 * empty for an image.
 *
 * @param render Render. Must not be `NULL`.
 * @param media  Media item to resolve against. Must not be `NULL`.
 * @param name   Requested field name.
 * @param arena  Arena that owns the node. Must not be `NULL`.
 * @return The resolved node, or `NULL` when the name is unmatched.
 */
static struct Node* resolve_media(const struct ContextRender* render,
                                  const struct MediaItem* media,
                                  struct Str name,
                                  struct Arena* arena) __attribute__((nonnull(1, 2, 4)));

/**
 * @brief Resolves a bare field name against one derivative of a media item.
 *
 * @param render     Render. Must not be `NULL`.
 * @param derivative Derivative to resolve against. Must not be `NULL`.
 * @param name       Requested field name.
 * @param arena      Arena that owns the node. Must not be `NULL`.
 * @return The resolved node, or `NULL` when the name is unmatched.
 */
static struct Node* resolve_derivative(const struct ContextRender* render,
                                       const struct MediaDerivative* derivative,
                                       struct Str name,
                                       struct Arena* arena) __attribute__((nonnull(1, 2, 4)));

int template_context_render(const struct TemplateSet* set,
                            struct Str template_name,
                            const struct TemplateContext* context,
                            struct Str output_name,
                            struct Arena* perm,
                            struct Arena scratch,
                            struct Str* html_out,
                            struct Log* log) {
  // Nothing in this file may allocate into an arena reached through `context`, because renders run
  // concurrently in pool jobs. Every node goes into the render's scratch, which the callbacks get.
  struct ContextRender render = {.context = context};
  const struct TemplateData data = {
      .root = template_context_render_root,
      .child_by_name = template_context_render_child_by_name,
      .child_by_index = template_context_render_child_by_index,
      .text = template_context_render_text,
      .userdata = &render,
  };
  return template_set_render(set, template_name, &data, output_name, TEMPLATE_RENDER_OUTPUT_LEN_MAX,
                             perm, scratch, html_out, log);
}

static const void* template_context_render_root(struct Arena* arena, void* userdata) {
  (void)userdata;
  return node_alloc(arena, NODE_ROOT);
}

static const void* template_context_render_child_by_name(const void* node,
                                                         struct Str name,
                                                         struct Arena* arena,
                                                         void* userdata) {
  const struct ContextRender* render = userdata;
  const struct Node* parent = node;
  switch (parent->kind) {
    case NODE_ROOT:
      if (str_eq(name, S("site"))) {
        return node_alloc(arena, NODE_SITE);
      }
      if (str_eq(name, S("album"))) {
        struct Node* child = node_alloc(arena, NODE_ALBUM);
        child->album = render->context->album_current;
        return child;
      }
      if (str_eq(name, S("sub_albums"))) {
        return node_alloc(arena, NODE_ALBUM_LIST);
      }
      if (str_eq(name, S("media"))) {
        return node_alloc(arena, NODE_MEDIA_LIST);
      }
      if (str_eq(name, S("breadcrumbs"))) {
        return node_alloc(arena, NODE_BREADCRUMB_LIST);
      }
      if (str_eq(name, S("generator"))) {
        return node_scalar(arena, fram_generator_string());
      }
      if (str_eq(name, S("path_to_root"))) {
        return node_scalar(arena, render->context->album_current->path_to_root);
      }
      return resolve_album(render, render->context->album_current, name, arena);
    case NODE_SITE:
      return resolve_site(render, name, arena);
    case NODE_ALBUM:
      return resolve_album(render, parent->album, name, arena);
    case NODE_BREADCRUMB:
      return resolve_breadcrumb(render, parent->album, name, arena);
    case NODE_MEDIA:
      return resolve_media(render, parent->media, name, arena);
    case NODE_DERIVATIVE:
      return resolve_derivative(render, parent->derivative, name, arena);
    case NODE_ALBUM_LIST:
    case NODE_MEDIA_LIST:
    case NODE_BREADCRUMB_LIST:
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
  const struct Album* current = render->context->album_current;
  if (parent->kind == NODE_ALBUM_LIST) {
    if (index >= current->sub_album_count) {
      return NULL;
    }
    struct Node* child = node_alloc(arena, NODE_ALBUM);
    child->album = current->sub_albums[index];
    return child;
  }
  if (parent->kind == NODE_MEDIA_LIST) {
    if (index >= current->media_count) {
      return NULL;
    }
    struct Node* child = node_alloc(arena, NODE_MEDIA);
    child->media = current->media[index];
    return child;
  }
  if (parent->kind == NODE_BREADCRUMB_LIST) {
    if (index > current->depth) {
      return NULL;
    }
    const struct Album* crumb = current;
    for (ptrdiff_t remaining = current->depth - index; remaining > 0; remaining--) {
      crumb = crumb->parent;
    }
    struct Node* child = node_alloc(arena, NODE_BREADCRUMB);
    child->album = crumb;
    return child;
  }
  // An empty scalar is false, so `{{#field}}` skips an absent value or a false flag and
  // `{{^field}}` fires.
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
  if (text_node->kind != NODE_SCALAR) {
    return (struct TemplateText){0};
  }
  return (struct TemplateText){.text = text_node->scalar};
}

static struct Node* node_alloc(struct Arena* arena, enum NodeKind kind) {
  // This allocates a fresh node per resolution, never a shared singleton, even for the kinds that
  // carry no per-instance state. `vendor/mustache4c/mustache.h` requires each node of the hierarchy
  // to be uniquely identified by its pointer. Today's `mustache.c` only pushes and pops them, so
  // sharing would work, but that is an implementation detail a vendor bump can change without a
  // word. The allocation reads as an unnoticed inefficiency, so do not replace it with a shared
  // node.
  struct Node* node = arena_new(arena, 1, struct Node);
  node->kind = kind;
  return node;
}

static struct Node* node_scalar(struct Arena* arena, struct Str value) {
  // An empty value is a present field, not a missing name. mustache4c looks a missing name up in
  // the enclosing contexts, so inside `{{#sub_albums}}`, an album with no `cover` would show the
  // current album's. A present empty node stops the lookup at the listed album, and
  // `template_context_render_child_by_index` makes it false, so `{{#field}}` skips it and
  // `{{^field}}` fires.
  struct Node* node = node_alloc(arena, NODE_SCALAR);
  node->scalar = value;
  return node;
}

static struct Node* node_scalar_number(struct Arena* arena, ptrdiff_t value) {
  char number[SCALAR_NUMBER_SIZE];
  const int number_len = snprintf(number, sizeof(number), "%td", value);
  return node_scalar(arena, str_clone((struct Str){number, number_len}, arena));
}

static struct Node* node_scalar_url(const struct ContextRender* render,
                                    struct Str root_relative_url,
                                    struct Arena* arena) {
  return node_scalar(
      arena, path_join(render->context->album_current->path_to_root, root_relative_url, arena));
}

static struct Node* node_scalar_url_absolute(const struct ContextRender* render,
                                             struct Str root_relative_url,
                                             struct Arena* arena) {
  // Without a `base_url` a gallery has no absolute address, so `{{#absolute_url}}` is skipped.
  const struct Str base_url = render->context->site_config->base_url;
  if (base_url.len == 0) {
    return node_scalar(arena, S(""));
  }
  return node_scalar(arena, path_join(base_url, root_relative_url, arena));
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

static struct Node* resolve_album(const struct ContextRender* render,
                                  const struct Album* album,
                                  struct Str name,
                                  struct Arena* arena) {
  if (str_eq(name, S("title"))) {
    return node_scalar(arena, album->title);
  }
  if (str_eq(name, S("url"))) {
    return node_scalar_url(render, album->url_path, arena);
  }
  if (str_eq(name, S("absolute_url"))) {
    return node_scalar_url_absolute(render, album->url_path, arena);
  }
  if (str_eq(name, S("media_count_total"))) {
    return node_scalar_number(arena, album->media_count_total);
  }
  if (str_eq(name, S("is_media_count_total_one"))) {
    return node_scalar(arena, album->media_count_total == 1 ? S("true") : S(""));
  }
  if (str_eq(name, S("cover"))) {
    if (album->cover == NULL) {
      return node_scalar(arena, S(""));
    }
    struct Node* cover = node_alloc(arena, NODE_MEDIA);
    cover->media = album->cover;
    return cover;
  }
  if (str_eq(name, S("has_media"))) {
    return node_scalar(arena, album->media_count == 0 ? S("") : S("true"));
  }
  if (str_eq(name, S("has_sub_albums"))) {
    return node_scalar(arena, album->sub_album_count == 0 ? S("") : S("true"));
  }
  if (str_eq(name, S("parent"))) {
    if (album->parent == NULL) {
      return node_scalar(arena, S(""));
    }
    struct Node* parent = node_alloc(arena, NODE_ALBUM);
    parent->album = album->parent;
    return parent;
  }
  return NULL;
}

static struct Node* resolve_breadcrumb(const struct ContextRender* render,
                                       const struct Album* album,
                                       struct Str name,
                                       struct Arena* arena) {
  // An aggregate renders against the root album without being its page, so no crumb is last there.
  const struct TemplateContext* context = render->context;
  const bool is_last = context->is_album_page && album == context->album_current;
  if (str_eq(name, S("is_last"))) {
    return node_scalar(arena, is_last ? S("true") : S(""));
  }
  if (str_eq(name, S("url")) && is_last) {
    return node_scalar(arena, S(""));
  }
  return resolve_album(render, album, name, arena);
}

static struct Node* resolve_media(const struct ContextRender* render,
                                  const struct MediaItem* media,
                                  struct Str name,
                                  struct Arena* arena) {
  if (str_eq(name, S("title"))) {
    return node_scalar(arena, media->title);
  }
  if (str_eq(name, S("kind"))) {
    return node_scalar(arena, MEDIA_KIND_NAMES[media->kind]);
  }
  if (str_eq(name, S("is_video"))) {
    return node_scalar(arena, media->kind == MEDIA_KIND_VIDEO ? S("true") : S(""));
  }
  if (str_eq(name, S("is_image"))) {
    return node_scalar(arena, media->kind == MEDIA_KIND_IMAGE ? S("true") : S(""));
  }
  if (str_eq(name, S("url"))) {
    return node_scalar_url(render, media->original_url, arena);
  }
  if (str_eq(name, S("absolute_url"))) {
    return node_scalar_url_absolute(render, media->original_url, arena);
  }
  if (str_eq(name, S("duration"))) {
    // An image has no length, so its `duration` is empty, not a truthy `0`.
    if (media->kind != MEDIA_KIND_VIDEO) {
      return node_scalar(arena, S(""));
    }
    return node_scalar_number(arena, media->duration_ms);
  }
  for (ptrdiff_t i = 0; i < media->derivative_count; i++) {
    const struct MediaDerivative* derivative = &media->derivatives[i];
    if (str_eq(name, derivative->name)) {
      struct Node* child = node_alloc(arena, NODE_DERIVATIVE);
      child->derivative = derivative;
      return child;
    }
  }
  return NULL;
}

static struct Node* resolve_derivative(const struct ContextRender* render,
                                       const struct MediaDerivative* derivative,
                                       struct Str name,
                                       struct Arena* arena) {
  if (str_eq(name, S("url"))) {
    return node_scalar_url(render, derivative->url, arena);
  }
  if (str_eq(name, S("absolute_url"))) {
    return node_scalar_url_absolute(render, derivative->url, arena);
  }
  if (str_eq(name, S("width"))) {
    return node_scalar_number(arena, derivative->width_px);
  }
  if (str_eq(name, S("height"))) {
    return node_scalar_number(arena, derivative->height_px);
  }
  return NULL;
}
