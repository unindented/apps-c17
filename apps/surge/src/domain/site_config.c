#include "domain/site_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <tomlc17.h>

#include "core/arena.h"
#include "core/ascii.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/path.h"
#include "core/str.h"
#include "core/text.h"
#include "formats/toml.h"
#include "os/os.h"
#include "output/output_path.h"

/** A parsed table and the key prefix that its diagnostics put before a key. */
struct SiteConfigTable {
  /** Parsed table. */
  toml_datum_t datum;

  /** Dotted path of the table that diagnostics put before a key, such as `repo[0].`. */
  struct Str key_prefix;
};

const struct Str SITE_CONFIG_PATH_DEFAULT = STR_INIT("surge.toml");

#define OUTPUT_DIR_DEFAULT S("public")
#define TEMPLATES_DIR_DEFAULT S("templates")
#define STATIC_DIR_DEFAULT S("static")
#define NOT_FOUND_PATH_DEFAULT S("404.html")

/** Commits on one log page when `log_page_size` is unset, as many as GitHub shows. */
enum { LOG_PAGE_SIZE_DEFAULT = 100 };

/** Most commits one log page lists, which keeps the count of pages within range. */
enum { LOG_PAGE_SIZE_MAX = 10000 };

/** Entries in one feed when `feed_count` is unset. */
enum { FEED_COUNT_DEFAULT = 25 };

/** Default `limits.blob_len_max`: 1 MiB, about where GitHub stops showing a file's contents. */
enum { BLOB_LEN_MAX_DEFAULT = 1024 * 1024 };

/**
 * Largest `limits.blob_len_max` and `limits.diff_len_max`: 4 MiB, a 64th of the 256 MiB that one
 * render writes at most (`TEMPLATE_RENDER_OUTPUT_LEN_MAX` in `templates/template_set.h`). A blob
 * page and a commit page write a table row for each line, with its number, its anchor and its
 * highlighted text, and a blob of one-byte lines makes a page about 55 times its size, so a blob or
 * a diff under this bound renders within that limit. A commit page also embeds up to
 * `limits.image_len_max` of images, at 4/3 of their size in base64, which the same bound leaves
 * room for. Changing it must update `test_load_bounds_integers`.
 */
enum { BLOB_LEN_MAX_LIMIT = 4 * 1024 * 1024 };

/** Default `limits.raw_len_max`: 100 MiB, the largest file GitHub accepts in a repository. */
enum { RAW_LEN_MAX_DEFAULT = 100 * 1024 * 1024 };

/** Default `limits.diff_file_count_max`: GitHub shows the diffs of 300 files. */
enum { DIFF_FILE_COUNT_MAX_DEFAULT = 300 };

/** Default `limits.diff_line_count_max`: GitHub shows 20,000 diff lines. */
enum { DIFF_LINE_COUNT_MAX_DEFAULT = 20000 };

/** Default `limits.diff_len_max`: GitHub shows 1 MB of a diff. */
enum { DIFF_LEN_MAX_DEFAULT = 1024 * 1024 };

/** Default `limits.image_len_max`: 2 MiB, a handful of screenshots. */
enum { IMAGE_LEN_MAX_DEFAULT = 2 * 1024 * 1024 };

/** Default `limits.pack_len_max`: 25 MiB, the largest file Cloudflare Pages serves. */
enum { PACK_LEN_MAX_DEFAULT = 25 * 1024 * 1024 };

/** Every key the top-level table may hold. */
static const struct Str config_keys[] = {
    // clang-format off: one key per line, so adding or removing a key is a one-line change.
    STR_INIT("title"),
    STR_INIT("description"),
    STR_INIT("author"),
    STR_INIT("base_url"),
    STR_INIT("output_dir"),
    STR_INIT("templates_dir"),
    STR_INIT("static_dir"),
    STR_INIT("not_found_path"),
    STR_INIT("log_page_size"),
    STR_INIT("feed_count"),
    STR_INIT("headers_file"),
    STR_INIT("llm_trailer_keys"),
    STR_INIT("llm_trailer_values"),
    STR_INIT("limits"),
    STR_INIT("templates"),
    STR_INIT("repo"),
    // clang-format on
};

/**
 * Default `llm_trailer_keys`: the trailers that name who wrote a commit with its author, as Claude
 * Code, aider and the Linux kernel's convention write them.
 */
static const struct Str LLM_TRAILER_KEYS_DEFAULT[] = {
    STR_INIT("Co-authored-by"),
    STR_INIT("Assisted-by"),
    STR_INIT("Generated-by"),
};

/** Every key the `[limits]` table may hold. */
static const struct Str limits_keys[] = {
    // clang-format off: one key per line, so adding or removing a key is a one-line change.
    STR_INIT("blob_len_max"),
    STR_INIT("raw_len_max"),
    STR_INIT("diff_file_count_max"),
    STR_INIT("diff_line_count_max"),
    STR_INIT("diff_len_max"),
    STR_INIT("image_len_max"),
    STR_INIT("pack_len_max"),
    // clang-format on
};

/** Every key a `[[repo]]` table may hold. */
static const struct Str repo_keys[] = {
    // clang-format off: one key per line, so adding or removing a key is a one-line change.
    STR_INIT("name"),
    STR_INIT("path"),
    STR_INIT("description"),
    STR_INIT("default_branch"),
    STR_INIT("releases_dir"),
    STR_INIT("mise"),
    STR_INIT("project"),
    // clang-format on
};

/** Every key a `[[repo.project]]` table may hold. */
static const struct Str project_keys[] = {
    // clang-format off: one key per line, so adding or removing a key is a one-line change.
    STR_INIT("name"),
    STR_INIT("path"),
    STR_INIT("description"),
    STR_INIT("tag_prefix"),
    STR_INIT("mise"),
    // clang-format on
};

/**
 * Names a repo may not have, compared after ASCII case folding, because the output root holds a
 * file or directory of that name. The first segment of `not_found_path` is reserved too, by
 * `site_config_load_repos`.
 */
static const struct Str REPO_NAMES_RESERVED[] = {
    STR_INIT("index.html"),
    STR_INIT("_surge"),
    STR_INIT("_headers"),
};

/**
 * Names a project may not have, compared after ASCII case folding, because `releases/` already
 * holds them: the directories next to a project's page, and the releases page.
 */
static const struct Str PROJECT_NAMES_RESERVED[] = {
    STR_INIT("tag"),
    STR_INIT("download"),
    STR_INIT("index.html"),
};

/**
 * @brief Copies the top-level keys, the `[limits]` table and the `[templates]` table into
 *        `site_config`, stopping at the first error.
 *
 * @param site_config Config that receives the values. Must not be `NULL`.
 * @param table       Parsed top-level table.
 * @param config_dir  Directory the relative paths resolve against, or empty for the working one.
 * @param perm        Arena that owns the copied values. Must not be `NULL`.
 * @param log         Log that receives the error. Must not be `NULL`.
 * @return `0` on success, or `-1` after logging the first error.
 */
static int site_config_load_fields(struct SiteConfig* site_config,
                                   toml_datum_t table,
                                   struct Str config_dir,
                                   struct Arena* perm,
                                   struct Log* log) __attribute__((nonnull(1, 4, 5)));

/**
 * @brief Copies `llm_trailer_keys` and `llm_trailer_values` into `llm_trailers`, and requires each
 *        of their strings to be non-empty, stopping at the first error.
 *
 * @param llm_trailers Match that receives the lists, holding their defaults. Must not be `NULL`.
 * @param table        Parsed top-level table.
 * @param perm         Arena that owns the copied strings. Must not be `NULL`.
 * @param log          Log that receives the error. Must not be `NULL`.
 * @return `0` on success or when the keys are absent, or `-1` after logging the first error.
 */
static int site_config_load_fields_llm_trailers(struct TrailerMatch* llm_trailers,
                                                toml_datum_t table,
                                                struct Arena* perm,
                                                struct Log* log) __attribute__((nonnull(1, 3, 4)));

/**
 * @brief Copies the `[limits]` table into `limits`, stopping at the first error.
 *
 * @param limits Limits that receive the values, holding their defaults. Must not be `NULL`.
 * @param table  Parsed top-level table.
 * @param log    Log that receives the error. Must not be `NULL`.
 * @return `0` on success or when the table is absent, or `-1` after logging the first error.
 */
static int site_config_load_fields_limits(struct SurgeLimits* limits,
                                          toml_datum_t table,
                                          struct Log* log) __attribute__((nonnull(1, 3)));

/**
 * @brief Copies the `[templates]` table into `templates`, stopping at the first error.
 *
 * @param templates Templates that receive the names, holding their defaults. Must not be `NULL`.
 * @param table     Parsed top-level table.
 * @param perm      Arena that owns the copied names. Must not be `NULL`.
 * @param log       Log that receives the error. Must not be `NULL`.
 * @return `0` on success or when the table is absent, or `-1` after logging the first error.
 */
static int site_config_load_fields_templates(struct SurgeTemplates* templates,
                                             toml_datum_t table,
                                             struct Arena* perm,
                                             struct Log* log) __attribute__((nonnull(1, 3, 4)));

/**
 * @brief Copies every `[[repo]]` table into `site_config`, and checks the names across repos and
 *        against the first segment of `not_found_path`.
 *
 * @param site_config Config that receives the repos, with its `not_found_path` loaded. Must not be
 *                    `NULL`.
 * @param table       Parsed top-level table.
 * @param config_dir  Directory the relative paths resolve against, or empty for the working one.
 * @param perm        Arena that owns the copied values. Must not be `NULL`.
 * @param scratch     Arena for the key prefixes of the diagnostics.
 * @param log         Log that receives the error. Must not be `NULL`.
 * @return `0` on success, or `-1` after logging the first error.
 */
static int site_config_load_repos(struct SiteConfig* site_config,
                                  toml_datum_t table,
                                  struct Str config_dir,
                                  struct Arena* perm,
                                  struct Arena scratch,
                                  struct Log* log) __attribute__((nonnull(1, 4, 6)));

/**
 * @brief Copies one `[[repo]]` table and its projects into `repo`.
 *
 * @param repo       Repo that receives the values, zeroed. Must not be `NULL`.
 * @param table      Parsed repo table and its key prefix. Must not be `NULL`.
 * @param config_dir Directory the relative paths resolve against, or empty for the working one.
 * @param perm       Arena that owns the copied values. Must not be `NULL`.
 * @param scratch    Arena for the key prefixes of the diagnostics.
 * @param log        Log that receives the error. Must not be `NULL`.
 * @return `0` on success, or `-1` after logging the first error.
 */
static int site_config_load_repos_one(struct SurgeRepo* repo,
                                      const struct SiteConfigTable* table,
                                      struct Str config_dir,
                                      struct Arena* perm,
                                      struct Arena scratch,
                                      struct Log* log) __attribute__((nonnull(1, 2, 4, 6)));

/**
 * @brief Copies every `[[repo.project]]` table of one repo into `repo`, and checks their names and
 *        tag prefixes against each other.
 *
 * @param repo    Repo that receives the projects. Must not be `NULL`.
 * @param table   Parsed repo table and its key prefix. Must not be `NULL`.
 * @param perm    Arena that owns the copied values. Must not be `NULL`.
 * @param scratch Arena for the key prefixes of the diagnostics.
 * @param log     Log that receives the error. Must not be `NULL`.
 * @return `0` on success, or `-1` after logging the first error.
 */
static int site_config_load_repos_one_projects(struct SurgeRepo* repo,
                                               const struct SiteConfigTable* table,
                                               struct Arena* perm,
                                               struct Arena scratch,
                                               struct Log* log)
    __attribute__((nonnull(1, 2, 3, 5)));

/**
 * @brief Copies one `[[repo.project]]` table into `project`.
 *
 * @param project Project that receives the values, zeroed. Must not be `NULL`.
 * @param table   Parsed project table and its key prefix. Must not be `NULL`.
 * @param perm    Arena that owns the copied values. Must not be `NULL`.
 * @param log     Log that receives the error. Must not be `NULL`.
 * @return `0` on success, or `-1` after logging the first error.
 */
static int site_config_load_repos_one_projects_one(struct SurgeProject* project,
                                                   const struct SiteConfigTable* table,
                                                   struct Arena* perm,
                                                   struct Log* log)
    __attribute__((nonnull(1, 2, 3, 4)));

/**
 * @brief Returns the tables of an array-of-tables key, rejecting any other type.
 *
 * @param table           Parsed table holding `key`, and its key prefix. Must not be `NULL`.
 * @param key             Key of the array. Must not be `NULL`.
 * @param is_required     Whether absence is an error.
 * @param arena           Arena that owns the returned tables and their prefixes. Must not be
 *                        `NULL`.
 * @param tables_out      Receives one entry per table, each with its prefix, such as `repo[0].`.
 *                        Must not be `NULL`.
 * @param table_count_out Receives the number of tables, `0` when the key is absent. Must not be
 *                        `NULL`.
 * @param log             Log that receives the error. Must not be `NULL`.
 * @return `0` on success or when an optional key is absent, or `-1` on a missing required key, a
 *         value that is not an array of tables, or an empty array for a required key.
 */
static int copy_tables(const struct SiteConfigTable* table,
                       const char* key,
                       bool is_required,
                       struct Arena* arena,
                       struct SiteConfigTable** tables_out,
                       ptrdiff_t* table_count_out,
                       struct Log* log) __attribute__((nonnull(1, 2, 4, 5, 6, 7)));

/**
 * @brief Copies a required repo or project name, and rejects one that is not a valid name.
 *
 * @param table          Parsed table and its key prefix. Must not be `NULL`.
 * @param reserved_names Names refused after case folding, `reserved_count` of them. Must not be
 *                       `NULL`.
 * @param reserved_count Number of entries in `reserved_names`.
 * @param arena          Arena that owns the copied name. Must not be `NULL`.
 * @param name_out       Receives the name when it is valid. Must not be `NULL`.
 * @param log            Log that receives the error. Must not be `NULL`.
 * @return `0` when the name was copied, or `-1` when it is missing, empty, too long, not a valid
 *         name, or reserved.
 */
static int copy_name(const struct SiteConfigTable* table,
                     const struct Str* reserved_names,
                     ptrdiff_t reserved_count,
                     struct Arena* arena,
                     struct Str* name_out,
                     struct Log* log) __attribute__((nonnull(1, 2, 4, 5, 6)));

/**
 * @brief Reports whether `name` uses only `[A-Za-z0-9._-]`, does not start with `.` and does not
 *        end in `.git` in any case.
 *
 * @param name Name to check.
 * @return `true` when the name is valid.
 */
static bool is_valid_name(struct Str name);

/**
 * @brief Reports whether `path` is a relative path in a repo: not empty, with no leading `/`, and
 *        with no empty, `.` or `..` segment. Any other byte is allowed, because the path only has
 *        to name a directory of the repo's tree, which the scan checks.
 *
 * @param path Path to check, without trailing `/` bytes.
 * @return `true` when `path` is such a path.
 */
static bool is_repo_relative_path(struct Str path);

void site_config_init(struct SiteConfig* site_config) {
  *site_config = (struct SiteConfig){
      .output_dir = OUTPUT_DIR_DEFAULT,
      .templates_dir = TEMPLATES_DIR_DEFAULT,
      .static_dir = STATIC_DIR_DEFAULT,
      .not_found_path = NOT_FOUND_PATH_DEFAULT,
      .log_page_size = LOG_PAGE_SIZE_DEFAULT,
      .feed_count = FEED_COUNT_DEFAULT,
      .llm_trailers =
          {
              .keys = LLM_TRAILER_KEYS_DEFAULT,
              .key_count = (ptrdiff_t)(sizeof(LLM_TRAILER_KEYS_DEFAULT) /
                                       sizeof(LLM_TRAILER_KEYS_DEFAULT[0])),
          },
      .limits =
          {
              .blob_len_max = BLOB_LEN_MAX_DEFAULT,
              .raw_len_max = RAW_LEN_MAX_DEFAULT,
              .diff_file_count_max = DIFF_FILE_COUNT_MAX_DEFAULT,
              .diff_line_count_max = DIFF_LINE_COUNT_MAX_DEFAULT,
              .diff_len_max = DIFF_LEN_MAX_DEFAULT,
              .image_len_max = IMAGE_LEN_MAX_DEFAULT,
              .pack_len_max = PACK_LEN_MAX_DEFAULT,
          },
      .templates =
          {
              .index = S("index.html"),
              .tree = S("tree.html"),
              .blob = S("blob.html"),
              .branch_list = S("branch-list.html"),
              .tag_list = S("tag-list.html"),
              .tag_feed = S("tag-feed.atom"),
              .commit = S("commit.html"),
              .commit_list = S("commit-list.html"),
              .commit_feed = S("commit-feed.atom"),
              .release = S("release.html"),
              .release_list = S("release-list.html"),
              .release_project = S("release-project.html"),
              .release_feed = S("release-feed.atom"),
              .not_found = S("404.html"),
          },
  };
}

int site_config_load(struct SiteConfig* site_config,
                     struct Str config_path,
                     struct Os* os,
                     struct Arena* perm,
                     struct Arena scratch,
                     struct Log* log) {
  // Zero-initialized so that clang-analyzer can follow the struct across `toml_free`.
  toml_result_t parsed = {0};
  if (toml_load_config(config_path, os, scratch, &parsed, log) != 0) {
    return -1;
  }
  // A relative path in the file names a place next to the file, so the build can run from any
  // directory.
  const struct Str config_dir = path_file_dir(config_path);
  // The parse's strings must outlive both steps, so the second runs only after the first and the
  // parse is released after either fails.
  int rc = site_config_load_fields(site_config, parsed.toptab, config_dir, perm, log);
  if (rc == 0) {
    rc = site_config_load_repos(site_config, parsed.toptab, config_dir, perm, scratch, log);
  }
  toml_free(parsed);
  return rc;
}

int site_config_print(const struct SiteConfig* site_config, struct Os* os, struct Arena scratch) {
  struct Buf text = {.arena = &scratch};
  toml_print_key_string(&text, "title", site_config->title);
  toml_print_key_string(&text, "description", site_config->description);
  toml_print_key_string(&text, "author", site_config->author);
  toml_print_key_string(&text, "base_url", site_config->base_url);
  toml_print_key_string(&text, "output_dir", site_config->output_dir);
  toml_print_key_string(&text, "templates_dir", site_config->templates_dir);
  toml_print_key_string(&text, "static_dir", site_config->static_dir);
  toml_print_key_string(&text, "not_found_path", site_config->not_found_path);
  buf_printf(&text, "log_page_size = %td\n", site_config->log_page_size);
  buf_printf(&text, "feed_count = %td\n", site_config->feed_count);
  buf_printf(&text, "headers_file = %s\n", site_config->has_headers_file ? "true" : "false");
  const struct TrailerMatch* llm_trailers = &site_config->llm_trailers;
  toml_print_key_string_array(&text, "llm_trailer_keys", llm_trailers->keys,
                              llm_trailers->key_count);
  toml_print_key_string_array(&text, "llm_trailer_values", llm_trailers->values,
                              llm_trailers->value_count);

  const struct SurgeLimits* limits = &site_config->limits;
  buf_printf(&text,
             "\n[limits]\n"
             "blob_len_max = %td\n"
             "raw_len_max = %td\n"
             "diff_file_count_max = %td\n"
             "diff_line_count_max = %td\n"
             "diff_len_max = %td\n"
             "image_len_max = %td\n"
             "pack_len_max = %td\n",
             limits->blob_len_max, limits->raw_len_max, limits->diff_file_count_max,
             limits->diff_line_count_max, limits->diff_len_max, limits->image_len_max,
             limits->pack_len_max);

  struct SurgeTemplateKey template_keys[SURGE_TEMPLATE_KEY_COUNT];
  site_config_template_keys(site_config, template_keys);
  buf_append(&text, S("\n[templates]\n"));
  for (ptrdiff_t i = 0; i < SURGE_TEMPLATE_KEY_COUNT; i++) {
    toml_print_key_string(&text, template_keys[i].key, template_keys[i].name);
  }

  for (ptrdiff_t i = 0; i < site_config->repo_count; i++) {
    const struct SurgeRepo* repo = &site_config->repos[i];
    buf_append(&text, S("\n[[repo]]\n"));
    toml_print_key_string(&text, "name", repo->name);
    toml_print_key_string(&text, "path", repo->path);
    if (repo->has_description) {
      toml_print_key_string(&text, "description", repo->description);
    }
    toml_print_key_string_optional(&text, "default_branch", repo->default_branch);
    toml_print_key_string_optional(&text, "releases_dir", repo->releases_dir);
    if (repo->is_mise) {
      buf_append(&text, S("mise = true\n"));
    }
    for (ptrdiff_t j = 0; j < repo->project_count; j++) {
      const struct SurgeProject* project = &repo->projects[j];
      buf_append(&text, S("\n[[repo.project]]\n"));
      toml_print_key_string(&text, "name", project->name);
      toml_print_key_string(&text, "path", project->path);
      toml_print_key_string_optional(&text, "description", project->description);
      toml_print_key_string_optional(&text, "tag_prefix", project->tag_prefix);
      if (project->is_mise) {
        buf_append(&text, S("mise = true\n"));
      }
    }
  }
  return os_print(os, 1, buf_str(&text));
}

void site_config_template_keys(const struct SiteConfig* site_config,
                               struct SurgeTemplateKey keys_out[SURGE_TEMPLATE_KEY_COUNT]) {
  const struct SurgeTemplates* templates = &site_config->templates;
  const struct SurgeTemplateKey keys[] = {
      {"index", templates->index, false},
      {"tree", templates->tree, false},
      {"blob", templates->blob, false},
      {"branch_list", templates->branch_list, false},
      {"tag_list", templates->tag_list, false},
      {"tag_feed", templates->tag_feed, false},
      {"commit", templates->commit, false},
      {"commit_list", templates->commit_list, false},
      {"commit_feed", templates->commit_feed, false},
      {"release", templates->release, true},
      {"release_list", templates->release_list, true},
      {"release_project", templates->release_project, true},
      {"release_feed", templates->release_feed, true},
      {"not_found", templates->not_found, false},
  };
  _Static_assert(sizeof(keys) / sizeof(keys[0]) == SURGE_TEMPLATE_KEY_COUNT,
                 "every field of SurgeTemplates is a key");
  for (ptrdiff_t i = 0; i < SURGE_TEMPLATE_KEY_COUNT; i++) {
    keys_out[i] = keys[i];
  }
}

static int site_config_load_fields(struct SiteConfig* site_config,
                                   toml_datum_t table,
                                   struct Str config_dir,
                                   struct Arena* perm,
                                   struct Log* log) {
  if (toml_require_known_keys(table, config_keys, sizeof(config_keys) / sizeof(config_keys[0]),
                              S("config"), S(""), (struct Str){0}, log) != 0 ||
      toml_copy_string(table, S(""), "title", true, perm, &site_config->title, log) != 0 ||
      toml_copy_string(table, S(""), "description", true, perm, &site_config->description, log) !=
          0 ||
      toml_copy_string(table, S(""), "author", true, perm, &site_config->author, log) != 0 ||
      toml_copy_base_url(table, S(""), "base_url", true, perm, &site_config->base_url, log) != 0) {
    return -1;
  }
  // A default directory resolves against the config's directory too, so that `-c sub/surge.toml`
  // without `output_dir` writes `sub/public`. A key that is absent keeps it.
  site_config->output_dir = path_resolve(config_dir, site_config->output_dir, perm);
  site_config->templates_dir = path_resolve(config_dir, site_config->templates_dir, perm);
  site_config->static_dir = path_resolve(config_dir, site_config->static_dir, perm);
  if (toml_copy_dir(table, S(""), "output_dir", false, config_dir, perm, &site_config->output_dir,
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

  const struct TomlSizeKey counts[] = {
      {
          .key = "log_page_size",
          .field = &site_config->log_page_size,
          .value_min = 1,
          .value_max = LOG_PAGE_SIZE_MAX,
          .limit_name = "log page size",
      },
      {
          .key = "feed_count",
          .field = &site_config->feed_count,
          .value_max = PTRDIFF_MAX,
          .limit_name = "feed count",
      },
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(counts) / sizeof(counts[0])); i++) {
    if (toml_copy_size(table, S(""), &counts[i], log) != 0) {
      return -1;
    }
  }
  if (toml_copy_bool(table, S(""), "headers_file", &site_config->has_headers_file, log) != 0 ||
      site_config_load_fields_llm_trailers(&site_config->llm_trailers, table, perm, log) != 0) {
    return -1;
  }
  if (site_config_load_fields_limits(&site_config->limits, table, log) != 0) {
    return -1;
  }
  return site_config_load_fields_templates(&site_config->templates, table, perm, log);
}

static int site_config_load_fields_llm_trailers(struct TrailerMatch* llm_trailers,
                                                toml_datum_t table,
                                                struct Arena* perm,
                                                struct Log* log) {
  const struct {
    const char* key;
    const struct Str** items;
    ptrdiff_t* count;
  } lists[] = {
      {"llm_trailer_keys", &llm_trailers->keys, &llm_trailers->key_count},
      {"llm_trailer_values", &llm_trailers->values, &llm_trailers->value_count},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(lists) / sizeof(lists[0])); i++) {
    if (toml_copy_strings(table, S(""), lists[i].key, false, perm, lists[i].items, lists[i].count,
                          log) != 0) {
      return -1;
    }
    // An empty key names no trailer, and an empty value is in every one.
    for (ptrdiff_t j = 0; j < *lists[i].count; j++) {
      if ((*lists[i].items)[j].len == 0) {
        return log_error(log, "config key '%s' must not contain an empty string", lists[i].key);
      }
    }
  }
  return 0;
}

static int site_config_load_fields_limits(struct SurgeLimits* limits,
                                          toml_datum_t table,
                                          struct Log* log) {
  const toml_datum_t value = toml_get(table, "limits");
  if (value.type == TOML_UNKNOWN) {
    return 0;
  }
  if (value.type != TOML_TABLE) {
    return log_error(log, "config key 'limits' must be a table");
  }
  const struct Str key_prefix = S("limits.");
  if (toml_require_known_keys(value, limits_keys, sizeof(limits_keys) / sizeof(limits_keys[0]),
                              S("config"), key_prefix, (struct Str){0}, log) != 0) {
    return -1;
  }
  const struct TomlSizeKey keys[] = {
      {
          .key = "blob_len_max",
          .field = &limits->blob_len_max,
          .value_max = BLOB_LEN_MAX_LIMIT,
          .limit_name = "blob length",
          .is_len = true,
      },
      {
          .key = "raw_len_max",
          .field = &limits->raw_len_max,
          .value_max = SURGE_RAW_LEN_MAX_LIMIT,
          .limit_name = "raw length",
          .is_len = true,
      },
      {
          .key = "diff_file_count_max",
          .field = &limits->diff_file_count_max,
          .value_max = PTRDIFF_MAX,
          .limit_name = "diff file count",
      },
      {
          .key = "diff_line_count_max",
          .field = &limits->diff_line_count_max,
          .value_max = PTRDIFF_MAX,
          .limit_name = "diff line count",
      },
      {
          .key = "diff_len_max",
          .field = &limits->diff_len_max,
          .value_max = BLOB_LEN_MAX_LIMIT,
          .limit_name = "diff length",
          .is_len = true,
      },
      {
          .key = "image_len_max",
          .field = &limits->image_len_max,
          .value_max = BLOB_LEN_MAX_LIMIT,
          .limit_name = "image length",
          .is_len = true,
      },
      {
          .key = "pack_len_max",
          .field = &limits->pack_len_max,
          .value_max = PTRDIFF_MAX,
          .limit_name = "pack length",
          .is_len = true,
      },
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(keys) / sizeof(keys[0])); i++) {
    if (toml_copy_size(value, key_prefix, &keys[i], log) != 0) {
      return -1;
    }
  }
  return 0;
}

static int site_config_load_fields_templates(struct SurgeTemplates* templates,
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
  // The keys in the order the table lists them, each with the field it fills.
  const struct {
    const char* key;
    struct Str* name;
  } keys[] = {
      {"index", &templates->index},
      {"tree", &templates->tree},
      {"blob", &templates->blob},
      {"branch_list", &templates->branch_list},
      {"tag_list", &templates->tag_list},
      {"tag_feed", &templates->tag_feed},
      {"commit", &templates->commit},
      {"commit_list", &templates->commit_list},
      {"commit_feed", &templates->commit_feed},
      {"release", &templates->release},
      {"release_list", &templates->release_list},
      {"release_project", &templates->release_project},
      {"release_feed", &templates->release_feed},
      {"not_found", &templates->not_found},
  };
  _Static_assert(sizeof(keys) / sizeof(keys[0]) == SURGE_TEMPLATE_KEY_COUNT,
                 "every field of SurgeTemplates is a key");
  struct Str known_keys[SURGE_TEMPLATE_KEY_COUNT];
  for (ptrdiff_t i = 0; i < SURGE_TEMPLATE_KEY_COUNT; i++) {
    known_keys[i] = str_from_cstr(keys[i].key);
  }
  const struct Str key_prefix = S("templates.");
  if (toml_require_known_keys(value, known_keys, SURGE_TEMPLATE_KEY_COUNT, S("config"), key_prefix,
                              (struct Str){0}, log) != 0) {
    return -1;
  }
  for (ptrdiff_t i = 0; i < SURGE_TEMPLATE_KEY_COUNT; i++) {
    if (toml_copy_template_name(value, key_prefix, keys[i].key, perm, keys[i].name, log) != 0) {
      return -1;
    }
  }
  return 0;
}

static int site_config_load_repos(struct SiteConfig* site_config,
                                  toml_datum_t table,
                                  struct Str config_dir,
                                  struct Arena* perm,
                                  struct Arena scratch,
                                  struct Log* log) {
  struct SiteConfigTable* tables = NULL;
  ptrdiff_t table_count = 0;
  const struct SiteConfigTable site = {.datum = table};
  if (copy_tables(&site, "repo", true, &scratch, &tables, &table_count, log) != 0) {
    return -1;
  }
  struct SurgeRepo* repos = arena_new(perm, table_count, struct SurgeRepo);
  for (ptrdiff_t i = 0; i < table_count; i++) {
    if (site_config_load_repos_one(&repos[i], &tables[i], config_dir, perm, scratch, log) != 0) {
      return -1;
    }
  }
  // The not-found page's first segment is a file or a directory at the output root, as a repo's
  // directory is, so a repo of that name in any case would share it.
  const struct Str not_found_segment = str_cut(site_config->not_found_path, '/').head;
  // Two repos whose names differ only in case would share one directory on a case-insensitive host,
  // so both names are refused, and the message names both.
  for (ptrdiff_t i = 0; i < table_count; i++) {
    if (str_eq_folded(repos[i].name, not_found_segment)) {
      return log_error(log,
                       "config key '%.*sname' must not be the first segment of 'not_found_path' in "
                       "any case: '%.*s'",
                       STR_FMT(tables[i].key_prefix), STR_FMT(repos[i].name));
    }
    for (ptrdiff_t j = 0; j < i; j++) {
      if (str_eq_folded(repos[i].name, repos[j].name)) {
        return log_error(log,
                         "config keys '%.*sname' and '%.*sname' name repos that collide after case "
                         "folding: '%.*s' and '%.*s'",
                         STR_FMT(tables[j].key_prefix), STR_FMT(tables[i].key_prefix),
                         STR_FMT(repos[j].name), STR_FMT(repos[i].name));
      }
    }
  }
  site_config->repos = repos;
  site_config->repo_count = table_count;
  return 0;
}

static int site_config_load_repos_one(struct SurgeRepo* repo,
                                      const struct SiteConfigTable* table,
                                      struct Str config_dir,
                                      struct Arena* perm,
                                      struct Arena scratch,
                                      struct Log* log) {
  if (toml_require_known_keys(table->datum, repo_keys, sizeof(repo_keys) / sizeof(repo_keys[0]),
                              S("config"), table->key_prefix, (struct Str){0}, log) != 0 ||
      copy_name(table, REPO_NAMES_RESERVED,
                sizeof(REPO_NAMES_RESERVED) / sizeof(REPO_NAMES_RESERVED[0]), perm, &repo->name,
                log) != 0 ||
      toml_copy_dir(table->datum, table->key_prefix, "path", true, config_dir, perm, &repo->path,
                    log) != 0 ||
      toml_copy_string(table->datum, table->key_prefix, "description", false, perm,
                       &repo->description, log) != 0) {
    return -1;
  }
  // A description set to an empty string shows none, so the scan must tell it from an absent key.
  repo->has_description = toml_get(table->datum, "description").type == TOML_STRING;
  if (toml_copy_string(table->datum, table->key_prefix, "default_branch", false, perm,
                       &repo->default_branch, log) != 0) {
    return -1;
  }
  if (repo->default_branch.data != NULL && repo->default_branch.len == 0) {
    return log_error(log, "config key '%.*sdefault_branch' must not be empty",
                     STR_FMT(table->key_prefix));
  }
  if (toml_copy_dir(table->datum, table->key_prefix, "releases_dir", false, config_dir, perm,
                    &repo->releases_dir, log) != 0 ||
      toml_copy_bool(table->datum, table->key_prefix, "mise", &repo->is_mise, log) != 0 ||
      site_config_load_repos_one_projects(repo, table, perm, scratch, log) != 0) {
    return -1;
  }
  if (repo->is_mise && repo->releases_dir.len == 0) {
    return log_error(log, "config key '%.*smise' needs '%.*sreleases_dir'",
                     STR_FMT(table->key_prefix), STR_FMT(table->key_prefix));
  }
  // A project with a `tag_prefix` takes the repo's releases, and has a `mise` of its own.
  for (ptrdiff_t i = 0; repo->is_mise && i < repo->project_count; i++) {
    if (repo->projects[i].tag_prefix.len > 0) {
      return log_error(log,
                       "config key '%.*smise' must not be set in a repo whose projects have a "
                       "'tag_prefix': set the projects' 'mise' instead",
                       STR_FMT(table->key_prefix));
    }
  }
  return 0;
}

static int site_config_load_repos_one_projects(struct SurgeRepo* repo,
                                               const struct SiteConfigTable* table,
                                               struct Arena* perm,
                                               struct Arena scratch,
                                               struct Log* log) {
  struct SiteConfigTable* tables = NULL;
  ptrdiff_t table_count = 0;
  if (copy_tables(table, "project", false, &scratch, &tables, &table_count, log) != 0) {
    return -1;
  }
  struct SurgeProject* projects = arena_new(perm, table_count, struct SurgeProject);
  for (ptrdiff_t i = 0; i < table_count; i++) {
    if (site_config_load_repos_one_projects_one(&projects[i], &tables[i], perm, log) != 0) {
      return -1;
    }
  }
  for (ptrdiff_t i = 0; i < table_count; i++) {
    for (ptrdiff_t j = 0; j < i; j++) {
      if (str_eq_folded(projects[i].name, projects[j].name)) {
        return log_error(log,
                         "config keys '%.*sname' and '%.*sname' name projects that collide after "
                         "case folding: '%.*s' and '%.*s'",
                         STR_FMT(tables[j].key_prefix), STR_FMT(tables[i].key_prefix),
                         STR_FMT(projects[j].name), STR_FMT(projects[i].name));
      }
    }
    // A tag that starts with both prefixes would belong to two projects. Overlap is not transitive,
    // so every pair is checked.
    for (ptrdiff_t j = 0; j < i; j++) {
      const struct Str prefix_a = projects[j].tag_prefix;
      const struct Str prefix_b = projects[i].tag_prefix;
      if (prefix_a.len > 0 && prefix_b.len > 0 &&
          (str_startswith(prefix_a, prefix_b) || str_startswith(prefix_b, prefix_a))) {
        return log_error(log,
                         "config keys '%.*stag_prefix' and '%.*stag_prefix' overlap: '%.*s' and "
                         "'%.*s'",
                         STR_FMT(tables[j].key_prefix), STR_FMT(tables[i].key_prefix),
                         STR_FMT(prefix_a), STR_FMT(prefix_b));
      }
    }
  }
  repo->projects = projects;
  repo->project_count = table_count;
  return 0;
}

static int site_config_load_repos_one_projects_one(struct SurgeProject* project,
                                                   const struct SiteConfigTable* table,
                                                   struct Arena* perm,
                                                   struct Log* log) {
  struct Str path = {0};
  if (toml_require_known_keys(table->datum, project_keys,
                              sizeof(project_keys) / sizeof(project_keys[0]), S("config"),
                              table->key_prefix, (struct Str){0}, log) != 0 ||
      copy_name(table, PROJECT_NAMES_RESERVED,
                sizeof(PROJECT_NAMES_RESERVED) / sizeof(PROJECT_NAMES_RESERVED[0]), perm,
                &project->name, log) != 0 ||
      toml_copy_string(table->datum, table->key_prefix, "path", true, perm, &path, log) != 0) {
    return -1;
  }
  const struct Str trimmed = path_trim_trailing_slashes(path);
  if (path.len == 0) {
    return log_error(log, "config key '%.*spath' must not be empty", STR_FMT(table->key_prefix));
  }
  if (!is_repo_relative_path(trimmed)) {
    return log_error(log,
                     "config key '%.*spath' must be a relative path in the repo without an empty, "
                     "'.' or '..' segment: '%.*s'",
                     STR_FMT(table->key_prefix), STR_FMT(path));
  }
  project->path = trimmed;
  if (toml_copy_string(table->datum, table->key_prefix, "description", false, perm,
                       &project->description, log) != 0 ||
      toml_copy_string(table->datum, table->key_prefix, "tag_prefix", false, perm,
                       &project->tag_prefix, log) != 0) {
    return -1;
  }
  if (project->tag_prefix.data != NULL && project->tag_prefix.len == 0) {
    return log_error(log, "config key '%.*stag_prefix' must not be empty",
                     STR_FMT(table->key_prefix));
  }
  if (toml_copy_bool(table->datum, table->key_prefix, "mise", &project->is_mise, log) != 0) {
    return -1;
  }
  if (project->is_mise && project->tag_prefix.data == NULL) {
    return log_error(log, "config key '%.*smise' needs '%.*stag_prefix'",
                     STR_FMT(table->key_prefix), STR_FMT(table->key_prefix));
  }
  return 0;
}

static int copy_tables(const struct SiteConfigTable* table,
                       const char* key,
                       bool is_required,
                       struct Arena* arena,
                       struct SiteConfigTable** tables_out,
                       ptrdiff_t* table_count_out,
                       struct Log* log) {
  *tables_out = NULL;
  *table_count_out = 0;
  const struct Str key_prefix = table->key_prefix;
  const toml_datum_t value = toml_get(table->datum, key);
  if (value.type == TOML_UNKNOWN) {
    if (is_required) {
      return log_error(log, "missing required config key '%.*s%s'", STR_FMT(key_prefix), key);
    }
    return 0;
  }
  if (value.type != TOML_ARRAY) {
    return log_error(log, "config key '%.*s%s' must be an array of tables", STR_FMT(key_prefix),
                     key);
  }
  const ptrdiff_t count = value.u.arr.size;
  for (ptrdiff_t i = 0; i < count; i++) {
    if (value.u.arr.elem[i].type != TOML_TABLE) {
      return log_error(log, "config key '%.*s%s' must be an array of tables", STR_FMT(key_prefix),
                       key);
    }
  }
  if (count == 0 && is_required) {
    return log_error(log, "config key '%.*s%s' must not be empty", STR_FMT(key_prefix), key);
  }
  struct SiteConfigTable* tables = arena_new(arena, count, struct SiteConfigTable);
  for (ptrdiff_t i = 0; i < count; i++) {
    struct Buf prefix = {.arena = arena};
    buf_printf(&prefix, "%.*s%s[%td].", STR_FMT(key_prefix), key, i);
    tables[i] =
        (struct SiteConfigTable){.datum = value.u.arr.elem[i], .key_prefix = buf_str(&prefix)};
  }
  *tables_out = tables;
  *table_count_out = count;
  return 0;
}

static int copy_name(const struct SiteConfigTable* table,
                     const struct Str* reserved_names,
                     ptrdiff_t reserved_count,
                     struct Arena* arena,
                     struct Str* name_out,
                     struct Log* log) {
  struct Str name = {0};
  if (toml_copy_string(table->datum, table->key_prefix, "name", true, arena, &name, log) != 0) {
    return -1;
  }
  if (name.len == 0) {
    return log_error(log, "config key '%.*sname' must not be empty", STR_FMT(table->key_prefix));
  }
  if (name.len > SURGE_NAME_LEN_MAX) {
    return log_error(
        log, "config key '%.*sname' exceeds max name length (%d bytes) at %td bytes: '%.*s'",
        STR_FMT(table->key_prefix), SURGE_NAME_LEN_MAX, name.len, STR_FMT(name));
  }
  if (!is_valid_name(name)) {
    return log_error(log,
                     "config key '%.*sname' must use only letters, digits, '.', '_' and '-', not "
                     "start with '.' and not end in '.git': '%.*s'",
                     STR_FMT(table->key_prefix), STR_FMT(name));
  }
  for (ptrdiff_t i = 0; i < reserved_count; i++) {
    if (str_eq_folded(name, reserved_names[i])) {
      return log_error(log, "config key '%.*sname' must not be '%.*s' in any case: '%.*s'",
                       STR_FMT(table->key_prefix), STR_FMT(reserved_names[i]), STR_FMT(name));
    }
  }
  *name_out = name;
  return 0;
}

static bool is_valid_name(struct Str name) {
  if (name.len == 0 || name.data[0] == '.' || text_has_suffix_folded(name, S(".git"))) {
    return false;
  }
  for (ptrdiff_t i = 0; i < name.len; i++) {
    const unsigned char c = (unsigned char)name.data[i];
    if (!ascii_is_alphanumeric(c) && c != '.' && c != '_' && c != '-') {
      return false;
    }
  }
  return true;
}

static bool is_repo_relative_path(struct Str path) {
  // A leading `/` makes an empty first segment. Each segment but the last takes a `/`, so a path
  // has at most one segment more than it has bytes.
  struct StrCut cut = {.tail = path, .is_found = true};
  for (ptrdiff_t i = 0; cut.is_found && i <= path.len; i++) {
    cut = str_cut(cut.tail, '/');
    if (cut.head.len == 0 || str_eq(cut.head, S(".")) || str_eq(cut.head, S(".."))) {
      return false;
    }
  }
  return true;
}
