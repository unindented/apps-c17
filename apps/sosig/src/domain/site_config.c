#include "domain/site_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <tomlc17.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/path.h"
#include "core/str.h"
#include "domain/content_entry.h"
#include "domain/permalink.h"
#include "formats/toml.h"
#include "os/os.h"
#include "output/output_path.h"

const struct Str SITE_CONFIG_PATH_DEFAULT = STR_INIT("sosig.toml");

#define PERMALINK_DEFAULT S("/{section}/{slug}.html")
#define CONTENT_DIR_DEFAULT S("content")
#define OUTPUT_DIR_DEFAULT S("public")
#define TEMPLATES_DIR_DEFAULT S("templates")
#define STATIC_DIR_DEFAULT S("static")
#define NOT_FOUND_PATH_DEFAULT S("404.html")
#define CONTENT_TEMPLATE_DEFAULT S("content.html")
#define NOT_FOUND_TEMPLATE_DEFAULT S("404.html")
static const struct Str AGGREGATE_TEMPLATES_DEFAULT[] = {STR_INIT("index.html")};
static const struct Str FEED_TEMPLATES_DEFAULT[] = {STR_INIT("atom.xml")};

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
    STR_INIT("permalink"),
    STR_INIT("content_dir"),
    STR_INIT("output_dir"),
    STR_INIT("templates_dir"),
    STR_INIT("static_dir"),
    STR_INIT("not_found_path"),
    STR_INIT("feed_count"),
    STR_INIT("frontmatter_ignored_keys"),
    STR_INIT("templates"),
    // clang-format on
};

/** Every key the `[templates]` table may hold, each read by `site_config_load_fields_templates`. */
static const struct Str templates_keys[] = {
    // clang-format off: one key per line, so adding or removing a key is a one-line change.
    STR_INIT("content"),
    STR_INIT("aggregates"),
    STR_INIT("feeds"),
    STR_INIT("not_found"),
    // clang-format on
};

/** Entries in one feed when `feed_count` is unset. */
enum { FEED_COUNT_DEFAULT = 25 };

/**
 * Sample values a permalink pattern is expanded with during validation. They stand in for real
 * ones, which are always slugified and so always safe on their own.
 *
 * The grid checks both a populated and an empty section. An empty one changes how redundant `/`
 * collapse, so a pattern like `{section}/..` must be rejected either way. The two slugs make a
 * pattern's per-entry variation observable, so both checks below draw from the same pair.
 */
static const struct Str SECTION_SAMPLES[] = {STR_INIT("section"), STR_INIT("")};
static const struct Str SLUG_SAMPLES[] = {STR_INIT("slug-a"), STR_INIT("slug-b")};

enum { SECTION_SAMPLE_COUNT = sizeof(SECTION_SAMPLES) / sizeof(SECTION_SAMPLES[0]) };
enum { SLUG_SAMPLE_COUNT = sizeof(SLUG_SAMPLES) / sizeof(SLUG_SAMPLES[0]) };

/**
 * Permalink-pattern check verdict, in the precedence order `check_permalink` applies. It catches an
 * unsafe expansion first, then two slugs colliding.
 */
enum PermalinkVerdict {
  /** Every expansion is a safe relative path and slug-dependent. */
  PERMALINK_VALID,

  /** Some expansion escapes the output directory or is otherwise unsafe. */
  PERMALINK_UNSAFE,

  /** Two different slugs expand to the same path, so every entry claims one output. */
  PERMALINK_NOT_DISTINCT,
};

/**
 * @brief Copies recognized configuration keys into site config storage.
 *
 * Rejects a key outside `config_keys`, then applies required and optional string keys, the
 * directories, `not_found_path`, `feed_count`, `frontmatter_ignored_keys` and the `[templates]`
 * table, and stops at the first key that fails. It copies every value into `perm` rather than
 * borrowing from `table`. `site_config_load` calls `toml_free` as soon as this returns, which
 * releases the pool every `toml_datum_t` string points into, while the config outlives the parse.
 *
 * @param site_config Config that receives the values. Must not be `NULL`.
 * @param table       Parsed TOML top-level table.
 * @param config_dir  Directory a relative directory resolves against, or empty for the working one.
 * @param perm        Arena that owns the copied values. Must not be `NULL`.
 * @param scratch     Arena for validation temporaries.
 * @param log         Log that receives the first error. Must not be `NULL`.
 * @return `0` when every key is valid, or `-1` at the first key that is not.
 */
static int site_config_load_fields(struct SiteConfig* site_config,
                                   toml_datum_t table,
                                   struct Str config_dir,
                                   struct Arena* perm,
                                   struct Arena scratch,
                                   struct Log* log) __attribute__((nonnull(1, 4, 6)));

/**
 * @brief Copies the `[templates]` table into `templates`, stopping at the first error.
 *
 * @param templates Templates that receive the names, holding their defaults. Must not be `NULL`.
 * @param table     Parsed TOML top-level table.
 * @param perm      Arena that owns the copied names. Must not be `NULL`.
 * @param log       Log that receives the error. Must not be `NULL`.
 * @return `0` on success or when the table is absent, or `-1` after logging the first error.
 */
static int site_config_load_fields_templates(struct SiteTemplates* templates,
                                             toml_datum_t table,
                                             struct Arena* perm,
                                             struct Log* log) __attribute__((nonnull(1, 3, 4)));

/**
 * @brief Validates the `permalink` key, translating each verdict into its diagnostic.
 *
 * Splitting the translation from `check_permalink` keeps the grid check free of message wording and
 * leaves this function as the only place that decides what a verdict tells the user.
 *
 * @param pattern Permalink pattern to validate.
 * @param scratch Arena for the sample expansions.
 * @param log     Log that receives every error. Must not be `NULL`.
 * @return `0` when the pattern is valid, or `-1` with a diagnostic naming the verdict.
 */
static int require_valid_permalink(struct Str pattern, struct Arena scratch, struct Log* log)
    __attribute__((nonnull(3)));

/**
 * @brief Validates a permalink pattern by expanding it over the sample grid.
 *
 * Validating the pattern here keeps a bad `permalink` a single config error. If left to the render
 * phase, the render phase catches the same mistake per entry and reports it once for every content
 * file. The pattern is expanded once and every question reads that one grid. Reasoning about the
 * pattern's bytes directly would be a second implementation of `path_is_safe_relative`'s policy and
 * of `permalink_expand`'s meaning, each free to drift. Output path lengths are checked per entry,
 * in the render phase.
 *
 * @param pattern Permalink pattern to check.
 * @param scratch Arena for the sample expansions.
 * @return The first verdict that failed, or `PERMALINK_VALID` when every check passed.
 */
static enum PermalinkVerdict check_permalink(struct Str pattern, struct Arena scratch);

/**
 * @brief Judges the expanded sample grid, one question at a time over the whole grid.
 *
 * @param urls Expansion of the pattern for each section and slug sample, each with its leading `/`.
 *             Must not be `NULL`.
 * @return The first verdict that failed in `enum PermalinkVerdict` precedence, or `PERMALINK_VALID`
 *         when every check passed.
 */
static enum PermalinkVerdict check_permalink_expansions(
    struct Str urls[SECTION_SAMPLE_COUNT][SLUG_SAMPLE_COUNT]) __attribute__((nonnull(1)));

void site_config_init(struct SiteConfig* site_config) {
  // This is one literal naming only the non-zero defaults, so a key added to `SiteConfig` cannot be
  // left out here. The four required keys stay empty until the load fills them.
  *site_config = (struct SiteConfig){
      .permalink = PERMALINK_DEFAULT,
      .content_dir = CONTENT_DIR_DEFAULT,
      .output_dir = OUTPUT_DIR_DEFAULT,
      .templates_dir = TEMPLATES_DIR_DEFAULT,
      .static_dir = STATIC_DIR_DEFAULT,
      .not_found_path = NOT_FOUND_PATH_DEFAULT,
      .feed_count = FEED_COUNT_DEFAULT,
      .templates =
          {
              .content = CONTENT_TEMPLATE_DEFAULT,
              .aggregates = AGGREGATE_TEMPLATES_DEFAULT,
              .aggregate_template_count =
                  sizeof(AGGREGATE_TEMPLATES_DEFAULT) / sizeof(AGGREGATE_TEMPLATES_DEFAULT[0]),
              .feeds = FEED_TEMPLATES_DEFAULT,
              .feed_template_count =
                  sizeof(FEED_TEMPLATES_DEFAULT) / sizeof(FEED_TEMPLATES_DEFAULT[0]),
              .not_found = NOT_FOUND_TEMPLATE_DEFAULT,
          },
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
  if (toml_load_config(config_path, os, scratch, &parsed, log) != 0) {
    return -1;
  }

  // A relative path in the file names a place next to the file, so the build can run from any
  // directory.
  const struct Str config_dir = path_file_dir(config_path);
  const int rc =
      site_config_load_fields(site_config, parsed.toptab, config_dir, perm, scratch, log);
  toml_free(parsed);
  return rc;
}

int site_config_print(const struct SiteConfig* site_config, struct Os* os, struct Arena scratch) {
  struct Buf text = {.arena = &scratch};
  toml_print_key_string(&text, "title", site_config->title);
  toml_print_key_string(&text, "description", site_config->description);
  toml_print_key_string(&text, "author", site_config->author);
  toml_print_key_string(&text, "base_url", site_config->base_url);
  toml_print_key_string(&text, "permalink", site_config->permalink);
  toml_print_key_string(&text, "content_dir", site_config->content_dir);
  toml_print_key_string(&text, "output_dir", site_config->output_dir);
  toml_print_key_string(&text, "templates_dir", site_config->templates_dir);
  toml_print_key_string(&text, "static_dir", site_config->static_dir);
  toml_print_key_string(&text, "not_found_path", site_config->not_found_path);
  buf_printf(&text, "feed_count = %td\n", site_config->feed_count);
  toml_print_key_string_array(&text, "frontmatter_ignored_keys",
                              site_config->frontmatter_ignored_keys,
                              site_config->frontmatter_ignored_key_count);
  // The table comes last, because every key after its header belongs to it.
  const struct SiteTemplates* templates = &site_config->templates;
  buf_append(&text, S("\n[templates]\n"));
  toml_print_key_string(&text, "content", templates->content);
  toml_print_key_string_array(&text, "aggregates", templates->aggregates,
                              templates->aggregate_template_count);
  toml_print_key_string_array(&text, "feeds", templates->feeds, templates->feed_template_count);
  toml_print_key_string(&text, "not_found", templates->not_found);
  return os_print(os, 1, buf_str(&text));
}

static int site_config_load_fields(struct SiteConfig* site_config,
                                   toml_datum_t table,
                                   struct Str config_dir,
                                   struct Arena* perm,
                                   struct Arena scratch,
                                   struct Log* log) {
  if (toml_require_known_keys(table, config_keys, sizeof(config_keys) / sizeof(config_keys[0]),
                              S("config"), S(""), (struct Str){0}, log) != 0 ||
      toml_copy_string(table, S(""), "title", true, perm, &site_config->title, log) != 0 ||
      toml_copy_string(table, S(""), "description", true, perm, &site_config->description, log) !=
          0 ||
      toml_copy_string(table, S(""), "author", true, perm, &site_config->author, log) != 0 ||
      toml_copy_base_url(table, S(""), "base_url", true, perm, &site_config->base_url, log) != 0 ||
      toml_copy_string(table, S(""), "permalink", false, perm, &site_config->permalink, log) != 0 ||
      require_valid_permalink(site_config->permalink, scratch, log) != 0) {
    return -1;
  }
  // A default directory resolves against the config's directory too, so that `-c sub/sosig.toml`
  // without `output_dir` writes `sub/public`. A key that is absent keeps it.
  site_config->content_dir = path_resolve(config_dir, site_config->content_dir, perm);
  site_config->output_dir = path_resolve(config_dir, site_config->output_dir, perm);
  site_config->templates_dir = path_resolve(config_dir, site_config->templates_dir, perm);
  site_config->static_dir = path_resolve(config_dir, site_config->static_dir, perm);
  if (toml_copy_dir(table, S(""), "content_dir", false, config_dir, perm, &site_config->content_dir,
                    log) != 0 ||
      toml_copy_dir(table, S(""), "output_dir", false, config_dir, perm, &site_config->output_dir,
                    log) != 0 ||
      toml_copy_dir(table, S(""), "templates_dir", false, config_dir, perm,
                    &site_config->templates_dir, log) != 0 ||
      toml_copy_dir(table, S(""), "static_dir", false, config_dir, perm, &site_config->static_dir,
                    log) != 0 ||
      toml_copy_string(table, S(""), "not_found_path", false, perm, &site_config->not_found_path,
                       log) != 0 ||
      output_path_check_not_found(site_config->not_found_path, log) != 0) {
    return -1;
  }
  const struct TomlSizeKey feed_count = {
      .key = "feed_count",
      .field = &site_config->feed_count,
      // `PTRDIFF_MAX` binds only where `ptrdiff_t` is narrower than `int64_t`, as on a 32-bit
      // target.
      .value_max = PTRDIFF_MAX,
      .limit_name = "feed count",
  };
  if (toml_copy_size(table, S(""), &feed_count, log) != 0 ||
      toml_copy_strings(table, S(""), "frontmatter_ignored_keys", false, perm,
                        &site_config->frontmatter_ignored_keys,
                        &site_config->frontmatter_ignored_key_count, log) != 0) {
    return -1;
  }
  return site_config_load_fields_templates(&site_config->templates, table, perm, log);
}

static int site_config_load_fields_templates(struct SiteTemplates* templates,
                                             toml_datum_t table,
                                             struct Arena* perm,
                                             struct Log* log) {
  const toml_datum_t value = toml_get(table, "templates");
  if (value.type == TOML_UNKNOWN) {
    return 0;
  }
  if (value.type != TOML_TABLE) {
    return log_error(log, "config key 'templates' must be a table");
  }
  const struct Str key_prefix = S("templates.");
  if (toml_require_known_keys(value, templates_keys,
                              sizeof(templates_keys) / sizeof(templates_keys[0]), S("config"),
                              key_prefix, (struct Str){0}, log) != 0 ||
      toml_copy_template_name(value, key_prefix, "content", perm, &templates->content, log) != 0 ||
      toml_copy_strings(value, key_prefix, "aggregates", true, perm, &templates->aggregates,
                        &templates->aggregate_template_count, log) != 0 ||
      toml_copy_strings(value, key_prefix, "feeds", true, perm, &templates->feeds,
                        &templates->feed_template_count, log) != 0) {
    return -1;
  }
  return toml_copy_template_name(value, key_prefix, "not_found", perm, &templates->not_found, log);
}

static int require_valid_permalink(struct Str pattern, struct Arena scratch, struct Log* log) {
  switch (check_permalink(pattern, scratch)) {
    case PERMALINK_VALID:
      break;
    case PERMALINK_UNSAFE:
      return log_error(log,
                       "config key 'permalink' must expand to a safe relative path using only "
                       "letters, digits, '_', '-', '.', '/' and the '{slug}'/'{section}' tokens, "
                       "with no empty, '.' or '..' path segment: '%.*s'",
                       STR_FMT(pattern));
    case PERMALINK_NOT_DISTINCT:
      return log_error(log,
                       "config key 'permalink' must expand to a distinct path per content entry; "
                       "include the '{slug}' token: '%.*s'",
                       STR_FMT(pattern));
  }
  return 0;
}

static enum PermalinkVerdict check_permalink(struct Str pattern, struct Arena scratch) {
  struct Str urls[SECTION_SAMPLE_COUNT][SLUG_SAMPLE_COUNT];
  for (ptrdiff_t i = 0; i < SECTION_SAMPLE_COUNT; i++) {
    for (ptrdiff_t j = 0; j < SLUG_SAMPLE_COUNT; j++) {
      urls[i][j] = permalink_expand(pattern, SECTION_SAMPLES[i], SLUG_SAMPLES[j], &scratch);
    }
  }
  return check_permalink_expansions(urls);
}

static enum PermalinkVerdict check_permalink_expansions(
    struct Str urls[SECTION_SAMPLE_COUNT][SLUG_SAMPLE_COUNT]) {
  // Each question runs over the whole grid before the next one starts, so the verdict follows the
  // precedence `enum PermalinkVerdict` documents whichever expansion trips which check. Checking
  // one expansion at a time would let an early expansion fail a later question before a later
  // expansion failed an earlier one. A pattern that is unsafe only with an empty `{section}` would
  // then report a slug collision instead.
  for (ptrdiff_t i = 0; i < SECTION_SAMPLE_COUNT; i++) {
    for (ptrdiff_t j = 0; j < SLUG_SAMPLE_COUNT; j++) {
      if (!path_is_safe_relative(str_tail(urls[i][j], 1))) {
        return PERMALINK_UNSAFE;
      }
    }
  }
  for (ptrdiff_t i = 0; i < SECTION_SAMPLE_COUNT; i++) {
    for (ptrdiff_t j = 1; j < SLUG_SAMPLE_COUNT; j++) {
      if (str_eq(urls[i][0], urls[i][j])) {
        return PERMALINK_NOT_DISTINCT;
      }
    }
  }
  return PERMALINK_VALID;
}
