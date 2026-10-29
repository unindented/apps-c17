#include "build/highlight_diff.h"

#include <stdbool.h>
#include <stddef.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "formats/html.h"
#include "markup/highlight.h"

/** Where each line's fragment starts and ends in the HTML the writer grows. */
struct HighlightDiffSpans {
  /** Offset of each line's start, then of its end, two per line, in the arena of the spans. */
  ptrdiff_t* offsets;

  /** Number of entries in `offsets`. */
  ptrdiff_t count;

  /** Slots allocated at `offsets`. */
  ptrdiff_t capacity;

  /** Arena the offsets grow in, apart from the HTML. */
  struct Arena* arena;
};

/**
 * @brief Records where a line starts or ends in the HTML. This is the `open` callback of
 *        `highlight_diff_lines`, and what its `close` callback does.
 *
 * @param buffer   Buffer the HTML grows in. Must not be `NULL`.
 * @param line     Index of the line, from 0.
 * @param userdata `struct HighlightDiffSpans*`. Must not be `NULL`.
 */
static void highlight_diff_lines_mark(struct Buf* buffer, ptrdiff_t line, void* userdata)
    __attribute__((nonnull(1, 3)));

/**
 * @brief Records where a line ends in the HTML, and writes no line end, which no fragment holds.
 *        This is the `close` callback of `highlight_diff_lines`.
 *
 * @param buffer   Buffer the HTML grows in. Must not be `NULL`.
 * @param line     Index of the line, from 0.
 * @param is_ended Whether the line ends in `\n`.
 * @param userdata `struct HighlightDiffSpans*`. Must not be `NULL`.
 */
static void highlight_diff_lines_end(struct Buf* buffer,
                                     ptrdiff_t line,
                                     bool is_ended,
                                     void* userdata) __attribute__((nonnull(1, 4)));

void highlight_diff_lines(struct Str text,
                          const struct HighlightGrammar* grammar,
                          struct Arena* perm,
                          struct Arena scratch,
                          struct Str** fragments_out,
                          ptrdiff_t* count_out) {
  // The HTML grows in the perm, and the offsets in half of the scratch, apart from tree-sitter's
  // memory in the other half.
  const char* region_end = scratch.limit != NULL ? scratch.limit : scratch.end;
  struct Arena offset_arena = arena_split(&scratch, (region_end - scratch.beg) / 2);
  struct HighlightDiffSpans spans = {.arena = &offset_arena};
  const struct HighlightLines lines = {
      .open = highlight_diff_lines_mark,
      .close = highlight_diff_lines_end,
      .userdata = &spans,
  };
  struct Buf html = {.arena = perm};
  highlight_write(&html, grammar, text, &lines, scratch);
  const ptrdiff_t count = spans.count / 2;
  struct Str* fragments = arena_new(perm, count, struct Str);
  const struct Str written = buf_str(&html);
  for (ptrdiff_t i = 0; i < count; i++) {
    fragments[i] = str_head(str_tail(written, spans.offsets[2 * i]),
                            spans.offsets[2 * i + 1] - spans.offsets[2 * i]);
  }
  *fragments_out = fragments;
  *count_out = count;
}

const struct Str* highlight_diff_file(const struct ChangedFile* file,
                                      const struct HighlightDiffSides* sides,
                                      struct Arena* perm) {
  struct Str* fragments = arena_new(perm, file->line_count, struct Str);
  for (ptrdiff_t i = 0; i < file->line_count; i++) {
    const struct DiffLine* line = &file->lines[i];
    const bool is_old = line->origin == '-';
    const ptrdiff_t number = is_old ? line->old_line : line->new_line;
    const struct Str* side = is_old ? sides->old_fragments : sides->new_fragments;
    const ptrdiff_t side_count = is_old ? sides->old_fragment_count : sides->new_fragment_count;
    if (side != NULL && number > 0 && number <= side_count) {
      fragments[i] = str_clone(side[number - 1], perm);
    } else {
      struct Buf escaped = {.arena = perm};
      html_escape_append(&escaped, line->text);
      fragments[i] = buf_str(&escaped);
    }
  }
  return fragments;
}

static void highlight_diff_lines_mark(struct Buf* buffer, ptrdiff_t line, void* userdata) {
  (void)line;
  struct HighlightDiffSpans* spans = userdata;
  if (spans->count == spans->capacity) {
    spans->offsets = arena_grow(spans->arena, spans->offsets, &spans->capacity,
                                (ptrdiff_t)sizeof(ptrdiff_t), (ptrdiff_t)_Alignof(ptrdiff_t));
  }
  spans->offsets[spans->count++] = buffer->len;
}

static void highlight_diff_lines_end(struct Buf* buffer,
                                     ptrdiff_t line,
                                     bool is_ended,
                                     void* userdata) {
  (void)is_ended;
  highlight_diff_lines_mark(buffer, line, userdata);
}
