#include "build/template_value.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/arena.h"
#include "core/str.h"
#include "templates/template_set.h"

/**
 * @brief Returns a new value of `kind` with no payload.
 *
 * @param kind  Kind of the value.
 * @param arena Arena that owns the value. Must not be `NULL`.
 * @return The value.
 */
static struct TemplateValue* template_value_new(enum TemplateValueKind kind, struct Arena* arena)
    __attribute__((nonnull(2), returns_nonnull));

/**
 * @brief Returns the root of a render's tree, as `TemplateRootFn`.
 *
 * @param arena    Arena of the render. Unused, since the tree is built before the render.
 * @param userdata The root `struct TemplateValue`.
 * @return The root.
 */
static const void* template_value_render_root(struct Arena* arena, void* userdata);

/**
 * @brief Returns the field `name` of a map, as `TemplateChildByNameFn`.
 *
 * @param node     Parent `struct TemplateValue`.
 * @param name     Field name.
 * @param arena    Arena of the render. Unused.
 * @param userdata Unused.
 * @return The field's value, or `NULL`.
 */
static const void* template_value_render_child_by_name(const void* node,
                                                       struct Str name,
                                                       struct Arena* arena,
                                                       void* userdata);

/**
 * @brief Returns a list's item at `index`, or a value that a section enters itself at index `0`, as
 *        `TemplateChildByIndexFn`.
 *
 * @param node     Parent `struct TemplateValue`.
 * @param index    Zero-based index.
 * @param arena    Arena of the render. Unused.
 * @param userdata Unused.
 * @return The item, or `NULL`.
 */
static const void* template_value_render_child_by_index(const void* node,
                                                        ptrdiff_t index,
                                                        struct Arena* arena,
                                                        void* userdata);

/**
 * @brief Returns what a tag of a value writes, as `TemplateTextFn`: a text's text, and nothing for
 *        another value.
 *
 * @param node     The `struct TemplateValue`.
 * @param userdata Unused.
 * @return The value's text, and whether it is HTML.
 */
static struct TemplateText template_value_render_text(const void* node, void* userdata);

struct TemplateValue* template_value_map(struct Arena* arena) {
  return template_value_new(TEMPLATE_VALUE_MAP, arena);
}

struct TemplateValue* template_value_list(ptrdiff_t count, struct Arena* arena) {
  assert(count >= 0);
  struct TemplateValue* list = template_value_new(TEMPLATE_VALUE_LIST, arena);
  list->items = arena_new(arena, count, const struct TemplateValue*);
  list->item_count = count;
  return list;
}

struct TemplateValue* template_value_text(struct Str text, struct Arena* arena) {
  struct TemplateValue* value = template_value_new(TEMPLATE_VALUE_TEXT, arena);
  value->text = text;
  return value;
}

struct TemplateValue* template_value_bool(bool is_true, struct Arena* arena) {
  return template_value_new(is_true ? TEMPLATE_VALUE_TRUE : TEMPLATE_VALUE_FALSE, arena);
}

void template_value_set(struct TemplateValue* map,
                        struct Str name,
                        const struct TemplateValue* value,
                        struct Arena* arena) {
  assert(map->kind == TEMPLATE_VALUE_MAP);
  assert(template_value_find(map, name) == NULL);
  if (map->field_count == map->field_capacity) {
    map->fields = arena_grow(arena, map->fields, &map->field_capacity,
                             (ptrdiff_t)sizeof(struct TemplateField),
                             (ptrdiff_t)_Alignof(struct TemplateField));
  }
  map->fields[map->field_count++] = (struct TemplateField){.name = name, .value = value};
}

void template_value_set_text(struct TemplateValue* map,
                             struct Str name,
                             struct Str text,
                             struct Arena* arena) {
  struct TemplateValue* value = template_value_text(text, arena);
  // This setter is the one that sees a text and its key together, so it marks the HTML. A text from
  // `template_value_text` that `template_value_set` adds stays unmarked, and so escaped.
  value->is_html = str_endswith(name, S("_html"));
  template_value_set(map, name, value, arena);
}

void template_value_set_bool(struct TemplateValue* map,
                             struct Str name,
                             bool is_true,
                             struct Arena* arena) {
  template_value_set(map, name, template_value_bool(is_true, arena), arena);
}

const struct TemplateValue* template_value_find(const struct TemplateValue* map, struct Str name) {
  if (map->kind != TEMPLATE_VALUE_MAP) {
    return NULL;
  }
  for (ptrdiff_t i = 0; i < map->field_count; i++) {
    if (str_eq(map->fields[i].name, name)) {
      return map->fields[i].value;
    }
  }
  return NULL;
}

bool template_value_is_truthy(const struct TemplateValue* value) {
  switch (value->kind) {
    case TEMPLATE_VALUE_MAP:
    case TEMPLATE_VALUE_TRUE:
      return true;
    case TEMPLATE_VALUE_LIST:
      return value->item_count > 0;
    case TEMPLATE_VALUE_TEXT:
      return value->text.len > 0;
    case TEMPLATE_VALUE_FALSE:
      return false;
  }
  return false;
}

int template_value_render(const struct TemplateSet* set,
                          struct Str page_name,
                          const struct TemplateValue* root,
                          struct Str output_name,
                          struct Arena* perm,
                          struct Arena scratch,
                          struct Str* html_out,
                          struct Log* log) {
  const struct TemplateData data = {
      .root = template_value_render_root,
      .child_by_name = template_value_render_child_by_name,
      .child_by_index = template_value_render_child_by_index,
      .text = template_value_render_text,
      // The callbacks only read the tree, so the `const` is dropped on purpose.
      .userdata = (void*)(uintptr_t)root,
  };
  return template_set_render(set, page_name, &data, output_name, TEMPLATE_RENDER_OUTPUT_LEN_MAX,
                             perm, scratch, html_out, log);
}

static struct TemplateValue* template_value_new(enum TemplateValueKind kind, struct Arena* arena) {
  struct TemplateValue* value = arena_new(arena, 1, struct TemplateValue);
  value->kind = kind;
  return value;
}

static const void* template_value_render_root(struct Arena* arena, void* userdata) {
  (void)arena;
  return userdata;
}

static const void* template_value_render_child_by_name(const void* node,
                                                       struct Str name,
                                                       struct Arena* arena,
                                                       void* userdata) {
  (void)arena;
  (void)userdata;
  return template_value_find(node, name);
}

static const void* template_value_render_child_by_index(const void* node,
                                                        ptrdiff_t index,
                                                        struct Arena* arena,
                                                        void* userdata) {
  (void)arena;
  (void)userdata;
  const struct TemplateValue* value = node;
  if (value->kind == TEMPLATE_VALUE_LIST) {
    return index < value->item_count ? value->items[index] : NULL;
  }
  // Per the Mustache specification a single value is iterable once, so a section enters a value
  // that is true at index `0`.
  return index == 0 && template_value_is_truthy(value) ? node : NULL;
}

static struct TemplateText template_value_render_text(const void* node, void* userdata) {
  (void)userdata;
  const struct TemplateValue* value = node;
  // Every value, a list included, answers whether it is HTML, so an unescaped tag fails for any
  // value that is not HTML text. A true value writes `true`, as fram's flags do, so a flag works as
  // text too.
  if (value->kind == TEMPLATE_VALUE_TRUE) {
    return (struct TemplateText){.text = S("true")};
  }
  if (value->kind != TEMPLATE_VALUE_TEXT) {
    return (struct TemplateText){0};
  }
  return (struct TemplateText){.text = value->text, .is_html = value->is_html};
}
