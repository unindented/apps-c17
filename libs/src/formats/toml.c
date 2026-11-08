#include "formats/toml.h"

#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/path.h"
#include "core/str.h"
#include "core/text.h"
#include "core/url.h"

/**
 * Size in bytes of the longest timestamp `toml_datum_format_rfc3339` writes,
 * `YYYY-MM-DDThh:mm:ss.uuuuuu+hh:mm`, including the `NUL`. tomlc17 accepts only a four-digit year,
 * in-range date and time fields, at most six fraction digits, and an offset within `±23:59`, so
 * every datum it parses fits.
 */
enum { RFC3339_SIZE = sizeof("YYYY-MM-DDThh:mm:ss.uuuuuu+hh:mm") };

// The components are formatted and negated as `int`, and `toml_datum_to_epoch`'s product stays
// inside `int64_t` because `year` is 16 bits. A vendor bump that widens a field must revisit both.
_Static_assert(sizeof(((toml_datum_t*)NULL)->u.ts.year) == sizeof(int16_t) &&
                   sizeof(((toml_datum_t*)NULL)->u.ts.usec) == sizeof(int32_t) &&
                   sizeof(((toml_datum_t*)NULL)->u.ts.tz) == sizeof(int16_t),
               "tomlc17 timestamp fields changed width");

/**
 * @brief Installs `toml_parse_document_realloc` as tomlc17's allocator. Run once through
 *        `pthread_once`.
 */
static void toml_parse_document_install_allocator(void);

/**
 * @brief Reallocates like `realloc`, calling `os_oom` instead of returning a failure.
 *
 * This is tomlc17's allocator once `toml_parse_document` installs it.
 *
 * @param ptr  Allocation to resize, or `NULL` to allocate.
 * @param size New size in bytes.
 * @return The resized allocation. `NULL` only for a zero `size`, as `realloc` allows.
 */
static void* toml_parse_document_realloc(void* ptr, size_t size);

/**
 * @brief Converts a proleptic Gregorian date to days since `1970-01-01`.
 *
 * @param year  Full Gregorian year.
 * @param month Month in the range `1` to `12`.
 * @param day   Day of month in the range `1` to `31`.
 * @return Signed day count relative to the Unix epoch (negative before `1970-01-01`).
 */
static int64_t days_from_civil(int year, unsigned month, unsigned day);

/**
 * @brief Reports whether a key matches one of a list of names.
 *
 * @param key             Key bytes.
 * @param known_keys      Names to match against. May be `NULL` only when `known_key_count` is 0.
 * @param known_key_count Number of names in `known_keys`.
 * @return `true` when `key` equals one of `known_keys`, otherwise `false`.
 */
static bool is_known_key(struct Str key, const struct Str* known_keys, ptrdiff_t known_key_count);

int toml_parse_document(struct Str document,
                        struct Str file_path,
                        toml_result_t* parsed_out,
                        struct Str operation,
                        struct Log* log) {
  assert(document.len >= 0 && document.len <= INT_MAX);
  static pthread_once_t allocator_once = PTHREAD_ONCE_INIT;
  (void)pthread_once(&allocator_once, toml_parse_document_install_allocator);
  // The source name tomlc17 can attach to each datum goes unread here, and the error message never
  // includes it, so the document is parsed unnamed.
  *parsed_out = toml_parse(document.data, (int)document.len);
  if (!parsed_out->ok) {
    (void)log_error_in(log, file_path, "%.*s: %s", STR_FMT(operation), parsed_out->errmsg);
    // The call currently releases nothing. tomlc17 frees its datum tree and pool on failure, then
    // returns a zeroed result. The vendor requires callers to pass every result to `toml_free`.
    // Following that contract keeps this code correct if a later version retains data after
    // failure.
    toml_free(*parsed_out);
    return -1;
  }
  return 0;
}

bool toml_datum_is_text(toml_datum_t value) {
  // tomlc17 terminates the string but excludes that terminator from `len`, so scanning exactly
  // `len` bytes finds only a `NUL` that the parser decoded from an escape.
  return value.type == TOML_STRING && text_is_nul_free(toml_datum_str(value));
}

struct Str toml_datum_str(toml_datum_t value) {
  assert(value.type == TOML_STRING);
  return (struct Str){value.u.str.ptr, value.u.str.len};
}

int toml_datum_to_epoch(toml_datum_t value, int64_t* epoch_out) {
  int64_t day_seconds;
  int timezone_minutes;
  switch (value.type) {
    case TOML_DATE:
      day_seconds = 0;
      timezone_minutes = 0;
      break;
    case TOML_DATETIME:
    case TOML_DATETIMETZ:
      day_seconds =
          (int64_t)value.u.ts.hour * 3600 + (int64_t)value.u.ts.minute * 60 + value.u.ts.second;
      timezone_minutes = value.type == TOML_DATETIMETZ ? value.u.ts.tz : 0;
      break;
    default:
      return -1;
  }
  const int64_t days =
      days_from_civil(value.u.ts.year, (unsigned)value.u.ts.month, (unsigned)value.u.ts.day);
  // Signed overflow below would be undefined behavior, and cannot arise. `value.u.ts.year` is an
  // `int16_t`, as the assertion above pins, so even at its extreme the day count stays near 1.2e7
  // and the product near 1e12, orders of magnitude inside `int64_t`.
  *epoch_out = days * 86400 + day_seconds - (int64_t)timezone_minutes * 60;
  return 0;
}

struct Str toml_datum_format_rfc3339(toml_datum_t value, struct Arena* arena) {
  assert(value.type == TOML_DATE || value.type == TOML_DATETIME || value.type == TOML_DATETIMETZ);
  // tomlc17 fills every time field of a `TOML_DATE` with `-1`, so a date-only datum takes midnight
  // rather than reading them.
  const bool has_time = value.type != TOML_DATE;
  // The fraction drops its trailing zeros. A zero fraction keeps a precision of `0`, which prints
  // no digits, so a whole second gets no `.` either.
  int fraction = has_time ? value.u.ts.usec : 0;
  int fraction_digits = fraction != 0 ? 6 : 0;
  while (fraction != 0 && fraction % 10 == 0) {
    fraction /= 10;
    fraction_digits--;
  }
  const int timezone_minutes = value.type == TOML_DATETIMETZ ? value.u.ts.tz : 0;
  const int offset_minutes = timezone_minutes < 0 ? -timezone_minutes : timezone_minutes;
  char buf[RFC3339_SIZE];
  const int n =
      snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d%s%.*d%c%02d:%02d", value.u.ts.year,
               value.u.ts.month, value.u.ts.day, has_time ? value.u.ts.hour : 0,
               has_time ? value.u.ts.minute : 0, has_time ? value.u.ts.second : 0,
               fraction_digits > 0 ? "." : "", fraction_digits, fraction,
               timezone_minutes < 0 ? '-' : '+', offset_minutes / 60, offset_minutes % 60);
  // `RFC3339_SIZE` holds every timestamp tomlc17 can parse.
  assert(n > 0 && n < (int)sizeof(buf));
  struct Str timestamp = {buf, n};
  // A zero offset is spelled `Z` rather than `+00:00`.
  if (timezone_minutes == 0) {
    const ptrdiff_t offset_len = S("+00:00").len;
    buf[n - offset_len] = 'Z';
    timestamp = str_head(timestamp, n - offset_len + 1);
  }
  return str_clone(timestamp, arena);
}

int toml_require_known_keys(toml_datum_t table,
                            const struct Str* known_keys,
                            ptrdiff_t known_key_count,
                            struct Str key_kind,
                            struct Str key_prefix,
                            struct Str file_path,
                            struct Log* log) {
  int rc = 0;
  for (int32_t i = 0; i < table.u.tab.size; i++) {
    const struct Str key = {table.u.tab.key[i], table.u.tab.len[i]};
    if (is_known_key(key, known_keys, known_key_count)) {
      continue;
    }
    // A quoted key can hold a `NUL` decoded from an escape, where `%.*s` would stop and name a
    // different key, so such a key is rejected without being named, but with the table it is in. No
    // known key holds a `NUL`, so it is unknown too.
    if (!text_is_nul_free(key) && key_prefix.len > 0) {
      // The table is named without the `.` that joins the prefix to a key.
      const struct Str table_name = str_head(key_prefix, key_prefix.len - 1);
      rc = log_error_in(log, file_path, "%.*s key under '%.*s' must not contain a NUL byte",
                        STR_FMT(key_kind), STR_FMT(table_name));
    } else if (!text_is_nul_free(key)) {
      rc = log_error_in(log, file_path, "%.*s key must not contain a NUL byte", STR_FMT(key_kind));
    } else {
      rc = log_error_in(log, file_path, "unknown %.*s key '%.*s%.*s'", STR_FMT(key_kind),
                        STR_FMT(key_prefix), STR_FMT(key));
    }
  }
  return rc;
}

int toml_copy_string(toml_datum_t table,
                     struct Str key_prefix,
                     const char* key,
                     bool is_required,
                     struct Arena* arena,
                     struct Str* value_out,
                     struct Log* log) {
  const toml_datum_t value = toml_get(table, key);
  if (value.type == TOML_UNKNOWN) {
    if (is_required) {
      return log_error(log, "missing required config key '%.*s%s'", STR_FMT(key_prefix), key);
    }
    return 0;
  }
  if (value.type != TOML_STRING) {
    return log_error(log, "config key '%.*s%s' must be a string", STR_FMT(key_prefix), key);
  }
  if (!toml_datum_is_text(value)) {
    return log_error(log, "config key '%.*s%s' must not contain a NUL byte", STR_FMT(key_prefix),
                     key);
  }
  *value_out = str_clone(toml_datum_str(value), arena);
  return 0;
}

int toml_copy_base_url(toml_datum_t table,
                       struct Str key_prefix,
                       const char* key,
                       bool is_required,
                       struct Arena* arena,
                       struct Str* value_out,
                       struct Log* log) {
  // A present key gives a copy whose `data` is never `NULL`, so a `NULL` one is an absent key.
  struct Str url = {0};
  // A value that is missing or mistyped is already reported, so it is not judged a second time.
  if (toml_copy_string(table, key_prefix, key, is_required, arena, &url, log) != 0) {
    return -1;
  }
  if (url.data == NULL) {
    return 0;
  }
  if (url.len == 0 && !is_required) {
    *value_out = url;
    return 0;
  }
  if (!url_is_http_with_host(url)) {
    return log_error(
        log,
        "config key '%.*s%s' must be an absolute 'http://' or 'https://' URL with a host: '%.*s'",
        STR_FMT(key_prefix), key, STR_FMT(url));
  }
  // Every link appends a path to the URL, so a query or a fragment in it would swallow the path,
  // and a space or a control byte would reach the links raw.
  if (url_has_path_breaking_byte(url)) {
    return log_error(log,
                     "config key '%.*s%s' must not hold '?', '#', a space or a control "
                     "character: '%.*s'",
                     STR_FMT(key_prefix), key, STR_FMT(url));
  }
  // Every link joins the URL with a path that starts with `/`, so the trailing `/` goes.
  *value_out = path_trim_trailing_slashes(url);
  return 0;
}

int toml_copy_dir(toml_datum_t table,
                  struct Str key_prefix,
                  const char* key,
                  bool is_required,
                  struct Str config_dir,
                  struct Arena* arena,
                  struct Str* value_out,
                  struct Log* log) {
  struct Str dir = {0};
  if (toml_copy_string(table, key_prefix, key, is_required, arena, &dir, log) != 0) {
    return -1;
  }
  // A present key is a copy that is never `NULL`, even when it is empty.
  if (dir.data == NULL) {
    return 0;
  }
  dir = path_trim_trailing_slashes(dir);
  if (dir.len == 0) {
    return log_error(log, "config key '%.*s%s' must not be empty", STR_FMT(key_prefix), key);
  }
  *value_out = path_resolve(config_dir, dir, arena);
  return 0;
}

int toml_copy_bool(toml_datum_t table,
                   struct Str key_prefix,
                   const char* key,
                   bool* value_out,
                   struct Log* log) {
  const toml_datum_t value = toml_get(table, key);
  if (value.type == TOML_UNKNOWN) {
    return 0;
  }
  if (value.type != TOML_BOOLEAN) {
    return log_error(log, "config key '%.*s%s' must be a boolean", STR_FMT(key_prefix), key);
  }
  *value_out = value.u.boolean;
  return 0;
}

int toml_copy_template_name(toml_datum_t table,
                            struct Str key_prefix,
                            const char* key,
                            struct Arena* arena,
                            struct Str* value_out,
                            struct Log* log) {
  // A present key gives a copy whose `data` is never `NULL`, so a `NULL` one is an absent key.
  struct Str name = {0};
  if (toml_copy_string(table, key_prefix, key, false, arena, &name, log) != 0) {
    return -1;
  }
  if (name.data == NULL) {
    return 0;
  }
  // `path_is_safe_relative` rejects an empty name too, so an empty value gets the same message.
  if (!path_is_safe_relative(name)) {
    return log_error(log, "config key '%.*s%s' must be a safe relative template name: '%.*s'",
                     STR_FMT(key_prefix), key, STR_FMT(name));
  }
  *value_out = name;
  return 0;
}

int toml_copy_strings(toml_datum_t table,
                      struct Str key_prefix,
                      const char* key,
                      bool is_template_names,
                      struct Arena* arena,
                      const struct Str** items_out,
                      ptrdiff_t* item_count_out,
                      struct Log* log) {
  const toml_datum_t value = toml_get(table, key);
  if (value.type == TOML_UNKNOWN) {
    return 0;
  }
  if (value.type != TOML_ARRAY) {
    return log_error(log, "config key '%.*s%s' must be an array", STR_FMT(key_prefix), key);
  }

  const ptrdiff_t item_count = value.u.arr.size;
  // A zero-count allocation still returns a pointer, so a list copied from the file is never
  // `NULL`.
  struct Str* items = arena_new(arena, item_count, struct Str);
  int rc = 0;
  bool is_type_reported = false;
  bool is_nul_reported = false;
  for (ptrdiff_t i = 0; i < item_count; i++) {
    const toml_datum_t item = value.u.arr.elem[i];
    if (item.type != TOML_STRING) {
      if (!is_type_reported) {
        rc = log_error(log, "config key '%.*s%s' must contain only strings", STR_FMT(key_prefix),
                       key);
        is_type_reported = true;
      }
      continue;
    }
    if (!toml_datum_is_text(item)) {
      if (!is_nul_reported) {
        rc = log_error(log, "config key '%.*s%s' must not contain a NUL byte", STR_FMT(key_prefix),
                       key);
        is_nul_reported = true;
      }
      continue;
    }
    const struct Str text = toml_datum_str(item);
    if (is_template_names && !path_is_safe_relative(text)) {
      rc = log_error(log,
                     "config key '%.*s%s' must contain only safe relative template names: '%.*s'",
                     STR_FMT(key_prefix), key, STR_FMT(text));
      continue;
    }
    items[i] = str_clone(text, arena);
  }
  if (rc != 0) {
    return -1;
  }
  // The list is filled through a mutable local and published as `const`, so a caller that shares
  // the config with other threads cannot write it.
  *items_out = items;
  *item_count_out = item_count;
  return 0;
}

int toml_copy_size(toml_datum_t table,
                   struct Str key_prefix,
                   const struct TomlSizeKey* spec,
                   struct Log* log) {
  const toml_datum_t value = toml_get(table, spec->key);
  if (value.type == TOML_UNKNOWN) {
    if (spec->is_required) {
      return log_error(log, "missing required config key '%.*s%s'", STR_FMT(key_prefix), spec->key);
    }
    return 0;
  }
  if (value.type != TOML_INT64) {
    return log_error(log, "config key '%.*s%s' must be an integer", STR_FMT(key_prefix), spec->key);
  }
  if (value.u.int64 < spec->value_min) {
    return log_error(log, "config key '%.*s%s' must be at least %td: %" PRId64, STR_FMT(key_prefix),
                     spec->key, spec->value_min, value.u.int64);
  }
  // The ceiling also guards the conversion below. `value_max` is a `ptrdiff_t`, so a value that
  // these checks accept fits `ptrdiff_t` on every target.
  if (value.u.int64 > spec->value_max && spec->is_len) {
    return log_error(log, "config key '%.*s%s' exceeds max %s (%td bytes) at %" PRId64 " bytes",
                     STR_FMT(key_prefix), spec->key, spec->limit_name, spec->value_max,
                     value.u.int64);
  }
  if (value.u.int64 > spec->value_max) {
    return log_error(log, "config key '%.*s%s' exceeds max %s (%td) at %" PRId64,
                     STR_FMT(key_prefix), spec->key, spec->limit_name, spec->value_max,
                     value.u.int64);
  }
  *spec->field = (ptrdiff_t)value.u.int64;
  return 0;
}

void toml_print_key_string(struct Buf* buffer, const char* key, struct Str value) {
  buf_printf(buffer, "%s = ", key);
  toml_print_string(buffer, value);
  buf_append_char(buffer, '\n');
}

void toml_print_key_string_optional(struct Buf* buffer, const char* key, struct Str value) {
  if (value.len > 0) {
    toml_print_key_string(buffer, key, value);
  }
}

void toml_print_key_string_array(struct Buf* buffer,
                                 const char* key,
                                 const struct Str* items,
                                 ptrdiff_t item_count) {
  buf_printf(buffer, "%s = [", key);
  for (ptrdiff_t i = 0; i < item_count; i++) {
    if (i > 0) {
      buf_append(buffer, S(", "));
    }
    toml_print_string(buffer, items[i]);
  }
  buf_append(buffer, S("]\n"));
}

void toml_print_string(struct Buf* buffer, struct Str text) {
  buf_append_char(buffer, '"');
  // Walk the bytes as `unsigned char`. Where `char` is signed, every byte of a multi-byte UTF-8
  // sequence is negative, so the `< 0x20` test below would treat it as a control byte, and passing
  // a negative `int` to the `%04X` conversion is undefined.
  for (ptrdiff_t i = 0; i < text.len; i++) {
    const unsigned char c = (unsigned char)text.data[i];
    switch (c) {
      case '"':
        buf_append(buffer, S("\\\""));
        break;
      case '\\':
        buf_append(buffer, S("\\\\"));
        break;
      case '\b':
        buf_append(buffer, S("\\b"));
        break;
      case '\t':
        buf_append(buffer, S("\\t"));
        break;
      case '\n':
        buf_append(buffer, S("\\n"));
        break;
      case '\f':
        buf_append(buffer, S("\\f"));
        break;
      case '\r':
        buf_append(buffer, S("\\r"));
        break;
      default:
        if (c < 0x20 || c == 0x7F) {
          buf_printf(buffer, "\\u%04X", c);
        } else {
          buf_append_char(buffer, (char)c);
        }
        break;
    }
  }
  buf_append_char(buffer, '"');
}

static void toml_parse_document_install_allocator(void) {
  toml_option_t option = toml_default_option();
  option.mem_realloc = toml_parse_document_realloc;
  toml_set_option(option);
}

static void* toml_parse_document_realloc(void* ptr, size_t size) {
  void* result = realloc(ptr, size);
  if (result == NULL && size > 0) {
    os_oom();
  }
  return result;
}

static int64_t days_from_civil(int year, unsigned month, unsigned day) {
  // This is Howard Hinnant's civil calendar algorithm, kept in its published form. `era`, `yoe`,
  // `doy` and `doe` are his variable names and the expression shapes are his, so a reader can check
  // this line by line against the derivation. That compatibility constraint exempts these names
  // from the naming rule. Expanding the names would break the line-by-line correspondence with the
  // published algorithm.
  //
  // `(unsigned)-3` is a deliberate modular wrap rather than a mistake. Conversion to an unsigned
  // type and unsigned overflow are both defined, and `month + (unsigned)-3` lands on `month - 3`
  // for every `month` above 2. The signed spelling a reader reaches for instead would not be
  // defined.
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = (unsigned)(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? (unsigned)-3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static bool is_known_key(struct Str key, const struct Str* known_keys, ptrdiff_t known_key_count) {
  for (ptrdiff_t i = 0; i < known_key_count; i++) {
    if (str_eq(known_keys[i], key)) {
      return true;
    }
  }
  return false;
}
