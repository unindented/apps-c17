#include "domain/site_config.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <tomlc17.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/path.h"
#include "core/str.h"
#include "core/text.h"
#include "core/url.h"
#include "formats/toml.h"
#include "media/image.h"
#include "os/os.h"

/** String-valued gallery config key policy. */
struct SiteConfigStringKey {
  /** TOML key name. */
  const char* key;

  /** Address of the `SiteConfig` field this key fills. */
  struct Str* field;

  /** Whether absence is a load error rather than leaving the initialized default. */
  bool is_required;
};

const struct Str SITE_CONFIG_PATH_DEFAULT = STR_INIT("fram.toml");

#define MEDIA_DIR_DEFAULT S("media")
#define OUTPUT_DIR_DEFAULT S("public")
#define TEMPLATES_DIR_DEFAULT S("templates")
#define STATIC_DIR_DEFAULT S("static")
#define ALBUM_TEMPLATE_DEFAULT S("album.html")

// The root album already renders to `index.html`. A default aggregate of that name would make the
// default configuration fail its manifest phase, so the default array is intentionally empty.
static const struct Str AGGREGATE_TEMPLATES_DEFAULT[] = {0};

/**
 * Every key the top-level table may hold. The load rejects a key outside this list, so a misspelled
 * key cannot silently leave its default in place. A key `site_config_load_fields` reads must be
 * listed here.
 */
static const struct Str config_keys[] = {
    // clang-format off: one key per line, so adding or removing a key is a one-line change.
    STR_INIT("title"),
    STR_INIT("description"),
    STR_INIT("author"),
    STR_INIT("base_url"),
    STR_INIT("media_dir"),
    STR_INIT("output_dir"),
    STR_INIT("templates_dir"),
    STR_INIT("static_dir"),
    STR_INIT("album_template"),
    STR_INIT("aggregate_templates"),
    STR_INIT("derivatives"),
    STR_INIT("sort"),
    STR_INIT("video_frame_seconds"),
    // clang-format on
};

/** The names a derivative can have, from the smallest size to the largest. */
static const struct Str DERIVATIVE_NAMES[] = {STR_INIT("xs"), STR_INIT("s"),  STR_INIT("m"),
                                              STR_INIT("l"),  STR_INIT("xl"), STR_INIT("xxl")};

_Static_assert(sizeof(DERIVATIVE_NAMES) / sizeof(DERIVATIVE_NAMES[0]) ==
                   (size_t)GALLERY_DERIVATIVE_COUNT_MAX,
               "a gallery configures at most one derivative of each name");

/** Every key a `derivatives.<name>` table may hold, each read by `copy_derivatives_entry`. */
static const struct Str derivative_keys[] = {STR_INIT("width"), STR_INIT("height"),
                                             STR_INIT("quality"), STR_INIT("crop")};

/** Every key the `sort` table may hold, each read by `copy_sort`. */
static const struct Str sort_keys[] = {STR_INIT("albums"), STR_INIT("media")};

static const struct GalleryDerivative DERIVATIVES_DEFAULT[] = {
    {.name = STR_INIT("s"), .width_px = 120, .height_px = 120, .quality = 70, .is_crop = true},
    {.name = STR_INIT("m"), .width_px = 520, .height_px = 360, .quality = 80, .is_crop = true},
    {.name = STR_INIT("l"), .width_px = 792, .height_px = 594, .quality = 85, .is_crop = false},
};

enum {
  /** Video-frame timestamp used when `video_frame_seconds` is absent from the configuration. */
  VIDEO_FRAME_SECONDS_DEFAULT = 1,

  /** Largest accepted `video_frame_seconds`, one day in seconds. */
  VIDEO_FRAME_SECONDS_MAX = 86400,
};

/**
 * Largest config file read, in bytes.
 *
 * A config is a few dozen keys and derivative tables, well under a kilobyte, so 1 MiB is three
 * orders of magnitude of headroom while keeping what tomlc17 copies and builds from it to a few
 * MiB. The read checks it before loading the file, so an oversize config is never resident.
 */
enum { CONFIG_FILE_LEN_MAX = 1024 * 1024 };

// `toml_parse_document` takes only a document whose length fits an `int`.
_Static_assert(CONFIG_FILE_LEN_MAX <= INT_MAX,
               "an accepted config must fit the int length tomlc17 takes");

/**
 * @brief Reads and parses a TOML config file into a `toml_result_t`.
 *
 * On success `parsed_out` owns the parse result until the caller runs `toml_free`.
 *
 * @param config_path Path to the config file.
 * @param os          Host the file is read from. Must not be `NULL`.
 * @param scratch     Arena for the file bytes, which tomlc17 copies.
 * @param parsed_out  Receives the parsed TOML result on success. Must not be `NULL`.
 * @param log         Log that receives every error. Must not be `NULL`.
 * @return `0` on success, or `-1` on a read, size, embedded `NUL`, or TOML parse error.
 */
static int site_config_load_toml(struct Str config_path,
                                 struct Os* os,
                                 struct Arena scratch,
                                 toml_result_t* parsed_out,
                                 struct Log* log) __attribute__((nonnull(2, 4, 5)));

/**
 * @brief Copies recognized configuration keys into gallery config storage.
 *
 * Rejects a key outside `config_keys`, then applies required and optional string keys, the
 * directory keys, which resolve against `config_dir` as their defaults do, the template-name array,
 * the derivative tables, and `video_frame_seconds`. It copies every value into `perm` rather than
 * borrowing from `table`. `site_config_load` calls `toml_free` as soon as this returns, which
 * releases the pool every `toml_datum_t` string points into, while the config outlives the parse.
 * The `sort` table is applied just before `video_frame_seconds`.
 *
 * @param site_config Config that receives the values. Must not be `NULL`.
 * @param table       Parsed TOML top-level table.
 * @param config_dir  Directory the relative paths resolve against, or empty for the working one.
 * @param perm        Arena that owns the copied values. Must not be `NULL`.
 * @param log         Log that receives every error. Must not be `NULL`.
 */
static void site_config_load_fields(struct SiteConfig* site_config,
                                    toml_datum_t table,
                                    struct Str config_dir,
                                    struct Arena* perm,
                                    struct Log* log) __attribute__((nonnull(1, 4, 5)));

/**
 * @brief Copies and validates the optional named-derivative table.
 *
 * When the key is absent, this leaves `derivatives` untouched and keeps the defaults from
 * `site_config_init`. It checks every derivative even after one fails, and publishes them only when
 * every one is valid, so a rejected table also keeps the defaults.
 *
 * @param site_config Config that receives the derivatives. Must not be `NULL`.
 * @param table       Parsed TOML top-level table.
 * @param perm        Arena that owns the copied derivatives. Must not be `NULL`.
 * @param log         Log that receives every error. Must not be `NULL`.
 * @return `0` on success or when the key is absent, or `-1` on a wrong type or an invalid name or
 *         value.
 */
static int copy_derivatives(struct SiteConfig* site_config,
                            toml_datum_t table,
                            struct Arena* perm,
                            struct Log* log) __attribute__((nonnull(1, 3, 4)));

/**
 * @brief Copies and validates one `derivatives.<name>` entry.
 *
 * It checks every key of the entry even after one fails, so one run reports each mistake in it.
 *
 * @param name           Entry name, the key in the `derivatives` table.
 * @param entry_table    Entry value, which must be a table.
 * @param perm           Arena that owns the copied name. Must not be `NULL`.
 * @param derivative_out Receives the name, dimensions, quality and crop flag, which the caller
 *                       stores only when every derivative was accepted. Must not be `NULL`.
 * @param log            Log that receives every error. Must not be `NULL`.
 * @return `0` on success, or `-1` on an invalid name, a wrong type, an unknown key, or an invalid
 *         value.
 */
static int copy_derivatives_entry(struct Str name,
                                  toml_datum_t entry_table,
                                  struct Arena* perm,
                                  struct GalleryDerivative* derivative_out,
                                  struct Log* log) __attribute__((nonnull(3, 4, 5)));

/**
 * @brief Reports whether `name` is one of `DERIVATIVE_NAMES`.
 *
 * @param name Name to check.
 * @return `true` when `name` is a derivative name, `false` otherwise.
 */
static bool is_derivative_name(struct Str name);

/**
 * @brief Copies and validates the optional `sort` table.
 *
 * When the key is absent, this leaves both orders untouched and keeps the defaults from
 * `site_config_init`. An empty table also keeps them. It checks both keys even after one fails, and
 * a rejected key keeps its default.
 *
 * @param site_config Config that receives the orders. Must not be `NULL`.
 * @param table       Parsed TOML top-level table.
 * @param log         Log that receives every error. Must not be `NULL`.
 * @return `0` on success or when the key is absent, or `-1` on a wrong type, an unknown key, or an
 *         invalid order.
 */
static int copy_sort(struct SiteConfig* site_config, toml_datum_t table, struct Log* log)
    __attribute__((nonnull(1, 3)));

/**
 * @brief Copies one optional `sort.<key>` order, which must be exactly `asc` or `desc`.
 *
 * @param sort_table Parsed `sort` table.
 * @param key        Key to read. Must not be `NULL`.
 * @param order_out  Receives the order when the key is present and valid, and keeps its value
 *                   otherwise. Must not be `NULL`.
 * @param log        Log that receives the error. Must not be `NULL`.
 * @return `0` on success or when the key is absent, or `-1` on a wrong type, a `NUL` byte, or a
 *         value other than `asc` or `desc`.
 */
static int copy_sort_order(toml_datum_t sort_table,
                           const char* key,
                           enum GallerySortOrder* order_out,
                           struct Log* log) __attribute__((nonnull(2, 3, 4)));

/**
 * @brief Validates `base_url` as an absolute HTTP(S) URL with a host, and with no byte that a link
 *        below it could not hold as written, and trims any trailing `/`. An empty `base_url` means
 *        the key is absent, and passes unchanged.
 *
 * @param site_config Config whose `base_url` is checked, and emptied when rejected. Must not be
 *                    `NULL`.
 * @param log         Log that receives the error. Must not be `NULL`.
 */
static void normalize_base_url(struct SiteConfig* site_config, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Rejects an `album_template` that is not a safe relative template name.
 *
 * A rejected name is reset to the default, so the render still has a template to look for.
 *
 * @param site_config Config whose `album_template` is validated. Must not be `NULL`.
 * @param log         Log that receives the error. Must not be `NULL`.
 * @return `0` when the name is safe, or `-1` when it is not.
 */
static int normalize_album_template(struct SiteConfig* site_config, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Rejects every derivative whose bounding box exceeds `IMAGE_PIXEL_COUNT_MAX`.
 *
 * @param derivatives      Array of `derivative_count` derivatives to validate. Must not be `NULL`.
 * @param derivative_count Number of derivatives in `derivatives`.
 * @param log              Log that receives every error. Must not be `NULL`.
 * @return `0` when every derivative fits, or `-1` when any does not.
 */
static int require_derivative_bounds(const struct GalleryDerivative* derivatives,
                                     ptrdiff_t derivative_count,
                                     struct Log* log) __attribute__((nonnull(1, 3)));

/**
 * @brief Writes the `sort = { ... }` line, naming both orders even at their defaults.
 *
 * The table is inline, so a key printed after it still belongs to the top-level table.
 *
 * @param buffer      Buffer the line is appended to. Must not be `NULL`.
 * @param site_config Config whose orders are written. Must not be `NULL`.
 */
static void site_config_print_sort(struct Buf* buffer, const struct SiteConfig* site_config)
    __attribute__((nonnull(1, 2)));

void site_config_init(struct SiteConfig* site_config) {
  // This is one literal naming only the non-zero defaults, so a key added to `SiteConfig` cannot be
  // left out here. The three required keys stay empty until the load fills them.
  *site_config = (struct SiteConfig){
      .media_dir = MEDIA_DIR_DEFAULT,
      .output_dir = OUTPUT_DIR_DEFAULT,
      .templates_dir = TEMPLATES_DIR_DEFAULT,
      .static_dir = STATIC_DIR_DEFAULT,
      .album_template = ALBUM_TEMPLATE_DEFAULT,
      .aggregate_templates = AGGREGATE_TEMPLATES_DEFAULT,
      .aggregate_template_count = 0,
      .derivatives = DERIVATIVES_DEFAULT,
      .derivative_count = sizeof(DERIVATIVES_DEFAULT) / sizeof(DERIVATIVES_DEFAULT[0]),
      .video_frame_seconds = VIDEO_FRAME_SECONDS_DEFAULT,
  };
}

int site_config_load(struct SiteConfig* site_config,
                     struct Str config_path,
                     struct Os* os,
                     struct Arena* perm,
                     struct Arena scratch,
                     struct Log* log) {
  // This is zero-initialized, although every path reaching `toml_free` writes it. The struct passes
  // to `toml_free` by value, and without this clang-analyzer cannot prove across the call boundary
  // that its fields are set.
  toml_result_t parsed = {0};
  if (site_config_load_toml(config_path, os, scratch, &parsed, log) != 0) {
    return -1;
  }

  // A relative path in the file names a place next to the file, so the build can run from any
  // directory.
  const struct Str config_dir = path_file_dir(config_path);
  // A key that fails validation is in `log` already, and the load still succeeded in the sense this
  // result reports: the file was read and parsed.
  site_config_load_fields(site_config, parsed.toptab, config_dir, perm, log);
  toml_free(parsed);
  return 0;
}

int site_config_print(const struct SiteConfig* site_config, struct Os* os, struct Arena scratch) {
  struct Buf text = {.arena = &scratch};
  toml_print_key_string(&text, "title", site_config->title);
  toml_print_key_string(&text, "description", site_config->description);
  toml_print_key_string(&text, "author", site_config->author);
  // An empty `base_url` is an absent key, so it is left out.
  toml_print_key_string_optional(&text, "base_url", site_config->base_url);
  toml_print_key_string(&text, "media_dir", site_config->media_dir);
  toml_print_key_string(&text, "output_dir", site_config->output_dir);
  toml_print_key_string(&text, "templates_dir", site_config->templates_dir);
  toml_print_key_string(&text, "static_dir", site_config->static_dir);
  toml_print_key_string(&text, "album_template", site_config->album_template);
  toml_print_key_string_array(&text, "aggregate_templates", site_config->aggregate_templates,
                              site_config->aggregate_template_count);
  buf_append(&text, S("derivatives = { "));
  for (ptrdiff_t i = 0; i < site_config->derivative_count; i++) {
    const struct GalleryDerivative* derivative = &site_config->derivatives[i];
    if (i > 0) {
      buf_append(&text, S(", "));
    }
    buf_printf(&text, "%.*s = { width = %td, height = %td, quality = %td",
               STR_FMT(derivative->name), derivative->width_px, derivative->height_px,
               derivative->quality);
    if (derivative->is_crop) {
      buf_append(&text, S(", crop = true"));
    }
    buf_append(&text, S(" }"));
  }
  buf_append(&text, S(" }\n"));
  site_config_print_sort(&text, site_config);
  buf_printf(&text, "video_frame_seconds = %td\n", site_config->video_frame_seconds);
  return os_print(os, 1, buf_str(&text));
}

struct Str site_config_derivative_recipe(const struct SiteConfig* site_config,
                                         struct Arena* arena) {
  struct Buf recipe = {.arena = arena};
  buf_printf(&recipe, "v%d", DERIVATIVE_RECIPE_VERSION);
  for (ptrdiff_t i = 0; i < site_config->derivative_count; i++) {
    const struct GalleryDerivative* derivative = &site_config->derivatives[i];
    buf_printf(&recipe, "-%.*s-%tdx%td%s-q%td", STR_FMT(derivative->name), derivative->width_px,
               derivative->height_px, derivative->is_crop ? "c" : "", derivative->quality);
  }
  buf_printf(&recipe, "-f%td", site_config->video_frame_seconds * 1000);
  return buf_str(&recipe);
}

static int site_config_load_toml(struct Str config_path,
                                 struct Os* os,
                                 struct Arena scratch,
                                 toml_result_t* parsed_out,
                                 struct Log* log) {
  struct Str config_data = {0};
  if (os_read(os, config_path, CONFIG_FILE_LEN_MAX, OS_READ_TEXT, &scratch, &config_data,
              S("failed to read config"), log) != 0) {
    return -1;
  }
  // tomlc17 copies the input, so the bytes can stay in `scratch`. The read bounds the length at
  // `CONFIG_FILE_LEN_MAX`, which fits an `int`.
  return toml_parse_document(config_data, config_path, parsed_out, S("failed to parse config"),
                             log);
}

static void site_config_load_fields(struct SiteConfig* site_config,
                                    toml_datum_t table,
                                    struct Str config_dir,
                                    struct Arena* perm,
                                    struct Log* log) {
  // Every key is checked even after one fails, so a config with several mistakes reports all of
  // them in one run. A rejected value is never stored: the key keeps its default, so a build that
  // goes on to check its media does so against a usable config rather than reporting the same
  // mistake again once per media file.
  (void)toml_require_known_keys(table, config_keys, sizeof(config_keys) / sizeof(config_keys[0]),
                                S("config"), S(""), (struct Str){0}, log);

  const struct SiteConfigStringKey strings[] = {
      {"title", &site_config->title, true},
      {"description", &site_config->description, true},
      {"author", &site_config->author, true},
      {"base_url", &site_config->base_url, false},
      {"album_template", &site_config->album_template, false},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(strings) / sizeof(strings[0])); i++) {
    (void)toml_copy_string(table, S(""), strings[i].key, strings[i].is_required, perm,
                           strings[i].field, log);
  }

  normalize_base_url(site_config, log);
  // A default directory resolves against the config's directory too, so that `-c sub/fram.toml`
  // without `output_dir` writes `sub/public`. A key that is absent or rejected keeps it.
  site_config->media_dir = path_resolve(config_dir, site_config->media_dir, perm);
  site_config->output_dir = path_resolve(config_dir, site_config->output_dir, perm);
  site_config->templates_dir = path_resolve(config_dir, site_config->templates_dir, perm);
  site_config->static_dir = path_resolve(config_dir, site_config->static_dir, perm);
  site_config->is_media_dir_rejected = toml_copy_dir(table, S(""), "media_dir", false, config_dir,
                                                     perm, &site_config->media_dir, log) != 0;
  site_config->is_output_dir_rejected = toml_copy_dir(table, S(""), "output_dir", false, config_dir,
                                                      perm, &site_config->output_dir, log) != 0;
  site_config->is_templates_dir_rejected =
      toml_copy_dir(table, S(""), "templates_dir", false, config_dir, perm,
                    &site_config->templates_dir, log) != 0;
  site_config->is_static_dir_rejected = toml_copy_dir(table, S(""), "static_dir", false, config_dir,
                                                      perm, &site_config->static_dir, log) != 0;
  (void)normalize_album_template(site_config, log);
  (void)toml_copy_strings(table, S(""), "aggregate_templates", true, perm,
                          &site_config->aggregate_templates, &site_config->aggregate_template_count,
                          log);
  (void)copy_derivatives(site_config, table, perm, log);
  (void)copy_sort(site_config, table, log);

  const struct TomlSizeKey video_frame_seconds = {
      .key = "video_frame_seconds",
      .field = &site_config->video_frame_seconds,
      .value_max = VIDEO_FRAME_SECONDS_MAX,
      .limit_name = "video frame seconds",
  };
  (void)toml_copy_size(table, S(""), &video_frame_seconds, log);
}

static int copy_derivatives(struct SiteConfig* site_config,
                            toml_datum_t table,
                            struct Arena* perm,
                            struct Log* log) {
  const toml_datum_t value = toml_get(table, "derivatives");
  if (value.type == TOML_UNKNOWN) {
    return 0;
  }
  if (value.type != TOML_TABLE) {
    return log_error(log, "config key 'derivatives' must be a table");
  }
  if (value.u.tab.size < 1) {
    return log_error(log, "config key 'derivatives' must not be empty");
  }
  // Each table key is unique and each name is one of `DERIVATIVE_NAMES`, so a table of more than
  // `GALLERY_DERIVATIVE_COUNT_MAX` entries has a name that fails below.
  const ptrdiff_t derivative_count = value.u.tab.size;
  struct GalleryDerivative* derivatives =
      arena_new(perm, derivative_count, struct GalleryDerivative);
  int rc = 0;
  for (ptrdiff_t i = 0; i < derivative_count; i++) {
    const struct Str name = {value.u.tab.key[i], value.u.tab.len[i]};
    if (copy_derivatives_entry(name, value.u.tab.value[i], perm, &derivatives[i], log) != 0) {
      rc = -1;
    }
  }
  // The bounds check reads both dimensions, so it runs only once every one of them was accepted.
  if (rc == 0 && require_derivative_bounds(derivatives, derivative_count, log) != 0) {
    rc = -1;
  }
  if (rc == 0) {
    site_config->derivatives = derivatives;
    site_config->derivative_count = derivative_count;
  }
  return rc;
}

static int copy_derivatives_entry(struct Str name,
                                  toml_datum_t entry_table,
                                  struct Arena* perm,
                                  struct GalleryDerivative* derivative_out,
                                  struct Log* log) {
  // A quoted key can hold a `NUL` decoded from an escape, where `%.*s` would stop and name a
  // different derivative, so such a name is rejected without being named.
  if (!text_is_nul_free(name)) {
    return log_error(log, "config key 'derivatives' name must not contain a NUL byte");
  }
  int rc = 0;
  // A wrong name still lets the entry's keys be checked, so one run reports every mistake in it.
  if (!is_derivative_name(name)) {
    rc = log_error(log,
                   "config key 'derivatives' name must be one of 'xs', 's', 'm', 'l', 'xl' or "
                   "'xxl': '%.*s'",
                   STR_FMT(name));
  }
  derivative_out->name = str_clone(name, perm);

  if (entry_table.type != TOML_TABLE) {
    return log_error(log, "config key 'derivatives.%.*s' must be a table",
                     STR_FMT(derivative_out->name));
  }
  // The prefix lives in a copy of `perm`, past the name, for this call only.
  struct Arena prefix_arena = *perm;
  struct Buf key_prefix_buffer = {.arena = &prefix_arena};
  buf_printf(&key_prefix_buffer, "derivatives.%.*s.", STR_FMT(derivative_out->name));
  const struct Str key_prefix = buf_str(&key_prefix_buffer);
  if (toml_require_known_keys(entry_table, derivative_keys,
                              sizeof(derivative_keys) / sizeof(derivative_keys[0]), S("config"),
                              key_prefix, (struct Str){0}, log) != 0) {
    rc = -1;
  }
  const struct TomlSizeKey integers[] = {
      {
          .key = "width",
          .field = &derivative_out->width_px,
          .value_min = 1,
          .value_max = IMAGE_ENCODED_DIMENSION_MAX,
          .limit_name = "WebP dimension",
          .is_required = true,
      },
      {
          .key = "height",
          .field = &derivative_out->height_px,
          .value_min = 1,
          .value_max = IMAGE_ENCODED_DIMENSION_MAX,
          .limit_name = "WebP dimension",
          .is_required = true,
      },
      {
          .key = "quality",
          .field = &derivative_out->quality,
          .value_min = 1,
          .value_max = 100,
          .limit_name = "WebP quality",
          .is_required = true,
      },
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(integers) / sizeof(integers[0])); i++) {
    if (toml_copy_size(entry_table, key_prefix, &integers[i], log) != 0) {
      rc = -1;
    }
  }

  derivative_out->is_crop = false;
  if (toml_copy_bool(entry_table, key_prefix, "crop", &derivative_out->is_crop, log) != 0) {
    rc = -1;
  }
  return rc;
}

static bool is_derivative_name(struct Str name) {
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(DERIVATIVE_NAMES) / sizeof(DERIVATIVE_NAMES[0]));
       i++) {
    if (str_eq(name, DERIVATIVE_NAMES[i])) {
      return true;
    }
  }
  return false;
}

static int copy_sort(struct SiteConfig* site_config, toml_datum_t table, struct Log* log) {
  const toml_datum_t value = toml_get(table, "sort");
  if (value.type == TOML_UNKNOWN) {
    return 0;
  }
  if (value.type != TOML_TABLE) {
    return log_error(log, "config key 'sort' must be a table");
  }
  int rc = 0;
  if (toml_require_known_keys(value, sort_keys, sizeof(sort_keys) / sizeof(sort_keys[0]),
                              S("config"), S("sort."), (struct Str){0}, log) != 0) {
    rc = -1;
  }
  if (copy_sort_order(value, "albums", &site_config->album_sort_order, log) != 0) {
    rc = -1;
  }
  if (copy_sort_order(value, "media", &site_config->media_sort_order, log) != 0) {
    rc = -1;
  }
  return rc;
}

static int copy_sort_order(toml_datum_t sort_table,
                           const char* key,
                           enum GallerySortOrder* order_out,
                           struct Log* log) {
  const toml_datum_t value = toml_get(sort_table, key);
  if (value.type == TOML_UNKNOWN) {
    return 0;
  }
  if (value.type != TOML_STRING) {
    return log_error(log, "config key 'sort.%s' must be a string", key);
  }
  // A `NUL` decoded from an escape would cut the quoted value below, so the message names none.
  if (!toml_datum_is_text(value)) {
    return log_error(log, "config key 'sort.%s' must not contain a NUL byte", key);
  }
  const struct Str order = toml_datum_str(value);
  if (str_eq(order, S("asc"))) {
    *order_out = GALLERY_SORT_ASC;
    return 0;
  }
  if (str_eq(order, S("desc"))) {
    *order_out = GALLERY_SORT_DESC;
    return 0;
  }
  return log_error(log, "config key 'sort.%s' must be 'asc' or 'desc': '%.*s'", key,
                   STR_FMT(order));
}

static void normalize_base_url(struct SiteConfig* site_config, struct Log* log) {
  if (site_config->base_url.len == 0) {
    return;
  }
  if (!url_is_http_with_host(site_config->base_url)) {
    (void)log_error(
        log,
        "config key 'base_url' must be an absolute 'http://' or 'https://' URL with a host: '%.*s'",
        STR_FMT(site_config->base_url));
    site_config->base_url = (struct Str){0};
    return;
  }
  // Every link appends a path to `base_url`, so a query or a fragment in it would swallow the path,
  // and a space or a control byte would reach the links raw.
  if (url_has_path_breaking_byte(site_config->base_url)) {
    (void)log_error(log,
                    "config key 'base_url' must not hold '?', '#', a space or a control "
                    "character: '%.*s'",
                    STR_FMT(site_config->base_url));
    site_config->base_url = (struct Str){0};
    return;
  }
  // Every link joins `base_url` with a path that starts with `/`, so the trailing `/` goes.
  site_config->base_url = path_trim_trailing_slashes(site_config->base_url);
}

static int normalize_album_template(struct SiteConfig* site_config, struct Log* log) {
  if (path_is_safe_relative(site_config->album_template)) {
    return 0;
  }
  const int rc =
      log_error(log, "config key 'album_template' must be a safe relative template name: '%.*s'",
                STR_FMT(site_config->album_template));
  site_config->album_template = ALBUM_TEMPLATE_DEFAULT;
  return rc;
}

static int require_derivative_bounds(const struct GalleryDerivative* derivatives,
                                     ptrdiff_t derivative_count,
                                     struct Log* log) {
  int rc = 0;
  for (ptrdiff_t i = 0; i < derivative_count; i++) {
    const struct GalleryDerivative* derivative = &derivatives[i];
    if (derivative->height_px > IMAGE_PIXEL_COUNT_MAX / derivative->width_px) {
      rc = log_error(log,
                     "config key 'derivatives.%.*s' dimensions exceed max pixel count (%d pixels) "
                     "at %td x %td",
                     STR_FMT(derivative->name), IMAGE_PIXEL_COUNT_MAX, derivative->width_px,
                     derivative->height_px);
    }
  }
  return rc;
}

static void site_config_print_sort(struct Buf* buffer, const struct SiteConfig* site_config) {
  buf_printf(buffer, "sort = { albums = \"%s\", media = \"%s\" }\n",
             site_config->album_sort_order == GALLERY_SORT_DESC ? "desc" : "asc",
             site_config->media_sort_order == GALLERY_SORT_DESC ? "desc" : "asc");
}
