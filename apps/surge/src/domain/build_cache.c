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
#include "domain/surge_version.h"
#include "formats/tsv.h"
#include "markup/highlight.h"
#include "os/os.h"
#include "output/build_cache_file.h"

/** Nanoseconds in a second, the bound of a row's `mtime_ns`. */
enum { BUILD_CACHE_NS_PER_S = 1000000000 };

/** Most fields a row has after its kind and name: an export row's five. */
enum { BUILD_CACHE_FIELD_COUNT_MAX = 5 };

/** First field of each row, by `enum BuildCacheKind`. */
static const struct Str BUILD_CACHE_KIND_NAMES[] = {
    [BUILD_CACHE_KIND_TEMPLATE] = STR_INIT("template"),
    [BUILD_CACHE_KIND_STATIC] = STR_INIT("static"),
    [BUILD_CACHE_KIND_REPO] = STR_INIT("repo"),
    [BUILD_CACHE_KIND_REPO_MAP] = STR_INIT("repo-map"),
    [BUILD_CACHE_KIND_REF] = STR_INIT("ref"),
    [BUILD_CACHE_KIND_EXPORT] = STR_INIT("export"),
    [BUILD_CACHE_KIND_RELEASES] = STR_INIT("releases"),
    [BUILD_CACHE_KIND_PAGE] = STR_INIT("page"),
};

/** Fields after the name, by `enum BuildCacheKind`. */
static const ptrdiff_t BUILD_CACHE_FIELD_COUNTS[] = {
    [BUILD_CACHE_KIND_TEMPLATE] = 3, [BUILD_CACHE_KIND_STATIC] = 3, [BUILD_CACHE_KIND_REPO] = 1,
    [BUILD_CACHE_KIND_REPO_MAP] = 1, [BUILD_CACHE_KIND_REF] = 1,    [BUILD_CACHE_KIND_EXPORT] = 5,
    [BUILD_CACHE_KIND_RELEASES] = 1, [BUILD_CACHE_KIND_PAGE] = 1,
};

/** Text of an empty id field, which a tab-separated row cannot leave empty and still parse. */
#define BUILD_CACHE_ID_NONE S("-")

/**
 * @brief Appends the page recipe, the first line of a build cache, with its newline.
 *
 * @param buffer      Buffer that receives the line. Must not be `NULL`.
 * @param site_config Configuration of the build. Must not be `NULL`.
 */
static void format_recipe(struct Buf* buffer, const struct SiteConfig* site_config)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Appends one row, or nothing for a name that holds a tab or a newline.
 *
 * @param buffer Buffer that receives the row. Must not be `NULL`.
 * @param row    Row to append. Must not be `NULL`.
 */
static void build_cache_format_row(struct Buf* buffer, const struct BuildCacheRow* row)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Appends an id field, `-` for an empty one.
 *
 * @param buffer Buffer that receives the field. Must not be `NULL`.
 * @param id     Id to append.
 */
static void format_id(struct Buf* buffer, struct Str id) __attribute__((nonnull(1)));

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
 * @param row_out Receives the row, whose strings point into `line`. Must not be `NULL`.
 * @return `0` on success, or `-1` when the row has another kind, the wrong number of fields for its
 *         kind, or a field out of range.
 */
static int parse_row(struct Str line, struct BuildCacheRow* row_out) __attribute__((nonnull(2)));

/**
 * @brief Returns the id an id field holds, empty for `-`.
 *
 * @param field Text of the field.
 * @return The id.
 */
static struct Str parse_id(struct Str field);

/**
 * @brief Reports whether two rows of one kind record the same fields.
 *
 * @param a First row. Must not be `NULL`.
 * @param b Second row. Must not be `NULL`.
 * @return `true` when every field that the kind carries is equal.
 */
static bool is_same_row(const struct BuildCacheRow* a, const struct BuildCacheRow* b)
    __attribute__((nonnull(1, 2)));

struct Str build_cache_path(struct Str output_dir, struct Arena* arena) {
  struct Buf name = {.arena = arena};
  buf_printf(&name, "_surge/v%d-build-cache.tsv", BUILD_CACHE_VERSION);
  return path_join(output_dir, buf_str(&name), arena);
}

struct Str build_cache_ref_name(struct Str repo_name, struct Str ref_name, struct Arena* arena) {
  struct Buf name = {.arena = arena};
  buf_printf(&name, "%.*s/%.*s", STR_FMT(repo_name), STR_FMT(ref_name));
  return buf_str(&name);
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
  if (parse_row(line, row) != 0 ||
      (index > 0 && build_cache_compare_rows(&cache->rows[index - 1], row) >= 0)) {
    return -1;
  }
  row->name = str_clone(row->name, perm);
  row->id = str_clone(row->id, perm);
  row->info_refs_id = str_clone(row->info_refs_id, perm);
  row->previous_pack_id = str_clone(row->previous_pack_id, perm);
  cache->count = index + 1;
  return 0;
}

const struct BuildCacheRow* build_cache_find(const struct BuildCache* cache,
                                             enum BuildCacheKind kind,
                                             struct Str name) {
  if (cache->count == 0) {
    return NULL;
  }
  const struct BuildCacheRow key = {.kind = kind, .name = name};
  return bsearch(&key, cache->rows, (size_t)cache->count, sizeof(*cache->rows),
                 build_cache_compare_rows);
}

bool build_cache_has_stat(const struct BuildCache* cache,
                          enum BuildCacheKind kind,
                          struct Str name,
                          const struct OsStat* stat) {
  const struct BuildCacheRow* row = build_cache_find(cache, kind, name);
  return row != NULL && row->len == stat->len && row->mtime_s == stat->mtime_s &&
         row->mtime_ns == stat->mtime_ns;
}

bool build_cache_has_rows(const struct BuildCache* cache,
                          enum BuildCacheKind kind,
                          const struct BuildCacheRow* rows,
                          ptrdiff_t count) {
  // The cache's rows of one kind are contiguous and sorted by name, as `rows` are, so the two
  // compare in one pass.
  ptrdiff_t start = 0;
  while (start < cache->count && cache->rows[start].kind < kind) {
    start++;
  }
  ptrdiff_t cached_count = 0;
  while (start + cached_count < cache->count && cache->rows[start + cached_count].kind == kind) {
    cached_count++;
  }
  if (cached_count != count) {
    return false;
  }
  for (ptrdiff_t i = 0; i < count; i++) {
    if (!str_eq(cache->rows[start + i].name, rows[i].name) ||
        !is_same_row(&cache->rows[start + i], &rows[i])) {
      return false;
    }
  }
  return true;
}

void build_cache_format(struct Buf* buffer,
                        const struct SiteConfig* site_config,
                        struct BuildCache* cache) {
  format_recipe(buffer, site_config);
  if (cache->count > 0) {
    qsort(cache->rows, (size_t)cache->count, sizeof(*cache->rows), build_cache_compare_rows);
  }
  for (ptrdiff_t i = 0; i < cache->count; i++) {
    build_cache_format_row(buffer, &cache->rows[i]);
  }
}

int build_cache_compare_rows(const void* a, const void* b) {
  const struct BuildCacheRow* left = a;
  const struct BuildCacheRow* right = b;
  if (left->kind != right->kind) {
    return left->kind < right->kind ? -1 : 1;
  }
  return str_compare(left->name, right->name);
}

static void format_recipe(struct Buf* buffer, const struct SiteConfig* site_config) {
  const struct Str generator = surge_generator_string();
  const struct Str grammars = highlight_grammar_list();
  const struct SurgeLimits* limits = &site_config->limits;
  buf_append(buffer, S("title="));
  tsv_escape_append(buffer, site_config->title);
  buf_append(buffer, S("\tdescription="));
  tsv_escape_append(buffer, site_config->description);
  buf_append(buffer, S("\tauthor="));
  tsv_escape_append(buffer, site_config->author);
  buf_append(buffer, S("\tbase_url="));
  tsv_escape_append(buffer, site_config->base_url);
  buf_printf(buffer,
             "\tlog_page_size=%td\tfeed_count=%td\tblob_len_max=%td\traw_len_max=%td\t"
             "diff_file_count_max=%td\tdiff_line_count_max=%td\tdiff_len_max=%td\t"
             "image_len_max=%td\t"
             "generator=%.*s\tgrammars=%.*s\tpage_recipe_version=%d\n",
             site_config->log_page_size, site_config->feed_count, limits->blob_len_max,
             limits->raw_len_max, limits->diff_file_count_max, limits->diff_line_count_max,
             limits->diff_len_max, limits->image_len_max, STR_FMT(generator), STR_FMT(grammars),
             PAGE_RECIPE_VERSION);
}

static void build_cache_format_row(struct Buf* buffer, const struct BuildCacheRow* row) {
  for (ptrdiff_t i = 0; i < row->name.len; i++) {
    if (row->name.data[i] == '\t' || row->name.data[i] == '\n') {
      return;
    }
  }
  const struct Str kind = BUILD_CACHE_KIND_NAMES[row->kind];
  buf_printf(buffer, "%.*s\t%.*s", STR_FMT(kind), STR_FMT(row->name));
  switch (row->kind) {
    case BUILD_CACHE_KIND_TEMPLATE:
    case BUILD_CACHE_KIND_STATIC:
      buf_printf(buffer, "\t%td\t%" PRId64 "\t%" PRId64, row->len, row->mtime_s, row->mtime_ns);
      break;
    case BUILD_CACHE_KIND_REPO:
    case BUILD_CACHE_KIND_REPO_MAP:
    case BUILD_CACHE_KIND_REF:
    case BUILD_CACHE_KIND_RELEASES:
    case BUILD_CACHE_KIND_PAGE:
      buf_append_char(buffer, '\t');
      format_id(buffer, row->id);
      break;
    case BUILD_CACHE_KIND_EXPORT:
      buf_append_char(buffer, '\t');
      format_id(buffer, row->id);
      buf_printf(buffer, "\t%td\t%td\t", row->pack_len, row->idx_len);
      format_id(buffer, row->info_refs_id);
      buf_append_char(buffer, '\t');
      format_id(buffer, row->previous_pack_id);
      break;
  }
  buf_append_char(buffer, '\n');
}

static void format_id(struct Buf* buffer, struct Str id) {
  buf_append(buffer, id.len > 0 ? id : BUILD_CACHE_ID_NONE);
}

static int parse_row(struct Str line, struct BuildCacheRow* row_out) {
  struct Str fields[2 + BUILD_CACHE_FIELD_COUNT_MAX] = {0};
  ptrdiff_t field_count = 0;
  struct StrCut cut = {.tail = line, .is_found = true};
  while (cut.is_found) {
    if (field_count == 2 + BUILD_CACHE_FIELD_COUNT_MAX) {
      return -1;
    }
    cut = str_cut(cut.tail, '\t');
    fields[field_count++] = cut.head;
  }
  struct BuildCacheRow row = {.name = fields[1]};
  ptrdiff_t kind_index = 0;
  const ptrdiff_t kind_count =
      (ptrdiff_t)(sizeof(BUILD_CACHE_KIND_NAMES) / sizeof(BUILD_CACHE_KIND_NAMES[0]));
  while (kind_index < kind_count && !str_eq(fields[0], BUILD_CACHE_KIND_NAMES[kind_index])) {
    kind_index++;
  }
  if (kind_index == kind_count || row.name.len == 0 ||
      field_count != 2 + BUILD_CACHE_FIELD_COUNTS[kind_index]) {
    return -1;
  }
  row.kind = (enum BuildCacheKind)kind_index;
  int64_t numbers[3] = {0};
  switch (row.kind) {
    case BUILD_CACHE_KIND_TEMPLATE:
    case BUILD_CACHE_KIND_STATIC:
      for (ptrdiff_t i = 0; i < 3; i++) {
        if (parse_int64(fields[2 + i], &numbers[i]) != 0) {
          return -1;
        }
      }
      // Only the seconds may be negative, for a file dated before 1970.
      if (numbers[0] < 0 || numbers[2] < 0 || numbers[2] >= BUILD_CACHE_NS_PER_S ||
          numbers[0] > PTRDIFF_MAX) {
        return -1;
      }
      row.len = (ptrdiff_t)numbers[0];
      row.mtime_s = numbers[1];
      row.mtime_ns = numbers[2];
      break;
    case BUILD_CACHE_KIND_REPO:
    case BUILD_CACHE_KIND_REPO_MAP:
    case BUILD_CACHE_KIND_REF:
    case BUILD_CACHE_KIND_RELEASES:
    case BUILD_CACHE_KIND_PAGE:
      row.id = parse_id(fields[2]);
      break;
    case BUILD_CACHE_KIND_EXPORT:
      if (parse_int64(fields[3], &numbers[0]) != 0 || parse_int64(fields[4], &numbers[1]) != 0 ||
          numbers[0] < 0 || numbers[1] < 0 || numbers[0] > PTRDIFF_MAX ||
          numbers[1] > PTRDIFF_MAX) {
        return -1;
      }
      row.id = parse_id(fields[2]);
      row.pack_len = (ptrdiff_t)numbers[0];
      row.idx_len = (ptrdiff_t)numbers[1];
      row.info_refs_id = parse_id(fields[5]);
      row.previous_pack_id = parse_id(fields[6]);
      break;
  }
  *row_out = row;
  return 0;
}

static struct Str parse_id(struct Str field) {
  return !str_eq(field, BUILD_CACHE_ID_NONE) ? field : (struct Str){0};
}

static bool is_same_row(const struct BuildCacheRow* a, const struct BuildCacheRow* b) {
  switch (a->kind) {
    case BUILD_CACHE_KIND_TEMPLATE:
    case BUILD_CACHE_KIND_STATIC:
      return a->len == b->len && a->mtime_s == b->mtime_s && a->mtime_ns == b->mtime_ns;
    case BUILD_CACHE_KIND_REPO:
    case BUILD_CACHE_KIND_REPO_MAP:
    case BUILD_CACHE_KIND_REF:
    case BUILD_CACHE_KIND_RELEASES:
    case BUILD_CACHE_KIND_PAGE:
      return str_eq(a->id, b->id);
    case BUILD_CACHE_KIND_EXPORT:
      return str_eq(a->id, b->id) && a->pack_len == b->pack_len && a->idx_len == b->idx_len &&
             str_eq(a->info_refs_id, b->info_refs_id) &&
             str_eq(a->previous_pack_id, b->previous_pack_id);
  }
  return false;
}
