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

const struct Str SITE_CONFIG_PATH_DEFAULT = STR_INIT("sosig.toml");

#define PERMALINK_DEFAULT S("/{section}/{slug}.html")
#define CONTENT_DIR_DEFAULT S("content")
#define OUTPUT_DIR_DEFAULT S("public")
#define TEMPLATES_DIR_DEFAULT S("templates")
#define STATIC_DIR_DEFAULT S("static")
#define CONTENT_TEMPLATE_DEFAULT S("content.html")
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
    STR_INIT("content_template"),
    STR_INIT("aggregate_templates"),
    STR_INIT("feed_templates"),
    STR_INIT("feed_count"),
    STR_INIT("frontmatter_ignored_keys"),
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
 * Rejects a key outside `config_keys`, then applies required and optional string keys,
 * template-name arrays, and `feed_count`. It copies every value into `perm` rather than borrowing
 * from `table`. `site_config_load` calls `toml_free` as soon as this returns, which releases the
 * pool every `toml_datum_t` string points into, while the config outlives the parse.
 *
 * @param site_config Config that receives the values. Must not be `NULL`.
 * @param table       Parsed TOML top-level table.
 * @param config_dir  Directory a relative directory resolves against, or empty for the working one.
 * @param perm        Arena that owns the copied values. Must not be `NULL`.
 * @param scratch     Arena for validation temporaries.
 * @param log         Log that receives every error. Must not be `NULL`.
 */
static void site_config_load_fields(struct SiteConfig* site_config,
                                    toml_datum_t table,
                                    struct Str config_dir,
                                    struct Arena* perm,
                                    struct Arena scratch,
                                    struct Log* log) __attribute__((nonnull(1, 4, 6)));

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
      .content_template = CONTENT_TEMPLATE_DEFAULT,
      .aggregate_templates = AGGREGATE_TEMPLATES_DEFAULT,
      .aggregate_template_count =
          sizeof(AGGREGATE_TEMPLATES_DEFAULT) / sizeof(AGGREGATE_TEMPLATES_DEFAULT[0]),
      .feed_templates = FEED_TEMPLATES_DEFAULT,
      .feed_template_count = sizeof(FEED_TEMPLATES_DEFAULT) / sizeof(FEED_TEMPLATES_DEFAULT[0]),
      .feed_count = FEED_COUNT_DEFAULT,
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
  // directory. A key that fails validation is in `log` already, and the load still succeeded in the
  // sense this result reports: the file was read and parsed.
  const struct Str config_dir = path_file_dir(config_path);
  site_config_load_fields(site_config, parsed.toptab, config_dir, perm, scratch, log);
  toml_free(parsed);
  return 0;
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
  toml_print_key_string(&text, "content_template", site_config->content_template);
  toml_print_key_string_array(&text, "aggregate_templates", site_config->aggregate_templates,
                              site_config->aggregate_template_count);
  toml_print_key_string_array(&text, "feed_templates", site_config->feed_templates,
                              site_config->feed_template_count);
  buf_printf(&text, "feed_count = %td\n", site_config->feed_count);
  toml_print_key_string_array(&text, "frontmatter_ignored_keys",
                              site_config->frontmatter_ignored_keys,
                              site_config->frontmatter_ignored_key_count);
  return os_print(os, 1, buf_str(&text));
}

static void site_config_load_fields(struct SiteConfig* site_config,
                                    toml_datum_t table,
                                    struct Str config_dir,
                                    struct Arena* perm,
                                    struct Arena scratch,
                                    struct Log* log) {
  // Every key is checked even after one fails, so a config with several mistakes reports all of
  // them in one run. A rejected value is never stored: the key keeps its default, so a build that
  // goes on to check its content does so against a usable config rather than reporting the same
  // mistake again once per content file.
  (void)toml_require_known_keys(table, config_keys, sizeof(config_keys) / sizeof(config_keys[0]),
                                S("config"), S(""), (struct Str){0}, log);

  // Each key is read and validated at its row of the README's key table, which `sosig config` also
  // follows, so the errors of one load come in that order.
  (void)toml_copy_string(table, S(""), "title", true, perm, &site_config->title, log);
  (void)toml_copy_string(table, S(""), "description", true, perm, &site_config->description, log);
  (void)toml_copy_string(table, S(""), "author", true, perm, &site_config->author, log);
  (void)toml_copy_base_url(table, S(""), "base_url", true, perm, &site_config->base_url, log);
  if (toml_copy_string(table, S(""), "permalink", false, perm, &site_config->permalink, log) != 0 ||
      require_valid_permalink(site_config->permalink, scratch, log) != 0) {
    site_config->permalink = PERMALINK_DEFAULT;
    site_config->is_output_key_rejected = true;
  }
  // A default directory resolves against the config's directory too, so that `-c sub/sosig.toml`
  // without `output_dir` writes `sub/public`. A key that is absent or rejected keeps it.
  site_config->content_dir = path_resolve(config_dir, site_config->content_dir, perm);
  site_config->output_dir = path_resolve(config_dir, site_config->output_dir, perm);
  site_config->templates_dir = path_resolve(config_dir, site_config->templates_dir, perm);
  site_config->static_dir = path_resolve(config_dir, site_config->static_dir, perm);
  site_config->is_content_dir_rejected =
      toml_copy_dir(table, S(""), "content_dir", false, config_dir, perm, &site_config->content_dir,
                    log) != 0;
  site_config->is_output_dir_rejected = toml_copy_dir(table, S(""), "output_dir", false, config_dir,
                                                      perm, &site_config->output_dir, log) != 0;
  site_config->is_templates_dir_rejected =
      toml_copy_dir(table, S(""), "templates_dir", false, config_dir, perm,
                    &site_config->templates_dir, log) != 0;
  site_config->is_static_dir_rejected = toml_copy_dir(table, S(""), "static_dir", false, config_dir,
                                                      perm, &site_config->static_dir, log) != 0;
  if (toml_copy_string(table, S(""), "content_template", false, perm,
                       &site_config->content_template, log) == 0 &&
      !path_is_safe_relative(site_config->content_template)) {
    (void)log_error(log,
                    "config key 'content_template' must be a safe relative template name: '%.*s'",
                    STR_FMT(site_config->content_template));
    site_config->content_template = CONTENT_TEMPLATE_DEFAULT;
  }
  if (toml_copy_strings(table, S(""), "aggregate_templates", true, perm,
                        &site_config->aggregate_templates, &site_config->aggregate_template_count,
                        log) != 0) {
    site_config->is_output_key_rejected = true;
  }
  if (toml_copy_strings(table, S(""), "feed_templates", true, perm, &site_config->feed_templates,
                        &site_config->feed_template_count, log) != 0) {
    site_config->is_output_key_rejected = true;
  }
  const struct TomlSizeKey feed_count = {
      .key = "feed_count",
      .field = &site_config->feed_count,
      // `PTRDIFF_MAX` binds only where `ptrdiff_t` is narrower than `int64_t`, as on a 32-bit
      // target.
      .value_max = PTRDIFF_MAX,
      .limit_name = "feed count",
  };
  (void)toml_copy_size(table, S(""), &feed_count, log);
  (void)toml_copy_strings(table, S(""), "frontmatter_ignored_keys", false, perm,
                          &site_config->frontmatter_ignored_keys,
                          &site_config->frontmatter_ignored_key_count, log);
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
