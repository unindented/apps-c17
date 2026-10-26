#include "domain/build_cache.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/parse.h"
#include "core/path.h"
#include "core/str.h"
#include "domain/site_config.h"
#include "domain/sosig_version.h"
#include "formats/tsv.h"
#include "markup/highlight.h"
#include "os/os.h"
#include "output/build_cache_file.h"

/** Nanoseconds in a second, the bound of a row's `mtime_ns`. */
enum { BUILD_CACHE_NS_PER_S = 1000000000 };

/** First field of each row, by `enum BuildCacheKind`. */
static const struct Str BUILD_CACHE_KIND_NAMES[] = {
    [BUILD_CACHE_KIND_CONTENT] = STR_INIT("content"),
    [BUILD_CACHE_KIND_TEMPLATE] = STR_INIT("template"),
    [BUILD_CACHE_KIND_STATIC] = STR_INIT("static"),
    [BUILD_CACHE_KIND_ASSET] = STR_INIT("asset"),
};

/**
 * @brief Appends the page recipe, the first line of a build cache, with its newline.
 *
 * @param buffer      Buffer that receives the line. Must not be `NULL`.
 * @param site_config Configuration of the build. Must not be `NULL`.
 */
static void format_recipe(struct Buf* buffer, const struct SiteConfig* site_config)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Appends a list of names to the page recipe, joined by commas, with each name escaped as a
 *        tab-separated field and each `,` inside a name written as `\,`.
 *
 * @param buffer Buffer that receives the list. Must not be `NULL`.
 * @param names  Array of `count` names. May be `NULL` only when `count` is 0.
 * @param count  Number of names.
 */
static void format_recipe_list(struct Buf* buffer, const struct Str* names, ptrdiff_t count)
    __attribute__((nonnull(1)));

/**
 * @brief Appends one row of a build cache, or nothing for a name that holds a tab or a newline.
 *
 * @param buffer Buffer that receives the row. Must not be `NULL`.
 * @param row    Row to append. Must not be `NULL`.
 */
static void build_cache_format_row(struct Buf* buffer, const struct BuildCacheRow* row)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Sizes the rows of a parse, as `BuildCacheFileBeginFn`.
 *
 * @param row_count Number of rows.
 * @param perm      Arena that owns the rows. Must not be `NULL`.
 * @param userdata  The `struct BuildCache` being parsed.
 */
static void build_cache_parse_begin(ptrdiff_t row_count, struct Arena* perm, void* userdata)
    __attribute__((nonnull(2, 3)));

/**
 * @brief Parses one row, which must sort after the row before it, as `BuildCacheFileRowFn`.
 *
 * @param line     The row, without its newline.
 * @param index    Index of the row.
 * @param perm     Arena that owns the row's copied text. Must not be `NULL`.
 * @param userdata The `struct BuildCache` being parsed.
 * @return `0` on success, or `-1` when the row does not parse or is out of order.
 */
static int build_cache_parse_row(struct Str line,
                                 ptrdiff_t index,
                                 struct Arena* perm,
                                 void* userdata) __attribute__((nonnull(3, 4)));

/**
 * @brief Parses one row of a build cache, without its newline.
 *
 * @param line    Row to parse.
 * @param row_out Receives the row, whose name points into `line`. Must not be `NULL`.
 * @return `0` on success, or `-1` when the row has another kind, the wrong number of fields for its
 *         kind, or a field out of range.
 */
static int parse_row(struct Str line, struct BuildCacheRow* row_out) __attribute__((nonnull(2)));

/**
 * @brief Splits the next tab-separated field off `rest`.
 *
 * @param rest      Text the field is taken from, which receives what follows its tab. Must not be
 *                  `NULL`.
 * @param field_out Receives the field. Must not be `NULL`.
 * @return `true` when a field was taken, or `false` when `rest` was already used up.
 */
static bool take_field(struct StrCut* rest, struct Str* field_out) __attribute__((nonnull(1, 2)));

/**
 * @brief Orders two rows by kind and then bytewise by name, for `bsearch`.
 *
 * @param a First `struct BuildCacheRow`. Must not be `NULL`.
 * @param b Second `struct BuildCacheRow`. Must not be `NULL`.
 * @return A negative, zero, or positive value as `a` orders before, equal to, or after `b`.
 */
static int compare_rows(const void* a, const void* b) __attribute__((nonnull(1, 2)));

struct Str build_cache_path(struct Str output_dir, struct Arena* arena) {
  struct Buf name = {.arena = arena};
  buf_printf(&name, "_sosig/v%d-build-cache.tsv", BUILD_CACHE_VERSION);
  return path_join(output_dir, buf_str(&name), arena);
}

int build_cache_parse(struct Str text,
                      const struct SiteConfig* site_config,
                      struct Arena* perm,
                      struct BuildCache* cache_out,
                      struct Log* log) {
  struct Arena arena = *perm;
  struct Buf header = {.arena = &arena};
  format_recipe(&header, site_config);
  struct BuildCache cache = {0};
  if (build_cache_file_parse(
          text, buf_str(&header), S("build cache was written for another page recipe"),
          build_cache_parse_begin, build_cache_parse_row, &cache, &arena, log) != 0) {
    return -1;
  }
  *perm = arena;
  *cache_out = cache;
  return 0;
}

static void build_cache_parse_begin(ptrdiff_t row_count, struct Arena* perm, void* userdata) {
  struct BuildCache* cache = userdata;
  cache->rows = arena_new(perm, row_count, struct BuildCacheRow);
}

static int build_cache_parse_row(struct Str line,
                                 ptrdiff_t index,
                                 struct Arena* perm,
                                 void* userdata) {
  struct BuildCache* cache = userdata;
  struct BuildCacheRow* row = &cache->rows[index];
  if (parse_row(line, row) != 0 || (index > 0 && compare_rows(&cache->rows[index - 1], row) >= 0)) {
    return -1;
  }
  row->name = str_clone(row->name, perm);
  cache->count = index + 1;
  return 0;
}

const struct BuildCacheRow* build_cache_find(const struct BuildCache* cache,
                                             enum BuildCacheKind kind,
                                             struct Str name,
                                             const struct OsStat* stat) {
  if (cache->count == 0) {
    return NULL;
  }
  const struct BuildCacheRow key = {.kind = kind, .name = name};
  const struct BuildCacheRow* row =
      bsearch(&key, cache->rows, (size_t)cache->count, sizeof(*cache->rows), compare_rows);
  if (row == NULL || row->len != stat->len || row->mtime_s != stat->mtime_s ||
      row->mtime_ns != stat->mtime_ns) {
    return NULL;
  }
  return row;
}

void build_cache_format(struct Buf* buffer,
                        const struct SiteConfig* site_config,
                        struct BuildCache* cache) {
  format_recipe(buffer, site_config);
  if (cache->count > 0) {
    qsort(cache->rows, (size_t)cache->count, sizeof(*cache->rows), compare_rows);
  }
  for (ptrdiff_t i = 0; i < cache->count; i++) {
    build_cache_format_row(buffer, &cache->rows[i]);
  }
}

static void format_recipe(struct Buf* buffer, const struct SiteConfig* site_config) {
  const struct Str generator = sosig_generator_string();
  const struct Str grammars = highlight_grammar_list();
  buf_append(buffer, S("title="));
  tsv_escape_append(buffer, site_config->title);
  buf_append(buffer, S("\tdescription="));
  tsv_escape_append(buffer, site_config->description);
  buf_append(buffer, S("\tauthor="));
  tsv_escape_append(buffer, site_config->author);
  buf_append(buffer, S("\tbase_url="));
  tsv_escape_append(buffer, site_config->base_url);
  buf_append(buffer, S("\tpermalink="));
  tsv_escape_append(buffer, site_config->permalink);
  buf_append(buffer, S("\tcontent_template="));
  tsv_escape_append(buffer, site_config->content_template);
  buf_append(buffer, S("\taggregate_templates="));
  format_recipe_list(buffer, site_config->aggregate_templates,
                     site_config->aggregate_template_count);
  buf_append(buffer, S("\tfeed_templates="));
  format_recipe_list(buffer, site_config->feed_templates, site_config->feed_template_count);
  buf_printf(buffer, "\tfeed_count=%td\tgenerator=%.*s\tgrammars=%.*s\tpage_recipe_version=%d\n",
             site_config->feed_count, STR_FMT(generator), STR_FMT(grammars), PAGE_RECIPE_VERSION);
}

static void format_recipe_list(struct Buf* buffer, const struct Str* names, ptrdiff_t count) {
  for (ptrdiff_t i = 0; i < count; i++) {
    if (i > 0) {
      buf_append_char(buffer, ',');
    }
    // A `,` inside a name is written `\,`, so one name that holds a comma never formats as two.
    struct StrCut cut = str_cut(names[i], ',');
    tsv_escape_append(buffer, cut.head);
    while (cut.is_found) {
      buf_append(buffer, S("\\,"));
      cut = str_cut(cut.tail, ',');
      tsv_escape_append(buffer, cut.head);
    }
  }
}

static void build_cache_format_row(struct Buf* buffer, const struct BuildCacheRow* row) {
  for (ptrdiff_t i = 0; i < row->name.len; i++) {
    if (row->name.data[i] == '\t' || row->name.data[i] == '\n') {
      return;
    }
  }
  const struct Str kind = BUILD_CACHE_KIND_NAMES[row->kind];
  buf_printf(buffer, "%.*s\t%.*s\t%td\t%" PRId64 "\t%" PRId64, STR_FMT(kind), STR_FMT(row->name),
             row->len, row->mtime_s, row->mtime_ns);
  if (row->kind == BUILD_CACHE_KIND_CONTENT) {
    buf_printf(buffer, "\t%d", row->is_entry_list_read ? 1 : 0);
  }
  buf_append_char(buffer, '\n');
}

static int parse_row(struct Str line, struct BuildCacheRow* row_out) {
  struct StrCut rest = {.tail = line, .is_found = true};
  struct Str kind_name = {0};
  struct BuildCacheRow row = {0};
  if (!take_field(&rest, &kind_name) || !take_field(&rest, &row.name) || row.name.len == 0) {
    return -1;
  }
  ptrdiff_t kind_index = 0;
  const ptrdiff_t kind_count =
      (ptrdiff_t)(sizeof(BUILD_CACHE_KIND_NAMES) / sizeof(BUILD_CACHE_KIND_NAMES[0]));
  while (kind_index < kind_count && !str_eq(kind_name, BUILD_CACHE_KIND_NAMES[kind_index])) {
    kind_index++;
  }
  if (kind_index == kind_count) {
    return -1;
  }
  row.kind = (enum BuildCacheKind)kind_index;
  struct Str fields[3] = {0};
  for (ptrdiff_t i = 0; i < 3; i++) {
    if (!take_field(&rest, &fields[i])) {
      return -1;
    }
  }
  // The seconds are negative for a file dated before 1970, as `stat` reports them.
  ptrdiff_t mtime_ns = 0;
  if (parse_size(fields[0], &row.len) != 0 || parse_int64(fields[1], &row.mtime_s) != 0 ||
      parse_size(fields[2], &mtime_ns) != 0 || mtime_ns >= BUILD_CACHE_NS_PER_S) {
    return -1;
  }
  row.mtime_ns = mtime_ns;
  if (row.kind == BUILD_CACHE_KIND_CONTENT) {
    struct Str flag = {0};
    if (!take_field(&rest, &flag) || !(str_eq(flag, S("0")) || str_eq(flag, S("1")))) {
      return -1;
    }
    row.is_entry_list_read = str_eq(flag, S("1"));
  }
  if (rest.is_found) {
    return -1;
  }
  *row_out = row;
  return 0;
}

static bool take_field(struct StrCut* rest, struct Str* field_out) {
  if (!rest->is_found) {
    return false;
  }
  *rest = str_cut(rest->tail, '\t');
  *field_out = rest->head;
  return true;
}

static int compare_rows(const void* a, const void* b) {
  const struct BuildCacheRow* left = a;
  const struct BuildCacheRow* right = b;
  if (left->kind != right->kind) {
    return left->kind < right->kind ? -1 : 1;
  }
  return str_compare(left->name, right->name);
}
