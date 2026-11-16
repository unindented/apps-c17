#include <acutest.h>

#include "build/submodule_link.h"
#include "core/arena.h"
#include "core/str.h"
#include "domain/site_config.h"
#include "testing/test_support.h"

/** Hex id of the fixture submodule's commit. */
#define SUBMODULE_TEST_OID "4444444444444444444444444444444444444444"

/**
 * @brief Returns the commit link of a submodule at `url` of the repo `app` in a site at
 *        `https://git.example.org/code` with the repos `app` and `lib`, from a page whose way back
 *        to the output root is `../../`.
 *
 * @param url Terminated URL as `.gitmodules` writes it. Must not be `NULL`.
 * @return The link.
 */
static struct Str commit_url(const char* url) {
  static const struct SurgeRepo repos[] = {{.name = STR_INIT("app")}, {.name = STR_INIT("lib")}};
  const struct SiteConfig site_config = {
      .base_url = S("https://git.example.org/code"),
      .repos = repos,
      .repo_count = 2,
  };
  struct Arena arena = test_arena();
  return submodule_link_commit_url(str_from_cstr(url), S(SUBMODULE_TEST_OID), &site_config,
                                   S("app"), S("../../"), &arena);
}

/**
 * @brief Returns `url` as `submodule_link_shown_url` shows it.
 *
 * @param url Terminated URL. Must not be `NULL`.
 * @return The URL.
 */
static struct Str shown_url(const char* url) {
  struct Arena arena = test_arena();
  return submodule_link_shown_url(str_from_cstr(url), &arena);
}

// A URL with no credentials shows as written, and an `http(s)://` URL loses its whole userinfo,
// which can be a token.
static void test_shown_url_removes_credentials(void) {
  TEST_CHECK(test_text_is(shown_url("../lib.git"), "../lib.git"));
  TEST_CHECK(test_text_is(shown_url("https://ada:secret@github.com/a/b.git"),
                          "https://github.com/a/b.git"));
  TEST_CHECK(test_text_is(shown_url("HTTP://token@example.org/a.git"), "HTTP://example.org/a.git"));
  TEST_CHECK(test_text_is(shown_url("https://example.org/a@b.git"), "https://example.org/a@b.git"));
}

// Another scheme loses a `user:password@`, and keeps a user name alone, such as `git@`.
static void test_shown_url_keeps_a_user_name_outside_http(void) {
  TEST_CHECK(test_text_is(shown_url("ssh://git@example.org/a.git"), "ssh://git@example.org/a.git"));
  TEST_CHECK(test_text_is(shown_url("ssh://git:pw@example.org/a.git"), "ssh://example.org/a.git"));
  TEST_CHECK(test_text_is(shown_url("git@github.com:a/b.git"), "git@github.com:a/b.git"));
}

// A repo of the site, by its clone URL, credentials aside, or by a URL relative to the clone URL of
// the repo that holds the submodule, links its commit page.
static void test_commit_url_links_a_repo_of_the_site(void) {
  const char* expected = "../../lib/commit/" SUBMODULE_TEST_OID "/index.html";
  TEST_CHECK(test_text_is(commit_url("https://git.example.org/code/lib.git"), "%s", expected));
  TEST_CHECK(test_text_is(commit_url("https://ada@git.example.org/code/lib.git"), "%s", expected));
  TEST_CHECK(test_text_is(commit_url("../lib.git"), "%s", expected));
  TEST_CHECK(test_text_is(commit_url("./.././../code/lib.git"), "%s", expected));
  TEST_CHECK(
      test_text_is(commit_url("../app.git"), "../../app/commit/" SUBMODULE_TEST_OID "/index.html"));
}

// Each forge links its own commit path, with or without `.git` and a trailing `/`, as an
// `https://`, a `git@<host>:` or an `ssh://` URL, with the host in any case.
static void test_commit_url_links_each_forge(void) {
  const char* github = "https://github.com/mrdoob/three.js/tree/" SUBMODULE_TEST_OID;
  TEST_CHECK(test_text_is(commit_url("https://github.com/mrdoob/three.js.git"), "%s", github));
  TEST_CHECK(test_text_is(commit_url("https://GitHub.com/mrdoob/three.js/"), "%s", github));
  TEST_CHECK(test_text_is(commit_url("git@github.com:mrdoob/three.js.git"), "%s", github));
  TEST_CHECK(test_text_is(commit_url("ssh://git@github.com/mrdoob/three.js"), "%s", github));
  TEST_CHECK(test_text_is(commit_url("https://gitlab.com/group/sub/project"),
                          "https://gitlab.com/group/sub/project/-/tree/" SUBMODULE_TEST_OID));
  TEST_CHECK(test_text_is(commit_url("ssh://codeberg.org/ada/notes.git"),
                          "https://codeberg.org/ada/notes/src/commit/" SUBMODULE_TEST_OID));
}

// A URL of another host, of a forge with less than an owner and a name, a relative URL that names
// no repo of the site or leaves the host, and a forge over plain HTTP link nothing.
static void test_commit_url_links_nothing_else(void) {
  TEST_CHECK(commit_url("ssh://git@git.example.com/elsewhere.git").len == 0);
  TEST_CHECK(commit_url("https://example.org/a/b.git").len == 0);
  TEST_CHECK(commit_url("https://github.com/mrdoob").len == 0);
  TEST_CHECK(commit_url("https://github.com/a//b").len == 0);
  TEST_CHECK(commit_url("http://github.com/mrdoob/three.js").len == 0);
  TEST_CHECK(commit_url("git@github.com/mrdoob:three.js").len == 0);
  TEST_CHECK(commit_url("../missing.git").len == 0);
  TEST_CHECK(commit_url("../../../lib.git").len == 0);
  TEST_CHECK(commit_url("lib.git").len == 0);
  TEST_CHECK(commit_url("").len == 0);
}

TEST_LIST = {
    {"shown url removes credentials", test_shown_url_removes_credentials},
    {"shown url keeps a user name outside http", test_shown_url_keeps_a_user_name_outside_http},
    {"commit url links a repo of the site", test_commit_url_links_a_repo_of_the_site},
    {"commit url links each forge", test_commit_url_links_each_forge},
    {"commit url links nothing else", test_commit_url_links_nothing_else},
    {NULL, NULL},
};
