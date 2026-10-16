#include "markup/markdown.h"

#include <assert.h>
#include <md4c.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/str.h"
#include "formats/html.h"
#include "markup/body_html.h"

_Static_assert((uintmax_t)MARKDOWN_INPUT_LEN_MAX <= (uintmax_t)(MD_SIZE)-1,
               "the product cap must stay inside md4c's addressable input range");
_Static_assert((uintmax_t)MARKDOWN_INPUT_LEN_MAX <= UINT32_MAX,
               "a code block must fit tree-sitter's 32-bit lengths");

/** UTF-8 byte order mark, skipped at the start of a body. */
#define UTF8_BOM S("\xEF\xBB\xBF")

/** UTF-8 encoding of U+FFFD, written for a `NUL` and for a reference to an invalid code point. */
#define UTF8_REPLACEMENT S("\xEF\xBF\xBD")

/**
 * @brief Appends text to a buffer, escaped for the context it is written in.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 * @param text   Text to escape and append.
 */
typedef void EscapeFn(struct Buf* buffer, struct Str text);

/** State of one render. It lives in `markdown_to_html`'s frame. */
struct MarkdownRender {
  /** HTML written so far, growing in `perm`. */
  struct Buf html;

  /** Text of the code block being read, in scratch, written whole when the block ends. */
  struct Buf code;

  /** Region of scratch a code block is highlighted in, in which no buffer of the render grows. */
  struct Arena code_scratch;

  /** Callback that may write the URL of a link's or an image's target, or `NULL`. */
  const struct LinkTargets* link_targets;

  /** HTML level of a level 1 heading. */
  ptrdiff_t top_level;

  /**
   * Raw HTML of the text runs since anything else was written, in scratch, kept only when
   * `link_targets` asks for raw HTML's targets. md4c can split one tag across runs, such as a
   * block's lines, so the runs are written together, before anything that follows them, for their
   * targets to be rewritten whole.
   */
  struct Buf raw_html;

  /**
   * Count of images whose label is being written, zero outside one. An image's label becomes its
   * `alt` attribute, so nothing but its text is written there.
   */
  ptrdiff_t image_depth;

  /** Whether the text is inside a code block. */
  bool is_in_code_block;

  /** Text of the heading being written, in scratch, for its id. */
  struct Buf heading_text;

  /** Offset in `html` of the `<` of the open heading's tag. */
  ptrdiff_t heading_start;

  /** Offset in `html` of the `>` of the open heading's tag, or `-1` outside a heading. */
  ptrdiff_t heading_tag_end;

  /** Every heading written so far, which gets its id once the body is written. */
  struct BodyHtmlHeadings headings;

  /** Arena the headings and their texts live in. */
  struct Arena* heading_arena;
};

/** Opening and closing tags of an element whose detail its HTML does not depend on. */
struct MarkdownTags {
  /** Written when the element starts, or empty. */
  struct Str open;

  /** Written when the element ends, or empty. */
  struct Str close;
};

/**
 * Tags of each block type, by type. A block whose open or close tag depends on its detail, or one
 * that `body_html` writes because Org has it too, leaves that tag empty here and writes it in the
 * callback.
 */
static const struct MarkdownTags block_tags[MD_BLOCK_BLANK + 1] = {
    [MD_BLOCK_QUOTE] = {STR_INIT("<blockquote>\n"), STR_INIT("</blockquote>\n")},
    [MD_BLOCK_UL] = {STR_INIT("<ul>\n"), STR_INIT("</ul>\n")},
    [MD_BLOCK_OL] = {STR_INIT(""), STR_INIT("</ol>\n")},
    [MD_BLOCK_LI] = {STR_INIT(""), STR_INIT("</li>\n")},
    [MD_BLOCK_HR] = {STR_INIT("<hr>\n"), STR_INIT("")},
    [MD_BLOCK_P] = {STR_INIT("<p>"), STR_INIT("</p>\n")},
    [MD_BLOCK_TABLE] = {STR_INIT("<table>\n"), STR_INIT("</table>\n")},
    [MD_BLOCK_THEAD] = {STR_INIT("<thead>\n"), STR_INIT("</thead>\n")},
    [MD_BLOCK_TBODY] = {STR_INIT("<tbody>\n"), STR_INIT("</tbody>\n")},
    [MD_BLOCK_TR] = {STR_INIT("<tr>\n"), STR_INIT("</tr>\n")},
    [MD_BLOCK_TH] = {STR_INIT(""), STR_INIT("</th>\n")},
    [MD_BLOCK_TD] = {STR_INIT(""), STR_INIT("</td>\n")},
};

/**
 * Tags of each span type, by type. A span whose open or close tag depends on its detail, or one
 * that `body_html` writes because Org has it too, leaves that tag empty here and writes it in the
 * callback.
 */
static const struct MarkdownTags span_tags[MD_SPAN_MARK + 1] = {
    [MD_SPAN_EM] = {STR_INIT("<em>"), STR_INIT("</em>")},
    [MD_SPAN_STRONG] = {STR_INIT("<strong>"), STR_INIT("</strong>")},
    [MD_SPAN_A] = {STR_INIT(""), STR_INIT("</a>")},
    [MD_SPAN_CODE] = {STR_INIT("<code>"), STR_INIT("</code>")},
    [MD_SPAN_DEL] = {STR_INIT("<del>"), STR_INIT("</del>")},
    // A spoiler is a switch that a reader turns on to see its text, which a stylesheet hides, from
    // sight and from a screen reader, while the switch is off. The label holds both, so it stays
    // inline in its sentence, and a click anywhere on it toggles the switch with no script.
    [MD_SPAN_SPOILER] = {STR_INIT("<label class=\"spoiler\"><input type=\"checkbox\" "
                                  "role=\"switch\"><span>"),
                         STR_INIT("</span></label>")},
    [MD_SPAN_SUPERSCRIPT] = {STR_INIT("<sup>"), STR_INIT("</sup>")},
    [MD_SPAN_SUBSCRIPT] = {STR_INIT("<sub>"), STR_INIT("</sub>")},
    [MD_SPAN_MARK] = {STR_INIT("<mark>"), STR_INIT("</mark>")},
};

/**
 * @brief Writes the HTML that starts a block.
 *
 * This is installed as md4c's `enter_block` callback.
 *
 * @param type     Block type.
 * @param detail   Type-specific detail, or `NULL`.
 * @param userdata Pointer to the `struct MarkdownRender`.
 * @return `0`, so the parse continues.
 */
static int markdown_to_html_enter_block(MD_BLOCKTYPE type, void* detail, void* userdata);

/**
 * @brief Writes the HTML that ends a block.
 *
 * This is installed as md4c's `leave_block` callback.
 *
 * @param type     Block type.
 * @param detail   Type-specific detail, or `NULL`.
 * @param userdata Pointer to the `struct MarkdownRender`.
 * @return `0`, so the parse continues.
 */
static int markdown_to_html_leave_block(MD_BLOCKTYPE type, void* detail, void* userdata);

/**
 * @brief Writes the HTML that starts a span.
 *
 * Inside an image's label it writes nothing, because the label is the image's `alt` text.
 *
 * This is installed as md4c's `enter_span` callback.
 *
 * @param type     Span type.
 * @param detail   Type-specific detail, or `NULL`.
 * @param userdata Pointer to the `struct MarkdownRender`.
 * @return `0`, so the parse continues.
 */
static int markdown_to_html_enter_span(MD_SPANTYPE type, void* detail, void* userdata);

/**
 * @brief Writes the HTML that ends a span.
 *
 * This is installed as md4c's `leave_span` callback.
 *
 * @param type     Span type.
 * @param detail   Type-specific detail, or `NULL`.
 * @param userdata Pointer to the `struct MarkdownRender`.
 * @return `0`, so the parse continues.
 */
static int markdown_to_html_leave_span(MD_SPANTYPE type, void* detail, void* userdata);

/**
 * @brief Writes a run of text, escaped for where it falls.
 *
 * This is installed as md4c's `text` callback.
 *
 * @param type     Text type.
 * @param text     Text bytes, valid only for the duration of this call.
 * @param size     Number of bytes in `text`.
 * @param userdata Pointer to the `struct MarkdownRender`.
 * @return `0`, so the parse continues.
 */
static int markdown_to_html_text(MD_TEXTTYPE type,
                                 const MD_CHAR* text,
                                 MD_SIZE size,
                                 void* userdata);

/**
 * @brief Writes the opening tag of a table cell, with its alignment.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 * @param tag    Cell tag name, `th` or `td`.
 * @param cell   Cell detail. Must not be `NULL`.
 */
static void write_table_cell_open(struct Buf* buffer,
                                  struct Str tag,
                                  const MD_BLOCK_TD_DETAIL* cell) __attribute__((nonnull(1, 3)));

/**
 * @brief Writes the opening tag of a link.
 *
 * @param render Render state, whose `html` receives the tag. Must not be `NULL`.
 * @param link   Link detail. Must not be `NULL`.
 */
static void write_link_open(struct MarkdownRender* render, const MD_SPAN_A_DETAIL* link)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Writes the end of an image's tag, after its `alt` text.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 * @param image  Image detail. Must not be `NULL`.
 */
static void write_image_close(struct Buf* buffer, const MD_SPAN_IMG_DETAIL* image)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Writes an attribute's text, escaped by `escape`, with its entities as written.
 *
 * @param buffer    Destination buffer. Must not be `NULL`.
 * @param attribute Attribute to write. Must not be `NULL`.
 * @param escape    Escaping for the context the attribute is written in. Must not be `NULL`.
 */
static void write_attribute(struct Buf* buffer, const MD_ATTRIBUTE* attribute, EscapeFn* escape)
    __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Returns an attribute's text as md4c gives it, with its entities as written.
 *
 * @param attribute Attribute. Must not be `NULL`.
 * @return The text, which borrows the document.
 */
static struct Str attribute_text(const MD_ATTRIBUTE* attribute) __attribute__((nonnull(1)));

/**
 * @brief Writes the raw HTML gathered since anything else was written, through
 *        `body_html_raw_html_write`, and empties it.
 *
 * @param render The render. Must not be `NULL`.
 */
static void flush_raw_html(struct MarkdownRender* render) __attribute__((nonnull(1)));

struct Str markdown_to_html(struct Str markdown,
                            const struct LinkTargets* link_targets,
                            ptrdiff_t top_level,
                            struct Arena* perm,
                            struct Arena scratch) {
  struct MarkdownRender render = {
      .html = {.arena = perm},
      .code = {.arena = &scratch},
      .link_targets = link_targets,
      .top_level = top_level,
      .raw_html = {.arena = &scratch},
      .heading_text = {.arena = &scratch},
      .heading_tag_end = -1,
      .heading_arena = &scratch,
  };
  render.code_scratch = body_html_code_block_scratch(&scratch);

  // This names extensions one at a time rather than through an `MD_DIALECT_*` constant, because
  // md4c documents that a dialect constant's meaning changes as it implements more of that dialect.
  // A vendor bump would then silently change how existing content renders. Raw HTML blocks and
  // spans stay enabled (no `MD_FLAG_NOHTML`) because the Markdown is first-party and the content
  // template emits the body unescaped as `{{{body_html}}}`. Md4c does no sanitizing, so this is a
  // trust decision about who authors content, not an omission. The callbacks write only the block,
  // span and text types these extensions produce.
  const MD_PARSER parser = {
      .flags = MD_FLAG_COLLAPSEWHITESPACE | MD_FLAG_PERMISSIVEAUTOLINKS | MD_FLAG_TABLES |
               MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS | MD_FLAG_LATEXMATHSPANS |
               MD_FLAG_SPOILERS | MD_FLAG_SUPERSCRIPTS | MD_FLAG_SUBSCRIPTS | MD_FLAG_ADMONITIONS |
               MD_FLAG_FOOTNOTES | MD_FLAG_HIGHLIGHT,
      .enter_block = markdown_to_html_enter_block,
      .leave_block = markdown_to_html_leave_block,
      .enter_span = markdown_to_html_enter_span,
      .leave_span = markdown_to_html_leave_span,
      .text = markdown_to_html_text,
  };
  // `frontmatter_split` skips a BOM at the start of the file, but one right after the closing fence
  // starts the body, and would otherwise put a stray U+FEFF in the page text.
  if (str_startswith(markdown, UTF8_BOM)) {
    markdown = str_tail(markdown, UTF8_BOM.len);
  }
  // This is the one boundary where md4c's error convention becomes this project's. `md_parse` fails
  // only when its own allocation fails or a callback aborts, and these callbacks never abort, so a
  // failure here is the same condition as `perm` running out.
  assert(markdown.len >= 0 && markdown.len <= MARKDOWN_INPUT_LEN_MAX);
  if (md_parse(markdown.data, (MD_SIZE)markdown.len, &parser, &render) != 0) {
    os_oom();
  }
  flush_raw_html(&render);
  body_html_headings_write(&render.html, &render.headings, scratch);
  return buf_str(&render.html);
}

static int markdown_to_html_enter_block(MD_BLOCKTYPE type, void* detail, void* userdata) {
  struct MarkdownRender* render = userdata;
  flush_raw_html(render);
  struct Buf* html = &render->html;
  switch (type) {
    case MD_BLOCK_OL: {
      const MD_BLOCK_OL_DETAIL* list = detail;
      if (list->start == 1) {
        buf_append(html, S("<ol>\n"));
      } else {
        buf_printf(html, "<ol start=\"%u\">\n", list->start);
      }
      break;
    }
    case MD_BLOCK_LI: {
      const MD_BLOCK_LI_DETAIL* item = detail;
      buf_append(html, S("<li>"));
      if (item->is_task) {
        body_html_checkbox_write(html, item->task_mark == 'x' || item->task_mark == 'X');
      }
      break;
    }
    case MD_BLOCK_H:
      render->heading_start = html->len;
      body_html_heading_open(html, (ptrdiff_t)((const MD_BLOCK_H_DETAIL*)detail)->level,
                             render->top_level);
      render->heading_tag_end = html->len - 1;
      render->heading_text.len = 0;
      break;
    case MD_BLOCK_CODE:
      // The block is written whole when it ends, through the writer Org's source blocks share.
      render->code.len = 0;
      render->is_in_code_block = true;
      break;
    case MD_BLOCK_TH:
      write_table_cell_open(html, S("th"), detail);
      break;
    case MD_BLOCK_TD:
      write_table_cell_open(html, S("td"), detail);
      break;
    case MD_BLOCK_FOOTNOTE_DEF_SECTION:
      body_html_footnotes_open(html);
      break;
    case MD_BLOCK_FOOTNOTE_DEF:
      body_html_footnote_open(html, (ptrdiff_t)((const MD_BLOCK_FOOTNOTE_DEF_DETAIL*)detail)->id);
      break;
    case MD_BLOCK_ADMONITION: {
      // md4c names the type in lowercase, as `body_html` takes it.
      const MD_ATTRIBUTE* admonition_type = &((const MD_BLOCK_ADMONITION_DETAIL*)detail)->type;
      body_html_admonition_open(
          html, (struct Str){admonition_type->text, (ptrdiff_t)admonition_type->size}, S(""));
      break;
    }
    default:
      buf_append(html, block_tags[type].open);
      break;
  }
  return 0;
}

static int markdown_to_html_leave_block(MD_BLOCKTYPE type, void* detail, void* userdata) {
  struct MarkdownRender* render = userdata;
  flush_raw_html(render);
  struct Buf* html = &render->html;
  switch (type) {
    case MD_BLOCK_H:
      body_html_heading_close(html, (ptrdiff_t)((const MD_BLOCK_H_DETAIL*)detail)->level,
                              render->top_level);
      body_html_headings_add(
          &render->headings, render->heading_start, render->heading_tag_end, html->len - 1,
          str_clone(buf_str(&render->heading_text), render->heading_arena), render->heading_arena);
      render->heading_tag_end = -1;
      break;
    case MD_BLOCK_CODE: {
      const MD_ATTRIBUTE* language = &((const MD_BLOCK_CODE_DETAIL*)detail)->lang;
      body_html_code_block_write(html, (struct Str){language->text, (ptrdiff_t)language->size},
                                 S(""), buf_str(&render->code), render->code_scratch);
      render->is_in_code_block = false;
      break;
    }
    case MD_BLOCK_FOOTNOTE_DEF_SECTION:
      body_html_footnotes_close(html);
      break;
    case MD_BLOCK_FOOTNOTE_DEF: {
      const MD_BLOCK_FOOTNOTE_DEF_DETAIL* footnote = detail;
      body_html_footnote_back_references_write(html, (ptrdiff_t)footnote->id,
                                               (ptrdiff_t)footnote->ref_count);
      body_html_footnote_close(html);
      break;
    }
    case MD_BLOCK_ADMONITION:
      body_html_admonition_close(html);
      break;
    default:
      buf_append(html, block_tags[type].close);
      break;
  }
  return 0;
}

static int markdown_to_html_enter_span(MD_SPANTYPE type, void* detail, void* userdata) {
  struct MarkdownRender* render = userdata;
  flush_raw_html(render);
  struct Buf* html = &render->html;
  const bool is_inside_image = render->image_depth > 0;
  if (type == MD_SPAN_IMG) {
    render->image_depth++;
  }
  if (is_inside_image) {
    return 0;
  }
  switch (type) {
    case MD_SPAN_A:
      write_link_open(render, detail);
      break;
    case MD_SPAN_IMG: {
      const MD_ATTRIBUTE* source = &((const MD_SPAN_IMG_DETAIL*)detail)->src;
      buf_append(html, S("<img src=\""));
      if (body_html_link_target_write(html, S(""), attribute_text(source), true,
                                      render->link_targets) != 0) {
        write_attribute(html, source, body_html_url_escape_append);
      }
      buf_append(html, S("\" alt=\""));
      break;
    }
    case MD_SPAN_LATEXMATH:
      body_html_math_open(html, false);
      break;
    case MD_SPAN_LATEXMATH_DISPLAY:
      body_html_math_open(html, true);
      break;
    case MD_SPAN_FOOTNOTE_REF: {
      const MD_SPAN_FOOTNOTE_REF_DETAIL* reference = detail;
      body_html_footnote_reference_write(html, (ptrdiff_t)reference->id,
                                         (ptrdiff_t)reference->ref_id);
      break;
    }
    default:
      buf_append(html, span_tags[type].open);
      break;
  }
  return 0;
}

static int markdown_to_html_leave_span(MD_SPANTYPE type, void* detail, void* userdata) {
  struct MarkdownRender* render = userdata;
  flush_raw_html(render);
  if (type == MD_SPAN_IMG) {
    render->image_depth--;
  }
  if (render->image_depth > 0) {
    return 0;
  }
  switch (type) {
    case MD_SPAN_IMG:
      write_image_close(&render->html, detail);
      break;
    case MD_SPAN_LATEXMATH:
    case MD_SPAN_LATEXMATH_DISPLAY:
      body_html_math_close(&render->html);
      break;
    default:
      buf_append(&render->html, span_tags[type].close);
      break;
  }
  return 0;
}

static int markdown_to_html_text(MD_TEXTTYPE type,
                                 const MD_CHAR* text,
                                 MD_SIZE size,
                                 void* userdata) {
  struct MarkdownRender* render = userdata;
  struct Buf* html = &render->html;
  const struct Str run = {text, (ptrdiff_t)size};
  const bool is_inside_image = render->image_depth > 0;
  if (render->is_in_code_block) {
    buf_append(&render->code, type == MD_TEXT_NULLCHAR ? UTF8_REPLACEMENT : run);
    return 0;
  }
  if (type == MD_TEXT_HTML && !is_inside_image && render->link_targets != NULL &&
      render->link_targets->is_raw_html_linked) {
    buf_append(&render->raw_html, run);
    return 0;
  }
  flush_raw_html(render);
  // A heading's id comes from the text a reader sees in it: not raw HTML, an entity or an image's
  // label.
  if (render->heading_tag_end >= 0 && !is_inside_image) {
    if (type == MD_TEXT_NORMAL || type == MD_TEXT_CODE) {
      buf_append(&render->heading_text, run);
    } else if (type == MD_TEXT_BR || type == MD_TEXT_SOFTBR) {
      buf_append_char(&render->heading_text, ' ');
    }
  }
  switch (type) {
    case MD_TEXT_NULLCHAR:
      buf_append(html, UTF8_REPLACEMENT);
      break;
    case MD_TEXT_BR:
      buf_append(html, is_inside_image ? S(" ") : S("<br>\n"));
      break;
    case MD_TEXT_SOFTBR:
      buf_append(html, is_inside_image ? S(" ") : S("\n"));
      break;
    case MD_TEXT_HTML:
      // Inside an image's label the text falls into its `alt` attribute, where raw HTML would end
      // the attribute, so it is escaped like any other text there.
      if (is_inside_image) {
        html_escape_append(html, run);
      } else {
        buf_append(html, run);
      }
      break;
    case MD_TEXT_ENTITY:
      // An entity stays as written, such as `&copy;`, which the browser shows as the character it
      // names. Org's entities are HTML entities too.
      buf_append(html, run);
      break;
    default:
      html_escape_append(html, run);
      break;
  }
  return 0;
}

static void write_table_cell_open(struct Buf* buffer,
                                  struct Str tag,
                                  const MD_BLOCK_TD_DETAIL* cell) {
  buf_append(buffer, S("<"));
  buf_append(buffer, tag);
  switch (cell->align) {
    case MD_ALIGN_LEFT:
      buf_append(buffer, S(" align=\"left\">"));
      break;
    case MD_ALIGN_CENTER:
      buf_append(buffer, S(" align=\"center\">"));
      break;
    case MD_ALIGN_RIGHT:
      buf_append(buffer, S(" align=\"right\">"));
      break;
    default:
      buf_append(buffer, S(">"));
      break;
  }
}

static void write_link_open(struct MarkdownRender* render, const MD_SPAN_A_DETAIL* link) {
  struct Buf* buffer = &render->html;
  buf_append(buffer, S("<a href=\""));
  if (body_html_link_target_write(buffer, S(""), attribute_text(&link->href), false,
                                  render->link_targets) != 0) {
    write_attribute(buffer, &link->href, body_html_url_escape_append);
  }
  if (link->title.text != NULL) {
    buf_append(buffer, S("\" title=\""));
    write_attribute(buffer, &link->title, html_escape_append);
  }
  buf_append(buffer, S("\">"));
}

static void write_image_close(struct Buf* buffer, const MD_SPAN_IMG_DETAIL* image) {
  if (image->title.text != NULL) {
    buf_append(buffer, S("\" title=\""));
    write_attribute(buffer, &image->title, html_escape_append);
  }
  buf_append(buffer, S("\">"));
}

static void write_attribute(struct Buf* buffer, const MD_ATTRIBUTE* attribute, EscapeFn* escape) {
  for (ptrdiff_t i = 0; attribute->substr_offsets[i] < attribute->size; i++) {
    const MD_OFFSET offset = attribute->substr_offsets[i];
    const struct Str text = {attribute->text + offset,
                             (ptrdiff_t)(attribute->substr_offsets[i + 1] - offset)};
    switch (attribute->substr_types[i]) {
      case MD_TEXT_NULLCHAR:
        buf_append(buffer, UTF8_REPLACEMENT);
        break;
      case MD_TEXT_ENTITY:
        buf_append(buffer, text);
        break;
      default:
        escape(buffer, text);
        break;
    }
  }
}

static struct Str attribute_text(const MD_ATTRIBUTE* attribute) {
  return (struct Str){attribute->text, (ptrdiff_t)attribute->size};
}

static void flush_raw_html(struct MarkdownRender* render) {
  if (render->raw_html.len > 0) {
    body_html_raw_html_write(&render->html, buf_str(&render->raw_html), render->link_targets);
    render->raw_html.len = 0;
  }
}
