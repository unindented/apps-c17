#ifndef SURGE_TRAILER_H
#define SURGE_TRAILER_H

#include <stddef.h>

#include "core/str.h"

struct Arena;
struct Trailer;

/**
 * Which trailers mark a commit LLM-assisted: a trailer whose key is one of `keys` and whose value,
 * address included, holds one of `values`, both compared in any ASCII letter case.
 */
struct TrailerMatch {
  /** Keys of the trailers looked at, each not empty. */
  const struct Str* keys;

  /** Number of entries in `keys`. */
  ptrdiff_t key_count;

  /** Texts looked for in their values, each not empty. */
  const struct Str* values;

  /** Number of entries in `values`. */
  ptrdiff_t value_count;
};

/** Key of the trailers that name a commit's co-authors, which matches in any ASCII letter case. */
#define TRAILER_CO_AUTHOR_KEY S("Co-authored-by")

/**
 * @brief Returns a trailer value without a `<…>` part at its end, such as the address of
 *        `Grace Hopper <grace@example.org>`, and without the white space before that part.
 *
 * @param value A trailer value.
 * @return The value without that part, which points into `value`, or `value` when it ends in none.
 */
struct Str trailer_name(struct Str value);

/**
 * @brief Lists the co-authors of a commit: the `trailer_name` of each `Co-authored-by` trailer, its
 *        key matched in any ASCII letter case, in message order.
 *
 * A trailer whose value repeats an earlier one's, byte for byte, and one whose name is empty, such
 * as a bare `<address>`, add no name.
 *
 * @param trailers       The commit's trailers, in message order. Must not be `NULL` unless
 *                       `trailer_count` is `0`.
 * @param trailer_count  Number of entries in `trailers`.
 * @param arena          Arena that owns the list. The names point into `trailers`. Must not be
 *                       `NULL`.
 * @param names_out      Receives the names. Must not be `NULL`.
 * @param name_count_out Receives the number of names. Must not be `NULL`.
 */
void trailer_co_authors(const struct Trailer* trailers,
                        ptrdiff_t trailer_count,
                        struct Arena* arena,
                        struct Str** names_out,
                        ptrdiff_t* name_count_out) __attribute__((nonnull(3, 4, 5)));

/**
 * @brief Lists the LLM tools of a commit: for each trailer that `match` marks, in message order,
 *        its `trailer_name`, or its whole value when that name is empty.
 *
 * A trailer whose value repeats an earlier marked one's, byte for byte, adds no tool. Only the
 * trailers are read, so a tool that the subject, the body or the author names marks nothing.
 *
 * @param trailers       The commit's trailers, in message order. Must not be `NULL` unless
 *                       `trailer_count` is `0`.
 * @param trailer_count  Number of entries in `trailers`.
 * @param match          Which trailers mark a commit. Must not be `NULL`.
 * @param arena          Arena that owns the list. The tools point into `trailers`. Must not be
 *                       `NULL`.
 * @param tools_out      Receives the tools. Must not be `NULL`.
 * @param tool_count_out Receives the number of tools, which is positive exactly when the commit is
 *                       LLM-assisted. Must not be `NULL`.
 */
void trailer_llm_tools(const struct Trailer* trailers,
                       ptrdiff_t trailer_count,
                       const struct TrailerMatch* match,
                       struct Arena* arena,
                       struct Str** tools_out,
                       ptrdiff_t* tool_count_out) __attribute__((nonnull(3, 4, 5, 6)));

#endif
