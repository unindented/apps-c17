#ifndef APPS_FORMATS_FRONTMATTER_H
#define APPS_FORMATS_FRONTMATTER_H

#include "core/str.h"

/** The language a front matter block's fences name. */
enum FrontmatterKind {
  /** No front matter. */
  FRONTMATTER_KIND_NONE,

  /** TOML between `+++` lines. */
  FRONTMATTER_KIND_TOML,

  /** YAML between `---` lines. */
  FRONTMATTER_KIND_YAML,
};

/** What `frontmatter_find` found at the start of a Markdown file. */
enum FrontmatterResult {
  /** A block with its opening and closing fences. */
  FRONTMATTER_FOUND,

  /** No opening fence on the first line. */
  FRONTMATTER_ABSENT,

  /** An opening fence that no line closes. */
  FRONTMATTER_UNCLOSED,
};

/** The front matter of a Markdown file and the body after it, as slices of the file. */
struct Frontmatter {
  /** The language the fences name, `FRONTMATTER_KIND_NONE` when no opening fence starts the
      file. */
  enum FrontmatterKind kind;

  /** Bytes between the fences, each line with its line end, or empty. */
  struct Str text;

  /** Bytes after the closing fence's line, or the whole file, without its BOM, when there is no
      block. */
  struct Str body;
};

/**
 * @brief Finds the front matter at the start of a Markdown file: a `+++` or `---` line, after a
 *        UTF-8 BOM, then the lines up to the next line of the same fence.
 *
 * A fence line holds only the fence, with an optional `\r` before its `\n`. Nothing between the
 * fences is read, so a YAML block needs no YAML parser. A file that starts with a `---` thematic
 * break and has a later one reads as YAML front matter, as GitHub reads it.
 *
 * @param markdown Bytes of the file.
 * @param out      Receives the kind, the text and the body, which borrow `markdown`. With
 *                 `FRONTMATTER_UNCLOSED`, `kind` names the opening fence and `body` is the whole
 *                 file. Must not be `NULL`.
 * @return Whether the file has a closed block, none, or an unclosed one.
 */
enum FrontmatterResult frontmatter_find(struct Str markdown, struct Frontmatter* out)
    __attribute__((nonnull(2)));

/**
 * @brief Returns the fence of a kind, `+++` or `---`.
 *
 * @param kind Kind, other than `FRONTMATTER_KIND_NONE`.
 * @return The fence.
 */
struct Str frontmatter_fence(enum FrontmatterKind kind);

/**
 * @brief Returns the name of a kind's language, `toml` or `yaml`, as a fenced code block names it.
 *
 * @param kind Kind, other than `FRONTMATTER_KIND_NONE`.
 * @return The name.
 */
struct Str frontmatter_language(enum FrontmatterKind kind);

#endif
