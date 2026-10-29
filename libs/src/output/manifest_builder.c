#include "output/manifest_builder.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/path.h"
#include "core/path_list.h"
#include "core/str.h"
#include "os/os.h"
#include "output/manifest.h"
#include "output/output_path.h"

/**
 * @brief Records the output `relative_path` below `output_dir`, made as `output` says, unless it is
 *        refused, as `manifest_builder_add` describes.
 *
 * @param builder       Prepared builder. Must not be `NULL`.
 * @param relative_path Output path relative to `output_dir`.
 * @param source_label  What produces the output, which a diagnostic names.
 * @param output        How the output is made. Must not be `NULL`.
 * @param scratch       Arena for the joined path.
 * @param log           Log that receives the failure. Must not be `NULL`.
 * @return `0` when the path was recorded, or `-1` when it was refused.
 */
static int add_output(struct ManifestBuilder* builder,
                      struct Str relative_path,
                      struct Str source_label,
                      const struct ManifestOutput* output,
                      struct Arena scratch,
                      struct Log* log) __attribute__((nonnull(1, 4, 6)));

/**
 * @brief Returns the attributes of a copy of the file `source_name` whose stat is `source_stat`:
 *        its name, size and modification time, or empty when the stat failed.
 *
 * @param source_name Path of the source relative to its root.
 * @param source_stat What `os_stat` reported for the source. Must not be `NULL`.
 * @param arena       Arena that owns the text. Must not be `NULL`.
 * @return The attributes.
 */
static struct Str copy_attributes(struct Str source_name,
                                  const struct OsStat* source_stat,
                                  struct Arena* arena) __attribute__((nonnull(2, 3)));

/**
 * @brief Returns the root whose identity is `identity`.
 *
 * @param roots      Roots to search. Must not be `NULL` unless `root_count` is `0`.
 * @param root_count Number of entries in `roots`.
 * @param identity   Identity to look for. Must not be `NULL`.
 * @return The index of the root, or `-1` when none has that identity.
 */
static ptrdiff_t find_root(const struct ManifestRoot* roots,
                           ptrdiff_t root_count,
                           const struct OsIdentity* identity) __attribute__((nonnull(3)));

/**
 * @brief Returns the label of the root that holds `output_dir` or one of its physical ancestors.
 *
 * @param output_dir Output directory, which need not exist yet.
 * @param roots      Roots with their identities. Must not be `NULL` unless `root_count` is `0`.
 * @param root_count Number of entries in `roots`.
 * @param os         Host the directories are on. Must not be `NULL`.
 * @param scratch    Arena for the ancestor paths.
 * @return The root's label, or empty when no root holds `output_dir`.
 */
static struct Str find_root_holding_output_dir(struct Str output_dir,
                                               const struct ManifestRoot* roots,
                                               ptrdiff_t root_count,
                                               struct Os* os,
                                               struct Arena scratch) __attribute__((nonnull(4)));

/**
 * @brief Returns the nearest existing ancestor of `dir_path`, resolving `..` in the missing part.
 *
 * @param dir_path Directory, which need not exist.
 * @param os       Host the directories are on. Must not be `NULL`.
 * @param arena    Arena that owns the path. Must not be `NULL`.
 * @return The ancestor, at worst `.` or `/`.
 */
static struct Str find_nearest_existing_ancestor(struct Str dir_path,
                                                 struct Os* os,
                                                 struct Arena* arena)
    __attribute__((nonnull(2, 3)));

/**
 * @brief Reports whether `dir_path` or one of its physical ancestors is the output directory.
 *
 * @param dir_path        Directory to check.
 * @param output_identity Identity of the output directory. Must not be `NULL`.
 * @param os              Host the directories are on. Must not be `NULL`.
 * @param scratch         Arena for the ancestor paths.
 * @return `true` when the output directory holds `dir_path`.
 */
static bool is_held_by_output_dir(struct Str dir_path,
                                  const struct OsIdentity* output_identity,
                                  struct Os* os,
                                  struct Arena scratch) __attribute__((nonnull(2, 3)));

/**
 * @brief Returns the root at or below which `output_path` would land, checking each existing
 *        directory between `output_dir` and the path.
 *
 * @param builder     Prepared builder. Must not be `NULL`.
 * @param output_path Output path, joined onto `output_dir`.
 * @param dir_out     Receives the existing directory through which the path reaches the root, the
 *                    path itself when it is the root's. Must not be `NULL`.
 * @return The index of the root, or `-1` when no root holds the path.
 */
static ptrdiff_t find_root_holding_output_path(const struct ManifestBuilder* builder,
                                               struct Str output_path,
                                               struct Str* dir_out) __attribute__((nonnull(1, 3)));

/**
 * @brief Reports whether an output inside a root shares the cause of one reported already: it
 *        reaches the root through the same directory, or one below it, and its source is the same
 *        as the reported one's, or below it, as a page's directory is below its parent's.
 *
 * @param builder      Builder holding the reports. Must not be `NULL`.
 * @param dir_path     Existing directory through which the output reaches the root.
 * @param source_label What produces the output.
 * @return `true` when a report covers the output.
 */
static bool is_reported(const struct ManifestBuilder* builder,
                        struct Str dir_path,
                        struct Str source_label) __attribute__((nonnull(1)));

/**
 * @brief Records that an output inside a root was reported.
 *
 * @param builder      Builder that keeps the report. Must not be `NULL`.
 * @param dir_path     Existing directory through which the output reaches the root.
 * @param source_label What produced the output.
 */
static void add_report(struct ManifestBuilder* builder,
                       struct Str dir_path,
                       struct Str source_label) __attribute__((nonnull(1)));

/**
 * @brief Reads the identity of `file_path`, following symlinks.
 *
 * @param file_path    Path to identify.
 * @param os           Host the path is on. Must not be `NULL`.
 * @param identity_out Receives the identity on success. Must not be `NULL`.
 * @return `0` on success, or `-1` when the path has no identity.
 */
static int identify(struct Str file_path, struct Os* os, struct OsIdentity* identity_out)
    __attribute__((nonnull(2, 3)));

int manifest_builder_check_output_dir(struct Str output_dir,
                                      const struct ManifestRoot* roots,
                                      ptrdiff_t root_count,
                                      struct Str config_path,
                                      struct Os* os,
                                      struct Arena scratch,
                                      struct Log* log) {
  struct ManifestRoot* present = arena_new(&scratch, root_count, struct ManifestRoot);
  for (ptrdiff_t i = 0; i < root_count; i++) {
    present[i] = roots[i];
    present[i].is_present = identify(roots[i].dir_path, os, &present[i].identity) == 0;
  }
  const struct Str root_label =
      find_root_holding_output_dir(output_dir, present, root_count, os, scratch);
  if (root_label.len > 0) {
    return log_error(log, "output directory would write inside %.*s: '%.*s'", STR_FMT(root_label),
                     STR_FMT(output_dir));
  }
  struct OsIdentity output_identity;
  if (identify(output_dir, os, &output_identity) != 0) {
    return 0;
  }
  for (ptrdiff_t i = 0; i < root_count; i++) {
    if (present[i].is_present &&
        is_held_by_output_dir(present[i].dir_path, &output_identity, os, scratch)) {
      return log_error(log, "output directory would hold %.*s: '%.*s'", STR_FMT(present[i].label),
                       STR_FMT(output_dir));
    }
  }
  // A config in the working directory has no directory of its own, so it is `.` here.
  struct Str config_dir = path_file_dir(config_path);
  if (config_dir.len == 0) {
    config_dir = S(".");
  }
  if (is_held_by_output_dir(config_dir, &output_identity, os, scratch)) {
    return log_error(log, "output directory would hold the config: '%.*s'", STR_FMT(output_dir));
  }
  return 0;
}

void manifest_builder_init(struct ManifestBuilder* builder,
                           struct Manifest* manifest,
                           struct Str output_dir,
                           struct ManifestRoot* roots,
                           ptrdiff_t root_count,
                           const struct PathList* input_paths,
                           struct Os* os,
                           struct Arena* perm) {
  *builder = (struct ManifestBuilder){
      .manifest = manifest,
      .output_dir = output_dir,
      .roots = roots,
      .root_count = root_count,
      .input_items = arena_new(perm, input_paths->count, struct OsIdentity),
      .os = os,
      .perm = perm,
  };
  for (ptrdiff_t i = 0; i < root_count; i++) {
    roots[i].is_present = identify(roots[i].dir_path, os, &roots[i].identity) == 0;
  }
  // A path with no identity names no file yet, so there is nothing of it to overwrite.
  for (ptrdiff_t i = 0; i < input_paths->count; i++) {
    struct OsIdentity identity;
    if (identify(input_paths->items[i], os, &identity) == 0) {
      builder->input_items[builder->input_count++] = identity;
    }
  }
  if (builder->input_count > 0) {
    qsort(builder->input_items, (size_t)builder->input_count, sizeof(*builder->input_items),
          os_identity_compare);
  }
}

int manifest_builder_add(struct ManifestBuilder* builder,
                         struct Str relative_path,
                         struct Str source_label,
                         struct Arena scratch,
                         struct Log* log) {
  return add_output(builder, relative_path, source_label, &(struct ManifestOutput){0}, scratch,
                    log);
}

int manifest_builder_add_output(struct ManifestBuilder* builder,
                                struct Str relative_path,
                                struct Str source_label,
                                struct Str attributes,
                                struct Arena scratch,
                                struct Log* log) {
  return add_output(builder, relative_path, source_label,
                    &(struct ManifestOutput){.attributes = attributes}, scratch, log);
}

int manifest_builder_add_copy(struct ManifestBuilder* builder,
                              struct Str relative_path,
                              struct Str source_path,
                              struct Str source_name,
                              const struct OsStat* source_stat,
                              struct Arena scratch,
                              struct Log* log) {
  const struct ManifestOutput output = {
      .attributes = copy_attributes(source_name, source_stat, &scratch),
      .copy_source_path = source_path,
      .copy_source_len = source_stat->len,
  };
  return add_output(builder, relative_path, source_path, &output, scratch, log);
}

int manifest_builder_add_copies(struct ManifestBuilder* builder,
                                const struct PathList* paths,
                                const struct OsStat* stats,
                                struct Str root_dir,
                                const bool* is_skipped,
                                struct Arena scratch,
                                struct Log* log) {
  int rc = 0;
  for (ptrdiff_t i = 0; i < paths->count; i++) {
    if (is_skipped != NULL && is_skipped[i]) {
      continue;
    }
    struct Str relative = {0};
    (void)path_is_below(paths->items[i], root_dir, &relative);
    // A copy's path below the output is its source's path below the root, which also names it.
    if (manifest_builder_add_copy(builder, relative, paths->items[i], relative, &stats[i], scratch,
                                  log) != 0) {
      rc = -1;
    }
  }
  return rc;
}

int manifest_builder_add_template_list(struct ManifestBuilder* builder,
                                       struct Str list_key,
                                       const struct Str* names,
                                       ptrdiff_t count,
                                       struct Arena scratch,
                                       struct Log* log) {
  int rc = 0;
  for (ptrdiff_t i = 0; i < count; i++) {
    struct Arena entry_scratch = scratch;
    struct Buf label = {.arena = &entry_scratch};
    buf_printf(&label, "%.*s[%td]", STR_FMT(list_key), i);
    if (manifest_builder_add(builder, names[i], buf_str(&label), entry_scratch, log) != 0) {
      rc = -1;
    }
  }
  return rc;
}

int manifest_builder_finish(const struct ManifestBuilder* builder, struct Log* log) {
  int rc = 0;
  struct ManifestPrefixCollision collision = {0};
  while (manifest_find_prefix_collision(builder->manifest, &collision)) {
    rc = log_error(log, "output path for '%.*s' nests under output path for '%.*s': '%.*s'",
                   STR_FMT(collision.descendant_label), STR_FMT(collision.ancestor_label),
                   STR_FMT(collision.descendant_path));
  }
  return rc;
}

static int add_output(struct ManifestBuilder* builder,
                      struct Str relative_path,
                      struct Str source_label,
                      const struct ManifestOutput* output,
                      struct Arena scratch,
                      struct Log* log) {
  if (output_path_check_limits(relative_path, source_label, log) != 0) {
    return -1;
  }
  const struct Str output_path = path_join(builder->output_dir, relative_path, &scratch);
  // The root check runs first, because it is the broader rule: it rejects an output below an input
  // root whether or not a file already sits there.
  struct Str root_dir = {0};
  const ptrdiff_t root_index = find_root_holding_output_path(builder, output_path, &root_dir);
  if (root_index >= 0) {
    // An output that reaches the root through a reported directory, from a reported source or one
    // below it, lands there for the same reason, so it fails without a line of its own. Any other
    // output is reported, because it may land there for a reason of its own.
    if (is_reported(builder, root_dir, source_label)) {
      return -1;
    }
    add_report(builder, root_dir, source_label);
    return log_error(log, "output path would write inside %.*s for '%.*s': '%.*s'",
                     STR_FMT(builder->roots[root_index].label), STR_FMT(source_label),
                     STR_FMT(output_path));
  }
  struct OsIdentity identity;
  if (identify(output_path, builder->os, &identity) == 0 && builder->input_count > 0 &&
      bsearch(&identity, builder->input_items, (size_t)builder->input_count,
              sizeof(*builder->input_items), os_identity_compare) != NULL) {
    return log_error(log, "output path would overwrite build input for '%.*s': '%.*s'",
                     STR_FMT(source_label), STR_FMT(output_path));
  }
  struct Str source_label_existing = {0};
  if (manifest_add_output(builder->manifest, output_path, source_label, output, builder->perm,
                          &source_label_existing) != 0) {
    return log_error(log, "duplicate output path for '%.*s' and '%.*s': '%.*s'",
                     STR_FMT(source_label_existing), STR_FMT(source_label), STR_FMT(output_path));
  }
  return 0;
}

static struct Str copy_attributes(struct Str source_name,
                                  const struct OsStat* source_stat,
                                  struct Arena* arena) {
  if (source_stat->len < 0) {
    return (struct Str){0};
  }
  struct Buf attributes = {.arena = arena};
  buf_printf(&attributes, "src=%.*s len=%td mtime=%" PRId64 ".%09" PRId64, STR_FMT(source_name),
             source_stat->len, source_stat->mtime_s, source_stat->mtime_ns);
  return buf_str(&attributes);
}

static ptrdiff_t find_root(const struct ManifestRoot* roots,
                           ptrdiff_t root_count,
                           const struct OsIdentity* identity) {
  for (ptrdiff_t i = 0; i < root_count; i++) {
    if (roots[i].is_present && os_identity_compare(&roots[i].identity, identity) == 0) {
      return i;
    }
  }
  return -1;
}

static struct Str find_root_holding_output_dir(struct Str output_dir,
                                               const struct ManifestRoot* roots,
                                               ptrdiff_t root_count,
                                               struct Os* os,
                                               struct Arena scratch) {
  struct Str dir_path = find_nearest_existing_ancestor(output_dir, os, &scratch);
  struct OsIdentity identity;
  if (identify(dir_path, os, &identity) != 0) {
    return (struct Str){0};
  }
  for (;;) {
    const ptrdiff_t root_index = find_root(roots, root_count, &identity);
    const struct Str label = root_index >= 0 ? roots[root_index].label : (struct Str){0};
    if (label.len > 0) {
      return label;
    }
    dir_path = path_join(dir_path, S(".."), &scratch);
    struct OsIdentity parent_identity;
    if (identify(dir_path, os, &parent_identity) != 0 ||
        os_identity_compare(&parent_identity, &identity) == 0) {
      return (struct Str){0};
    }
    identity = parent_identity;
  }
}

static struct Str find_nearest_existing_ancestor(struct Str dir_path,
                                                 struct Os* os,
                                                 struct Arena* arena) {
  struct Str ancestor_path = str_startswith(dir_path, S("/")) ? S("/") : S(".");
  ptrdiff_t missing_count = 0;
  struct StrCut cut = {.tail = dir_path, .is_found = true};
  while (cut.is_found) {
    cut = str_cut(cut.tail, '/');
    const struct Str component = cut.head;
    if (component.len == 0 || str_eq(component, S("."))) {
      continue;
    }
    if (missing_count > 0) {
      missing_count = str_eq(component, S("..")) ? missing_count - 1 : missing_count + 1;
      continue;
    }
    const struct Str candidate_path = path_join(ancestor_path, component, arena);
    struct OsIdentity identity;
    if (identify(candidate_path, os, &identity) == 0) {
      ancestor_path = candidate_path;
    } else {
      missing_count = 1;
    }
  }
  return ancestor_path;
}

static bool is_held_by_output_dir(struct Str dir_path,
                                  const struct OsIdentity* output_identity,
                                  struct Os* os,
                                  struct Arena scratch) {
  struct OsIdentity identity;
  if (identify(dir_path, os, &identity) != 0) {
    return false;
  }
  for (;;) {
    if (os_identity_compare(&identity, output_identity) == 0) {
      return true;
    }
    dir_path = path_join(dir_path, S(".."), &scratch);
    struct OsIdentity parent_identity;
    if (identify(dir_path, os, &parent_identity) != 0 ||
        os_identity_compare(&parent_identity, &identity) == 0) {
      return false;
    }
    identity = parent_identity;
  }
}

static ptrdiff_t find_root_holding_output_path(const struct ManifestBuilder* builder,
                                               struct Str output_path,
                                               struct Str* dir_out) {
  // Each directory below `output_dir` is a prefix of the path that ends before a `/`, and the last
  // pass checks the path itself.
  for (ptrdiff_t i = builder->output_dir.len + 1;; i++) {
    const bool is_end = i >= output_path.len;
    if (!is_end && output_path.data[i] != '/') {
      continue;
    }
    struct OsIdentity identity;
    if (identify(str_head(output_path, is_end ? output_path.len : i), builder->os, &identity) !=
        0) {
      return -1;
    }
    const ptrdiff_t root_index = find_root(builder->roots, builder->root_count, &identity);
    if (root_index >= 0 || is_end) {
      *dir_out = str_head(output_path, is_end ? output_path.len : i);
      return root_index;
    }
  }
}

static bool is_reported(const struct ManifestBuilder* builder,
                        struct Str dir_path,
                        struct Str source_label) {
  for (ptrdiff_t i = 0; i < builder->report_count; i++) {
    const struct ManifestRootReport* report = &builder->reports[i];
    struct Str relative;
    if ((str_eq(dir_path, report->dir_path) ||
         path_is_below(dir_path, report->dir_path, &relative)) &&
        (str_eq(source_label, report->source_label) ||
         path_is_below(source_label, report->source_label, &relative))) {
      return true;
    }
  }
  return false;
}

static void add_report(struct ManifestBuilder* builder,
                       struct Str dir_path,
                       struct Str source_label) {
  if (builder->report_count == builder->report_cap) {
    const ptrdiff_t cap = builder->report_cap > 0 ? 2 * builder->report_cap : 8;
    struct ManifestRootReport* reports = arena_new(builder->perm, cap, struct ManifestRootReport);
    for (ptrdiff_t i = 0; i < builder->report_count; i++) {
      reports[i] = builder->reports[i];
    }
    builder->reports = reports;
    builder->report_cap = cap;
  }
  builder->reports[builder->report_count++] = (struct ManifestRootReport){
      .dir_path = str_clone(dir_path, builder->perm),
      .source_label = str_clone(source_label, builder->perm),
  };
}

static int identify(struct Str file_path, struct Os* os, struct OsIdentity* identity_out) {
  struct OsStat st;
  if (os_stat(os, file_path, &st) != 0) {
    return -1;
  }
  *identity_out = st.identity;
  return 0;
}
