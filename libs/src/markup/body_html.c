#include "markup/body_html.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "core/arena.h"
#include "core/ascii.h"
#include "core/buf.h"
#include "core/str.h"
#include "formats/html.h"
#include "markup/highlight.h"

/** Uppercase hex digits of a percent-encoded URL byte. */
static const char HEX_DIGITS[] = "0123456789ABCDEF";

/** One edit of a body that `body_html_headings_write` makes: text in place of some bytes. */
struct HeadingEdit {
  /** Offset in the buffer of the first byte the edit replaces, or that the text goes before. */
  ptrdiff_t offset;

  /** Number of bytes the text replaces, 0 for an insertion. */
  ptrdiff_t replaced;

  /** Text the edit writes. */
  struct Str text;
};

/**
 * @brief Reports whether a URL may hold a byte as it is.
 *
 * @param c Byte to check.
 * @return `true` for a letter, a digit, or one of `~-_.+!*(),%#@?=;:/$`, `false` otherwise.
 */
static bool is_url_byte_kept(char c);

/** What a raw HTML attribute holds, for the targets that go to `link_targets`. */
enum RawHtmlTarget {
  /** Anything else, which stays as written. */
  RAW_HTML_TARGET_NONE,

  /** The URL of a file whose bytes the element loads, such as the `src` of an `<img>`. */
  RAW_HTML_TARGET_FILE,

  /** A link's URL: the `href` of an `<a>` or an `<area>`. */
  RAW_HTML_TARGET_LINK,

  /** Image candidates, each a URL and its descriptors: the `srcset` of an `<img>` or a
      `<source>`. */
  RAW_HTML_TARGET_SRCSET,
};

/** An attribute that holds a target, and the element that has it. */
struct RawHtmlTargetAttribute {
  /** Name of the element, in lowercase. */
  struct Str tag;

  /** Name of the attribute, in lowercase. */
  struct Str attribute;

  /** What the attribute holds. */
  enum RawHtmlTarget kind;
};

/**
 * Every attribute that holds a target. An element that embeds a file, such as a video, an iframe or
 * an object, loads its bytes as an image does, and an image map's area is followed as a link is.
 */
static const struct RawHtmlTargetAttribute RAW_HTML_TARGET_ATTRIBUTES[] = {
    {STR_INIT("a"), STR_INIT("href"), RAW_HTML_TARGET_LINK},
    {STR_INIT("area"), STR_INIT("href"), RAW_HTML_TARGET_LINK},
    {STR_INIT("audio"), STR_INIT("src"), RAW_HTML_TARGET_FILE},
    {STR_INIT("embed"), STR_INIT("src"), RAW_HTML_TARGET_FILE},
    {STR_INIT("iframe"), STR_INIT("src"), RAW_HTML_TARGET_FILE},
    {STR_INIT("img"), STR_INIT("src"), RAW_HTML_TARGET_FILE},
    {STR_INIT("img"), STR_INIT("srcset"), RAW_HTML_TARGET_SRCSET},
    {STR_INIT("object"), STR_INIT("data"), RAW_HTML_TARGET_FILE},
    {STR_INIT("source"), STR_INIT("src"), RAW_HTML_TARGET_FILE},
    {STR_INIT("source"), STR_INIT("srcset"), RAW_HTML_TARGET_SRCSET},
    {STR_INIT("track"), STR_INIT("src"), RAW_HTML_TARGET_FILE},
    {STR_INIT("video"), STR_INIT("poster"), RAW_HTML_TARGET_FILE},
    {STR_INIT("video"), STR_INIT("src"), RAW_HTML_TARGET_FILE},
};

/**
 * @brief Copies one tag of raw HTML, giving its targets to `link_targets`, as
 *        `body_html_raw_html_write` gives them.
 *
 * @param buffer       Buffer the HTML grows in. Must not be `NULL`.
 * @param tag          Raw HTML that starts with the tag's `<`, which a letter follows.
 * @param link_targets Callback. Must not be `NULL`.
 * @param name_out     Receives the tag's name, as written. Must not be `NULL`.
 * @return The number of bytes of `tag` the tag takes, through its `>`, or all of them when it is
 *         cut off.
 */
static ptrdiff_t raw_html_write_tag(struct Buf* buffer,
                                    struct Str tag,
                                    const struct LinkTargets* link_targets,
                                    struct Str* name_out) __attribute__((nonnull(1, 3, 4)));

/**
 * @brief Returns what an attribute of a tag holds.
 *
 * @param tag       Name of the tag, as written.
 * @param attribute Name of the attribute, as written.
 * @return The kind of target, or `RAW_HTML_TARGET_NONE`.
 */
static enum RawHtmlTarget raw_html_target(struct Str tag, struct Str attribute);

/**
 * @brief Writes a `srcset`, giving the URL of each candidate to `link_targets` and keeping its
 *        descriptors, such as `2x`, and the separators between candidates.
 *
 * @param buffer       Buffer the HTML grows in, inside a double-quoted value. Must not be `NULL`.
 * @param srcset       Value of the attribute, as written.
 * @param link_targets Callback. Must not be `NULL`.
 * @return `true` when the callback wrote the URL of at least one candidate.
 */
static bool raw_html_write_srcset(struct Buf* buffer,
                                  struct Str srcset,
                                  const struct LinkTargets* link_targets)
    __attribute__((nonnull(1, 3)));

/**
 * @brief Appends text of an attribute value inside a double-quoted value, with each `"` as
 *        `&quot;`, which a single-quoted or an unquoted value may hold.
 *
 * @param buffer Buffer the HTML grows in. Must not be `NULL`.
 * @param text   Text as written.
 */
static void raw_html_append_value(struct Buf* buffer, struct Str text) __attribute__((nonnull(1)));

/**
 * @brief Reports whether a tag or attribute name is `lower` in any ASCII letter case.
 *
 * @param name  Name as written.
 * @param lower Name to match, in lowercase.
 * @return `true` when they match.
 */
static bool raw_html_is_named(struct Str name, struct Str lower);

/**
 * @brief Returns where `needle` first starts in `text`, in any ASCII letter case.
 *
 * @param text   Text to search.
 * @param needle Text to find, in lowercase.
 * @return The offset of the first match, or `text.len` when there is none.
 */
static ptrdiff_t raw_html_find(struct Str text, struct Str needle);

/**
 * @brief Appends the slug of a heading's text, as `body_html_headings_write` makes it.
 *
 * @param buffer Buffer that receives the slug. Must not be `NULL`.
 * @param text   Text of the heading.
 */
static void heading_slug_append(struct Buf* buffer, struct Str text) __attribute__((nonnull(1)));

/**
 * @brief Reports whether an id is one of the first `count` ids.
 *
 * @param ids   Ids so far.
 * @param count Number of ids to look at.
 * @param id    Id to look for.
 * @return `true` when an earlier id is the same.
 */
static bool heading_id_is_taken(const struct Str* ids, ptrdiff_t count, struct Str id);

/**
 * @brief Returns the HTML level of a heading: its level in the source from `top_level` on, up to 6.
 *
 * @param level     Heading level in the source, from 1.
 * @param top_level HTML level of a level 1 heading.
 * @return The `N` of `<hN>`, from `top_level` to 6.
 */
static ptrdiff_t heading_html_level(ptrdiff_t level, ptrdiff_t top_level);

/**
 * @brief Writes one number of a code line: a link to itself when it has an anchor prefix, else a
 *        span, which is empty for no number.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 * @param number The number. Must not be `NULL`.
 */
static void code_line_number_write(struct Buf* buffer, const struct CodeLineNumber* number)
    __attribute__((nonnull(1, 2)));

void body_html_url_escape_append(struct Buf* buffer, struct Str text) {
  ptrdiff_t run_start = 0;
  for (ptrdiff_t i = 0; i < text.len; i++) {
    const unsigned char c = (unsigned char)text.data[i];
    if (is_url_byte_kept((char)c)) {
      continue;
    }
    buf_append(buffer, str_head(str_tail(text, run_start), i - run_start));
    if (c == '&') {
      buf_append(buffer, S("&amp;"));
    } else {
      const char percent[] = {'%', HEX_DIGITS[c >> 4], HEX_DIGITS[c & 0xF]};
      buf_append(buffer, (struct Str){percent, (ptrdiff_t)sizeof(percent)});
    }
    run_start = i + 1;
  }
  buf_append(buffer, str_tail(text, run_start));
}

int body_html_link_target_write(struct Buf* buffer,
                                struct Str type,
                                struct Str target,
                                bool is_image,
                                const struct LinkTargets* link_targets) {
  if (link_targets == NULL) {
    return -1;
  }
  const ptrdiff_t start = buffer->len;
  bool is_written = false;
  if (link_targets->write(buffer, type, target, is_image, &is_written, link_targets->userdata) !=
          0 ||
      !is_written) {
    buffer->len = start;
    return -1;
  }
  return 0;
}

void body_html_raw_html_write(struct Buf* buffer,
                              struct Str html,
                              const struct LinkTargets* link_targets) {
  if (link_targets == NULL || !link_targets->is_raw_html_linked) {
    buf_append(buffer, html);
    return;
  }
  ptrdiff_t i = 0;
  while (i < html.len) {
    const struct Str rest = str_tail(html, i);
    if (str_startswith(rest, S("<!--"))) {
      const struct Str body = str_tail(rest, 4);
      const ptrdiff_t end = raw_html_find(body, S("-->"));
      const ptrdiff_t len = end < body.len ? 4 + end + 3 : rest.len;
      buf_append(buffer, str_head(rest, len));
      i += len;
      continue;
    }
    const unsigned char next = rest.len > 1 ? ascii_to_lower((unsigned char)rest.data[1]) : 0;
    if (rest.data[0] == '<' && next >= 'a' && next <= 'z') {
      struct Str name = {0};
      i += raw_html_write_tag(buffer, rest, link_targets, &name);
      // A script's or a style's text is not HTML, so nothing in it is a tag.
      const bool is_script = raw_html_is_named(name, S("script"));
      if (is_script || raw_html_is_named(name, S("style"))) {
        const struct Str text = str_tail(html, i);
        const ptrdiff_t len = raw_html_find(text, is_script ? S("</script") : S("</style"));
        buf_append(buffer, str_head(text, len));
        i += len;
      }
      continue;
    }
    ptrdiff_t len = 1;
    while (len < rest.len && rest.data[len] != '<') {
      len++;
    }
    buf_append(buffer, str_head(rest, len));
    i += len;
  }
}

void body_html_heading_open(struct Buf* buffer, ptrdiff_t level, ptrdiff_t top_level) {
  buf_printf(buffer, "<h%td>", heading_html_level(level, top_level));
}

void body_html_heading_close(struct Buf* buffer, ptrdiff_t level, ptrdiff_t top_level) {
  buf_printf(buffer, "</h%td>\n", heading_html_level(level, top_level));
}

void body_html_headings_add(struct BodyHtmlHeadings* headings,
                            ptrdiff_t start,
                            ptrdiff_t tag_end,
                            ptrdiff_t end,
                            struct Str text,
                            struct Arena* arena) {
  if (headings->count == headings->cap) {
    const ptrdiff_t cap = headings->cap > 0 ? 2 * headings->cap : 16;
    struct BodyHtmlHeading* items = arena_new(arena, cap, struct BodyHtmlHeading);
    for (ptrdiff_t i = 0; i < headings->count; i++) {
      items[i] = headings->items[i];
    }
    headings->items = items;
    headings->cap = cap;
  }
  headings->items[headings->count++] =
      (struct BodyHtmlHeading){.start = start, .tag_end = tag_end, .end = end, .text = text};
}

void body_html_headings_write(struct Buf* buffer,
                              const struct BodyHtmlHeadings* headings,
                              struct Arena scratch) {
  if (headings->count == 0) {
    return;
  }
  // Each heading takes three edits, in the order of the buffer: the wrapper's opening tag before
  // the heading, the id in place of the opening tag's `>`, and the anchor link and the wrapper's
  // closing tag before the line end.
  const ptrdiff_t edit_count = 3 * headings->count;
  struct HeadingEdit* edits = arena_new(&scratch, edit_count, struct HeadingEdit);
  struct Str* ids = arena_new(&scratch, headings->count, struct Str);
  ptrdiff_t extra = 0;
  for (ptrdiff_t i = 0; i < headings->count; i++) {
    const struct BodyHtmlHeading* heading = &headings->items[i];
    struct Buf base = {.arena = &scratch};
    if (heading->id.len > 0) {
      buf_append(&base, heading->id);
    } else {
      heading_slug_append(&base, heading->text);
      if (base.len == 0) {
        buf_append(&base, S("section"));
      }
    }
    struct Str id = buf_str(&base);
    for (ptrdiff_t n = 1; heading->id.len == 0 && heading_id_is_taken(ids, i, id); n++) {
      struct Buf numbered = {.arena = &scratch};
      buf_printf(&numbered, "%.*s-%td", STR_FMT(buf_str(&base)), n);
      id = buf_str(&numbered);
    }
    ids[i] = id;
    struct Buf attribute = {.arena = &scratch};
    buf_append(&attribute, S(" id=\""));
    html_escape_append(&attribute, id);
    buf_append(&attribute, S("\">"));
    struct Buf anchor = {.arena = &scratch};
    buf_append(&anchor, S("<a class=\"anchor\" href=\"#"));
    body_html_url_escape_append(&anchor, id);
    // The link takes its name from the heading, so the page names it in the heading's language.
    buf_append(&anchor, S("\" aria-labelledby=\""));
    html_escape_append(&anchor, id);
    buf_append(&anchor, S("\"></a></div>"));
    edits[3 * i] =
        (struct HeadingEdit){.offset = heading->start, .text = S("<div class=\"heading\">")};
    edits[3 * i + 1] = (struct HeadingEdit){
        .offset = heading->tag_end, .replaced = 1, .text = buf_str(&attribute)};
    edits[3 * i + 2] = (struct HeadingEdit){.offset = heading->end, .text = buf_str(&anchor)};
    for (ptrdiff_t e = 3 * i; e < 3 * i + 3; e++) {
      extra += edits[e].text.len - edits[e].replaced;
    }
  }
  // The buffer grows by what the edits add, and its bytes move toward its end from the last edit
  // back, so each byte moves once.
  const ptrdiff_t old_len = buffer->len;
  for (ptrdiff_t i = 0; i < extra; i++) {
    buf_append_char(buffer, ' ');
  }
  char* data = buffer->data;
  ptrdiff_t src = old_len;
  ptrdiff_t dst = old_len + extra;
  for (ptrdiff_t e = edit_count - 1; e >= 0; e--) {
    const ptrdiff_t after = edits[e].offset + edits[e].replaced;
    dst -= src - after;
    memmove(data + dst, data + after, (size_t)(src - after));
    dst -= edits[e].text.len;
    memcpy(data + dst, edits[e].text.data, (size_t)edits[e].text.len);
    src = edits[e].offset;
  }
}

void body_html_footnote_reference_write(struct Buf* buffer, ptrdiff_t id, ptrdiff_t reference_id) {
  buf_printf(buffer,
             "<sup><a href=\"#fn-%td\" id=\"fnref-%td-%td\" role=\"doc-noteref\">%td</a></sup>", id,
             id, reference_id, id);
}

void body_html_footnotes_open(struct Buf* buffer) {
  buf_append(buffer, S("<section class=\"footnotes\" role=\"doc-endnotes\">\n<ol>\n"));
}

void body_html_footnotes_close(struct Buf* buffer) {
  buf_append(buffer, S("</ol>\n</section>\n"));
}

void body_html_footnote_open(struct Buf* buffer, ptrdiff_t id) {
  buf_printf(buffer, "<li id=\"fn-%td\">\n", id);
}

void body_html_footnote_back_references_write(struct Buf* buffer,
                                              ptrdiff_t id,
                                              ptrdiff_t reference_count) {
  for (ptrdiff_t reference_id = 1; reference_id <= reference_count; reference_id++) {
    if (reference_id > 1) {
      buf_append(buffer, S(" "));
    }
    buf_printf(
        buffer,
        "<a href=\"#fnref-%td-%td\" class=\"footnote-backref\" role=\"doc-backlink\">&#8617;</a>",
        id, reference_id);
  }
}

void body_html_footnote_close(struct Buf* buffer) {
  buf_append(buffer, S("\n</li>\n"));
}

void body_html_checkbox_write(struct Buf* buffer, bool is_checked) {
  buf_append(buffer,
             is_checked
                 ? S("<input type=\"checkbox\" class=\"task-list-item-checkbox\" disabled "
                     "checked>")
                 : S("<input type=\"checkbox\" class=\"task-list-item-checkbox\" disabled>"));
}

void body_html_admonition_open(struct Buf* buffer, struct Str type, struct Str id) {
  buf_append(buffer, S("<div class=\"admonition-"));
  html_escape_append(buffer, type);
  if (id.len > 0) {
    buf_append(buffer, S("\" id=\""));
    html_escape_append(buffer, id);
  }
  buf_append(buffer, S("\"><p class=\"admonition-title\">"));
  html_escape_append(buffer, type);
  buf_append(buffer, S("</p>"));
}

void body_html_admonition_close(struct Buf* buffer) {
  buf_append(buffer, S("</div>\n"));
}

void body_html_math_open(struct Buf* buffer, bool is_display) {
  buf_append(buffer, is_display ? S("<x-equation type=\"display\">") : S("<x-equation>"));
}

void body_html_math_close(struct Buf* buffer) {
  buf_append(buffer, S("</x-equation>"));
}

struct Arena body_html_code_block_scratch(struct Arena* scratch) {
  const char* scratch_end = scratch->limit != NULL ? scratch->limit : scratch->end;
  const ptrdiff_t free_len = scratch_end - scratch->beg;
  const ptrdiff_t region_len = 2 * (ptrdiff_t)HIGHLIGHT_MEMORY_LEN_MAX;
  return arena_split(scratch, region_len < free_len / 2 ? region_len : free_len / 2);
}

void body_html_code_block_write(struct Buf* buffer,
                                struct Str language,
                                struct Str id,
                                struct Str code,
                                struct Arena scratch) {
  body_html_code_block_open(buffer, language, id);
  body_html_code_write(buffer, language, code, NULL, scratch);
  body_html_code_block_close(buffer);
}

void body_html_code_block_open(struct Buf* buffer, struct Str language, struct Str id) {
  buf_append(buffer, S("<pre"));
  if (id.len > 0) {
    buf_append(buffer, S(" id=\""));
    html_escape_append(buffer, id);
    buf_append(buffer, S("\""));
  }
  buf_append(buffer, S("><code"));
  if (language.len > 0) {
    buf_append(buffer, S(" class=\""));
    if (!str_startswith(language, S("language-"))) {
      buf_append(buffer, S("language-"));
    }
    html_escape_append(buffer, language);
    buf_append(buffer, S("\""));
  }
  buf_append(buffer, S(">"));
}

void body_html_code_write(struct Buf* buffer,
                          struct Str language,
                          struct Str code,
                          const struct HighlightLines* lines,
                          struct Arena scratch) {
  highlight_write(buffer, highlight_grammar_find(language), code, lines, scratch);
}

void body_html_code_block_close(struct Buf* buffer) {
  buf_append(buffer, S("</code></pre>\n"));
}

void body_html_code_line_open(struct Buf* buffer, const struct CodeLine* line) {
  buf_append(buffer, S("<span class=\"code-line"));
  if (line->class_name.len > 0) {
    buf_append(buffer, S(" "));
    html_escape_append(buffer, line->class_name);
  }
  buf_append(buffer, S("\""));
  if (line->id.len > 0) {
    buf_append(buffer, S(" id=\""));
    html_escape_append(buffer, line->id);
    buf_append(buffer, S("\""));
  }
  buf_append(buffer, S(">"));
  if (line->number_count > 0) {
    buf_append(buffer, S("<span class=\"code-line-numbers\">"));
    for (ptrdiff_t i = 0; i < line->number_count; i++) {
      code_line_number_write(buffer, &line->numbers[i]);
    }
    buf_append(buffer, S("</span>"));
  }
  buf_append(buffer, S("<span class=\"code-line-text\">"));
}

void body_html_code_line_close(struct Buf* buffer, bool is_ended) {
  buf_append(buffer, is_ended ? S("\n</span></span>") : S("</span></span>"));
}

static bool is_url_byte_kept(char c) {
  if (ascii_is_alphanumeric((unsigned char)c)) {
    return true;
  }
  switch (c) {
    case '~':
    case '-':
    case '_':
    case '.':
    case '+':
    case '!':
    case '*':
    case '(':
    case ')':
    case ',':
    case '%':
    case '#':
    case '@':
    case '?':
    case '=':
    case ';':
    case ':':
    case '/':
    case '$':
      return true;
    default:
      return false;
  }
}

static ptrdiff_t heading_html_level(ptrdiff_t level, ptrdiff_t top_level) {
  const ptrdiff_t html_level = level + top_level - 1;
  return html_level < 6 ? html_level : 6;
}

static ptrdiff_t raw_html_write_tag(struct Buf* buffer,
                                    struct Str tag,
                                    const struct LinkTargets* link_targets,
                                    struct Str* name_out) {
  const char* data = tag.data;
  ptrdiff_t i = 1;
  while (i < tag.len && !ascii_is_space((unsigned char)data[i]) && data[i] != '/' &&
         data[i] != '>') {
    i++;
  }
  const struct Str name = {data + 1, i - 1};
  *name_out = name;
  buf_append(buffer, str_head(tag, i));
  while (i < tag.len) {
    const char c = data[i];
    if (c == '>') {
      buf_append_char(buffer, '>');
      return i + 1;
    }
    if (ascii_is_space((unsigned char)c) || c == '/') {
      buf_append_char(buffer, c);
      i++;
      continue;
    }
    const ptrdiff_t start = i;
    while (i < tag.len && !ascii_is_space((unsigned char)data[i]) && data[i] != '=' &&
           data[i] != '>' && data[i] != '/') {
      i++;
    }
    const struct Str attribute = {data + start, i - start};
    ptrdiff_t equals = i;
    while (equals < tag.len && ascii_is_space((unsigned char)data[equals])) {
      equals++;
    }
    if (equals == tag.len || data[equals] != '=') {
      // An attribute without a value, such as `hidden`, and the spaces after it.
      buf_append(buffer, str_head(str_tail(tag, start), i - start));
      continue;
    }
    ptrdiff_t value_start = equals + 1;
    while (value_start < tag.len && ascii_is_space((unsigned char)data[value_start])) {
      value_start++;
    }
    const char quote = value_start < tag.len ? data[value_start] : 0;
    const bool is_quoted = quote == '"' || quote == '\'';
    if (is_quoted) {
      value_start++;
    }
    ptrdiff_t value_end = value_start;
    while (value_end < tag.len && (is_quoted ? data[value_end] != quote
                                             : !ascii_is_space((unsigned char)data[value_end]) &&
                                                   data[value_end] != '>')) {
      value_end++;
    }
    const bool is_cut_off = is_quoted && value_end == tag.len;
    const ptrdiff_t end = is_quoted && !is_cut_off ? value_end + 1 : value_end;
    const enum RawHtmlTarget kind = raw_html_target(name, attribute);
    if (kind != RAW_HTML_TARGET_NONE && !is_cut_off) {
      const ptrdiff_t mark = buffer->len;
      buf_append(buffer, attribute);
      buf_append(buffer, S("=\""));
      const struct Str value = {data + value_start, value_end - value_start};
      const bool is_written =
          kind == RAW_HTML_TARGET_SRCSET
              ? raw_html_write_srcset(buffer, value, link_targets)
              : body_html_link_target_write(buffer, S(""), value, kind == RAW_HTML_TARGET_FILE,
                                            link_targets) == 0;
      if (is_written) {
        buf_append_char(buffer, '"');
        i = end;
        continue;
      }
      buffer->len = mark;
    }
    buf_append(buffer, str_head(str_tail(tag, start), end - start));
    i = end;
  }
  return tag.len;
}

static enum RawHtmlTarget raw_html_target(struct Str tag, struct Str attribute) {
  const ptrdiff_t count =
      (ptrdiff_t)(sizeof(RAW_HTML_TARGET_ATTRIBUTES) / sizeof(RAW_HTML_TARGET_ATTRIBUTES[0]));
  for (ptrdiff_t i = 0; i < count; i++) {
    const struct RawHtmlTargetAttribute* entry = &RAW_HTML_TARGET_ATTRIBUTES[i];
    if (raw_html_is_named(tag, entry->tag) && raw_html_is_named(attribute, entry->attribute)) {
      return entry->kind;
    }
  }
  return RAW_HTML_TARGET_NONE;
}

static bool raw_html_write_srcset(struct Buf* buffer,
                                  struct Str srcset,
                                  const struct LinkTargets* link_targets) {
  const char* data = srcset.data;
  bool is_rewritten = false;
  ptrdiff_t i = 0;
  while (i < srcset.len) {
    const ptrdiff_t gap = i;
    while (i < srcset.len && (ascii_is_space((unsigned char)data[i]) || data[i] == ',')) {
      i++;
    }
    raw_html_append_value(buffer, (struct Str){data + gap, i - gap});
    if (i == srcset.len) {
      break;
    }
    const ptrdiff_t url_start = i;
    while (i < srcset.len && !ascii_is_space((unsigned char)data[i])) {
      i++;
    }
    // Commas at the end of a URL end its candidate, which then has no descriptors.
    ptrdiff_t url_end = i;
    while (url_end > url_start && data[url_end - 1] == ',') {
      url_end--;
    }
    const struct Str url = {data + url_start, url_end - url_start};
    if (body_html_link_target_write(buffer, S(""), url, true, link_targets) == 0) {
      is_rewritten = true;
    } else {
      raw_html_append_value(buffer, url);
    }
    raw_html_append_value(buffer, (struct Str){data + url_end, i - url_end});
    if (url_end == i) {
      const ptrdiff_t descriptors = i;
      while (i < srcset.len && data[i] != ',') {
        i++;
      }
      raw_html_append_value(buffer, (struct Str){data + descriptors, i - descriptors});
    }
  }
  return is_rewritten;
}

static void raw_html_append_value(struct Buf* buffer, struct Str text) {
  for (ptrdiff_t i = 0; i < text.len; i++) {
    if (text.data[i] == '"') {
      buf_append(buffer, S("&quot;"));
    } else {
      buf_append_char(buffer, text.data[i]);
    }
  }
}

static bool raw_html_is_named(struct Str name, struct Str lower) {
  if (name.len != lower.len) {
    return false;
  }
  for (ptrdiff_t i = 0; i < name.len; i++) {
    if (ascii_to_lower((unsigned char)name.data[i]) != (unsigned char)lower.data[i]) {
      return false;
    }
  }
  return true;
}

static ptrdiff_t raw_html_find(struct Str text, struct Str needle) {
  for (ptrdiff_t i = 0; i + needle.len <= text.len; i++) {
    if (raw_html_is_named(str_head(str_tail(text, i), needle.len), needle)) {
      return i;
    }
  }
  return text.len;
}

static void heading_slug_append(struct Buf* buffer, struct Str text) {
  const ptrdiff_t start = buffer->len;
  // A space or a `-` waits until a kept character follows it, so a run of them is one `-` and none
  // starts or ends the slug.
  bool is_dash_pending = false;
  for (ptrdiff_t i = 0; i < text.len; i++) {
    const unsigned char c = (unsigned char)text.data[i];
    if (ascii_is_space(c) || c == '-') {
      is_dash_pending = true;
    } else if (c < 0x80 && (ascii_is_alphanumeric(c) || c == '_')) {
      if (is_dash_pending && buffer->len > start) {
        buf_append_char(buffer, '-');
      }
      is_dash_pending = false;
      buf_append_char(buffer, (char)ascii_to_lower(c));
    }
  }
}

static bool heading_id_is_taken(const struct Str* ids, ptrdiff_t count, struct Str id) {
  for (ptrdiff_t i = 0; i < count; i++) {
    if (str_eq(ids[i], id)) {
      return true;
    }
  }
  return false;
}

static void code_line_number_write(struct Buf* buffer, const struct CodeLineNumber* number) {
  const bool is_link = number->anchor_prefix.len > 0;
  if (is_link) {
    buf_append(buffer, S("<a class=\"code-line-number\" id=\""));
    html_escape_append(buffer, number->anchor_prefix);
    buf_printf(buffer, "%td\" href=\"#", number->value);
    body_html_url_escape_append(buffer, number->anchor_prefix);
    buf_printf(buffer, "%td\">", number->value);
  } else {
    buf_append(buffer, S("<span class=\"code-line-number\">"));
  }
  if (number->value > 0) {
    buf_printf(buffer, "%td", number->value);
  }
  buf_append(buffer, is_link ? S("</a>") : S("</span>"));
}
