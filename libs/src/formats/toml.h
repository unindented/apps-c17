#ifndef APPS_FORMATS_TOML_H
#define APPS_FORMATS_TOML_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <tomlc17.h>

#include "core/str.h"

struct Arena;
struct Buf;
struct Log;
struct Os;

/** Policy of one bounded size key that `toml_copy_size` reads, and the field it fills. */
struct TomlSizeKey {
  /** Terminated key name. */
  const char* key;

  /** Field the accepted value is written to. */
  ptrdiff_t* field;

  /** Smallest accepted value, named in the `must be at least` rejection. */
  ptrdiff_t value_min;

  /** Largest accepted value. A larger one is rejected as exceeding `limit_name`. */
  ptrdiff_t value_max;

  /** Prose name of `value_max` in the overflow diagnostic, such as `feed count`. */
  const char* limit_name;

  /** Whether absence is an error rather than leaving `field` as it is. */
  bool is_required;

  /**
   * Whether the value is a length in bytes, which the overflow diagnostic says after each number.
   */
  bool is_len;
};

// Every function here, and every caller that threads a datum onward, takes `toml_datum_t` by value
// rather than by pointer, against the general rule that a non-trivial struct is passed by pointer.
// This is the tomlc17 boundary. `toml_get` returns a datum by value (see
// `vendor/tomlc17/tomlc17.h`), so the alternative is a local copy per call whose only purpose is to
// have an address to take. Matching the library's shape at its own boundary keeps the translation
// in one layer, which is where the codebase confines every other external convention. Do not spread
// the by-value habit to structs this repository owns.

/**
 * @brief Parses a TOML document, treating an allocation failure inside tomlc17 as out of memory and
 *        logging a parse error.
 *
 * This is the one entry point to the tomlc17 parser, so every caller gets the project's allocation
 * policy. tomlc17 allocates through `malloc` and reports a failed allocation as a parse error. The
 * first call installs an allocator that calls `os_oom` instead, so a returned error is always a
 * problem with the document. The installation happens once, under `pthread_once`, which makes the
 * call safe from concurrent pool jobs.
 *
 * This is also where tomlc17's `ok` flag becomes the project's `0`/`-1`: a parse error is logged
 * against `file_path` and its result is released here, so a caller sees either a result to read or
 * nothing to free.
 *
 * tomlc17 reads exactly `document.len` bytes and needs no terminator. It takes the length as an
 * `int`, so the document must be no longer than `INT_MAX` bytes, which every caller's input limit
 * guarantees.
 *
 * @param document   Document bytes. `data` must not be `NULL`.
 * @param file_path  File the document came from, named in the parse error.
 * @param parsed_out Receives the parse result on success, which the caller must pass to `toml_free`
 *                   exactly once. Must not be `NULL`.
 * @param operation  What failed, the message's prefix, such as `failed to parse config`.
 * @param log        Log that receives the parse error. Must not be `NULL`.
 * @return `0` on success, or `-1` on a parse error.
 */
int toml_parse_document(struct Str document,
                        struct Str file_path,
                        toml_result_t* parsed_out,
                        struct Str operation,
                        struct Log* log) __attribute__((nonnull(3, 5)));

/**
 * Largest config file `toml_load_config` reads, in bytes.
 *
 * A config is a few dozen keys, well under a kilobyte, so 1 MiB is three orders of magnitude of
 * headroom while keeping what tomlc17 copies and builds from it to a few MiB. The read checks it
 * before loading the file, so an oversize config is never resident.
 */
enum { TOML_CONFIG_LEN_MAX = 1024 * 1024 };

/**
 * @brief Reads and parses the config file at `config_path`.
 *
 * A failure is `failed to read config: <cause> ('<path>')`, the read's message, which names the
 * limit for a file over `TOML_CONFIG_LEN_MAX`, or `failed to parse config: ...`, the parse's.
 *
 * @param config_path Config file to read.
 * @param os          Host the file is read on. Must not be `NULL`.
 * @param scratch     Arena for the file's bytes, which tomlc17 copies.
 * @param parsed_out  Receives the parse result on success, which the caller must pass to
 *                    `toml_free` exactly once. Must not be `NULL`.
 * @param log         Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the file cannot be read or does not parse.
 */
int toml_load_config(struct Str config_path,
                     struct Os* os,
                     struct Arena scratch,
                     toml_result_t* parsed_out,
                     struct Log* log) __attribute__((nonnull(2, 4, 5)));

/**
 * @brief Reports whether a datum is a string whose bytes hold no embedded `NUL`.
 *
 * TOML permits an escape that decodes to `U+0000`, and tomlc17 encodes it as a `NUL` byte inside an
 * otherwise ordinary string value. Every caller that turns a TOML string into a value of its own
 * checks this first, which keeps the input policy of `text_is_nul_free`.
 *
 * @param value Datum to classify.
 * @return `true` when `value` is a string with no embedded `NUL`, `false` otherwise.
 */
bool toml_datum_is_text(toml_datum_t value);

/**
 * @brief Returns a view of a string datum's bytes.
 *
 * The view borrows the bytes: they point into the `toml_result_t`'s own storage and die with
 * `toml_free`, so a caller that keeps the value clones it first.
 *
 * @param value Datum to view. Must be a `TOML_STRING` datum.
 * @return View of the string's bytes, without tomlc17's terminator.
 */
struct Str toml_datum_str(toml_datum_t value);

/**
 * @brief Converts a TOML date/datetime datum to Unix epoch seconds.
 *
 * It treats a date-only or timezone-less datetime as UTC. It truncates sub-second precision (the
 * result is whole seconds). It writes `epoch_out` only on success. It trusts the component ranges
 * because tomlc17 validates them while parsing.
 *
 * @param value     Datum to convert. Must be a TOML date, datetime, or datetime-with-timezone.
 * @param epoch_out Receives the resulting Unix timestamp in seconds. Must not be `NULL`.
 * @return `0` on success, or `-1` for any other datum type.
 */
int toml_datum_to_epoch(toml_datum_t value, int64_t* epoch_out) __attribute__((nonnull(2)));

/**
 * @brief Formats a TOML date/datetime datum into a normalized RFC 3339 timestamp string.
 *
 * It expands a date-only datum to midnight UTC. It preserves fractional seconds when present and
 * trims trailing zeros. It emits a datetime carrying an offset with that offset (`Z` for `+00:00`).
 * It treats a timezone-less datetime as UTC and emits it with `Z`, matching `toml_datum_to_epoch`.
 * It stores the result in arena-owned storage.
 *
 * @param value Datum to format. Must be a TOML date, datetime, or datetime-with-timezone, which
 *              `toml_datum_to_epoch` accepting it establishes.
 * @param arena Arena that owns the returned string. Must not be `NULL`.
 * @return Timestamp owned by the arena.
 */
struct Str toml_datum_format_rfc3339(toml_datum_t value, struct Arena* arena)
    __attribute__((nonnull(2)));

/**
 * @brief Rejects every key of a table that its level of the schema does not define.
 *
 * A loader passes every key it reads, before it reads any, so a misspelled key fails the load
 * rather than silently leaving its default in place. Keys match exactly, so a key that is a prefix
 * of a known key, or extends one, is as unknown as any other. Every unknown key is named. A quoted
 * key holding a `NUL` decoded from an escape is one of those unknown keys, but it is reported
 * without its name, which `%.*s` would cut at the `NUL`: as
 * `<key_kind> key must not contain a NUL byte` in the top-level table, and as
 * `<key_kind> key under '<table>' must not contain a NUL byte` in a nested one.
 *
 * @param table           Parsed TOML table. Must be a `TOML_TABLE` datum.
 * @param known_keys      Keys the table may hold. May be `NULL` only when `known_key_count` is 0.
 * @param known_key_count Number of keys in `known_keys`.
 * @param key_kind        Kind of key the diagnostic names, such as `config`.
 * @param key_prefix      Dotted path of `table` with a trailing `.`, such as `section.table.`, or
 *                        empty for the top-level table. It qualifies the key in the diagnostic.
 * @param file_path       File the table came from, named in each error, or empty to name none.
 * @param log             Log that receives `unknown <key_kind> key '<key_prefix><key>'` for each
 *                        unknown key, in table order. Must not be `NULL`.
 * @return `0` when every key is known, or `-1` when any is not.
 */
int toml_require_known_keys(toml_datum_t table,
                            const struct Str* known_keys,
                            ptrdiff_t known_key_count,
                            struct Str key_kind,
                            struct Str key_prefix,
                            struct Str file_path,
                            struct Log* log) __attribute__((nonnull(7)));

/**
 * @brief Copies one string key of a config table into the arena.
 *
 * Each diagnostic names the key as `<key_prefix><key>`: `missing required config key '<key>'`,
 * `config key '<key>' must be a string`, or `config key '<key>' must not contain a NUL byte`. A
 * rejected value is never stored.
 *
 * @param table       Parsed table that holds `key`. Must be a `TOML_TABLE` datum.
 * @param key_prefix  Dotted path of `table` with a trailing `.`, such as `limits.`, or empty for
 *                    the top-level table. It qualifies the key in each diagnostic.
 * @param key         Terminated key to read. Must not be `NULL`.
 * @param is_required Whether absence is an error. When `false`, absence leaves `*value_out` as it
 *                    is.
 * @param arena       Arena that owns the copied value. Must not be `NULL`.
 * @param value_out   Receives the copy when the key holds a string without a `NUL` byte. A present
 *                    key gives a copy whose `data` is never `NULL`, even when it is empty. Must not
 *                    be `NULL`.
 * @param log         Log that receives every error. Must not be `NULL`.
 * @return `0` on success or when an optional key is absent, or `-1` when a required key is missing
 *         or the value is not a string without a `NUL` byte.
 */
int toml_copy_string(toml_datum_t table,
                     struct Str key_prefix,
                     const char* key,
                     bool is_required,
                     struct Arena* arena,
                     struct Str* value_out,
                     struct Log* log) __attribute__((nonnull(3, 5, 6, 7)));

/**
 * @brief Copies one key of a config table that holds the site's base URL, which every absolute link
 *        of a page joins a path to.
 *
 * The value must be an absolute `http://` or `https://` URL with a host, and must not hold `?`,
 * `#`, a space or a control character, which would swallow or break the path a link appends. Its
 * trailing `/` characters go, because every link joins a path that starts with `/`. Each diagnostic
 * names the key as `toml_copy_string` does, as in
 * `config key 'base_url' must be an absolute 'http://' or 'https://' URL with a host: '<value>'`.
 * An optional key may also be empty, which means no base URL. A rejected value is never stored.
 *
 * @param table       Parsed table that holds `key`. Must be a `TOML_TABLE` datum.
 * @param key_prefix  Dotted path of `table` with a trailing `.`, or empty for the top-level table.
 * @param key         Terminated key to read. Must not be `NULL`.
 * @param is_required Whether absence, or an empty value, is an error. When `false`, absence leaves
 *                    `*value_out` as it is.
 * @param arena       Arena that owns the copied value. Must not be `NULL`.
 * @param value_out   Receives the URL, without its trailing `/`. Must not be `NULL`.
 * @param log         Log that receives every error. Must not be `NULL`.
 * @return `0` on success or when an optional key is absent or empty, or `-1` when the value is
 *         missing, not a string, or not a usable base URL.
 */
int toml_copy_base_url(toml_datum_t table,
                       struct Str key_prefix,
                       const char* key,
                       bool is_required,
                       struct Arena* arena,
                       struct Str* value_out,
                       struct Log* log) __attribute__((nonnull(3, 5, 6, 7)));

/**
 * @brief Copies one directory key of a config table, trims its trailing `/`, rejects an empty
 *        result, and resolves it against the config's directory.
 *
 * A trailing `/` would otherwise survive into path construction, where it defeats a prefix strip of
 * `<dir>/`, so the value is trimmed once here, and a printed config shows the trimmed form. A value
 * that trims to empty, `/` included, is rejected as `config key '<key>' must not be empty`, with
 * the key named as `toml_copy_string` names it. The location is not otherwise constrained, so an
 * absolute or parent-relative directory stays valid.
 *
 * @param table       Parsed table that holds `key`. Must be a `TOML_TABLE` datum.
 * @param key_prefix  Dotted path of `table` with a trailing `.`, or empty for the top-level table.
 * @param key         Terminated key to read. Must not be `NULL`.
 * @param is_required Whether absence is an error.
 * @param config_dir  Directory a relative value resolves against, or empty for the working one.
 * @param arena       Arena that owns the copied value. Must not be `NULL`.
 * @param value_out   Receives the resolved directory when the key is present and valid, and keeps
 *                    its value otherwise. Must not be `NULL`.
 * @param log         Log that receives every error. Must not be `NULL`.
 * @return `0` when the key is accepted or an optional key is absent, or `-1` when a required key is
 *         missing, or the value is not a string, holds a `NUL` byte, or is empty once trimmed.
 */
int toml_copy_dir(toml_datum_t table,
                  struct Str key_prefix,
                  const char* key,
                  bool is_required,
                  struct Str config_dir,
                  struct Arena* arena,
                  struct Str* value_out,
                  struct Log* log) __attribute__((nonnull(3, 6, 7, 8)));

/**
 * @brief Copies one optional boolean key of a config table.
 *
 * A value of another type is `config key '<key>' must be a boolean`, with the key named as
 * `toml_copy_string` names it.
 *
 * @param table      Parsed table that holds `key`. Must be a `TOML_TABLE` datum.
 * @param key_prefix Dotted path of `table` with a trailing `.`, or empty for the top-level table.
 * @param key        Terminated key to read. Must not be `NULL`.
 * @param value_out  Receives the value when the key is present and a boolean, and keeps its value
 *                   otherwise. Must not be `NULL`.
 * @param log        Log that receives the error. Must not be `NULL`.
 * @return `0` when the key is accepted or absent, or `-1` when it is not a boolean.
 */
int toml_copy_bool(toml_datum_t table,
                   struct Str key_prefix,
                   const char* key,
                   bool* value_out,
                   struct Log* log) __attribute__((nonnull(3, 4, 5)));

/**
 * @brief Copies one optional key of a config table that names one template, and rejects a name that
 *        is not a safe relative template name.
 *
 * This is the single-name check that `toml_copy_strings` applies to each element of a list of
 * template names. A wrong type or a `NUL` byte is reported as `toml_copy_string` reports it. An
 * empty or unsafe name is `config key '<key>' must be a safe relative template name: '<value>'`,
 * with the key named as `toml_copy_string` names it. A rejected value is never stored.
 *
 * @param table      Parsed table that holds `key`. Must be a `TOML_TABLE` datum.
 * @param key_prefix Dotted path of `table` with a trailing `.`, such as `templates.`, or empty for
 *                   the top-level table.
 * @param key        Terminated key to read. Must not be `NULL`.
 * @param arena      Arena that owns the copied name. Must not be `NULL`.
 * @param value_out  Receives the name when the key is present and valid, and keeps its value
 *                   otherwise, so an absent key leaves the default in place. Must not be `NULL`.
 * @param log        Log that receives the error. Must not be `NULL`.
 * @return `0` when the key is accepted or absent, or `-1` on a wrong type, a `NUL` byte, or a name
 *         that is empty or unsafe.
 */
int toml_copy_template_name(toml_datum_t table,
                            struct Str key_prefix,
                            const char* key,
                            struct Arena* arena,
                            struct Str* value_out,
                            struct Log* log) __attribute__((nonnull(3, 4, 5, 6)));

/**
 * @brief Copies one optional array-of-strings key of a config table, and validates each string as a
 *        safe relative template name when it names templates.
 *
 * Every element is checked even after one fails, so each unsafe name is reported. A wrong type or a
 * `NUL` byte is reported once, since its message names no element. Each diagnostic names the key as
 * `toml_copy_string` names it. A copied list is never `NULL`, even when it is empty.
 *
 * @param table             Parsed table that holds `key`. Must be a `TOML_TABLE` datum.
 * @param key_prefix        Dotted path of `table` with a trailing `.`, or empty for the top-level
 *                          table.
 * @param key               Terminated key to read. Must not be `NULL`.
 * @param is_template_names Whether each string must be a safe relative template name.
 * @param arena             Arena that owns the copied strings. Must not be `NULL`.
 * @param items_out         Receives the copied strings when the key is present and valid, and keeps
 *                          its value otherwise. Must not be `NULL`.
 * @param item_count_out    Receives the number of strings, as `items_out` does. Must not be `NULL`.
 * @param log               Log that receives every error. Must not be `NULL`.
 * @return `0` when the key is accepted or absent, or `-1` on a wrong type, a `NUL` byte, or an
 *         unsafe template name.
 */
int toml_copy_strings(toml_datum_t table,
                      struct Str key_prefix,
                      const char* key,
                      bool is_template_names,
                      struct Arena* arena,
                      const struct Str** items_out,
                      ptrdiff_t* item_count_out,
                      struct Log* log) __attribute__((nonnull(3, 5, 6, 7, 8)));

/**
 * @brief Validates one bounded integer key of a config table and copies it into `spec->field`.
 *
 * Each diagnostic names the key as `toml_copy_string` names it. A missing required key is
 * `missing required config key '<key>'`, a value of another type
 * `config key '<key>' must be an integer`, a value below the bound
 * `config key '<key>' must be at least <value_min>: <value>`, and a value above it
 * `config key '<key>' exceeds max <limit_name> (<value_max>) at <value>`, or
 * `... (<value_max> bytes) at <value> bytes` for a length.
 *
 * @param table      Parsed table that holds `spec->key`. Must be a `TOML_TABLE` datum.
 * @param key_prefix Dotted path of `table` with a trailing `.`, such as `limits.`, or empty for the
 *                   top-level table.
 * @param spec       Key policy and target field. Must not be `NULL`.
 * @param log        Log that receives every error. Must not be `NULL`.
 * @return `0` on success or when an optional key is absent, or `-1` on a missing required key, a
 *         wrong type, or a value out of bounds, which leaves the field as it was.
 */
int toml_copy_size(toml_datum_t table,
                   struct Str key_prefix,
                   const struct TomlSizeKey* spec,
                   struct Log* log) __attribute__((nonnull(3, 4)));

/**
 * @brief Writes one `key = "value"` line, quoting the value with `toml_print_string`.
 *
 * @param buffer Buffer the line is appended to. Must not be `NULL`.
 * @param key    Terminated key name, written as it is. Must not be `NULL`.
 * @param value  Value to quote.
 */
void toml_print_key_string(struct Buf* buffer, const char* key, struct Str value)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Writes one `key = "value"` line when `value` is not empty, and nothing otherwise.
 *
 * A config prints an empty optional string this way, because its loader reads an absent key as the
 * empty value.
 *
 * @param buffer Buffer the line is appended to. Must not be `NULL`.
 * @param key    Terminated key name, written as it is. Must not be `NULL`.
 * @param value  Value to quote, or empty to write nothing.
 */
void toml_print_key_string_optional(struct Buf* buffer, const char* key, struct Str value)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Writes one `key = [...]` line of strings, each quoted with `toml_print_string`.
 *
 * @param buffer     Buffer the line is appended to. Must not be `NULL`.
 * @param key        Terminated key name, written as it is. Must not be `NULL`.
 * @param items      Array of `item_count` strings. May be `NULL` only when `item_count` is 0.
 * @param item_count Number of items in `items`.
 */
void toml_print_key_string_array(struct Buf* buffer,
                                 const char* key,
                                 const struct Str* items,
                                 ptrdiff_t item_count) __attribute__((nonnull(1, 2)));

/**
 * @brief Writes `text` as a quoted TOML basic string.
 *
 * TOML basic strings forbid raw control bytes, so this escapes them alongside `"` and `\`. It
 * writes the short escapes `\b`, `\t`, `\n`, `\f` and `\r`, and `\uXXXX` for every other control
 * byte below `0x20` and for `0x7F`. It copies every other byte as it is, so UTF-8 text stays as it
 * was.
 *
 * @param buffer Buffer the quoted string is appended to. Must not be `NULL`.
 * @param text   Text to quote.
 */
void toml_print_string(struct Buf* buffer, struct Str text) __attribute__((nonnull(1)));

#endif
