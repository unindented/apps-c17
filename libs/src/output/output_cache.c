#include "output/output_cache.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/parse.h"
#include "core/path.h"
#include "core/str.h"
#include "os/os.h"
#include "output/build_cache_file.h"
#include "output/manifest.h"
#include "output/output_writer.h"

/**
 * First line of an output cache, which names the format and its version. Change the version
 * whenever the row format or the meaning of a field changes, so a build never reads a file of
 * another format.
 */
#define HEADER S("output cache v1\n")

/** A parse in progress: the cache it fills, and the row count that sizes its arrays. */
struct LoadParse {
  /** Cache that receives the rows and facts. */
  struct OutputCache cache;

  /** Number of rows in the file, outputs and facts together. */
  ptrdiff_t row_count;
};

/**
 * @brief Parses the text of an output cache into the `struct OutputCache` that `userdata` points
 *        to, as `BuildCacheFileParseFn`.
 *
 * @param text     Bytes of the output cache, which the cache's strings are views of.
 * @param perm     Arena that owns the rows and facts. Must not be `NULL`.
 * @param log      Log that receives the reason the cache cannot be used. Must not be `NULL`.
 * @param userdata The `struct OutputCache` that receives the cache on success.
 * @return `0` on success, or `-1` when the text does not parse.
 */
static int load_parse(struct Str text, struct Arena* perm, struct Log* log, void* userdata)
    __attribute__((nonnull(2, 3)));

/**
 * @brief Sizes the output rows for every row of the file, as `BuildCacheFileBeginFn`.
 *
 * The facts come after the outputs, so their array is sized at the first fact, for the rows left.
 *
 * @param row_count Number of rows the file holds.
 * @param perm      Arena that owns the rows. Must not be `NULL`.
 * @param userdata  The `struct LoadParse`.
 */
static void load_parse_begin(ptrdiff_t row_count, struct Arena* perm, void* userdata)
    __attribute__((nonnull(2)));

/**
 * @brief Parses one row, an output or a fact, as `BuildCacheFileRowFn`.
 *
 * @param line     The row, without its newline.
 * @param index    Zero-based index of the row.
 * @param perm     Arena that owns the facts' array. Must not be `NULL`.
 * @param userdata The `struct LoadParse`.
 * @return `0` on success, or `-1` when the row does not parse or is out of order.
 */
static int load_parse_row(struct Str line, ptrdiff_t index, struct Arena* perm, void* userdata)
    __attribute__((nonnull(3)));

/**
 * @brief Parses the fields of an output row, `<path><TAB><attributes><TAB><len>`.
 *
 * @param fields The row after its kind and its tab.
 * @param load   Parse that receives the row. Must not be `NULL`.
 * @return `0` on success, or `-1` when the row does not parse, follows a fact, or does not sort
 *         after the row before it.
 */
static int load_parse_output(struct Str fields, struct LoadParse* load) __attribute__((nonnull(2)));

/**
 * @brief Parses the fields of a fact row, `<key><TAB><value>`.
 *
 * @param fields The row after its kind and its tab.
 * @param index  Zero-based index of the row in the file.
 * @param perm   Arena that owns the facts' array, which the first fact allocates. Must not be
 *               `NULL`.
 * @param load   Parse that receives the fact. Must not be `NULL`.
 * @return `0` on success, or `-1` when the row does not parse or does not sort after the fact
 *         before it.
 */
static int load_parse_fact(struct Str fields,
                           ptrdiff_t index,
                           struct Arena* perm,
                           struct LoadParse* load) __attribute__((nonnull(3, 4)));

/**
 * @brief Collects a row for each output of the manifest that is written or kept, declares
 *        attributes, and is below `output_dir`, with the size its mark recorded, or else with the
 *        size a stat gives when it exists as a file.
 *
 * @param writer    Writer whose manifest and host are read. Must not be `NULL`.
 * @param arena     Arena that owns the rows. Must not be `NULL`.
 * @param count_out Receives the number of rows. Must not be `NULL`.
 * @return The rows, unsorted, whose strings are borrowed from the manifest.
 */
static struct OutputCacheRow* write_collect_rows(const struct OutputWriter* writer,
                                                 struct Arena* arena,
                                                 ptrdiff_t* count_out)
    __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Reports whether `text` can be a field of a row, because it holds no tab and no newline.
 *
 * @param text Text of the field.
 * @return `true` when `text` holds neither byte.
 */
static bool is_field(struct Str text);

/**
 * @brief Orders two rows bytewise by path, for `qsort` and `bsearch`.
 *
 * @param a First `struct OutputCacheRow`. Must not be `NULL`.
 * @param b Second `struct OutputCacheRow`. Must not be `NULL`.
 * @return A negative, zero, or positive value per `str_compare` of the two paths.
 */
static int compare_rows(const void* a, const void* b) __attribute__((nonnull(1, 2)));

/**
 * @brief Orders two facts bytewise by key, for `qsort` and `bsearch`.
 *
 * @param a First `struct OutputCacheFact`. Must not be `NULL`.
 * @param b Second `struct OutputCacheFact`. Must not be `NULL`.
 * @return A negative, zero, or positive value per `str_compare` of the two keys.
 */
static int compare_facts(const void* a, const void* b) __attribute__((nonnull(1, 2)));

enum BuildCacheFileState output_cache_load(struct Str cache_path,
                                           struct Os* os,
                                           struct Arena* perm,
                                           struct Arena scratch,
                                           struct OutputCache* cache_out,
                                           struct Str* reason_out) {
  *cache_out = (struct OutputCache){0};
  return build_cache_file_load(cache_path, OUTPUT_CACHE_FILE_LEN_MAX, load_parse, cache_out, os,
                               perm, scratch, reason_out);
}

const struct OutputCacheRow* output_cache_find(const struct OutputCache* cache,
                                               struct Str relative_path) {
  if (cache->row_count == 0) {
    return NULL;
  }
  const struct OutputCacheRow key = {.relative_path = relative_path};
  return bsearch(&key, cache->rows, (size_t)cache->row_count, sizeof(*cache->rows), compare_rows);
}

const struct OutputCacheFact* output_cache_find_fact(const struct OutputCache* cache,
                                                     struct Str key) {
  if (cache->fact_count == 0) {
    return NULL;
  }
  const struct OutputCacheFact fact = {.key = key};
  return bsearch(&fact, cache->facts, (size_t)cache->fact_count, sizeof(*cache->facts),
                 compare_facts);
}

int output_cache_write(const struct OutputWriter* writer,
                       struct Str cache_path,
                       const struct OutputCacheFact* facts,
                       ptrdiff_t fact_count,
                       struct Arena scratch,
                       struct Log* log) {
  if (writer->is_dry_run) {
    return output_writer_write(writer, cache_path, (struct OsSource){0},
                               S("failed to write output cache"), log);
  }
  ptrdiff_t row_count = 0;
  struct OutputCacheRow* rows = write_collect_rows(writer, &scratch, &row_count);
  if (row_count > 0) {
    qsort(rows, (size_t)row_count, sizeof(*rows), compare_rows);
  }
  struct OutputCacheFact* sorted_facts = arena_new(&scratch, fact_count, struct OutputCacheFact);
  for (ptrdiff_t i = 0; i < fact_count; i++) {
    sorted_facts[i] = facts[i];
  }
  if (fact_count > 0) {
    qsort(sorted_facts, (size_t)fact_count, sizeof(*sorted_facts), compare_facts);
  }
  struct Buf text = {.arena = &scratch};
  buf_append(&text, HEADER);
  for (ptrdiff_t i = 0; i < row_count; i++) {
    buf_printf(&text, "output\t%.*s\t%.*s\t%td\n", STR_FMT(rows[i].relative_path),
               STR_FMT(rows[i].attributes), rows[i].len);
  }
  for (ptrdiff_t i = 0; i < fact_count; i++) {
    const struct OutputCacheFact* fact = &sorted_facts[i];
    if (is_field(fact->key) && is_field(fact->value)) {
      buf_printf(&text, "fact\t%.*s\t%.*s\n", STR_FMT(fact->key), STR_FMT(fact->value));
    }
  }
  return output_writer_write(writer, cache_path, (struct OsSource){.bytes = buf_str(&text)},
                             S("failed to write output cache"), log);
}

static int load_parse(struct Str text, struct Arena* perm, struct Log* log, void* userdata) {
  struct LoadParse load = {0};
  if (build_cache_file_parse(text, HEADER, S("output cache is not in the current format"),
                             load_parse_begin, load_parse_row, &load, perm, log) != 0) {
    return -1;
  }
  *(struct OutputCache*)userdata = load.cache;
  return 0;
}

static void load_parse_begin(ptrdiff_t row_count, struct Arena* perm, void* userdata) {
  struct LoadParse* load = userdata;
  load->row_count = row_count;
  load->cache.rows = arena_new(perm, row_count, struct OutputCacheRow);
}

static int load_parse_row(struct Str line, ptrdiff_t index, struct Arena* perm, void* userdata) {
  struct LoadParse* load = userdata;
  const struct StrCut kind = str_cut(line, '\t');
  if (!kind.is_found) {
    return -1;
  }
  if (str_eq(kind.head, S("output"))) {
    return load_parse_output(kind.tail, load);
  }
  if (str_eq(kind.head, S("fact"))) {
    return load_parse_fact(kind.tail, index, perm, load);
  }
  return -1;
}

static int load_parse_output(struct Str fields, struct LoadParse* load) {
  struct OutputCache* cache = &load->cache;
  const struct StrCut path = str_cut(fields, '\t');
  const struct StrCut attributes = str_cut(path.tail, '\t');
  struct OutputCacheRow row = {.relative_path = path.head, .attributes = attributes.head};
  if (cache->fact_count > 0 || !path.is_found || !attributes.is_found || path.head.len == 0 ||
      attributes.head.len == 0 || parse_size(attributes.tail, &row.len) != 0) {
    return -1;
  }
  if (cache->row_count > 0 && compare_rows(&cache->rows[cache->row_count - 1], &row) >= 0) {
    return -1;
  }
  cache->rows[cache->row_count++] = row;
  return 0;
}

static int load_parse_fact(struct Str fields,
                           ptrdiff_t index,
                           struct Arena* perm,
                           struct LoadParse* load) {
  struct OutputCache* cache = &load->cache;
  const struct StrCut key = str_cut(fields, '\t');
  const struct OutputCacheFact fact = {.key = key.head, .value = key.tail};
  if (!key.is_found || !is_field(fact.value)) {
    return -1;
  }
  if (cache->fact_count == 0) {
    cache->facts = arena_new(perm, load->row_count - index, struct OutputCacheFact);
  } else if (compare_facts(&cache->facts[cache->fact_count - 1], &fact) >= 0) {
    return -1;
  }
  cache->facts[cache->fact_count++] = fact;
  return 0;
}

static struct OutputCacheRow* write_collect_rows(const struct OutputWriter* writer,
                                                 struct Arena* arena,
                                                 ptrdiff_t* count_out) {
  const struct Manifest* manifest = writer->manifest;
  struct OutputCacheRow* rows = arena_new(arena, manifest->count, struct OutputCacheRow);
  ptrdiff_t count = 0;
  struct Str output_path = {0};
  while (manifest_next_output_path(manifest, &output_path)) {
    const enum ManifestMark mark = manifest_find_mark(manifest, output_path);
    struct ManifestOutput output = {0};
    (void)manifest_find_output(manifest, output_path, &output);
    struct Str relative_path = {0};
    if (mark == MANIFEST_MARK_PENDING || output.attributes.len == 0 ||
        !is_field(output.attributes) ||
        !path_is_below(output_path, writer->output_dir, &relative_path) ||
        !is_field(relative_path)) {
      continue;
    }
    // Only an output whose mark recorded no size, such as one an app kept itself, gets a stat.
    ptrdiff_t len = manifest_find_len(manifest, output_path);
    struct OsStat st;
    if (len < 0 && os_stat(writer->os, output_path, &st) == 0 && !st.is_dir) {
      len = st.len;
    }
    if (len >= 0) {
      rows[count++] = (struct OutputCacheRow){
          .relative_path = relative_path, .attributes = output.attributes, .len = len};
    }
  }
  *count_out = count;
  return rows;
}

static bool is_field(struct Str text) {
  for (ptrdiff_t i = 0; i < text.len; i++) {
    if (text.data[i] == '\t' || text.data[i] == '\n') {
      return false;
    }
  }
  return true;
}

static int compare_rows(const void* a, const void* b) {
  return str_compare(((const struct OutputCacheRow*)a)->relative_path,
                     ((const struct OutputCacheRow*)b)->relative_path);
}

static int compare_facts(const void* a, const void* b) {
  return str_compare(((const struct OutputCacheFact*)a)->key,
                     ((const struct OutputCacheFact*)b)->key);
}
