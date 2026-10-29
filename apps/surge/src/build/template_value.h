#ifndef SURGE_TEMPLATE_VALUE_H
#define SURGE_TEMPLATE_VALUE_H

#include <stdbool.h>
#include <stddef.h>

#include "core/arena.h"
#include "core/str.h"

struct Log;
struct TemplateSet;

/** What one value of a template's data tree holds. */
enum TemplateValueKind {
  /** Named fields, looked up by name. A section over a map enters it once. */
  TEMPLATE_VALUE_MAP,

  /** Items in order. A section over a list enters it once per item, and not at all when empty. */
  TEMPLATE_VALUE_LIST,

  /**
   * Text, escaped by `{{x}}`, and printed as it is by `{{{x}}}` only when it is HTML, as `is_html`
   * says. Empty text is false.
   */
  TEMPLATE_VALUE_TEXT,

  /** True: a section enters it once, and it prints nothing. */
  TEMPLATE_VALUE_TRUE,

  /** False: a section skips it, an inverted section enters it, and it prints nothing. */
  TEMPLATE_VALUE_FALSE,
};

/** One named field of a map. */
struct TemplateField {
  /** Name the template looks the field up by. */
  struct Str name;

  /** Value of the field. */
  const struct TemplateValue* value;
};

/**
 * One value of a template's data tree, built in an arena before a render and read-only during it.
 *
 * mustache4c looks a name that a map does not hold up in the enclosing scopes, so a map built for a
 * page holds every key of its schema at its level, with empty text or false where a value is
 * absent. Each value is its own allocation, so each is identified by its pointer, as `mustache.h`
 * asks of its nodes.
 */
struct TemplateValue {
  /** What the value holds, which selects the fields below that carry meaning. */
  enum TemplateValueKind kind;

  /** Text of a `TEMPLATE_VALUE_TEXT`. */
  struct Str text;

  /**
   * Set for a text value under a key that ends in `_html`, which a template may write unescaped.
   * The text is HTML that surge made itself, and a render fails a `{{{x}}}` or `{{& x}}` of any
   * other value, so a value surge did not make, such as a file name or a commit subject, cannot put
   * markup in a page.
   */
  bool is_html;

  /** Fields of a `TEMPLATE_VALUE_MAP`, in the order they were set. */
  struct TemplateField* fields;

  /** Number of entries in `fields`. */
  ptrdiff_t field_count;

  /** Slots allocated at `fields`. */
  ptrdiff_t field_capacity;

  /** Items of a `TEMPLATE_VALUE_LIST`. */
  const struct TemplateValue** items;

  /** Number of entries in `items`. */
  ptrdiff_t item_count;
};

/**
 * @brief Returns a new map with no fields.
 *
 * @param arena Arena that owns the map and the fields set on it later. Must not be `NULL`.
 * @return The map.
 */
struct TemplateValue* template_value_map(struct Arena* arena)
    __attribute__((nonnull(1), returns_nonnull));

/**
 * @brief Returns a new list of `count` items, each `NULL` until the caller sets it.
 *
 * @param count Number of items. Must not be negative.
 * @param arena Arena that owns the list. Must not be `NULL`.
 * @return The list, whose `items` the caller fills.
 */
struct TemplateValue* template_value_list(ptrdiff_t count, struct Arena* arena)
    __attribute__((nonnull(2), returns_nonnull));

/**
 * @brief Returns a new text value.
 *
 * @param text  Text, borrowed for as long as the value is used.
 * @param arena Arena that owns the value. Must not be `NULL`.
 * @return The value.
 */
struct TemplateValue* template_value_text(struct Str text, struct Arena* arena)
    __attribute__((nonnull(2), returns_nonnull));

/**
 * @brief Returns a new true or false value.
 *
 * @param is_true Whether the value is true.
 * @param arena   Arena that owns the value. Must not be `NULL`.
 * @return The value.
 */
struct TemplateValue* template_value_bool(bool is_true, struct Arena* arena)
    __attribute__((nonnull(2), returns_nonnull));

/**
 * @brief Adds the field `name` to `map`.
 *
 * @param map   Map to add to, which holds no field of that name. Must not be `NULL`.
 * @param name  Field name, borrowed for as long as the map is used.
 * @param value Value of the field. Must not be `NULL`.
 * @param arena Arena that owns the map's fields, the one the map came from. Must not be `NULL`.
 */
void template_value_set(struct TemplateValue* map,
                        struct Str name,
                        const struct TemplateValue* value,
                        struct Arena* arena) __attribute__((nonnull(1, 3, 4)));

/**
 * @brief Adds the text field `name` to `map`, as `template_value_set` does, marked as HTML when
 *        `name` ends in `_html`.
 *
 * @param map   Map to add to. Must not be `NULL`.
 * @param name  Field name.
 * @param text  Text of the field, borrowed.
 * @param arena Arena of the map. Must not be `NULL`.
 */
void template_value_set_text(struct TemplateValue* map,
                             struct Str name,
                             struct Str text,
                             struct Arena* arena) __attribute__((nonnull(1, 4)));

/**
 * @brief Adds the true or false field `name` to `map`, as `template_value_set` does.
 *
 * @param map     Map to add to. Must not be `NULL`.
 * @param name    Field name.
 * @param is_true Whether the field is true.
 * @param arena   Arena of the map. Must not be `NULL`.
 */
void template_value_set_bool(struct TemplateValue* map,
                             struct Str name,
                             bool is_true,
                             struct Arena* arena) __attribute__((nonnull(1, 4)));

/**
 * @brief Finds the field `name` of a map.
 *
 * @param map  Map to search. Must not be `NULL`.
 * @param name Field name.
 * @return The field's value, or `NULL` when `map` is not a map or holds no such field.
 */
const struct TemplateValue* template_value_find(const struct TemplateValue* map, struct Str name)
    __attribute__((nonnull(1)));

/**
 * @brief Reports whether a section over `value` enters it at least once.
 *
 * @param value Value to judge. Must not be `NULL`.
 * @return `true` for a map, a non-empty list, non-empty text and true, `false` otherwise.
 */
bool template_value_is_truthy(const struct TemplateValue* value) __attribute__((nonnull(1)));

/**
 * @brief Renders the page template `page_name` of `set` over the tree `root`.
 *
 * The render is `template_set_render`'s, under `TEMPLATE_RENDER_OUTPUT_LEN_MAX`. A text under a key
 * that ends in `_html` is HTML, which a triple mustache writes as it is, and a section enters a
 * value as `template_value_is_truthy` judges it. True writes `true`, and false writes nothing.
 *
 * @param set         Loaded templates. Must not be `NULL`.
 * @param page_name   Name of a page template of `set`.
 * @param root        Tree the names resolve against. Must not be `NULL`.
 * @param output_name Output the render is for, relative to `output_dir`, which a failure names.
 * @param perm        Arena that owns the HTML, where nothing else of the render allocates, so the
 *                    output grows in place. A failed render may leave its partial output there.
 *                    Must not be `NULL`.
 * @param scratch     Arena for the render's temporaries. Must not overlap `perm`.
 * @param html_out    Receives the HTML on success. Must not be `NULL`.
 * @param log         Log that receives the render's first failure. Must not be `NULL`.
 * @return `0` on success, or `-1` on failure.
 */
int template_value_render(const struct TemplateSet* set,
                          struct Str page_name,
                          const struct TemplateValue* root,
                          struct Str output_name,
                          struct Arena* perm,
                          struct Arena scratch,
                          struct Str* html_out,
                          struct Log* log) __attribute__((nonnull(1, 3, 5, 7, 8)));

#endif
