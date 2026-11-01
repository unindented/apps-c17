#ifndef SURGE_SITE_CONFIG_H
#define SURGE_SITE_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#include "core/arena.h"
#include "core/str.h"

struct Log;
struct Os;

/**
 * Path every command loads its configuration from when `-c` names none, relative to the working
 * directory. `cli_spec` gives it to the command line, which hands it to both commands, so it is
 * declared once here.
 */
extern const struct Str SITE_CONFIG_PATH_DEFAULT;

/**
 * Longest repo or project name, in bytes. A repo's name is also the `<name>.git` directory of its
 * clone export, so the name leaves room for that suffix in one path segment.
 */
enum { SURGE_NAME_LEN_MAX = 255 - 4 };

/**
 * Largest `limits.raw_len_max`: 1 GiB. Comparing a raw copy holds its blob and the file in the
 * output side by side in one job's scratch, which `cmd_build.c` sizes for this bound. Changing it
 * must update `test_load_bounds_integers`.
 */
#define SURGE_RAW_LEN_MAX_LIMIT ((ptrdiff_t)1024 * 1024 * 1024)

/** One `[[repo.project]]` table: a sub-project of a repo that the site index nests under it. */
struct SurgeProject {
  /** Required name, unique within its repo after ASCII case folding. */
  struct Str name;

  /** Required directory of the project on the default branch, relative to the repo's root. */
  struct Str path;

  /** Optional description, empty when the table has none. It has no fallback. */
  struct Str description;

  /**
   * Optional prefix of the tags that are this project's releases, such as `sosig-v`, or empty when
   * the project has no releases of its own. No two prefixes of one repo overlap.
   */
  struct Str tag_prefix;

  /** Whether its release pages show how to install it with mise. Only with a `tag_prefix`. */
  bool is_mise;
};

/** One `[[repo]]` table: a git repository that the site renders. */
struct SurgeRepo {
  /** Required name: the repo's directory in the output, and its row in the site index. */
  struct Str name;

  /** Required path of the repository, its work tree or its bare git directory. */
  struct Str path;

  /** Optional description, empty when the table has none or sets it empty. */
  struct Str description;

  /**
   * Whether the table sets `description`, even to an empty string, which shows no description.
   * Without it the scan reads the repository's `description` file.
   */
  bool has_description;

  /** Optional default branch, or empty to use the branch that `HEAD` names. */
  struct Str default_branch;

  /** Optional directory of release assets, one per tag, or empty when the repo has none. */
  struct Str releases_dir;

  /**
   * Whether the release pages of the repo's own release project show how to install it with mise.
   * Only with a `releases_dir` and no project with a `tag_prefix`, which has a `mise` of its own.
   */
  bool is_mise;

  /** The repo's projects, in config order, which may be `NULL` when `project_count` is `0`. */
  const struct SurgeProject* projects;

  /** Number of entries in `projects`. */
  ptrdiff_t project_count;
};

/** The `[limits]` table, each limit in bytes or in a count, as its name says. */
struct SurgeLimits {
  /** Largest blob read for a page. A larger blob gets a page without contents. */
  ptrdiff_t blob_len_max;

  /** Largest blob given a raw copy. A larger blob gets none. */
  ptrdiff_t raw_len_max;

  /** Most changed files a commit page's diff shows before it is truncated. */
  ptrdiff_t diff_file_count_max;

  /** Most diff lines a commit page shows before its diff is truncated. */
  ptrdiff_t diff_line_count_max;

  /** Most diff bytes a commit page shows before its diff is truncated. */
  ptrdiff_t diff_len_max;

  /**
   * Most bytes of images a commit page embeds, over every side of every changed image it shows. An
   * image side that would pass it is left out.
   */
  ptrdiff_t image_len_max;

  /** Largest clone pack that builds without a warning. A larger pack still builds. */
  ptrdiff_t pack_len_max;
};

/**
 * Configuration loaded from `surge.toml`. Every string and array it points at is either a default
 * literal or a copy in the arena `site_config_load` was given. Each directory and repo path is
 * resolved against the directory of the config file, so the build reads the paths as they are.
 */
struct SiteConfig {
  /** Required title of the site. */
  struct Str title;

  /** Required description of the site. */
  struct Str description;

  /** Required name of the site's author. */
  struct Str author;

  /** Required absolute base URL, without a trailing `/`, for feeds, clone URLs and mise. */
  struct Str base_url;

  /** Directory the site is written to. Defaults to `public`. */
  struct Str output_dir;

  /** Directory of the templates. Defaults to `templates`. */
  struct Str templates_dir;

  /**
   * Directory of the static files, which a build skips when it does not exist. Defaults to
   * `static`.
   */
  struct Str static_dir;

  /** Commits on one log page. Defaults to `100`. */
  ptrdiff_t log_page_size;

  /** Entries in one Atom feed. Defaults to `25`. */
  ptrdiff_t feed_count;

  /**
   * Whether the build writes `_headers`, which Cloudflare Pages and Netlify read, with the cache
   * headers of the clone exports. Defaults to `false`.
   */
  bool has_headers_file;

  /** The `[limits]` table. */
  struct SurgeLimits limits;

  /** The repos, in config order, which may be `NULL` when `repo_count` is `0`. */
  const struct SurgeRepo* repos;

  /** Number of entries in `repos`. */
  ptrdiff_t repo_count;
};

/**
 * @brief Initializes a config with the defaults of its optional keys.
 *
 * @param site_config Config to prepare. Must not be `NULL`.
 */
void site_config_init(struct SiteConfig* site_config) __attribute__((nonnull(1)));

/**
 * @brief Loads `config_path` into `site_config`.
 *
 * Reads and parses the file, rejects a key outside the schema at every level, validates the
 * required keys, the names, the limits and the paths, and copies the values into `perm` over the
 * defaults from `site_config_init`. It stops at the first error.
 *
 * @param site_config Initialized config that receives the loaded values. Must not be `NULL`.
 * @param config_path Path of the TOML file. Relative paths in the file resolve against its
 *                    directory.
 * @param os          Host the file is read from. Must not be `NULL`.
 * @param perm        Arena that owns the loaded values. Must not be `NULL`.
 * @param scratch     Arena for the file bytes and validation temporaries.
 * @param log         Log that receives the error. Must not be `NULL`.
 * @return `0` on success, or `-1` when the file cannot be read or parsed or a value fails
 *         validation.
 */
int site_config_load(struct SiteConfig* site_config,
                     struct Str config_path,
                     struct Os* os,
                     struct Arena* perm,
                     struct Arena scratch,
                     struct Log* log) __attribute__((nonnull(1, 3, 4, 6)));

/**
 * @brief Writes the resolved configuration to standard output as TOML.
 *
 * Every path is printed resolved, so the text loads back to the same values from the working
 * directory. An optional string that is empty is left out, except a repo's `description` set to
 * `""`, which shows none. The text is formatted whole in `scratch` and printed in one `os_print`
 * call.
 *
 * @param site_config Config to print, as after a successful load. Must not be `NULL`.
 * @param os          Host to print on. Must not be `NULL`.
 * @param scratch     Arena the text is formatted in.
 * @return `0` on success, or `-1` when the write fails.
 */
int site_config_print(const struct SiteConfig* site_config, struct Os* os, struct Arena scratch)
    __attribute__((nonnull(1, 2)));

#endif
