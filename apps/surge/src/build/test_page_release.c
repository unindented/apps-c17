#include <acutest.h>
#include <stdbool.h>
#include <stddef.h>

#include "build/page_release.h"
#include "build/template_value.h"
#include "core/arena.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "domain/semver.h"
#include "domain/site_config.h"
#include "testing/test_support.h"

/**
 * A repo `r` whose project `tool` has the releases `tool-v1.0.0-rc.1`, `tool-v0.10.0` and
 * `tool-v0.9.0`, listed newest first, and `tool-vnext`, which is not listed.
 */
struct ReleaseFixture {
  /** The repo's table. */
  struct SurgeRepo repo_config;

  /** The configuration, whose `base_url` is `https://git.example.org`. */
  struct SiteConfig site_config;

  /** The tags. */
  struct Ref tags[4];

  /** The commit of each tag, all zeroed. */
  struct CommitSummary tag_commits[4];

  /** The releases, in the project's order. */
  struct Release releases[4];

  /** Pointers to `releases`, as the project holds them. */
  const struct Release* release_pointers[4];

  /** The archives of `tool-v0.10.0`. */
  struct ReleaseAsset assets[3];

  /** The project. */
  struct ReleaseProject project;

  /** The repo. */
  struct RepoModel repo;
};

/**
 * @brief Fills the repo `fixture` describes.
 *
 * @param fixture Fixture to fill. Must not be `NULL`.
 */
static void release_fixture_init(struct ReleaseFixture* fixture) {
  *fixture = (struct ReleaseFixture){
      .repo_config = {.name = S("r")},
      .site_config = {.base_url = S("https://git.example.org")},
      .tags =
          {
              {.short_name = S("tool-v1.0.0-rc.1")},
              {.short_name = S("tool-v0.10.0")},
              {.short_name = S("tool-v0.9.0")},
              {.short_name = S("tool-vnext")},
          },
      .assets =
          {
              {.name = S("SHA256SUMS")},
              {.name = S("tool-0.10.0-aarch64-macos.tar.gz")},
              {.name = S("tool-0.10.0-x86_64-linux-musl.tar.gz")},
          },
  };
  const char* versions[] = {"1.0.0-rc.1", "0.10.0", "0.9.0", "next"};
  const char* dates[] = {"2026-03-01T00:00:00Z", "2026-02-01T00:00:00Z", "2026-01-01T00:00:00Z",
                         "2026-04-01T00:00:00Z"};
  for (ptrdiff_t i = 0; i < 4; i++) {
    fixture->releases[i] = (struct Release){
        .tag = &fixture->tags[i],
        .version = str_from_cstr(versions[i]),
        .is_semver = i < 3,
        .note = {.date = str_from_cstr(dates[i])},
    };
    // The first three versions parse, as the scan would have found.
    if (i < 3) {
      (void)semver_parse(fixture->releases[i].version, &fixture->releases[i].semver);
    }
    fixture->release_pointers[i] = &fixture->releases[i];
  }
  fixture->releases[1].assets = fixture->assets;
  fixture->releases[1].asset_count = 3;
  fixture->releases[1].has_sums = true;
  fixture->project = (struct ReleaseProject){
      .name = S("tool"),
      .tag_prefix = S("tool-v"),
      .releases = fixture->release_pointers,
      .release_count = 4,
      .version_count = 3,
      .is_mise = true,
  };
  fixture->repo = (struct RepoModel){
      .config = &fixture->repo_config,
      .refset = {.refs = fixture->tags, .count = 4},
      .ref_commits = fixture->tag_commits,
      .releases = fixture->releases,
      .release_count = 4,
      .release_projects = &fixture->project,
      .release_project_count = 1,
  };
}

// `versions.txt` lists the listed versions oldest first, so the newest is last, and `versions.json`
// the same versions in the same order with their dates.
static void test_versions_list_the_oldest_first(void) {
  struct ReleaseFixture fixture;
  release_fixture_init(&fixture);
  struct Arena arena = test_arena();
  TEST_CHECK(test_text_is(page_release_versions_text(&fixture.project, &arena),
                          "0.9.0\n0.10.0\n1.0.0-rc.1\n"));
  TEST_CHECK(
      test_text_is(page_release_versions_json(&fixture.project, &arena),
                   "{\"versions\":[{\"version\":\"0.9.0\",\"date\":\"2026-01-01T00:00:00Z\"},"
                   "{\"version\":\"0.10.0\",\"date\":\"2026-02-01T00:00:00Z\"},"
                   "{\"version\":\"1.0.0-rc.1\",\"date\":\"2026-03-01T00:00:00Z\"}]}\n"));
}

// The example names the archives of the newest release that is no pre-release, one platform per
// target in mise's order, and the release's `SHA256SUMS`.
static void test_mise_toml_names_the_archives_of_the_newest_release(void) {
  struct ReleaseFixture fixture;
  release_fixture_init(&fixture);
  struct Arena arena = test_arena();
  const char* download = "https://git.example.org/r/releases/download/tool-v{{ version }}";
  TEST_CHECK(test_text_is(
      page_release_mise_toml(&fixture.site_config, &fixture.repo, &fixture.project, &arena),
      "[tools.\"http:tool\"]\n"
      "version = \"latest\"\n"
      "version_list_url = \"https://git.example.org/r/releases/tool/versions.txt\"\n"
      "version_date_url = \"https://git.example.org/r/releases/tool/versions.json\"\n"
      "version_date_expr = 'map(fromJSON(body).versions, { [#.version, #.date] })'\n"
      "checksum_url = \"%s/SHA256SUMS\"\n"
      "strip_components = 1\n"
      "bin_path = \"bin\"\n"
      "\n"
      "[tools.\"http:tool\".platforms]\n"
      "linux-x64 = { url = \"%s/tool-{{ version }}-x86_64-linux-musl.tar.gz\" }\n"
      "macos-arm64 = { url = \"%s/tool-{{ version }}-aarch64-macos.tar.gz\" }\n",
      download, download, download));
}

// When every listed version is a pre-release, the example names the archives of the newest one.
static void test_mise_toml_falls_back_to_the_newest_pre_release(void) {
  struct ReleaseFixture fixture;
  release_fixture_init(&fixture);
  struct Arena arena = test_arena();
  fixture.tags[1].short_name = S("tool-v0.10.0-beta");
  fixture.releases[1].version = S("0.10.0-beta");
  TEST_ASSERT(semver_parse(fixture.releases[1].version, &fixture.releases[1].semver) == 0);
  fixture.project.version_count = 2;
  struct ReleaseAsset rc_asset = {.name = S("tool-1.0.0-rc.1-x86_64-linux-musl.tar.gz")};
  fixture.releases[0].assets = &rc_asset;
  fixture.releases[0].asset_count = 1;
  TEST_CHECK(test_text_is(
      page_release_mise_toml(&fixture.site_config, &fixture.repo, &fixture.project, &arena),
      "[tools.\"http:tool\"]\n"
      "version = \"latest\"\n"
      "version_list_url = \"https://git.example.org/r/releases/tool/versions.txt\"\n"
      "version_date_url = \"https://git.example.org/r/releases/tool/versions.json\"\n"
      "version_date_expr = 'map(fromJSON(body).versions, { [#.version, #.date] })'\n"
      "strip_components = 1\n"
      "bin_path = \"bin\"\n"
      "\n"
      "[tools.\"http:tool\".platforms]\n"
      "linux-x64 = { url = "
      "\"https://git.example.org/r/releases/download/tool-v{{ version }}/"
      "tool-{{ version }}-x86_64-linux-musl.tar.gz\" }\n"));
}

// The download links of the example percent-encode the part of the tag before the version.
static void test_mise_toml_encodes_the_tag_lead(void) {
  struct ReleaseFixture fixture;
  release_fixture_init(&fixture);
  struct Arena arena = test_arena();
  fixture.tags[1].short_name = S("my tool%v0.10.0");
  fixture.releases[1].has_sums = false;
  fixture.releases[1].assets = &fixture.assets[1];
  fixture.releases[1].asset_count = 1;
  TEST_CHECK(test_text_is(
      page_release_mise_toml(&fixture.site_config, &fixture.repo, &fixture.project, &arena),
      "[tools.\"http:tool\"]\n"
      "version = \"latest\"\n"
      "version_list_url = \"https://git.example.org/r/releases/tool/versions.txt\"\n"
      "version_date_url = \"https://git.example.org/r/releases/tool/versions.json\"\n"
      "version_date_expr = 'map(fromJSON(body).versions, { [#.version, #.date] })'\n"
      "strip_components = 1\n"
      "bin_path = \"bin\"\n"
      "\n"
      "[tools.\"http:tool\".platforms]\n"
      "macos-arm64 = { url = "
      "\"https://git.example.org/r/releases/download/my%%20tool%%25v{{ version }}/"
      "tool-{{ version }}-aarch64-macos.tar.gz\" }\n"));
}

// A release's page link and its assets' download links percent-encode the tag and each asset's
// name.
static void test_tag_value_encodes_the_tag_and_the_assets(void) {
  struct ReleaseFixture fixture;
  release_fixture_init(&fixture);
  struct Arena arena = test_arena();
  fixture.tags[1].short_name = S("tool v0.10.0%");
  fixture.assets[0].name = S("notes #1.txt");
  const struct TemplateValue* value = page_release_tag_value(
      &fixture.site_config, &fixture.repo, &fixture.releases[1], &arena, test_arena());
  const struct TemplateValue* release = template_value_find(value, S("release"));
  TEST_CHECK(test_text_is(template_value_find(release, S("url"))->text,
                          "../../../../r/releases/tag/tool%%20v0.10.0%%25/index.html"));
  const struct TemplateValue* assets = template_value_find(release, S("assets"));
  TEST_ASSERT(assets->item_count == 3);
  TEST_CHECK(
      test_text_is(template_value_find(assets->items[0], S("url"))->text,
                   "../../../../r/releases/download/tool%%20v0.10.0%%25/notes%%20%%231.txt"));
}

// A release page names its project in `project`, and a release of no project has `false` there, so
// a section over it is skipped.
static void test_tag_value_has_no_project_for_a_release_of_none(void) {
  struct ReleaseFixture fixture;
  release_fixture_init(&fixture);
  struct Arena arena = test_arena();
  const struct TemplateValue* with = page_release_tag_value(
      &fixture.site_config, &fixture.repo, &fixture.releases[1], &arena, test_arena());
  TEST_CHECK(template_value_find(with, S("project"))->kind == TEMPLATE_VALUE_MAP);
  fixture.releases[1].project_index = -1;
  const struct TemplateValue* without = page_release_tag_value(
      &fixture.site_config, &fixture.repo, &fixture.releases[1], &arena, test_arena());
  TEST_CHECK(template_value_find(without, S("project"))->kind == TEMPLATE_VALUE_FALSE);
}

// Only a project with `mise` has the example and releases that say so.
static void test_project_value_shows_mise_only_when_asked(void) {
  struct ReleaseFixture fixture;
  release_fixture_init(&fixture);
  struct Arena arena = test_arena();
  const struct TemplateValue* project =
      template_value_find(page_release_project_value(&fixture.site_config, &fixture.repo,
                                                     &fixture.project, &arena, test_arena()),
                          S("project"));
  TEST_CHECK(template_value_find(project, S("mise_toml"))->text.len > 0);
  const struct TemplateValue* release = template_value_find(project, S("releases"))->items[1];
  TEST_CHECK(template_value_is_truthy(template_value_find(release, S("is_mise"))));
  fixture.project.is_mise = false;
  project = template_value_find(page_release_project_value(&fixture.site_config, &fixture.repo,
                                                           &fixture.project, &arena, test_arena()),
                                S("project"));
  TEST_CHECK(template_value_find(project, S("mise_toml"))->text.len == 0);
  release = template_value_find(project, S("releases"))->items[1];
  TEST_CHECK(!template_value_is_truthy(template_value_find(release, S("is_mise"))));
}

// A release without `SHA256SUMS` has no `checksum_url`, and one without an archive mise can
// install, or a project with no listed version, has no example.
static void test_mise_toml_needs_an_archive(void) {
  struct ReleaseFixture fixture;
  release_fixture_init(&fixture);
  struct Arena arena = test_arena();
  fixture.releases[1].has_sums = false;
  fixture.releases[1].assets = &fixture.assets[1];
  fixture.releases[1].asset_count = 1;
  TEST_CHECK(test_text_is(
      page_release_mise_toml(&fixture.site_config, &fixture.repo, &fixture.project, &arena),
      "[tools.\"http:tool\"]\n"
      "version = \"latest\"\n"
      "version_list_url = \"https://git.example.org/r/releases/tool/versions.txt\"\n"
      "version_date_url = \"https://git.example.org/r/releases/tool/versions.json\"\n"
      "version_date_expr = 'map(fromJSON(body).versions, { [#.version, #.date] })'\n"
      "strip_components = 1\n"
      "bin_path = \"bin\"\n"
      "\n"
      "[tools.\"http:tool\".platforms]\n"
      "macos-arm64 = { url = "
      "\"https://git.example.org/r/releases/download/tool-v{{ version }}/"
      "tool-{{ version }}-aarch64-macos.tar.gz\" }\n"));
  fixture.releases[1].assets = fixture.assets;
  fixture.releases[1].asset_count = 1;
  TEST_CHECK(
      page_release_mise_toml(&fixture.site_config, &fixture.repo, &fixture.project, &arena).len ==
      0);
  fixture.project.version_count = 0;
  TEST_CHECK(
      page_release_mise_toml(&fixture.site_config, &fixture.repo, &fixture.project, &arena).len ==
      0);
}

TEST_LIST = {
    {"versions list the oldest first", test_versions_list_the_oldest_first},
    {"mise toml names the archives of the newest release",
     test_mise_toml_names_the_archives_of_the_newest_release},
    {"mise toml falls back to the newest pre-release",
     test_mise_toml_falls_back_to_the_newest_pre_release},
    {"mise toml encodes the tag lead", test_mise_toml_encodes_the_tag_lead},
    {"tag value encodes the tag and the assets", test_tag_value_encodes_the_tag_and_the_assets},
    {"tag value has no project for a release of none",
     test_tag_value_has_no_project_for_a_release_of_none},
    {"project value shows mise only when asked", test_project_value_shows_mise_only_when_asked},
    {"mise toml needs an archive", test_mise_toml_needs_an_archive},
    {NULL, NULL},
};
