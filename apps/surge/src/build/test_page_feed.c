#include <acutest.h>
#include <stddef.h>

#include "build/page_context.h"
#include "build/page_feed.h"
#include "build/template_value.h"
#include "core/arena.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "domain/site_config.h"
#include "testing/test_support.h"

/** A repo `r` with the branch `feature/a b` and the tags `a`, `b`, `c` and `d`. */
struct FeedFixture {
  /** The repo's table. */
  struct SurgeRepo repo_config;

  /** The configuration, whose `base_url` is `https://git.example.org` and `feed_count` is 3. */
  struct SiteConfig site_config;

  /** The branch, then the tags. */
  struct Ref refs[5];

  /** The commit of each ref. */
  struct CommitSummary ref_commits[5];

  /** The note of each ref. */
  struct TagNote ref_notes[5];

  /** The branch's tree. */
  struct BranchTree tree;

  /** The repo. */
  struct RepoModel repo;
};

/**
 * @brief Fills the repo `fixture` describes. `a` and `c` share the newest time, `b` is older, and
 *        `d` has none.
 *
 * @param fixture Fixture to fill. Must not be `NULL`.
 */
static void feed_fixture_init(struct FeedFixture* fixture) {
  *fixture = (struct FeedFixture){
      .repo_config = {.name = S("r")},
      .site_config = {.base_url = S("https://git.example.org"), .feed_count = 3},
      .refs =
          {
              {.short_name = S("feature/a b"), .kind = REF_KIND_BRANCH},
              {.short_name = S("a"), .kind = REF_KIND_TAG},
              {.short_name = S("b"), .kind = REF_KIND_TAG},
              {.short_name = S("c"), .kind = REF_KIND_TAG},
              {.short_name = S("d"), .kind = REF_KIND_TAG},
          },
      .ref_notes =
          {
              [1] = {.date = S("2026-03-01T00:00:00Z"), .time = 300},
              [2] = {.date = S("2026-02-01T00:00:00Z"), .time = 200},
              [3] = {.date = S("2026-03-01T00:00:00Z"),
                     .time = 300,
                     .message = S("bell\x07 & <tag>")},
          },
  };
  fixture->ref_commits[3] = (struct CommitSummary){.oid = S("0123"), .author = S("Ada")};
  fixture->tree = (struct BranchTree){.branch = &fixture->refs[0]};
  fixture->repo = (struct RepoModel){
      .config = &fixture->repo_config,
      .refset = {.refs = fixture->refs, .count = 5},
      .ref_commits = fixture->ref_commits,
      .ref_notes = fixture->ref_notes,
  };
}

/**
 * @brief Returns a text field of the `index`th entry of a feed's data tree.
 *
 * @param value The feed's data tree. Must not be `NULL`.
 * @param index Index of the entry.
 * @param key   Name of the field.
 * @return The field's text.
 */
static struct Str entry_text(const struct TemplateValue* value, ptrdiff_t index, struct Str key) {
  const struct TemplateValue* entries = template_value_find(value, S("feed_entries"));
  return template_value_find(entries->items[index], key)->text;
}

// A commit feed's own URL, its page's and the branch's `commits_feed_url` encode each segment of
// the branch's name, its output path keeps the name as it is, and a feed without entries is dated
// at the Unix epoch.
static void test_commits_encode_the_branch(void) {
  struct FeedFixture fixture;
  feed_fixture_init(&fixture);
  struct Arena arena = test_arena();
  const struct TemplateValue* value =
      page_feed_commits_value(&fixture.site_config, &fixture.repo, &fixture.tree, NULL, 0, &arena);
  const struct TemplateValue* feed = template_value_find(value, S("feed"));
  TEST_CHECK(test_text_is(template_value_find(feed, S("url"))->text,
                          "https://git.example.org/r/commits/feature/a%%20b.atom"));
  TEST_CHECK(test_text_is(template_value_find(feed, S("html_url"))->text,
                          "https://git.example.org/r/commits/feature/a%%20b/index.html"));
  TEST_CHECK(test_text_is(template_value_find(feed, S("modified"))->text, "1970-01-01T00:00:00Z"));
  TEST_CHECK(test_text_is(page_feed_commits_path(S("r"), S("feature/a b"), &arena),
                          "r/commits/feature/a b.atom"));
  const struct TemplateValue* ref = template_value_find(value, S("ref"));
  TEST_CHECK(test_text_is(template_value_find(ref, S("commits_feed_url"))->text,
                          "https://git.example.org/r/commits/feature/a%%20b.atom"));
}

// A repo links its tag feed always and its release feed only with a release project, relative to a
// page and absolute in a feed, as every link of the repo map is.
static void test_repo_links_its_feeds(void) {
  struct FeedFixture fixture;
  feed_fixture_init(&fixture);
  struct Arena arena = test_arena();
  const struct TemplateValue* page =
      page_context_repo(&fixture.site_config, &fixture.repo, S("../"), &arena);
  TEST_CHECK(test_text_is(template_value_find(page, S("tags_feed_url"))->text, "../r/tags.atom"));
  TEST_CHECK(test_text_is(template_value_find(page, S("releases_feed_url"))->text, ""));
  fixture.repo.release_project_count = 1;
  const struct TemplateValue* feed = template_value_find(
      page_feed_tags_value(&fixture.site_config, &fixture.repo, &arena), S("repo"));
  TEST_CHECK(test_text_is(template_value_find(feed, S("tags_feed_url"))->text,
                          "https://git.example.org/r/tags.atom"));
  TEST_CHECK(test_text_is(template_value_find(feed, S("releases_feed_url"))->text,
                          "https://git.example.org/r/releases.atom"));
}

// A tag feed lists the newest `feed_count` tags, a tie in byte order of name, and an entry's text
// is the tag's message with its bytes as they are, which the render repairs and escapes.
static void test_tags_list_the_newest_first(void) {
  struct FeedFixture fixture;
  feed_fixture_init(&fixture);
  struct Arena arena = test_arena();
  const struct TemplateValue* value =
      page_feed_tags_value(&fixture.site_config, &fixture.repo, &arena);
  const struct TemplateValue* entries = template_value_find(value, S("feed_entries"));
  TEST_ASSERT(entries->item_count == 3);
  TEST_CHECK(test_text_is(entry_text(value, 0, S("title")), "a"));
  TEST_CHECK(test_text_is(entry_text(value, 1, S("title")), "c"));
  TEST_CHECK(test_text_is(entry_text(value, 2, S("title")), "b"));
  TEST_CHECK(test_text_is(entry_text(value, 1, S("content")), "bell\x07 & <tag>"));
  TEST_CHECK(test_text_is(entry_text(value, 1, S("author")), "Ada"));
  const struct TemplateValue* feed = template_value_find(value, S("feed"));
  TEST_CHECK(test_text_is(template_value_find(feed, S("modified"))->text, "2026-03-01T00:00:00Z"));
  // The tie holds whichever of the two the refset lists first.
  const struct Ref first = fixture.refs[1];
  const struct TagNote first_note = fixture.ref_notes[1];
  fixture.refs[1] = fixture.refs[3];
  fixture.ref_notes[1] = fixture.ref_notes[3];
  fixture.refs[3] = first;
  fixture.ref_notes[3] = first_note;
  value = page_feed_tags_value(&fixture.site_config, &fixture.repo, &arena);
  TEST_CHECK(test_text_is(entry_text(value, 0, S("title")), "a"));
  TEST_CHECK(test_text_is(entry_text(value, 1, S("title")), "c"));
}

// A tag links its commit's page when it has no release, else the tags page, and its id is the tags
// page with its name as the fragment, all below `base_url`.
static void test_tags_link_absolute_pages(void) {
  struct FeedFixture fixture;
  feed_fixture_init(&fixture);
  struct Arena arena = test_arena();
  const struct TemplateValue* value =
      page_feed_tags_value(&fixture.site_config, &fixture.repo, &arena);
  TEST_CHECK(test_text_is(entry_text(value, 1, S("url")),
                          "https://git.example.org/r/commit/0123/index.html"));
  TEST_CHECK(
      test_text_is(entry_text(value, 0, S("url")), "https://git.example.org/r/tags/index.html"));
  TEST_CHECK(
      test_text_is(entry_text(value, 0, S("id")), "https://git.example.org/r/tags/index.html#a"));
}

// A tag's id and the link to its release percent-encode its name.
static void test_tags_encode_the_name(void) {
  struct FeedFixture fixture;
  feed_fixture_init(&fixture);
  fixture.refs[1].short_name = S("a b%");
  struct Release release = {.tag = &fixture.refs[1]};
  fixture.repo.releases = &release;
  fixture.repo.release_count = 1;
  struct Arena arena = test_arena();
  const struct TemplateValue* value =
      page_feed_tags_value(&fixture.site_config, &fixture.repo, &arena);
  TEST_CHECK(test_text_is(entry_text(value, 0, S("title")), "a b%%"));
  TEST_CHECK(test_text_is(entry_text(value, 0, S("id")),
                          "https://git.example.org/r/tags/index.html#a%%20b%%25"));
  TEST_CHECK(test_text_is(entry_text(value, 0, S("url")),
                          "https://git.example.org/r/releases/tag/a%%20b%%25/index.html"));
}

TEST_LIST = {
    {"commits encode the branch", test_commits_encode_the_branch},
    {"repo links its feeds", test_repo_links_its_feeds},
    {"tags list the newest first", test_tags_list_the_newest_first},
    {"tags link absolute pages", test_tags_link_absolute_pages},
    {"tags encode the name", test_tags_encode_the_name},
    {NULL, NULL},
};
