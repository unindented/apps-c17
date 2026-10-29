#include <acutest.h>
#include <stddef.h>

#include "core/str.h"
#include "domain/semver.h"
#include "testing/test_support.h"

/**
 * @brief Parses a version that must parse.
 *
 * @param text Terminated text of the version. Must not be `NULL`.
 * @return The version.
 */
static struct SemVer parsed(const char* text) {
  struct SemVer version = {0};
  TEST_CHECK(semver_parse(str_from_cstr(text), &version) == 0);
  return version;
}

// A version's numbers and its pre-release identifiers are read as they are written.
static void test_parse_reads_numbers_and_pre_release(void) {
  const struct SemVer release = parsed("1.20.3");
  TEST_CHECK(release.major == 1 && release.minor == 20 && release.patch == 3);
  TEST_CHECK(release.pre.len == 0);
  const struct SemVer pre = parsed("0.10.0-rc.1");
  TEST_CHECK(pre.minor == 10 && test_text_is(pre.pre, "rc.1"));
}

// Precedence follows Semantic Versioning's own example list, each before the next.
static void test_compare_follows_semver_precedence(void) {
  const char* ordered[] = {"1.0.0-alpha",  "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
                           "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1",       "1.0.0",
                           "1.0.1",        "1.2.0",         "1.10.0",           "2.0.0"};
  const ptrdiff_t count = (ptrdiff_t)(sizeof(ordered) / sizeof(ordered[0]));
  for (ptrdiff_t i = 1; i < count; i++) {
    const struct SemVer a = parsed(ordered[i - 1]);
    const struct SemVer b = parsed(ordered[i]);
    TEST_CHECK(semver_compare(&a, &b) < 0);
    TEST_MSG("%s before %s", ordered[i - 1], ordered[i]);
    TEST_CHECK(semver_compare(&b, &a) > 0);
    TEST_MSG("%s after %s", ordered[i], ordered[i - 1]);
  }
  const struct SemVer same = parsed("1.0.0-rc.1");
  const struct SemVer again = parsed("1.0.0-rc.1");
  TEST_CHECK(semver_compare(&same, &again) == 0);
}

// Anything else is not a version: too few numbers, leading zeros, empty or bad identifiers, build
// metadata, a leading `v`.
static void test_parse_rejects_other_text(void) {
  const char* texts[] = {"1.0",        "01.0.0",      "1.0.0-",    "1.0.0-01",
                         "1.0.0-a..b", "1.0.0+build", "v1.0.0",    "1.0.x",
                         "",           "1.0.0.0",     "1.0.0-a_b", "99999999999999999999.0.0"};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(texts) / sizeof(texts[0])); i++) {
    struct SemVer version = {0};
    TEST_CHECK(semver_parse(str_from_cstr(texts[i]), &version) == -1);
    TEST_MSG("accepted '%s'", texts[i]);
  }
}

TEST_LIST = {
    {"parse reads numbers and pre release", test_parse_reads_numbers_and_pre_release},
    {"compare follows semver precedence", test_compare_follows_semver_precedence},
    {"parse rejects other text", test_parse_rejects_other_text},
    {NULL, NULL},
};
