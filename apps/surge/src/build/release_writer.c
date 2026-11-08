#include "build/release_writer.h"

#include <stdbool.h>
#include <stddef.h>

#include "build/page_release.h"
#include "build/repo_scan.h"
#include "build/template_value.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/path.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "domain/site_config.h"
#include "os/os.h"
#include "output/manifest_builder.h"
#include "output/output_writer.h"

/** What one release output is. */
enum ReleaseOutputKind {
  /** The repo's releases page. */
  RELEASE_OUTPUT_INDEX,

  /** A project's page. */
  RELEASE_OUTPUT_PROJECT,

  /** A project's `versions.txt`. */
  RELEASE_OUTPUT_VERSIONS_TEXT,

  /** A project's `versions.json`. */
  RELEASE_OUTPUT_VERSIONS_JSON,

  /** A release's page. */
  RELEASE_OUTPUT_TAG,

  /** A release's asset. */
  RELEASE_OUTPUT_ASSET,
};

/** One release output of a repo. */
struct ReleaseOutput {
  /** What the output is. */
  enum ReleaseOutputKind kind;

  /** Output path relative to `output_dir`. */
  struct Str relative_path;

  /** The project of a project's page or version list, else `NULL`. */
  const struct ReleaseProject* project;

  /** The release of a release's page or asset, else `NULL`. */
  const struct Release* release;

  /** The asset, else `NULL`. */
  const struct ReleaseAsset* asset;
};

/**
 * @brief Lists the release outputs of a repo, in the order the manifest records them.
 *
 * @param repo      What the scan found for the repo. Must not be `NULL`.
 * @param arena     Arena that owns the list and its paths. Must not be `NULL`.
 * @param count_out Receives the number of outputs. Must not be `NULL`.
 * @return The outputs.
 */
static struct ReleaseOutput* list_outputs(const struct RepoModel* repo,
                                          struct Arena* arena,
                                          ptrdiff_t* count_out)
    __attribute__((nonnull(1, 2, 3), returns_nonnull));

/** What makes one release output, for `output_writer_produce`. */
struct ReleaseMake {
  /** What the outputs are made from. */
  const struct ReleaseWriterInputs* inputs;

  /** The output. */
  const struct ReleaseOutput* output;
};

/**
 * @brief Records one release output with what it declares.
 *
 * @param builder   Prepared manifest builder. Must not be `NULL`.
 * @param scan      What the scan found for the repo. Must not be `NULL`.
 * @param output    The output. Must not be `NULL`.
 * @param recipe_id Id of the page recipe.
 * @param scratch   Arena for the label and the attributes.
 * @param log       Log that receives the refusal. Must not be `NULL`.
 * @return `0` when the path was recorded, or `-1` otherwise.
 */
static int release_writer_register_one(struct ManifestBuilder* builder,
                                       const struct RepoScan* scan,
                                       const struct ReleaseOutput* output,
                                       struct Str recipe_id,
                                       struct Arena scratch,
                                       struct Log* log) __attribute__((nonnull(1, 2, 3, 6)));

/**
 * @brief Makes a page or a version list, as `OutputWriterMakeFn`.
 *
 * A page's data tree goes in `perm` before the page, and its temporaries in `scratch`.
 *
 * @param perm      Arena that owns the bytes. Must not be `NULL`.
 * @param scratch   Arena for the data tree's temporaries and the render.
 * @param bytes_out Receives the bytes. Must not be `NULL`.
 * @param log       Log that receives the failure. Must not be `NULL`.
 * @param userdata  The `struct ReleaseMake`.
 * @return `0` on success, or `-1` when the render fails.
 */
static int release_writer_write_make(struct Arena* perm,
                                     struct Arena scratch,
                                     struct Str* bytes_out,
                                     struct Log* log,
                                     void* userdata) __attribute__((nonnull(1, 3, 4, 5)));

int release_writer_register(struct ManifestBuilder* builder,
                            const struct RepoScan* scan,
                            struct Str recipe_id,
                            struct Arena scratch,
                            struct Log* log) {
  ptrdiff_t count = 0;
  const struct ReleaseOutput* outputs = list_outputs(&scan->model, &scratch, &count);
  int rc = 0;
  for (ptrdiff_t i = 0; i < count; i++) {
    if (release_writer_register_one(builder, scan, &outputs[i], recipe_id, scratch, log) != 0) {
      rc = -1;
    }
  }
  return rc;
}

int release_writer_write(const struct ReleaseWriterInputs* inputs,
                         const struct OutputWriter* writer,
                         struct Arena scratch,
                         struct Log* log) {
  ptrdiff_t count = 0;
  const struct ReleaseOutput* outputs = list_outputs(inputs->repo, &scratch, &count);
  int rc = 0;
  for (ptrdiff_t i = 0; i < count; i++) {
    struct Arena output_scratch = scratch;
    const struct Str file_path =
        path_join(inputs->site_config->output_dir, outputs[i].relative_path, &output_scratch);
    // The output takes one half of the scratch, and its temporaries, and then the read of the file
    // in the output, the other.
    const char* region_end =
        output_scratch.limit != NULL ? output_scratch.limit : output_scratch.end;
    struct Arena output_arena = arena_split(&output_scratch, (region_end - output_scratch.beg) / 2);
    struct ReleaseMake make = {.inputs = inputs, .output = &outputs[i]};
    const struct Str operation = outputs[i].kind == RELEASE_OUTPUT_ASSET
                                     ? S("failed to copy release asset")
                                 : outputs[i].kind == RELEASE_OUTPUT_VERSIONS_TEXT ||
                                         outputs[i].kind == RELEASE_OUTPUT_VERSIONS_JSON
                                     ? S("failed to write release versions")
                                     : S("failed to write page");
    if (output_writer_produce(writer, file_path, inputs->cache, release_writer_write_make, &make,
                              output_arena, output_scratch, operation, log) != 0) {
      rc = -1;
    }
  }
  return rc;
}

static struct ReleaseOutput* list_outputs(const struct RepoModel* repo,
                                          struct Arena* arena,
                                          ptrdiff_t* count_out) {
  *count_out = 0;
  if (repo->release_project_count == 0) {
    return arena_new(arena, 0, struct ReleaseOutput);
  }
  ptrdiff_t count = 1 + 3 * repo->release_project_count + repo->release_count;
  for (ptrdiff_t i = 0; i < repo->release_count; i++) {
    count += repo->releases[i].asset_count;
  }
  struct ReleaseOutput* outputs = arena_new(arena, count, struct ReleaseOutput);
  const struct Str repo_name = repo->config->name;
  ptrdiff_t index = 0;
  outputs[index++] = (struct ReleaseOutput){
      .kind = RELEASE_OUTPUT_INDEX,
      .relative_path = page_release_index_path(repo_name, arena),
  };
  for (ptrdiff_t i = 0; i < repo->release_project_count; i++) {
    const struct ReleaseProject* project = &repo->release_projects[i];
    const struct {
      enum ReleaseOutputKind kind;
      struct Str file_name;
    } files[] = {
        {RELEASE_OUTPUT_PROJECT, S("index.html")},
        {RELEASE_OUTPUT_VERSIONS_TEXT, PAGE_RELEASE_VERSIONS_TEXT},
        {RELEASE_OUTPUT_VERSIONS_JSON, PAGE_RELEASE_VERSIONS_JSON},
    };
    for (ptrdiff_t j = 0; j < 3; j++) {
      outputs[index++] = (struct ReleaseOutput){
          .kind = files[j].kind,
          .relative_path =
              page_release_project_path(repo_name, project->name, files[j].file_name, arena),
          .project = project,
      };
    }
  }
  for (ptrdiff_t i = 0; i < repo->release_count; i++) {
    const struct Release* release = &repo->releases[i];
    const struct Str tag_name = release->tag->short_name;
    outputs[index++] = (struct ReleaseOutput){
        .kind = RELEASE_OUTPUT_TAG,
        .relative_path = page_release_tag_path(repo_name, tag_name, arena),
        .release = release,
    };
    for (ptrdiff_t j = 0; j < release->asset_count; j++) {
      const struct ReleaseAsset* asset = &release->assets[j];
      outputs[index++] = (struct ReleaseOutput){
          .kind = RELEASE_OUTPUT_ASSET,
          .relative_path = page_release_asset_path(repo_name, tag_name, asset->name, arena),
          .release = release,
          .asset = asset,
      };
    }
  }
  *count_out = index;
  return outputs;
}

static int release_writer_register_one(struct ManifestBuilder* builder,
                                       const struct RepoScan* scan,
                                       const struct ReleaseOutput* output,
                                       struct Str recipe_id,
                                       struct Arena scratch,
                                       struct Log* log) {
  const struct RepoModel* repo = &scan->model;
  if (output->asset != NULL) {
    // An asset's diagnostic names the file it is copied from, as a static file's does.
    const struct OsStat source_stat = {
        .len = output->asset->len,
        .mtime_s = output->asset->mtime_s,
        .mtime_ns = output->asset->mtime_ns,
    };
    struct Str source_name = {0};
    (void)path_is_below(output->asset->source_path, repo->config->releases_dir, &source_name);
    return manifest_builder_add_copy(builder, output->relative_path, output->asset->source_path,
                                     source_name, &source_stat, scratch, log);
  }
  // A release's page names its repo and tag, as a commit page's names its repo and commit.
  struct Str label = repo->config->name;
  if (output->release != NULL) {
    label = str_concat(str_concat(repo->config->name, S(":"), &scratch),
                       output->release->tag->short_name, &scratch);
  } else if (output->project != NULL) {
    label = str_concat(str_concat(repo->config->name, S(":"), &scratch), output->project->name,
                       &scratch);
  }
  // A page also shows the repo's `repo` map, and a version list does not.
  struct Buf attributes = {.arena = &scratch};
  buf_printf(&attributes, "recipe=%.*s", STR_FMT(recipe_id));
  if (output->kind != RELEASE_OUTPUT_VERSIONS_TEXT &&
      output->kind != RELEASE_OUTPUT_VERSIONS_JSON) {
    buf_printf(&attributes, " map=%.*s", STR_FMT(scan->repo_map_id));
  }
  buf_printf(&attributes, " releases=%.*s", STR_FMT(scan->releases_id));
  const struct Str text = buf_str(&attributes);
  return manifest_builder_add_output(builder, output->relative_path, label, text, scratch, log);
}

static int release_writer_write_make(struct Arena* perm,
                                     struct Arena scratch,
                                     struct Str* bytes_out,
                                     struct Log* log,
                                     void* userdata) {
  const struct ReleaseMake* make = userdata;
  const struct SiteConfig* site_config = make->inputs->site_config;
  const struct RepoModel* repo = make->inputs->repo;
  const struct ReleaseOutput* output = make->output;
  const struct TemplateValue* value = NULL;
  struct Str template_name = {0};
  switch (output->kind) {
    case RELEASE_OUTPUT_VERSIONS_TEXT:
      *bytes_out = page_release_versions_text(output->project, perm);
      return 0;
    case RELEASE_OUTPUT_VERSIONS_JSON:
      *bytes_out = page_release_versions_json(output->project, perm);
      return 0;
    case RELEASE_OUTPUT_INDEX:
      value = page_release_index_value(site_config, repo, perm, scratch);
      template_name = site_config->templates.release_list;
      break;
    case RELEASE_OUTPUT_PROJECT:
      value = page_release_project_value(site_config, repo, output->project, perm, scratch);
      template_name = site_config->templates.release_project;
      break;
    default:
      value = page_release_tag_value(site_config, repo, output->release, perm, scratch);
      template_name = site_config->templates.release;
      break;
  }
  // The page grows in `perm` after its data tree.
  return template_value_render(make->inputs->template_set, template_name, value,
                               output->relative_path, perm, scratch, bytes_out, log);
}
