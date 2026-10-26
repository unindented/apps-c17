#include <acutest.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "build/template_context.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/str.h"
#include "domain/album.h"
#include "domain/fram_version.h"
#include "domain/media_item.h"
#include "domain/site_config.h"
#include "os/os_memory.h"
#include "templates/template_set.h"
#include "testing/test_support.h"

/**
 * Failure of an unescaped interpolation, which a test follows with the `(in '<template>')` of the
 * template being rendered.
 */
#define UNESCAPED_FAILURE \
  "unescaped Mustache interpolation is supported only for a key that ends in '_html'"

/** Template directory every test renders from, which each test writes its templates below. */
#define TEMPLATES_DIR "templates"

/**
 * @brief Initializes shared gallery metadata for template tests.
 *
 * The assigned fields are string literals.
 *
 * @param site_config Configuration to initialize. Must not be `NULL`.
 */
static void init_test_site_config(struct SiteConfig* site_config) {
  site_config_init(site_config);
  site_config->title = S("Gallery");
  site_config->description = S("Description");
  site_config->author = S("Ada");
}

/**
 * @brief Loads a set whose one page template is `template_name`, renders it through
 *        `template_context_render` for the output of the same name, and hands back the HTML as a C
 *        string.
 *
 * This is a deliberate test-only bridge back to C strings. Every assertion below inspects rendered
 * text with C string functions such as `strcmp` and `strstr`, so the helper hands it back
 * terminated, copied after the HTML in `perm`. Rendered output holds no `NUL` byte, so the copy
 * loses nothing.
 *
 * @param os            Host that holds the templates below `TEMPLATES_DIR`. Must not be `NULL`.
 * @param template_name Terminated template name.
 * @param context       Template context. Must not be `NULL`.
 * @param perm          Arena that owns the set, the HTML and its terminated copy. Must not be
 *                      `NULL`.
 * @param html_out      Receives the terminated HTML on success, or `NULL` when the test asserts
 *                      only the result.
 * @param log           Log that receives the failure, or `NULL` when the test asserts only the
 *                      result.
 * @return `0` on success, or `-1` when the load or the render failed.
 */
static int render(struct Os* os,
                  const char* template_name,
                  const struct TemplateContext* context,
                  struct Arena* perm,
                  char** html_out,
                  struct Log* log) {
  struct Log discarded = {0};
  if (log == NULL) {
    discarded = test_log();
    log = &discarded;
  }
  const struct Str name = str_from_cstr(template_name);
  struct TemplateSet* set = NULL;
  if (template_set_load(S(TEMPLATES_DIR), &name, NULL, 1, os, perm, test_arena(), &set, log) != 0) {
    return -1;
  }
  struct Str html = {0};
  const int rc = template_context_render(set, name, context, name, perm, test_arena(), &html, log);
  template_set_release(set);
  if (rc != 0) {
    return -1;
  }
  if (html_out != NULL) {
    *html_out = str_cstr(html, perm);
  }
  return 0;
}

/**
 * @brief Reports whether a log holds exactly one render failure, of the output `output_name`.
 *
 * A render attributes its failure to the output it was for, so the expected line is the failure
 * followed by `(while rendering '<output>')`.
 *
 * @param log         Log the render wrote. Must not be `NULL`.
 * @param output_name Terminated name of the output the render was for. Must not be `NULL`.
 * @param failure     Terminated failure the log must hold, without the attribution. Must not be
 *                    `NULL`.
 * @return `true` when the log holds that one failure.
 */
static bool is_log_one_failure(const struct Log* log,
                               const char* output_name,
                               const char* failure) {
  return test_log_is(log, "%s (while rendering '%s')\n", failure, output_name) &&
         TEST_CHECK(log->errors == 1);
}

// Album, breadcrumb, repeated-media, boolean, escaping, and contextual URL nodes resolve together.
static void test_renders_gallery_tree_with_page_relative_urls(void) {
  struct Arena perm = test_arena();
  struct Os* os = os_memory();
  os_memory_write(
      os, TEMPLATES_DIR "/album.html",
      "{{site.title}}|{{album.title}}|{{#breadcrumbs}}[{{title}}={{url}}]{{/breadcrumbs}}|"
      "{{#media}}{{title}}={{s.url}}/{{s.width}}x{{s.height}},"
      "{{m.url}}/{{m.width}}x{{m.height}},{{url}}{{#is_image}}I{{/is_image}};"
      "{{/media}}");

  struct SiteConfig site_config;
  site_config_init(&site_config);
  site_config.title = S("A & B");
  site_config.description = S("Description");
  site_config.author = S("Ada");
  site_config.templates_dir = S(TEMPLATES_DIR);

  struct Album root = {.source_dir = S(""),
                       .title = S("Root"),
                       .url_path = S("index.html"),
                       .path_to_root = S(""),
                       .depth = 0};
  struct MediaDerivative derivatives[] = {
      {.name = S("s"), .url = S("_fram/r/s/x.jpg"), .width_px = 120, .height_px = 120},
      {.name = S("m"), .url = S("_fram/r/m/x.jpg"), .width_px = 480, .height_px = 320},
  };
  struct MediaItem media = {.title = S("x<y.jpg"),
                            .kind = MEDIA_KIND_IMAGE,
                            .derivatives = derivatives,
                            .derivative_count = 2,
                            .original_url = S("_fram/originals/x.jpg")};
  struct MediaItem* media_items[] = {&media};
  struct Album child = {.source_dir = S("trip"),
                        .title = S("Trip"),
                        .url_path = S("trip/index.html"),
                        .path_to_root = S("../"),
                        .media = media_items,
                        .media_count = 1,
                        .parent = &root,
                        .depth = 1};
  const struct TemplateContext context = {
      .site_config = &site_config, .album_current = &child, .is_album_page = true};
  char* rendered_html = NULL;
  TEST_ASSERT(render(os, "album.html", &context, &perm, &rendered_html, NULL) == 0);
  TEST_CHECK(strcmp(rendered_html,
                    "A &amp; B|Trip|[Root=../index.html][Trip=]|"
                    "x&lt;y.jpg=../_fram/r/s/x.jpg/120x120,"
                    "../_fram/r/m/x.jpg/480x320,"
                    "../_fram/originals/x.jpgI;") == 0);
}

// Every `gallery` field resolves to its configured value, escaped like any other scalar.
static void test_renders_site_fields(void) {
  struct Arena perm = test_arena();
  struct Os* os = os_memory();
  os_memory_write(os, TEMPLATES_DIR "/album.html",
                  "{{site.title}}|{{site.description}}|{{site.author}}|"
                  "{{site.base_url}}");

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  site_config.description = S("Photos <& videos>");
  site_config.base_url = S("https://example.com/gallery");
  struct Album root = {.title = S("Root"), .url_path = S("index.html"), .path_to_root = S("")};
  const struct TemplateContext context = {.site_config = &site_config, .album_current = &root};
  char* rendered_html = NULL;
  TEST_ASSERT(render(os, "album.html", &context, &perm, &rendered_html, NULL) == 0);
  TEST_CHECK(strcmp(rendered_html,
                    "Gallery|Photos &lt;&amp; videos&gt;|Ada|https://example.com/gallery") == 0);
}

// An album, a media item and a derivative each give `absolute_url`, their path below `base_url`,
// beside the `url` relative to the page, and an empty one when the gallery sets no `base_url`.
static void test_renders_absolute_urls_below_base_url(void) {
  struct Arena perm = test_arena();
  struct Os* os = os_memory();
  os_memory_write(os, TEMPLATES_DIR "/album.html",
                  "{{album.absolute_url}}|{{#media}}{{absolute_url}}|{{s.absolute_url}}{{/media}}|"
                  "{{^album.absolute_url}}local{{/album.absolute_url}}");
  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct MediaDerivative derivatives[] = {{.name = S("s"), .url = S("_fram/r/s/x.webp")}};
  struct MediaItem media = {.kind = MEDIA_KIND_IMAGE,
                            .derivatives = derivatives,
                            .derivative_count = 1,
                            .original_url = S("_fram/originals/x.jpg")};
  struct MediaItem* media_items[] = {&media};
  struct Album trip = {.title = S("Trip"),
                       .url_path = S("trip/index.html"),
                       .path_to_root = S("../"),
                       .media = media_items,
                       .media_count = 1,
                       .depth = 1};
  const struct TemplateContext context = {.site_config = &site_config, .album_current = &trip};
  char* rendered_html = NULL;
  TEST_ASSERT(render(os, "album.html", &context, &perm, &rendered_html, NULL) == 0);
  TEST_CHECK(strcmp(rendered_html, "|||local") == 0);
  TEST_MSG("rendered: '%s'", rendered_html);
  site_config.base_url = S("https://example.com/photos");
  TEST_ASSERT(render(os, "album.html", &context, &perm, &rendered_html, NULL) == 0);
  TEST_CHECK(strcmp(rendered_html,
                    "https://example.com/photos/trip/index.html|"
                    "https://example.com/photos/_fram/originals/x.jpg|"
                    "https://example.com/photos/_fram/r/s/x.webp|") == 0);
  TEST_MSG("rendered: '%s'", rendered_html);
}

// Sub-album links and inherited covers are materialized relative to the page being rendered.
static void test_renders_sub_album_cover(void) {
  struct Arena perm = test_arena();
  struct Os* os = os_memory();
  os_memory_write(os, TEMPLATES_DIR "/album.html",
                  "{{#sub_albums}}{{title}}={{url}}={{cover.s.url}}={{cover.s.width}}x"
                  "{{cover.s.height}}={{media_count_total}};"
                  "{{/sub_albums}}");
  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct MediaDerivative cover_derivatives[] = {
      {.name = S("s"), .url = S("_fram/r/s/trip/x.jpg"), .width_px = 120, .height_px = 120},
  };
  struct MediaItem cover = {.derivatives = cover_derivatives, .derivative_count = 1};
  struct Album child = {.title = S("Trip"),
                        .url_path = S("trip/index.html"),
                        .cover = &cover,
                        .media_count_total = 3,
                        .depth = 1};
  struct Album* children[] = {&child};
  struct Album root = {.title = S("Root"),
                       .url_path = S("index.html"),
                       .path_to_root = S(""),
                       .sub_albums = children,
                       .sub_album_count = 1};
  child.parent = &root;
  const struct TemplateContext context = {.site_config = &site_config, .album_current = &root};
  char* rendered_html = NULL;
  TEST_ASSERT(render(os, "album.html", &context, &perm, &rendered_html, NULL) == 0);
  TEST_CHECK(strcmp(rendered_html, "Trip=trip/index.html=_fram/r/s/trip/x.jpg=120x120=3;") == 0);
}

// `path_to_root` is the prefix of the page being rendered in every context: inside `sub_albums`,
// `breadcrumbs` and `parent`, whose albums have prefixes of their own, it still names the page's,
// because only the root answers it.
static void test_path_to_root_is_the_page_prefix_in_every_context(void) {
  struct Arena perm = test_arena();
  struct Os* os = os_memory();
  os_memory_write(os, TEMPLATES_DIR "/album.html",
                  "{{path_to_root}}|"
                  "{{#sub_albums}}[{{path_to_root}}]{{/sub_albums}}|"
                  "{{#breadcrumbs}}[{{path_to_root}}]{{/breadcrumbs}}|"
                  "{{#parent}}[{{path_to_root}}]{{/parent}}");
  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct Album root = {.title = S("Root"), .url_path = S("index.html"), .path_to_root = S("")};
  struct Album trip = {.title = S("Trip"),
                       .url_path = S("trip/index.html"),
                       .path_to_root = S("../"),
                       .parent = &root,
                       .depth = 1};
  struct Album dawn = {.title = S("Dawn"),
                       .url_path = S("trip/day/dawn/index.html"),
                       .path_to_root = S("../../../"),
                       .depth = 3};
  struct Album* children[] = {&dawn};
  struct Album day = {.title = S("Day"),
                      .url_path = S("trip/day/index.html"),
                      .path_to_root = S("../../"),
                      .sub_albums = children,
                      .sub_album_count = 1,
                      .parent = &trip,
                      .depth = 2};
  dawn.parent = &day;
  const struct TemplateContext context = {.site_config = &site_config, .album_current = &day};
  char* rendered_html = NULL;
  TEST_ASSERT(render(os, "album.html", &context, &perm, &rendered_html, NULL) == 0);
  TEST_CHECK(strcmp(rendered_html, "../../|[../../]|[../../][../../][../../]|[../../]") == 0);
  TEST_MSG("rendered: '%s'", rendered_html);
}

// On an album's page, the trail's last crumb is that album: only it has `is_last`, and its `url` is
// empty, so the page does not link itself. An album of `sub_albums` is no crumb, so it has no
// `is_last`, and keeps its `url`.
static void test_breadcrumbs_mark_the_current_album_last(void) {
  struct Arena perm = test_arena();
  struct Os* os = os_memory();
  os_memory_write(os, TEMPLATES_DIR "/album.html",
                  "{{#breadcrumbs}}[{{title}}:{{url}}:{{#is_last}}last{{/is_last}}]"
                  "{{/breadcrumbs}}|{{#sub_albums}}[{{title}}:{{url}}:"
                  "{{#is_last}}last{{/is_last}}]{{/sub_albums}}");
  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct Album root = {.title = S("Root"), .url_path = S("index.html")};
  struct Album dawn = {.title = S("Dawn"), .url_path = S("trip/day/dawn/index.html"), .depth = 3};
  struct Album* children[] = {&dawn};
  struct Album trip = {
      .title = S("Trip"), .url_path = S("trip/index.html"), .parent = &root, .depth = 1};
  struct Album day = {.title = S("Day"),
                      .url_path = S("trip/day/index.html"),
                      .path_to_root = S("../../"),
                      .sub_albums = children,
                      .sub_album_count = 1,
                      .parent = &trip,
                      .depth = 2};
  dawn.parent = &day;
  const struct TemplateContext context = {
      .site_config = &site_config, .album_current = &day, .is_album_page = true};
  // An aggregate renders against the root album without being its page, so its one crumb is not
  // last and keeps its link.
  const struct TemplateContext aggregate = {.site_config = &site_config, .album_current = &root};
  char* rendered_html = NULL;
  TEST_ASSERT(render(os, "album.html", &context, &perm, &rendered_html, NULL) == 0);
  TEST_CHECK(strcmp(rendered_html,
                    "[Root:../../index.html:][Trip:../../trip/index.html:][Day::last]|"
                    "[Dawn:../../trip/day/dawn/index.html:]") == 0);
  TEST_MSG("rendered: '%s'", rendered_html);

  TEST_ASSERT(render(os, "album.html", &aggregate, &perm, &rendered_html, NULL) == 0);
  TEST_CHECK(strcmp(rendered_html, "[Root:index.html:]|") == 0);
  TEST_MSG("rendered: '%s'", rendered_html);
}

// `generator` names the tool and its version, at the root and inside a section.
static void test_renders_generator(void) {
  struct Arena perm = test_arena();
  struct Os* os = os_memory();
  os_memory_write(os, TEMPLATES_DIR "/album.html",
                  "{{generator}}|{{#media}}{{generator}}{{/media}}");

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct MediaItem item = {0};
  struct MediaItem* media[] = {&item};
  struct Album root = {.title = S("Root"),
                       .url_path = S("index.html"),
                       .path_to_root = S(""),
                       .media = media,
                       .media_count = 1};
  const struct TemplateContext context = {.site_config = &site_config, .album_current = &root};
  char* rendered_html = NULL;
  const int render_rc = render(os, "album.html", &context, &perm, &rendered_html, NULL);
  const struct Str version = fram_version_string();
  char expected[64];
  const int expected_len = snprintf(expected, sizeof(expected), "fram %.*s|fram %.*s",
                                    STR_FMT(version), STR_FMT(version));
  TEST_ASSERT(render_rc == 0);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(rendered_html, expected) == 0);
  TEST_MSG("rendered: '%s'", rendered_html);
}

// An unknown variable renders as empty text per the Mustache spec, at the root and in a subtree.
static void test_unknown_variable_renders_empty(void) {
  struct Arena perm = test_arena();
  struct Os* os = os_memory();
  os_memory_write(os, TEMPLATES_DIR "/album.html", "[{{typo}}|{{site.typo}}]\n");

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct Album root = {.title = S("Root"), .url_path = S("index.html"), .path_to_root = S("")};
  const struct TemplateContext context = {.site_config = &site_config, .album_current = &root};
  char* rendered_html = NULL;
  TEST_ASSERT(render(os, "album.html", &context, &perm, &rendered_html, NULL) == 0);
  TEST_CHECK(strcmp(rendered_html, "[|]\n") == 0);
}

// An inverted section renders only when the list is empty.
static void test_inverted_section_renders_when_list_empty(void) {
  struct Arena perm = test_arena();
  struct Os* os = os_memory();
  os_memory_write(os, TEMPLATES_DIR "/album.html", "{{#media}}x{{/media}}{{^media}}none{{/media}}");

  struct SiteConfig site_config;
  init_test_site_config(&site_config);

  struct Album empty = {.title = S("Root"), .url_path = S("index.html"), .path_to_root = S("")};
  const struct TemplateContext context_empty = {.site_config = &site_config,
                                                .album_current = &empty};
  struct MediaItem item = {.title = S("x.jpg")};
  struct MediaItem* media[] = {&item};
  struct Album full = {.title = S("Root"),
                       .url_path = S("index.html"),
                       .path_to_root = S(""),
                       .media = media,
                       .media_count = 1};
  const struct TemplateContext context_full = {.site_config = &site_config, .album_current = &full};
  char* rendered_html = NULL;
  TEST_ASSERT(render(os, "album.html", &context_empty, &perm, &rendered_html, NULL) == 0);
  TEST_CHECK(strcmp(rendered_html, "none") == 0);

  TEST_ASSERT(render(os, "album.html", &context_full, &perm, &rendered_html, NULL) == 0);
  TEST_CHECK(strcmp(rendered_html, "x") == 0);
}

// An empty field value is falsey as a section, so an unset field reads as absent. Were an empty
// scalar truthy, `{{#url}}` would render for every item and `{{^url}}` for none, so both branches
// are asserted here. `site.base_url` is empty unless configured, so a template guarding it relies
// on this rule.
static void test_empty_field_is_falsey_as_section(void) {
  struct Arena perm = test_arena();
  struct Os* os = os_memory();
  os_memory_write(os, TEMPLATES_DIR "/album.html",
                  "{{#media}}[{{#url}}U:{{url}}{{/url}}"
                  "{{^url}}NONE{{/url}}]{{/media}}");

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct MediaItem first = {.original_url = S("")};
  struct MediaItem second = {.original_url = S("_fram/originals/b.jpg")};
  struct MediaItem* media[] = {&first, &second};
  struct Album root = {.title = S("Root"),
                       .url_path = S("index.html"),
                       .path_to_root = S(""),
                       .media = media,
                       .media_count = 2};
  const struct TemplateContext context = {.site_config = &site_config, .album_current = &root};
  char* rendered_html = NULL;
  TEST_ASSERT(render(os, "album.html", &context, &perm, &rendered_html, NULL) == 0);
  TEST_CHECK(strcmp(rendered_html, "[NONE][U:_fram/originals/b.jpg]") == 0);
}

// An absent value or a false flag is an empty field, so it never resolves from an enclosing album:
// inside `sub_albums` an album without a cover, media or sub-albums shows none of the current
// album's, an untitled media item shows no album title, and the root breadcrumb shows no parent.
// Each false flag also fires its inverted section.
static void test_absent_value_does_not_resolve_from_enclosing_album(void) {
  struct Arena perm = test_arena();
  struct Os* os = os_memory();
  os_memory_write(
      os, TEMPLATES_DIR "/album.html",
      "{{#sub_albums}}[{{title}}:{{cover.s.url}}{{^cover}}none{{/cover}}:"
      "{{#has_media}}M{{/has_media}}{{^has_media}}empty{{/has_media}}:"
      "{{#has_sub_albums}}S{{/has_sub_albums}}{{^has_sub_albums}}leaf{{/has_sub_albums}}]"
      "{{/sub_albums}}|"
      "{{#media}}[{{title}}:{{^is_video}}still{{/is_video}}{{^is_image}}moving{{/is_image}}]"
      "{{/media}}|"
      "{{#breadcrumbs}}[{{title}}:{{parent.title}}{{^parent}}top{{/parent}}]{{/"
      "breadcrumbs}}");
  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct MediaDerivative photo_derivatives[] = {
      {.name = S("s"), .url = S("_fram/r/s/trip/x.jpg"), .width_px = 120, .height_px = 120},
  };
  struct MediaItem photo = {
      .kind = MEDIA_KIND_IMAGE, .derivatives = photo_derivatives, .derivative_count = 1};
  struct MediaItem clip = {.title = S("Clip"), .kind = MEDIA_KIND_VIDEO};
  struct MediaItem* media[] = {&photo, &clip};
  struct Album root = {.title = S("Root"), .url_path = S("index.html")};
  struct Album empty = {.title = S("Empty"), .url_path = S("trip/empty/index.html"), .depth = 2};
  struct Album* children[] = {&empty};
  struct Album trip = {.title = S("Trip"),
                       .url_path = S("trip/index.html"),
                       .path_to_root = S("../"),
                       .media = media,
                       .media_count = 2,
                       .cover = &photo,
                       .sub_albums = children,
                       .sub_album_count = 1,
                       .parent = &root,
                       .depth = 1};
  empty.parent = &trip;
  const struct TemplateContext context = {.site_config = &site_config, .album_current = &trip};
  char* rendered_html = NULL;
  TEST_ASSERT(render(os, "album.html", &context, &perm, &rendered_html, NULL) == 0);
  TEST_CHECK(strcmp(rendered_html,
                    "[Empty:none:empty:leaf]|[:still][Clip:moving]|[Root:top][Trip:Root]") == 0);
  TEST_MSG("rendered: '%s'", rendered_html);
}

// A name that no media item has is looked up in the enclosing scopes, as Mustache asks, so inside
// `media` a bare `has_media` is the album's, and `url` is the item's own. Only the fields an album,
// item or derivative has stop the lookup.
static void test_unknown_name_resolves_from_enclosing_album(void) {
  struct Arena perm = test_arena();
  struct Os* os = os_memory();
  os_memory_write(os, TEMPLATES_DIR "/album.html", "{{#media}}[{{has_media}}|{{url}}]{{/media}}");
  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct MediaItem photo = {.kind = MEDIA_KIND_IMAGE, .original_url = S("_fram/originals/a.jpg")};
  struct MediaItem* media[] = {&photo};
  struct Album root = {
      .title = S("Root"), .url_path = S("index.html"), .media = media, .media_count = 1};
  const struct TemplateContext context = {.site_config = &site_config, .album_current = &root};
  char* rendered_html = NULL;
  TEST_ASSERT(render(os, "album.html", &context, &perm, &rendered_html, NULL) == 0);
  TEST_CHECK(strcmp(rendered_html, "[true|_fram/originals/a.jpg]") == 0);
  TEST_MSG("rendered: '%s'", rendered_html);
}

// A media item names its kind, `image` or `video`, and has the flag of that kind only.
static void test_media_names_its_kind(void) {
  struct Arena perm = test_arena();
  struct Os* os = os_memory();
  os_memory_write(os, TEMPLATES_DIR "/album.html",
                  "{{#media}}[{{kind}}:{{#is_image}}I{{/is_image}}"
                  "{{#is_video}}V{{/is_video}}]{{/media}}");
  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct MediaItem image = {.kind = MEDIA_KIND_IMAGE};
  struct MediaItem video = {.kind = MEDIA_KIND_VIDEO};
  struct MediaItem* media[] = {&image, &video};
  struct Album root = {
      .title = S("Root"), .url_path = S("index.html"), .media = media, .media_count = 2};
  const struct TemplateContext context = {.site_config = &site_config, .album_current = &root};
  char* rendered_html = NULL;
  TEST_ASSERT(render(os, "album.html", &context, &perm, &rendered_html, NULL) == 0);
  TEST_CHECK(strcmp(rendered_html, "[image:I][video:V]") == 0);
  TEST_MSG("rendered: '%s'", rendered_html);
}

// A video's `duration` is its length in milliseconds, even when that is `0`, and an image's is
// empty, so `{{#duration}}` renders only for a video.
static void test_duration_is_empty_for_an_image(void) {
  struct Arena perm = test_arena();
  struct Os* os = os_memory();
  os_memory_write(os, TEMPLATES_DIR "/album.html",
                  "{{#media}}[{{#duration}}D{{duration}}{{/duration}}]{{/media}}");
  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct MediaItem image = {.kind = MEDIA_KIND_IMAGE};
  struct MediaItem video = {.kind = MEDIA_KIND_VIDEO, .duration_ms = 0};
  struct MediaItem* media[] = {&image, &video};
  struct Album root = {
      .title = S("Root"), .url_path = S("index.html"), .media = media, .media_count = 2};
  const struct TemplateContext context = {.site_config = &site_config, .album_current = &root};
  char* rendered_html = NULL;
  TEST_ASSERT(render(os, "album.html", &context, &perm, &rendered_html, NULL) == 0);
  TEST_CHECK(strcmp(rendered_html, "[][D0]") == 0);
  TEST_MSG("rendered: '%s'", rendered_html);
}

// Triple braces and the ampersand tag cannot bypass the single HTML-escaping boundary, and neither
// can a partial, whose rejection is attributed to the template being rendered.
static void test_rejects_unescaped_interpolation(void) {
  struct Arena perm = test_arena();
  struct Os* os = os_memory();
  os_memory_write(os, TEMPLATES_DIR "/album.html", "{{{site.title}}}");

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct Album root = {.title = S("Root"), .url_path = S("index.html"), .path_to_root = S("")};
  const struct TemplateContext context = {.site_config = &site_config, .album_current = &root};

  static const struct {
    const char* template_name;
    const char* expected;
  } cases[] = {
      {"album.html", UNESCAPED_FAILURE " (in 'album.html')"},
      {"ampersand.html", UNESCAPED_FAILURE " (in 'ampersand.html')"},
      {"outer.html", UNESCAPED_FAILURE " (in 'outer.html')"},
  };
  os_memory_write(os, TEMPLATES_DIR "/ampersand.html", "{{& site.title}}");
  os_memory_write(os, TEMPLATES_DIR "/outer.html", "{{>raw}}");
  os_memory_write(os, TEMPLATES_DIR "/partials/raw.html", "{{{album.title}}}");
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct Log log = test_log();
    TEST_CHECK(render(os, cases[i].template_name, &context, &perm, NULL, &log) == -1);
    TEST_CHECK(is_log_one_failure(&log, cases[i].template_name, cases[i].expected));
    TEST_MSG("template: %s", cases[i].template_name);
  }
}

// A triple-brace tag fails whatever kind of value it names, so `{{{album.parent}}}` fails on a
// sub-album, whose parent is an album, as it does on the root album, whose parent is empty.
static void test_rejects_unescaped_interpolation_of_any_value(void) {
  struct Arena perm = test_arena();
  struct Os* os = os_memory();
  os_memory_write(os, TEMPLATES_DIR "/album.html", "{{{album.parent}}}");

  struct SiteConfig site_config;
  init_test_site_config(&site_config);
  struct Album root = {.title = S("Root"), .url_path = S("index.html")};
  struct Album trip = {.title = S("Trip"),
                       .url_path = S("trip/index.html"),
                       .path_to_root = S("../"),
                       .parent = &root,
                       .depth = 1};
  const struct Album* albums[] = {&root, &trip};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(albums) / sizeof(albums[0])); i++) {
    const struct TemplateContext context = {.site_config = &site_config,
                                            .album_current = albums[i]};
    struct Log log = test_log();
    TEST_CHECK(render(os, "album.html", &context, &perm, NULL, &log) == -1);
    TEST_CHECK(is_log_one_failure(&log, "album.html", UNESCAPED_FAILURE " (in 'album.html')"));
    TEST_MSG("album: %.*s", STR_FMT(albums[i]->title));
  }
}

TEST_LIST = {
    {"renders gallery tree with page-relative URLs",
     test_renders_gallery_tree_with_page_relative_urls},
    {"renders site fields", test_renders_site_fields},
    {"renders absolute urls below base url", test_renders_absolute_urls_below_base_url},
    {"renders sub-album cover", test_renders_sub_album_cover},
    {"path to root is the page prefix in every context",
     test_path_to_root_is_the_page_prefix_in_every_context},
    {"renders generator", test_renders_generator},
    {"unknown variable renders empty", test_unknown_variable_renders_empty},
    {"inverted section renders when list empty", test_inverted_section_renders_when_list_empty},
    {"empty field is falsey as section", test_empty_field_is_falsey_as_section},
    {"absent value does not resolve from enclosing album",
     test_absent_value_does_not_resolve_from_enclosing_album},
    {"unknown name resolves from enclosing album", test_unknown_name_resolves_from_enclosing_album},
    {"media names its kind", test_media_names_its_kind},
    {"duration is empty for an image", test_duration_is_empty_for_an_image},
    {"breadcrumbs mark the current album last", test_breadcrumbs_mark_the_current_album_last},
    {"rejects unescaped interpolation", test_rejects_unescaped_interpolation},
    {"rejects unescaped interpolation of any value",
     test_rejects_unescaped_interpolation_of_any_value},
    {NULL, NULL},
};
