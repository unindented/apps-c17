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
#include "domain/fram_version.h"
#include "media/image.h"
#include "os/os.h"
#include "output/build_cache_file.h"

/** First field of a build cache's first line, which names the file kind. */
#define BUILD_CACHE_FILE_KIND "fram-build-cache"

/** Name of each row kind in the build cache, indexed by `enum BuildCacheKind`. */
static const struct Str BUILD_CACHE_KIND_NAMES[] = {
    [BUILD_CACHE_KIND_MEDIA] = STR_INIT("media"),
    [BUILD_CACHE_KIND_STATIC] = STR_INIT("static"),
};

/** Nanoseconds in a second, the bound of a row's `mtime_ns`. */
enum { BUILD_CACHE_NS_PER_S = 1000000000 };

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
 * @return `0` on success, or `-1` when the row does not have the fields of its kind or one is out
 *         of range.
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
 * @brief Orders two rows by kind and then bytewise by name, for the sort check and `bsearch`.
 *
 * @param a First `struct BuildCacheRow`. Must not be `NULL`.
 * @param b Second `struct BuildCacheRow`. Must not be `NULL`.
 * @return A negative, zero, or positive value as `a` orders before, equal to, or after `b`.
 */
static int compare_rows(const void* a, const void* b) __attribute__((nonnull(1, 2)));

struct Str build_cache_path(struct Str output_dir, struct Arena* arena) {
  struct Buf name = {.arena = arena};
  buf_printf(&name, "_fram/v%d-build-cache.tsv", BUILD_CACHE_VERSION);
  return path_join(output_dir, buf_str(&name), arena);
}

int build_cache_parse(struct Str text,
                      struct Arena* perm,
                      struct BuildCache* cache_out,
                      struct Log* log) {
  struct Arena arena = *perm;
  struct Buf header = {.arena = &arena};
  build_cache_format_header(&header);
  struct BuildCache cache = {0};
  if (build_cache_file_parse(
          text, buf_str(&header), S("build cache was written by another fram version"),
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
                                             const struct OsStat* source_stat) {
  if (cache->count == 0) {
    return NULL;
  }
  const struct BuildCacheRow key = {.kind = kind, .name = name};
  const struct BuildCacheRow* row =
      bsearch(&key, cache->rows, (size_t)cache->count, sizeof(*cache->rows), compare_rows);
  if (row == NULL || row->len != source_stat->len || row->mtime_s != source_stat->mtime_s ||
      row->mtime_ns != source_stat->mtime_ns) {
    return NULL;
  }
  return row;
}

void build_cache_format_header(struct Buf* buffer) {
  const struct Str generator = fram_generator_string();
  buf_printf(buffer, "%s\t%.*s\n", BUILD_CACHE_FILE_KIND, STR_FMT(generator));
}

void build_cache_format_row(struct Buf* buffer, const struct BuildCacheRow* row) {
  for (ptrdiff_t i = 0; i < row->name.len; i++) {
    if (row->name.data[i] == '\t' || row->name.data[i] == '\n') {
      return;
    }
  }
  const struct Str kind = BUILD_CACHE_KIND_NAMES[row->kind];
  buf_printf(buffer, "%.*s\t%.*s\t%td\t%" PRId64 "\t%" PRId64, STR_FMT(kind), STR_FMT(row->name),
             row->len, row->mtime_s, row->mtime_ns);
  if (row->kind == BUILD_CACHE_KIND_MEDIA) {
    buf_printf(buffer, "\t%td\t%td\t%d\t%td", row->width_px, row->height_px, (int)row->orientation,
               row->duration_ms);
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
  const ptrdiff_t field_count = row.kind == BUILD_CACHE_KIND_MEDIA ? 7 : 3;
  ptrdiff_t values[7] = {0};
  for (ptrdiff_t i = 0; i < field_count; i++) {
    struct Str field = {0};
    if (!take_field(&rest, &field)) {
      return -1;
    }
    // The seconds, the second field, are negative for a file dated before 1970, as `stat` reports
    // them.
    const int parse_rc = i == 1 ? parse_int64(field, &row.mtime_s) : parse_size(field, &values[i]);
    if (parse_rc != 0) {
      return -1;
    }
  }
  if (rest.is_found || values[2] >= BUILD_CACHE_NS_PER_S) {
    return -1;
  }
  row.len = values[0];
  row.mtime_ns = values[2];
  if (row.kind == BUILD_CACHE_KIND_MEDIA) {
    const ptrdiff_t orientation = values[5];
    if (values[3] == 0 || values[4] == 0 || orientation < IMAGE_ORIENTATION_TOP_LEFT ||
        orientation > IMAGE_ORIENTATION_LEFT_BOTTOM) {
      return -1;
    }
    row.width_px = values[3];
    row.height_px = values[4];
    row.orientation = (enum ImageOrientation)orientation;
    row.duration_ms = values[6];
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
