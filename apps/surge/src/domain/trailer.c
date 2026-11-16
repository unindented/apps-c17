#include "domain/trailer.h"

#include <stdbool.h>
#include <stddef.h>

#include "core/arena.h"
#include "core/ascii.h"
#include "core/str.h"
#include "domain/repo_model.h"

/**
 * @brief Reports whether an earlier trailer of `trailers` than `index` has the value of the one at
 *        `index`, byte for byte, and passes `is_listed`.
 *
 * @param trailers  The trailers. Must not be `NULL`.
 * @param index     Index of the trailer to look before.
 * @param is_listed Whether each trailer is one the caller lists, parallel to `trailers`. Must not
 *                  be `NULL` unless `index` is `0`.
 * @return `true` when such an earlier trailer exists.
 */
static bool has_listed_value_before(const struct Trailer* trailers,
                                    ptrdiff_t index,
                                    const bool* is_listed) __attribute__((nonnull(1)));

/**
 * @brief Reports whether `match` marks a trailer: its key is one of `match`'s keys and its value
 *        holds one of `match`'s values, both in any ASCII letter case.
 *
 * @param trailer The trailer. Must not be `NULL`.
 * @param match   Which trailers mark a commit. Must not be `NULL`.
 * @return `true` when the trailer is marked.
 */
static bool is_marked(const struct Trailer* trailer, const struct TrailerMatch* match)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Reports whether `text` holds `needle`, with ASCII letters folded to lower case.
 *
 * @param text   Text to search.
 * @param needle Text to find, not empty.
 * @return `true` when `needle` is in `text`.
 */
static bool has_folded(struct Str text, struct Str needle);

struct Str trailer_name(struct Str value) {
  if (!str_endswith(value, S(">"))) {
    return value;
  }
  ptrdiff_t open = value.len - 1;
  while (open > 0 && value.data[open] != '<') {
    open--;
  }
  if (value.data[open] != '<') {
    return value;
  }
  struct Str name = str_head(value, open);
  while (name.len > 0 && ascii_is_space((unsigned char)name.data[name.len - 1])) {
    name = str_head(name, name.len - 1);
  }
  return name;
}

void trailer_co_authors(const struct Trailer* trailers,
                        ptrdiff_t trailer_count,
                        struct Arena* arena,
                        struct Str** names_out,
                        ptrdiff_t* name_count_out) {
  struct Str* names = arena_new(arena, trailer_count, struct Str);
  bool* is_co_author = arena_new(arena, trailer_count, bool);
  ptrdiff_t count = 0;
  for (ptrdiff_t i = 0; i < trailer_count; i++) {
    const struct Str name = trailer_name(trailers[i].value);
    is_co_author[i] = str_eq_folded(trailers[i].key, TRAILER_CO_AUTHOR_KEY) && name.len > 0;
    if (is_co_author[i] && !has_listed_value_before(trailers, i, is_co_author)) {
      names[count++] = name;
    }
  }
  *names_out = names;
  *name_count_out = count;
}

void trailer_llm_tools(const struct Trailer* trailers,
                       ptrdiff_t trailer_count,
                       const struct TrailerMatch* match,
                       struct Arena* arena,
                       struct Str** tools_out,
                       ptrdiff_t* tool_count_out) {
  struct Str* tools = arena_new(arena, trailer_count, struct Str);
  bool* is_tool = arena_new(arena, trailer_count, bool);
  ptrdiff_t count = 0;
  for (ptrdiff_t i = 0; i < trailer_count; i++) {
    is_tool[i] = is_marked(&trailers[i], match);
    if (is_tool[i] && !has_listed_value_before(trailers, i, is_tool)) {
      const struct Str name = trailer_name(trailers[i].value);
      tools[count++] = name.len > 0 ? name : trailers[i].value;
    }
  }
  *tools_out = tools;
  *tool_count_out = count;
}

static bool has_listed_value_before(const struct Trailer* trailers,
                                    ptrdiff_t index,
                                    const bool* is_listed) {
  for (ptrdiff_t i = 0; i < index; i++) {
    if (is_listed[i] && str_eq(trailers[i].value, trailers[index].value)) {
      return true;
    }
  }
  return false;
}

static bool is_marked(const struct Trailer* trailer, const struct TrailerMatch* match) {
  bool is_key = false;
  for (ptrdiff_t i = 0; i < match->key_count && !is_key; i++) {
    is_key = str_eq_folded(trailer->key, match->keys[i]);
  }
  bool has_value = false;
  for (ptrdiff_t i = 0; i < match->value_count && is_key && !has_value; i++) {
    has_value = has_folded(trailer->value, match->values[i]);
  }
  return has_value;
}

static bool has_folded(struct Str text, struct Str needle) {
  for (ptrdiff_t start = 0; start + needle.len <= text.len; start++) {
    if (str_eq_folded(str_head(str_tail(text, start), needle.len), needle)) {
      return true;
    }
  }
  return false;
}
