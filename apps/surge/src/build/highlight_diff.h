#ifndef SURGE_HIGHLIGHT_DIFF_H
#define SURGE_HIGHLIGHT_DIFF_H

#include <stddef.h>

#include "core/arena.h"
#include "core/str.h"

struct ChangedFile;
struct HighlightGrammar;

/** Each side of one file's diff, highlighted whole by `highlight_diff_lines`. */
struct HighlightDiffSides {
  /** The old blob's fragments, the first for line 1, or `NULL` when the side is not highlighted. */
  const struct Str* old_fragments;

  /** Number of entries in `old_fragments`. */
  ptrdiff_t old_fragment_count;

  /** The new blob's fragments, the first for line 1, or `NULL` when the side is not highlighted. */
  const struct Str* new_fragments;

  /** Number of entries in `new_fragments`. */
  ptrdiff_t new_fragment_count;
};

/**
 * @brief Highlights a whole blob once and returns each line's fragment, the escaped HTML of the
 *        line with its tokens' spans, which closes every span it opens.
 *
 * A diff gives each of its lines the fragment of its side by line number, so a multi-line comment
 * or string that starts outside a hunk still colours the lines inside it.
 *
 * @param text          The blob.
 * @param grammar       Grammar of the blob's language, or `NULL` to escape the text only.
 * @param perm          Arena that owns the fragments. Must not be `NULL`.
 * @param scratch       Arena for tree-sitter's memory, in which nothing else grows while this runs.
 * @param fragments_out Receives the fragments, the first for line 1. Must not be `NULL`.
 * @param count_out     Receives the number of lines. Must not be `NULL`.
 */
void highlight_diff_lines(struct Str text,
                          const struct HighlightGrammar* grammar,
                          struct Arena* perm,
                          struct Arena scratch,
                          struct Str** fragments_out,
                          ptrdiff_t* count_out) __attribute__((nonnull(3, 5, 6)));

/**
 * @brief Returns the fragment of each of a file's diff lines, in the order of its `lines`, copied
 *        into `perm`.
 *
 * A deletion takes the old side's fragment of its line, and an addition or a context line the new
 * side's. A line its side has no fragment for, such as a line of a side that is not highlighted or
 * the "no newline at end" line, is escaped alone.
 *
 * @param file  The file. Must not be `NULL`.
 * @param sides The file's highlighted sides. Must not be `NULL`.
 * @param perm  Arena that owns the fragments. Must not be `NULL`.
 * @return The fragments, one per line of `file`.
 */
const struct Str* highlight_diff_file(const struct ChangedFile* file,
                                      const struct HighlightDiffSides* sides,
                                      struct Arena* perm) __attribute__((nonnull(1, 2, 3)));

#endif
