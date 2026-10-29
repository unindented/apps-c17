#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "core/arena.h"
#include "core/log.h"
#include "core/str.h"
#include "os/os.h"
#include "os/os_memory.h"
#include "output/manifest.h"
#include "output/output_cache.h"
#include "output/output_writer.h"
#include "testing/test_support.h"

/**
 * @brief Initializes a writer on a new host whose manifest names `public/a.html` and
 *        `public/b.html`.
 *
 * @param writer     Writer to initialize. Must not be `NULL`.
 * @param manifest   Empty manifest that receives the two paths. Must not be `NULL`.
 * @param is_dry_run Whether the writer withholds its changes.
 * @param arena      Arena that owns the manifest's entries and the removed paths. Must not be
 *                   `NULL`.
 */
static void init_writer(struct OutputWriter* writer,
                        struct Manifest* manifest,
                        bool is_dry_run,
                        struct Arena* arena) {
  TEST_CHECK(manifest_add(manifest, S("public/a.html"), S("a"), arena, NULL) == 0);
  TEST_CHECK(manifest_add(manifest, S("public/b.html"), S("b"), arena, NULL) == 0);
  *writer = (struct OutputWriter){
      .manifest = manifest, .os = os_memory(), .is_dry_run = is_dry_run, .perm = arena};
}

/** What a test's make callback gives, and how often it was called. */
struct Maker {
  /** Bytes each call makes, or `NULL` for a make that fails. */
  const char* bytes;

  /** Number of calls so far. */
  ptrdiff_t call_count;
};

/**
 * @brief Makes the bytes of the `struct Maker` that `userdata` points to, as `OutputWriterMakeFn`,
 *        and counts the call.
 *
 * @param perm      Arena that owns the bytes. Must not be `NULL`.
 * @param scratch   Arena for temporaries, unused.
 * @param bytes_out Receives the bytes. Must not be `NULL`.
 * @param log       Log that receives the failure of a maker without bytes. Must not be `NULL`.
 * @param userdata  The `struct Maker`.
 * @return `0`, or `-1` when the maker has no bytes.
 */
static int make_bytes(struct Arena* perm,
                      struct Arena scratch,
                      struct Str* bytes_out,
                      struct Log* log,
                      void* userdata) {
  (void)scratch;
  struct Maker* maker = userdata;
  maker->call_count++;
  if (maker->bytes == NULL) {
    return log_error(log, "failed to render page");
  }
  *bytes_out = str_clone(str_from_cstr(maker->bytes), perm);
  return 0;
}

/**
 * @brief Initializes a writer below `public` on a new host whose manifest names `public/page.html`,
 *        with the attributes `blob=1`, `public/plain.html`, with none, and `public/copy.css`, a
 *        copy of `static/copy.css`, with the attributes `len=3` and the source's size, `3`.
 *
 * @param writer     Writer to initialize. Must not be `NULL`.
 * @param manifest   Empty manifest that receives the paths. Must not be `NULL`.
 * @param is_dry_run Whether the writer withholds its changes.
 * @param arena      Arena that owns the manifest's entries. Must not be `NULL`.
 */
static void init_produce_writer(struct OutputWriter* writer,
                                struct Manifest* manifest,
                                bool is_dry_run,
                                struct Arena* arena) {
  TEST_CHECK(manifest_add_output(manifest, S("public/page.html"), S("page"),
                                 &(struct ManifestOutput){.attributes = S("blob=1")}, arena,
                                 NULL) == 0);
  TEST_CHECK(manifest_add(manifest, S("public/plain.html"), S("plain"), arena, NULL) == 0);
  TEST_CHECK(manifest_add_output(manifest, S("public/copy.css"), S("static/copy.css"),
                                 &(struct ManifestOutput){.attributes = S("len=3"),
                                                          .copy_source_path = S("static/copy.css"),
                                                          .copy_source_len = 3},
                                 arena, NULL) == 0);
  *writer = (struct OutputWriter){.manifest = manifest,
                                  .os = os_memory(),
                                  .output_dir = S("public"),
                                  .is_dry_run = is_dry_run,
                                  .perm = arena};
  os_memory_write(writer->os, "static/copy.css", "new");
}

/**
 * @brief Returns a cache whose rows vouch for `page.html`, with the attributes `attributes`, and
 *        `copy.css`, with the attributes `len=3`, each of `len` bytes.
 *
 * @param attributes Terminated attributes of the row of `page.html`. Must not be `NULL`.
 * @param len        Size each row records.
 * @param arena      Arena that owns the rows. Must not be `NULL`.
 * @return The cache.
 */
static struct OutputCache make_cache(const char* attributes, ptrdiff_t len, struct Arena* arena) {
  struct OutputCacheRow* rows = arena_new(arena, 2, struct OutputCacheRow);
  rows[0] =
      (struct OutputCacheRow){.relative_path = S("copy.css"), .attributes = S("len=3"), .len = len};
  rows[1] = (struct OutputCacheRow){
      .relative_path = S("page.html"), .attributes = str_from_cstr(attributes), .len = len};
  return (struct OutputCache){.rows = rows, .row_count = 2};
}

/**
 * @brief Produces `file_path` through `writer` with `maker`, with fresh arenas.
 *
 * @param writer    Writer of the build. Must not be `NULL`.
 * @param file_path Terminated output path. Must not be `NULL`.
 * @param cache     Cache of the previous build. Must not be `NULL`.
 * @param maker     Maker of the bytes, or `NULL` for a copy.
 * @param log       Log that receives the failure. Must not be `NULL`.
 * @return What `output_writer_produce` returns.
 */
static int produce(const struct OutputWriter* writer,
                   const char* file_path,
                   const struct OutputCache* cache,
                   struct Maker* maker,
                   struct Log* log) {
  return output_writer_produce(writer, str_from_cstr(file_path), cache,
                               maker == NULL ? NULL : make_bytes, maker, test_arena(), test_arena(),
                               S("failed to write page"), log);
}

/**
 * @brief Returns the modification time of `file_path`, in nanoseconds, or `-1` when it has none.
 *
 * @param os        Host the file is on. Must not be `NULL`.
 * @param file_path Terminated path of the file. Must not be `NULL`.
 * @return The modification time.
 */
static int64_t mtime_of(struct Os* os, const char* file_path) {
  struct OsStat st;
  if (os_stat(os, str_from_cstr(file_path), &st) != 0) {
    return -1;
  }
  return st.mtime_s * 1000000000 + st.mtime_ns;
}

// A write writes its bytes and marks its path written with their size, and a keep marks its path
// kept without a size and leaves the file as it is. A path neither touched stays pending.
static void test_marks_writes_and_keeps(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, false, &arena);
  os_memory_write(writer.os, "public/b.html", "old");

  struct Log log = test_log();
  TEST_CHECK(output_writer_write(&writer, S("public/a.html"), (struct OsSource){.bytes = S("new")},
                                 S("failed to write output"), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(writer.os, "public/a.html", "new"));
  TEST_CHECK(manifest_find_mark(&manifest, S("public/a.html")) == MANIFEST_MARK_WRITTEN);
  TEST_CHECK(manifest_find_len(&manifest, S("public/a.html")) == 3);
  TEST_CHECK(manifest_find_mark(&manifest, S("public/b.html")) == MANIFEST_MARK_PENDING);

  TEST_CHECK(output_writer_keep(&writer, S("public/b.html"), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(writer.os, "public/b.html", "old"));
  TEST_CHECK(manifest_find_mark(&manifest, S("public/b.html")) == MANIFEST_MARK_KEPT);
  TEST_CHECK(manifest_find_len(&manifest, S("public/b.html")) == -1);
}

// A write that streams a file marks its path written without a size, which only the stream learns.
static void test_write_of_a_file_records_no_size(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, false, &arena);
  os_memory_write(writer.os, "static/a.html", "aaa");
  struct Log log = test_log();
  TEST_CHECK(output_writer_write(
                 &writer, S("public/a.html"),
                 (struct OsSource){.kind = OS_SOURCE_FILE, .file_path = S("static/a.html")},
                 S("failed to write output"), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(writer.os, "public/a.html", "aaa"));
  TEST_CHECK(manifest_find_mark(&manifest, S("public/a.html")) == MANIFEST_MARK_WRITTEN);
  TEST_CHECK(manifest_find_len(&manifest, S("public/a.html")) == -1);
}

// An output whose attributes its cache records, and which exists with the recorded size, is kept
// without being made, even when its bytes would differ, and its mark records that size.
static void test_produce_keeps_an_output_its_cache_vouches_for(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_produce_writer(&writer, &manifest, false, &arena);
  os_memory_write(writer.os, "public/page.html", "old");
  const struct OutputCache cache = make_cache("blob=1", 3, &arena);
  struct Maker maker = {.bytes = "new"};
  struct Log log = test_log();
  TEST_CHECK(produce(&writer, "public/page.html", &cache, &maker, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(maker.call_count == 0);
  TEST_CHECK(os_memory_is_file(writer.os, "public/page.html", "old"));
  TEST_CHECK(manifest_find_mark(&manifest, S("public/page.html")) == MANIFEST_MARK_KEPT);
  TEST_CHECK(manifest_find_len(&manifest, S("public/page.html")) == 3);
}

// An output is made and written when its cache records other attributes, when it is missing, when
// its size differs from the recorded one, and when the cache has no row for it, and its mark
// records the size of the made bytes.
static void test_produce_makes_an_output_its_cache_does_not_vouch_for(void) {
  static const struct {
    const char* name;
    const char* attributes;
    const char* existing;
    ptrdiff_t row_count;
  } cases[] = {
      {"other attributes", "blob=0", "old", 2},
      {"missing output", "blob=1", NULL, 2},
      {"other size", "blob=1", "longer", 2},
      {"no row", "blob=1", "old", 0},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    TEST_CASE(cases[i].name);
    struct Arena arena = test_arena();
    struct Manifest manifest = {0};
    struct OutputWriter writer;
    init_produce_writer(&writer, &manifest, false, &arena);
    if (cases[i].existing != NULL) {
      os_memory_write(writer.os, "public/page.html", cases[i].existing);
    }
    struct OutputCache cache = make_cache(cases[i].attributes, 3, &arena);
    cache.row_count = cases[i].row_count;
    struct Maker maker = {.bytes = "new"};
    struct Log log = test_log();
    TEST_CHECK(produce(&writer, "public/page.html", &cache, &maker, &log) == 0);
    TEST_CHECK(test_log_is_empty(&log));
    TEST_CHECK(maker.call_count == 1);
    TEST_CHECK(os_memory_is_file(writer.os, "public/page.html", "new"));
    TEST_CHECK(manifest_find_mark(&manifest, S("public/page.html")) == MANIFEST_MARK_WRITTEN);
    TEST_CHECK(manifest_find_len(&manifest, S("public/page.html")) == 3);
  }
}

// A directory at a path that the cache vouches for, with the size its row records, is no output the
// cache can keep, so a page is made and a copy streamed, and either write fails on the directory,
// which stays, and leaves the path pending.
static void test_produce_does_not_keep_a_directory_its_cache_vouches_for(void) {
  static const char* const paths[] = {"public/page.html", "public/copy.css"};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(paths) / sizeof(paths[0])); i++) {
    TEST_CASE(paths[i]);
    struct Arena arena = test_arena();
    struct Manifest manifest = {0};
    struct OutputWriter writer;
    init_produce_writer(&writer, &manifest, false, &arena);
    os_memory_mkdir(writer.os, paths[i]);
    struct OsStat dir_stat;
    TEST_ASSERT(os_stat(writer.os, str_from_cstr(paths[i]), &dir_stat) == 0);
    const struct OutputCache cache = make_cache("blob=1", dir_stat.len, &arena);
    struct Maker maker = {.bytes = "new"};
    struct Log log = test_log();
    TEST_CHECK(produce(&writer, paths[i], &cache, &maker, &log) == -1);
    TEST_CHECK(test_log_is(&log, "failed to write page: %s ('%s')\n", strerror(EISDIR), paths[i]));
    TEST_CHECK(maker.call_count == (i == 0 ? 1 : 0));
    struct OsStat now;
    TEST_CHECK(os_stat(writer.os, str_from_cstr(paths[i]), &now) == 0 && now.is_dir);
    TEST_CHECK(manifest_find_mark(&manifest, str_from_cstr(paths[i])) == MANIFEST_MARK_PENDING);
  }
}

// An output whose made bytes equal the file already there is kept and not written again, so its
// modification time stays, whether or not it declares attributes, and its mark records their size.
// One whose bytes differ is written, and its mark records the size of the new bytes.
static void test_produce_keeps_an_unchanged_file(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_produce_writer(&writer, &manifest, false, &arena);
  os_memory_write(writer.os, "public/plain.html", "same");
  os_memory_write(writer.os, "public/page.html", "same");
  const int64_t plain_mtime = mtime_of(writer.os, "public/plain.html");
  const int64_t page_mtime = mtime_of(writer.os, "public/page.html");
  const struct OutputCache cache = {0};
  struct Maker maker = {.bytes = "same"};
  struct Log log = test_log();
  TEST_CHECK(produce(&writer, "public/plain.html", &cache, &maker, &log) == 0);
  TEST_CHECK(produce(&writer, "public/page.html", &cache, &maker, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(maker.call_count == 2);
  TEST_CHECK(mtime_of(writer.os, "public/plain.html") == plain_mtime);
  TEST_CHECK(mtime_of(writer.os, "public/page.html") == page_mtime);
  TEST_CHECK(manifest_find_mark(&manifest, S("public/plain.html")) == MANIFEST_MARK_KEPT);
  TEST_CHECK(manifest_find_mark(&manifest, S("public/page.html")) == MANIFEST_MARK_KEPT);
  TEST_CHECK(manifest_find_len(&manifest, S("public/plain.html")) == 4);
  TEST_CHECK(manifest_find_len(&manifest, S("public/page.html")) == 4);

  maker.bytes = "same+";
  TEST_CHECK(produce(&writer, "public/plain.html", &cache, &maker, &log) == 0);
  TEST_CHECK(os_memory_is_file(writer.os, "public/plain.html", "same+"));
  TEST_CHECK(manifest_find_mark(&manifest, S("public/plain.html")) == MANIFEST_MARK_WRITTEN);
  TEST_CHECK(manifest_find_len(&manifest, S("public/plain.html")) == 5);
}

// A forced build keeps nothing: it makes and writes an output its cache vouches for, writes one
// whose bytes are unchanged, and copies a copy its cache vouches for.
static void test_produce_forced_writes_every_output(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_produce_writer(&writer, &manifest, false, &arena);
  writer.is_forced = true;
  os_memory_write(writer.os, "public/page.html", "old");
  os_memory_write(writer.os, "public/plain.html", "new");
  os_memory_write(writer.os, "public/copy.css", "old");
  const int64_t plain_mtime = mtime_of(writer.os, "public/plain.html");
  const struct OutputCache cache = make_cache("blob=1", 3, &arena);
  struct Maker maker = {.bytes = "new"};
  struct Log log = test_log();
  TEST_CHECK(produce(&writer, "public/page.html", &cache, &maker, &log) == 0);
  TEST_CHECK(produce(&writer, "public/plain.html", &cache, &maker, &log) == 0);
  TEST_CHECK(produce(&writer, "public/copy.css", &cache, NULL, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(maker.call_count == 2);
  TEST_CHECK(os_memory_is_file(writer.os, "public/page.html", "new"));
  TEST_CHECK(mtime_of(writer.os, "public/plain.html") > plain_mtime);
  TEST_CHECK(os_memory_is_file(writer.os, "public/copy.css", "new"));
  TEST_CHECK(manifest_find_mark(&manifest, S("public/page.html")) == MANIFEST_MARK_WRITTEN);
  TEST_CHECK(manifest_find_mark(&manifest, S("public/plain.html")) == MANIFEST_MARK_WRITTEN);
  TEST_CHECK(manifest_find_mark(&manifest, S("public/copy.css")) == MANIFEST_MARK_WRITTEN);
}

// A dry run lists an output its cache vouches for as kept without making it, and its mark records
// the size the cache records.
static void test_produce_dry_run_keeps_a_vouched_output(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_produce_writer(&writer, &manifest, true, &arena);
  os_memory_write(writer.os, "public/page.html", "old");
  os_memory_write(writer.os, "public/plain.html", "new");
  os_memory_write(writer.os, "public/copy.css", "old");
  const struct OutputCache cache = make_cache("blob=1", 3, &arena);
  struct Maker maker = {.bytes = "new"};
  struct Log log = test_log();
  TEST_CHECK(produce(&writer, "public/page.html", &cache, &maker, &log) == 0);
  TEST_CHECK(produce(&writer, "public/plain.html", &cache, &maker, &log) == 0);
  TEST_CHECK(produce(&writer, "public/copy.css", &cache, NULL, &log) == 0);
  TEST_CHECK(maker.call_count == 1);
  TEST_CHECK(output_writer_report(&writer, test_arena(), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_printed(writer.os, 1,
                                  "keep\tpublic/page.html\n"
                                  "keep\tpublic/plain.html\n"
                                  "keep\tpublic/copy.css\n"));
  TEST_CHECK(os_memory_is_file(writer.os, "public/page.html", "old"));
  TEST_CHECK(manifest_find_len(&manifest, S("public/page.html")) == 3);
}

// A dry run makes an output that declares no attributes in memory, and lists it as written when its
// bytes differ from the file there, or are missing, which it leaves as they are.
static void test_produce_dry_run_makes_an_output_without_attributes(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_produce_writer(&writer, &manifest, true, &arena);
  os_memory_write(writer.os, "public/plain.html", "old");
  const struct OutputCache cache = {0};
  struct Maker maker = {.bytes = "new"};
  struct Log log = test_log();
  TEST_CHECK(produce(&writer, "public/plain.html", &cache, &maker, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(maker.call_count == 1);
  TEST_CHECK(manifest_find_mark(&manifest, S("public/plain.html")) == MANIFEST_MARK_WRITTEN);
  TEST_CHECK(os_memory_is_file(writer.os, "public/plain.html", "old"));
}

// A dry run lists an output whose attributes do not match its cache as written without making it,
// because making it can be expensive, and lists a copy that does not match as written without
// copying it. Neither mark records a size, because neither file changed.
static void test_produce_dry_run_skips_the_make_of_a_mismatched_output(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_produce_writer(&writer, &manifest, true, &arena);
  os_memory_write(writer.os, "public/page.html", "old");
  os_memory_write(writer.os, "public/copy.css", "longer");
  const struct OutputCache cache = make_cache("blob=0", 3, &arena);
  struct Maker maker = {.bytes = "new"};
  struct Log log = test_log();
  TEST_CHECK(produce(&writer, "public/page.html", &cache, &maker, &log) == 0);
  TEST_CHECK(produce(&writer, "public/copy.css", &cache, NULL, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(maker.call_count == 0);
  TEST_CHECK(manifest_find_mark(&manifest, S("public/page.html")) == MANIFEST_MARK_WRITTEN);
  TEST_CHECK(manifest_find_mark(&manifest, S("public/copy.css")) == MANIFEST_MARK_WRITTEN);
  TEST_CHECK(manifest_find_len(&manifest, S("public/page.html")) == -1);
  TEST_CHECK(manifest_find_len(&manifest, S("public/copy.css")) == -1);
  TEST_CHECK(os_memory_is_file(writer.os, "public/page.html", "old"));
  TEST_CHECK(os_memory_is_file(writer.os, "public/copy.css", "longer"));
}

// A copy its cache vouches for is kept without reading its source or comparing bytes, with the size
// the cache records, and any other copy is streamed from its source, even over a file of equal
// bytes, without a make, with the size in its source's stat.
static void test_produce_copies_a_copy(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_produce_writer(&writer, &manifest, false, &arena);
  os_memory_write(writer.os, "public/copy.css", "old");
  struct OutputCache cache = make_cache("blob=1", 3, &arena);
  struct Log log = test_log();
  TEST_CHECK(produce(&writer, "public/copy.css", &cache, NULL, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(writer.os, "public/copy.css", "old"));
  TEST_CHECK(manifest_find_mark(&manifest, S("public/copy.css")) == MANIFEST_MARK_KEPT);
  TEST_CHECK(manifest_find_len(&manifest, S("public/copy.css")) == 3);

  cache.rows[0].attributes = S("len=2");
  TEST_CHECK(produce(&writer, "public/copy.css", &cache, NULL, &log) == 0);
  TEST_CHECK(os_memory_is_file(writer.os, "public/copy.css", "new"));
  TEST_CHECK(manifest_find_mark(&manifest, S("public/copy.css")) == MANIFEST_MARK_WRITTEN);
  TEST_CHECK(manifest_find_len(&manifest, S("public/copy.css")) == 3);

  const int64_t copy_mtime = mtime_of(writer.os, "public/copy.css");
  TEST_CHECK(produce(&writer, "public/copy.css", &cache, NULL, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(mtime_of(writer.os, "public/copy.css") > copy_mtime);
}

// A copy recorded without its source's size, as `0` gives none, is streamed with no size, so the
// output cache stats it.
static void test_produce_copies_a_copy_without_its_source_size(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  TEST_CHECK(manifest_add_output(&manifest, S("public/copy.css"), S("static/copy.css"),
                                 &(struct ManifestOutput){.attributes = S("len=3"),
                                                          .copy_source_path = S("static/copy.css")},
                                 &arena, NULL) == 0);
  struct OutputWriter writer = {
      .manifest = &manifest, .os = os_memory(), .output_dir = S("public"), .perm = &arena};
  os_memory_write(writer.os, "static/copy.css", "new");
  const struct OutputCache cache = {0};
  struct Log log = test_log();
  TEST_CHECK(produce(&writer, "public/copy.css", &cache, NULL, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(writer.os, "public/copy.css", "new"));
  TEST_CHECK(manifest_find_mark(&manifest, S("public/copy.css")) == MANIFEST_MARK_WRITTEN);
  TEST_CHECK(manifest_find_len(&manifest, S("public/copy.css")) == -1);
}

// A copy whose source is gone fails with the operation, the cause and the source, and leaves its
// path pending, with no size.
static void test_produce_reports_a_failed_copy(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_produce_writer(&writer, &manifest, false, &arena);
  struct Log log = test_log();
  TEST_CHECK(os_remove(writer.os, S("static/copy.css"), S("failed to remove"), &log) == 0);
  const struct OutputCache cache = {0};
  TEST_CHECK(produce(&writer, "public/copy.css", &cache, NULL, &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to write page: %s ('static/copy.css')\n", strerror(ENOENT)));
  TEST_CHECK(manifest_find_mark(&manifest, S("public/copy.css")) == MANIFEST_MARK_PENDING);
  TEST_CHECK(manifest_find_len(&manifest, S("public/copy.css")) == -1);
}

// A make that fails fails the produce with its own message, and writes nothing.
static void test_produce_reports_a_failed_make(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_produce_writer(&writer, &manifest, false, &arena);
  const struct OutputCache cache = {0};
  struct Maker maker = {0};
  struct Log log = test_log();
  TEST_CHECK(produce(&writer, "public/plain.html", &cache, &maker, &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to render page\n"));
  TEST_CHECK(!os_memory_has_path(writer.os, "public/plain.html"));
  TEST_CHECK(manifest_find_mark(&manifest, S("public/plain.html")) == MANIFEST_MARK_PENDING);
}

// A produce of a path the manifest does not name fails with the operation and the path, and makes
// nothing.
static void test_produce_rejects_path_outside_manifest(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_produce_writer(&writer, &manifest, false, &arena);
  const struct OutputCache cache = {0};
  struct Maker maker = {.bytes = "new"};
  struct Log log = test_log();
  TEST_CHECK(produce(&writer, "public/other.html", &cache, &maker, &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to write page: not a build output ('public/other.html')\n"));
  TEST_CHECK(maker.call_count == 0);
  TEST_CHECK(!os_memory_has_path(writer.os, "public/other.html"));
}

// A real build's writer makes each change: it creates directories and removes files and
// directories, recording nothing.
static void test_makes_changes(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, false, &arena);
  os_memory_write(writer.os, "public/old/page.html", "old");

  struct Log log = test_log();
  TEST_CHECK(output_writer_mkdirs(&writer, S("public/new"), S("failed to create"), &log) == 0);
  TEST_CHECK(
      output_writer_remove(&writer, S("public/old/page.html"), S("failed to remove"), &log) == 0);
  TEST_CHECK(output_writer_remove_dir(&writer, S("public/old"), S("failed to remove"), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_has_path(writer.os, "public/new"));
  TEST_CHECK(!os_memory_has_path(writer.os, "public/old"));
  TEST_CHECK(writer.removed_paths.count == 0);
}

// A dry run's writer marks a write without making it or recording a size, creates no directory, and
// records each removal in order while leaving the path in place.
static void test_dry_run_withholds_changes(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, true, &arena);
  os_memory_write(writer.os, "public/old/page.html", "old");

  struct Log log = test_log();
  TEST_CHECK(output_writer_write(&writer, S("public/a.html"), (struct OsSource){.bytes = S("new")},
                                 S("failed to write output"), &log) == 0);
  TEST_CHECK(output_writer_mkdirs(&writer, S("public/new"), S("failed to create"), &log) == 0);
  TEST_CHECK(
      output_writer_remove(&writer, S("public/old/page.html"), S("failed to remove"), &log) == 0);
  TEST_CHECK(output_writer_remove_dir(&writer, S("public/old"), S("failed to remove"), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(manifest_find_mark(&manifest, S("public/a.html")) == MANIFEST_MARK_WRITTEN);
  TEST_CHECK(manifest_find_len(&manifest, S("public/a.html")) == -1);
  TEST_CHECK(!os_memory_has_path(writer.os, "public/a.html"));
  TEST_CHECK(!os_memory_has_path(writer.os, "public/new"));
  TEST_CHECK(os_memory_is_file(writer.os, "public/old/page.html", "old"));
  TEST_CHECK(writer.removed_paths.count == 2);
  TEST_CHECK(str_eq(writer.removed_paths.items[0], S("public/old/page.html")));
  TEST_CHECK(str_eq(writer.removed_paths.items[1], S("public/old")));
}

// The report lists every output path in manifest order as `write` or `keep`, then every recorded
// removal as `remove`, sorted bytewise, each a mark, a tab and the path.
static void test_reports_marks_and_sorted_removals(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, true, &arena);
  struct Log log = test_log();
  TEST_CHECK(output_writer_keep(&writer, S("public/b.html"), &log) == 0);
  TEST_CHECK(output_writer_write(&writer, S("public/a.html"), (struct OsSource){0},
                                 S("failed to write output"), &log) == 0);
  TEST_CHECK(output_writer_remove(&writer, S("public/z.html"), S("failed to remove"), &log) == 0);
  TEST_CHECK(output_writer_remove(&writer, S("public/old/x.html"), S("failed to remove"), &log) ==
             0);
  TEST_CHECK(output_writer_remove_dir(&writer, S("public/old"), S("failed to remove"), &log) == 0);

  TEST_CHECK(output_writer_report(&writer, test_arena(), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_printed(writer.os, 1,
                                  "write\tpublic/a.html\n"
                                  "keep\tpublic/b.html\n"
                                  "remove\tpublic/old\n"
                                  "remove\tpublic/old/x.html\n"
                                  "remove\tpublic/z.html\n"));
}

// A write or a keep of a path the manifest does not name fails with the operation and the path, and
// the write changes nothing.
static void test_rejects_path_outside_manifest(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, false, &arena);

  struct Log log = test_log();
  TEST_CHECK(output_writer_write(&writer, S("public/c.html"), (struct OsSource){.bytes = S("new")},
                                 S("failed to write output"), &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to write output: not a build output ('public/c.html')\n"));
  TEST_CHECK(!os_memory_has_path(writer.os, "public/c.html"));

  log = test_log();
  TEST_CHECK(output_writer_keep(&writer, S("public/c.html"), &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to keep output: not a build output ('public/c.html')\n"));
}

// An output path that is still pending after the writes was written without the writer, so the
// report names it and prints no listing, which would leave that write out.
static void test_report_rejects_pending_output(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, true, &arena);
  struct Log log = test_log();
  TEST_CHECK(output_writer_write(&writer, S("public/a.html"), (struct OsSource){0},
                                 S("failed to write output"), &log) == 0);

  TEST_CHECK(output_writer_report(&writer, test_arena(), &log) == -1);
  TEST_CHECK(test_log_is(
      &log, "failed to report dry run: output was neither written nor kept ('public/b.html')\n"));
  TEST_CHECK(os_memory_is_printed(writer.os, 1, ""));
}

TEST_LIST = {
    {"marks writes and keeps", test_marks_writes_and_keeps},
    {"write of a file records no size", test_write_of_a_file_records_no_size},
    {"produce keeps an output its cache vouches for",
     test_produce_keeps_an_output_its_cache_vouches_for},
    {"produce makes an output its cache does not vouch for",
     test_produce_makes_an_output_its_cache_does_not_vouch_for},
    {"produce does not keep a directory its cache vouches for",
     test_produce_does_not_keep_a_directory_its_cache_vouches_for},
    {"produce keeps an unchanged file", test_produce_keeps_an_unchanged_file},
    {"produce forced writes every output", test_produce_forced_writes_every_output},
    {"produce dry run keeps a vouched output", test_produce_dry_run_keeps_a_vouched_output},
    {"produce dry run makes an output without attributes",
     test_produce_dry_run_makes_an_output_without_attributes},
    {"produce dry run skips the make of a mismatched output",
     test_produce_dry_run_skips_the_make_of_a_mismatched_output},
    {"produce copies a copy", test_produce_copies_a_copy},
    {"produce copies a copy without its source size",
     test_produce_copies_a_copy_without_its_source_size},
    {"produce reports a failed copy", test_produce_reports_a_failed_copy},
    {"produce reports a failed make", test_produce_reports_a_failed_make},
    {"produce rejects path outside manifest", test_produce_rejects_path_outside_manifest},
    {"makes changes", test_makes_changes},
    {"dry run withholds changes", test_dry_run_withholds_changes},
    {"reports marks and sorted removals", test_reports_marks_and_sorted_removals},
    {"rejects path outside manifest", test_rejects_path_outside_manifest},
    {"report rejects pending output", test_report_rejects_pending_output},
    {NULL, NULL},
};
