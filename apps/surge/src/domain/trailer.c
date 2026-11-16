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
