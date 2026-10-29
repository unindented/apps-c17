#include "build/release_writer.h"

#include <stdbool.h>
#include <stddef.h>

#include "build/page_release.h"
#include "build/template_value.h"
#include "core/arena.h"
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

/**
 * @brief Writes or keeps one release output.
 *
 * @param inputs  What the outputs are made from. Must not be `NULL`.
 * @param output  The output. Must not be `NULL`.
 * @param writer  Writer of the build. Must not be `NULL`.
 * @param os      Host the output is inspected on. Must not be `NULL`.
 * @param scratch Arena for the page.
 * @param log     Log that receives the failure. Must not be `NULL`.
 * @return `0` when the output was written or kept, or `-1` otherwise.
 */
static int release_writer_write_one(const struct ReleaseWriterInputs* inputs,
                                    const struct ReleaseOutput* output,
                                    const struct OutputWriter* writer,
                                    struct Os* os,
                                    struct Arena scratch,
                                    struct Log* log) __attribute__((nonnull(1, 2, 3, 4, 6)));

/**
 * @brief Renders a release page into the output.
 *
 * @param inputs  What the outputs are made from. Must not be `NULL`.
 * @param output  The page. Must not be `NULL`.
 * @param writer  Writer of the build. Must not be `NULL`.
 * @param scratch Arena for the page's data tree and its render.
 * @param log     Log that receives the failure. Must not be `NULL`.
 * @return `0` when the page was written, or `-1` otherwise.
 */
static int release_writer_write_one_page(const struct ReleaseWriterInputs* inputs,
                                         const struct ReleaseOutput* output,
                                         const struct OutputWriter* writer,
                                         struct Arena scratch,
                                         struct Log* log) __attribute__((nonnull(1, 2, 3, 5)));

int release_writer_register(struct ManifestBuilder* builder,
                            const struct RepoModel* repo,
                            struct Arena scratch,
                            struct Log* log) {
  ptrdiff_t count = 0;
  const struct ReleaseOutput* outputs = list_outputs(repo, &scratch, &count);
  int rc = 0;
  for (ptrdiff_t i = 0; i < count; i++) {
    const struct ReleaseOutput* output = &outputs[i];
    // An asset's diagnostic names the file it is copied from, as a static file's does, and a
    // release's page its repo and tag, as a commit page's names its repo and commit.
    struct Arena label_scratch = scratch;
    struct Str label = repo->config->name;
    if (output->asset != NULL) {
      label = output->asset->source_path;
    } else if (output->release != NULL) {
      label = str_concat(str_concat(repo->config->name, S(":"), &label_scratch),
                         output->release->tag->short_name, &label_scratch);
    } else if (output->project != NULL) {
      label = str_concat(str_concat(repo->config->name, S(":"), &label_scratch),
                         output->project->name, &label_scratch);
    }
    if (manifest_builder_add(builder, output->relative_path, label, label_scratch, log) != 0) {
      rc = -1;
    }
  }
  return rc;
}

int release_writer_write(const struct ReleaseWriterInputs* inputs,
                         const struct OutputWriter* writer,
                         struct Os* os,
                         struct Arena scratch,
                         struct Log* log) {
  ptrdiff_t count = 0;
  const struct ReleaseOutput* outputs = list_outputs(inputs->repo, &scratch, &count);
  int rc = 0;
  for (ptrdiff_t i = 0; i < count; i++) {
    if (release_writer_write_one(inputs, &outputs[i], writer, os, scratch, log) != 0) {
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

static int release_writer_write_one(const struct ReleaseWriterInputs* inputs,
                                    const struct ReleaseOutput* output,
                                    const struct OutputWriter* writer,
                                    struct Os* os,
                                    struct Arena scratch,
                                    struct Log* log) {
  const struct Str file_path =
      path_join(inputs->site_config->output_dir, output->relative_path, &scratch);
  struct OsStat st;
  const bool is_present = os_stat(os, file_path, &st) == 0 && !st.is_dir;
  if (output->kind == RELEASE_OUTPUT_ASSET) {
    if (inputs->is_releases_fresh && is_present && st.len == output->asset->len) {
      return output_writer_keep(writer, file_path, log);
    }
    return output_writer_write(
        writer, file_path,
        (struct OsSource){.kind = OS_SOURCE_FILE, .file_path = output->asset->source_path},
        S("failed to copy release asset"), log);
  }
  const bool is_versions =
      output->kind == RELEASE_OUTPUT_VERSIONS_TEXT || output->kind == RELEASE_OUTPUT_VERSIONS_JSON;
  // A page also shows the repo's `repo` map, and a version list does not.
  if (inputs->is_templates_fresh && inputs->is_releases_fresh &&
      (is_versions || inputs->is_repo_map_fresh) && is_present) {
    return output_writer_keep(writer, file_path, log);
  }
  if (is_versions) {
    const struct Str text = output->kind == RELEASE_OUTPUT_VERSIONS_TEXT
                                ? page_release_versions_text(output->project, &scratch)
                                : page_release_versions_json(output->project, &scratch);
    return output_writer_write(writer, file_path, (struct OsSource){.bytes = text},
                               S("failed to write release versions"), log);
  }
  return release_writer_write_one_page(inputs, output, writer, scratch, log);
}

static int release_writer_write_one_page(const struct ReleaseWriterInputs* inputs,
                                         const struct ReleaseOutput* output,
                                         const struct OutputWriter* writer,
                                         struct Arena scratch,
                                         struct Log* log) {
  const struct SiteConfig* site_config = inputs->site_config;
  const struct RepoModel* repo = inputs->repo;
  // The data tree takes one half of the scratch, and the render the other.
  const char* region_end = scratch.limit != NULL ? scratch.limit : scratch.end;
  struct Arena value_arena = arena_split(&scratch, (region_end - scratch.beg) / 2);
  struct Arena value_scratch = scratch;
  const struct TemplateValue* value = NULL;
  struct Str template_name = {0};
  switch (output->kind) {
    case RELEASE_OUTPUT_INDEX:
      value = page_release_index_value(site_config, repo, &value_arena, value_scratch);
      template_name = PAGE_RELEASE_INDEX_TEMPLATE;
      break;
    case RELEASE_OUTPUT_PROJECT:
      value = page_release_project_value(site_config, repo, output->project, &value_arena,
                                         value_scratch);
      template_name = PAGE_RELEASE_PROJECT_TEMPLATE;
      break;
    default:
      value =
          page_release_tag_value(site_config, repo, output->release, &value_arena, value_scratch);
      template_name = PAGE_RELEASE_TAG_TEMPLATE;
      break;
  }
  const struct Str file_path =
      path_join(site_config->output_dir, output->relative_path, &value_arena);
  struct Arena page_arena = value_arena;
  struct Str html = {0};
  if (template_value_render(inputs->template_set, template_name, value, output->relative_path,
                            &page_arena, scratch, &html, log) != 0) {
    return -1;
  }
  return output_writer_write(writer, file_path, (struct OsSource){.bytes = html},
                             S("failed to write page"), log);
}
