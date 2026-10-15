#include <acutest.h>

#include "core/str.h"
#include "core/url.h"

// An HTTP(S) URL with a host passes, whatever follows the host and in whatever case the scheme is
// spelled, because schemes are case-insensitive and the host is not judged beyond being present.
static void test_is_http_with_host_accepts_a_scheme_and_a_host(void) {
  static const char* const urls[] = {
      "http://example.com",     "https://example.com",
      "https://example.com/",   "https://example.com:8443/blog",
      "HTTPS://Example.COM",    "hTtP://x",
      "https://bücher.example",
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(urls) / sizeof(urls[0])); i++) {
    TEST_CHECK(url_is_http_with_host(str_from_cstr(urls[i])));
    TEST_MSG("url: %s", urls[i]);
  }
}

// A URL without an HTTP(S) scheme, or whose host is empty because a path, a query, a fragment or a
// port starts right after the scheme, fails.
static void test_is_http_with_host_rejects_other_schemes_and_empty_hosts(void) {
  static const char* const urls[] = {
      "",                    // no URL at all
      "example.com",         // no scheme
      "/relative",           // a path, not a URL
      "ftp://example.com",   // a scheme, but not one a browser follows from a feed
      "https:/example.com",  // one slash short of a scheme
      "http:",               // shorter than the scheme
      "https://",            // a scheme with no host
      "https:///path",       // the host ends before it starts
      "https://?draft=1",    // likewise, with a query
      "https://#top",        // likewise, with a fragment
      "https://:8080",       // a port with no host before it
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(urls) / sizeof(urls[0])); i++) {
    TEST_CHECK(!url_is_http_with_host(str_from_cstr(urls[i])));
    TEST_MSG("url: %s", urls[i]);
  }
}

// A URL whose bytes are all printable ASCII but `?` and `#`, or above 0x7F, has no byte that breaks
// a path joined after it.
static void test_has_path_breaking_byte_accepts_printable_and_utf8_bytes(void) {
  static const char* const urls[] = {
      "",
      "https://example.com",
      "https://example.com:8443/a/b/",
      "https://example.com/~user/a-b_c.d!$&'()*+,;=@%20",
      "https://bücher.example/straße",
      "https://example.com/\x80",
      "https://example.com/\xFF",
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(urls) / sizeof(urls[0])); i++) {
    TEST_CHECK(!url_has_path_breaking_byte(str_from_cstr(urls[i])));
    TEST_MSG("url: %s", urls[i]);
  }
}

// A query, a fragment, a space, any control byte and DEL break a joined path, at any position,
// including a `NUL`, which an ordinary byte of a `struct Str` can be.
static void test_has_path_breaking_byte_rejects_query_fragment_space_and_controls(void) {
  static const struct Str urls[] = {
      STR_INIT("https://example.com/x?y=1"), STR_INIT("https://example.com/#z"),
      STR_INIT("?https://example.com"),      STR_INIT("https://exa mple.com"),
      STR_INIT("https://example.com/\t"),    STR_INIT("https://example.com/\n"),
      STR_INIT("https://example.com/\x01"),  STR_INIT("https://example.com/\x1F"),
      STR_INIT("https://example.com/\x7F"),  STR_INIT("https://example.com/\0x"),
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(urls) / sizeof(urls[0])); i++) {
    TEST_CHECK(url_has_path_breaking_byte(urls[i]));
    TEST_MSG("case: %td", i);
  }
}

TEST_LIST = {
    {"is http with host accepts a scheme and a host",
     test_is_http_with_host_accepts_a_scheme_and_a_host},
    {"is http with host rejects other schemes and empty hosts",
     test_is_http_with_host_rejects_other_schemes_and_empty_hosts},
    {"has path breaking byte accepts printable and utf8 bytes",
     test_has_path_breaking_byte_accepts_printable_and_utf8_bytes},
    {"has path breaking byte rejects query fragment space and controls",
     test_has_path_breaking_byte_rejects_query_fragment_space_and_controls},
    {NULL, NULL},
};
