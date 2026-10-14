#include "formats/frontmatter.h"

#include <stdbool.h>
#include <stddef.h>

#include "core/str.h"

/** UTF-8 byte order mark, which may come before the opening fence. */
#define UTF8_BOM S("\xEF\xBB\xBF")

/**
 * @brief Reports whether a line, without its `\n`, is a fence: the fence alone, with an optional
 *        `\r` at its end.
 *
 * @param line  Line to check.
 * @param fence Fence, `+++` or `---`.
 * @return `true` for a fence line.
 */
static bool is_fence_line(struct Str line, struct Str fence);

enum FrontmatterResult frontmatter_find(struct Str markdown, struct Frontmatter* out) {
  struct Str rest = markdown;
  if (str_startswith(rest, UTF8_BOM)) {
    rest = str_tail(rest, UTF8_BOM.len);
  }
  *out = (struct Frontmatter){.kind = FRONTMATTER_KIND_NONE, .body = rest};
  struct StrCut line = str_cut(rest, '\n');
  if (is_fence_line(line.head, frontmatter_fence(FRONTMATTER_KIND_TOML))) {
    out->kind = FRONTMATTER_KIND_TOML;
  } else if (is_fence_line(line.head, frontmatter_fence(FRONTMATTER_KIND_YAML))) {
    out->kind = FRONTMATTER_KIND_YAML;
  } else {
    return FRONTMATTER_ABSENT;
  }
  const struct Str fence = frontmatter_fence(out->kind);
  const struct Str text = line.tail;
  rest = line.tail;
  while (rest.len > 0) {
    line = str_cut(rest, '\n');
    if (is_fence_line(line.head, fence)) {
      out->text = str_head(text, rest.data - text.data);
      out->body = line.tail;
      return FRONTMATTER_FOUND;
    }
    rest = line.tail;
  }
  return FRONTMATTER_UNCLOSED;
}

struct Str frontmatter_fence(enum FrontmatterKind kind) {
  return kind == FRONTMATTER_KIND_YAML ? S("---") : S("+++");
}

struct Str frontmatter_language(enum FrontmatterKind kind) {
  return kind == FRONTMATTER_KIND_YAML ? S("yaml") : S("toml");
}

static bool is_fence_line(struct Str line, struct Str fence) {
  if (str_endswith(line, S("\r"))) {
    line = str_head(line, line.len - 1);
  }
  return str_eq(line, fence);
}
