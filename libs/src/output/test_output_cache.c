#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "core/arena.h"
#include "core/log.h"
#include "core/path.h"
#include "core/str.h"
#include "os/os.h"
#include "os/os_memory.h"
#include "output/build_cache_file.h"
#include "output/manifest.h"
#include "output/output_cache.h"
#include "output/output_writer.h"
#include "testing/test_support.h"

/**
 * Path of the cache that every test writes. The first line of a cache, `output cache v1`, is a
 * file-local constant of `output_cache.c`, so changing its version must update the texts here.
 */
#define CACHE_PATH "public/_cache.tsv"

/** Facts that the tests record, out of order. */
static const struct OutputCacheFact FACTS[] = {
    {.key = STR_INIT("photos/z.jpg"), .value = STR_INIT("w=4000 h=3000")},
    {.key = STR_INIT("photos/a.jpg"), .value = STR_INIT("w=10 h=20")},
};

/**
 * @brief Records `relative_path` below `public` in `manifest` with `attributes`.
 *
 * @param manifest      Manifest that receives the path. Must not be `NULL`.
 * @param relative_path Terminated path below `public`. Must not be `NULL`.
 * @param attributes    Terminated attributes. Must not be `NULL`.
 * @param arena         Arena that owns the manifest's entries. Must not be `NULL`.
 */
static void add_output(struct Manifest* manifest,
                       const char* relative_path,
                       const char* attributes,
                       struct Arena* arena) {
  struct Arena scratch = test_arena();
  const struct Str output_path = path_join(S("public"), str_from_cstr(relative_path), &scratch);
  TEST_CHECK(manifest_add_output(manifest, output_path, S("source"),
                                 &(struct ManifestOutput){.attributes = str_from_cstr(attributes)},
                                 arena, NULL) == 0);
}

/**
 * @brief Initializes a writer below `public` on a new host, whose manifest names the cache and the
 *        outputs of a build that went through every output.
 *
 * `b.webp`, with the attributes `src=2 width=640`, is kept, and `a.html`, with `blob=1`, is
 * written. `c.html` declares no attributes and is written. `d.html` declares attributes but is
 * still pending, and `e.html` declares attributes and is marked written but is missing. No mark
 * records a size, so the cache write stats each output.
 *
 * @param writer     Writer to initialize. Must not be `NULL`.
 * @param manifest   Empty manifest that receives the paths. Must not be `NULL`.
 * @param is_dry_run Whether the writer withholds its changes.
 * @param arena      Arena that owns the manifest's entries. Must not be `NULL`.
 */
static void init_built_writer(struct OutputWriter* writer,
                              struct Manifest* manifest,
                              bool is_dry_run,
                              struct Arena* arena) {
  TEST_CHECK(manifest_add(manifest, S(CACHE_PATH), S("cache"), arena, NULL) == 0);
  add_output(manifest, "b.webp", "src=2 width=640", arena);
  add_output(manifest, "a.html", "blob=1", arena);
  add_output(manifest, "c.html", "", arena);
  add_output(manifest, "d.html", "x=1", arena);
  add_output(manifest, "e.html", "x=2", arena);
  *writer = (struct OutputWriter){.manifest = manifest,
                                  .os = os_memory(),
                                  .output_dir = S("public"),
                                  .is_dry_run = is_dry_run,
                                  .perm = arena};
  os_memory_write(writer->os, "public/b.webp", "bbbbb");
  os_memory_write(writer->os, "public/a.html", "aaa");
  os_memory_write(writer->os, "public/c.html", "c");
  os_memory_write(writer->os, "public/d.html", "d");
  struct Log log = test_log();
  TEST_CHECK(output_writer_keep(writer, S("public/b.webp"), &log) == 0);
  TEST_CHECK(manifest_mark(manifest, S("public/a.html"), MANIFEST_MARK_WRITTEN, -1) == 0);
  TEST_CHECK(manifest_mark(manifest, S("public/c.html"), MANIFEST_MARK_WRITTEN, -1) == 0);
  TEST_CHECK(manifest_mark(manifest, S("public/e.html"), MANIFEST_MARK_WRITTEN, -1) == 0);
  TEST_CHECK(test_log_is_empty(&log));
}

/**
 * @brief Loads the cache at `CACHE_PATH` on `os`.
 *
 * @param os         Host the cache is on. Must not be `NULL`.
 * @param perm       Arena that owns the cache. Must not be `NULL`.
 * @param cache_out  Receives the cache. Must not be `NULL`.
 * @param reason_out Receives the reason an unusable cache is not used. Must not be `NULL`.
 * @return What `output_cache_load` returns.
 */
static enum BuildCacheFileState load(struct Os* os,
                                     struct Arena* perm,
                                     struct OutputCache* cache_out,
                                     struct Str* reason_out) {
  return output_cache_load(S(CACHE_PATH), os, perm, test_arena(), cache_out, reason_out);
}

/**
 * @brief Makes the bytes `new`, as `OutputWriterMakeFn`, and counts the call.
 *
 * @param perm      Arena that owns the bytes. Must not be `NULL`.
 * @param scratch   Arena for temporaries, unused.
 * @param bytes_out Receives the bytes. Must not be `NULL`.
 * @param log       Log, unused.
 * @param userdata  The `ptrdiff_t` call count.
 * @return `0`.
 */
static int make_new(struct Arena* perm,
                    struct Arena scratch,
                    struct Str* bytes_out,
                    struct Log* log,
                    void* userdata) {
  (void)scratch;
  (void)log;
  ++*(ptrdiff_t*)userdata;
  *bytes_out = str_clone(S("new"), perm);
  return 0;
}

// The cache has a row for each output that is written or kept, declares attributes and exists, with
// the size a stat gives when its mark recorded none, sorted by path, then each fact sorted by key.
// An output without attributes, one still pending, and one that is missing have none.
static void test_write_records_each_written_or_kept_output_and_each_fact(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_built_writer(&writer, &manifest, false, &arena);
  struct Log log = test_log();
  TEST_CHECK(output_cache_write(&writer, S(CACHE_PATH), FACTS, 2, test_arena(), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(writer.os, CACHE_PATH,
                               "output cache v1\n"
                               "output\ta.html\tblob=1\t3\n"
                               "output\tb.webp\tsrc=2 width=640\t5\n"
                               "fact\tphotos/a.jpg\tw=10 h=20\n"
                               "fact\tphotos/z.jpg\tw=4000 h=3000\n"));
  TEST_CHECK(manifest_find_mark(&manifest, S(CACHE_PATH)) == MANIFEST_MARK_WRITTEN);
}

// Each output that `output_writer_produce` handles gets a row with the size its mark recorded, so
// the write stats none of them: one written, one kept on its attributes, one kept for its equal
// bytes, and a copy streamed from its source. A write that fails gets no row.
static void test_write_takes_each_size_from_its_mark(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  TEST_CHECK(manifest_add(&manifest, S(CACHE_PATH), S("cache"), &arena, NULL) == 0);
  add_output(&manifest, "written.html", "x=1", &arena);
  add_output(&manifest, "kept.html", "x=2", &arena);
  add_output(&manifest, "same.html", "x=3", &arena);
  add_output(&manifest, "failed.html", "x=4", &arena);
  TEST_CHECK(manifest_add_output(&manifest, S("public/copy.css"), S("static/copy.css"),
                                 &(struct ManifestOutput){.attributes = S("len=4"),
                                                          .copy_source_path = S("static/copy.css"),
                                                          .copy_source_len = 4},
                                 &arena, NULL) == 0);
  struct OutputWriter writer = {
      .manifest = &manifest, .os = os_memory(), .output_dir = S("public"), .perm = &arena};
  os_memory_write(writer.os, "public/kept.html", "kept!");
  os_memory_write(writer.os, "public/same.html", "new");
  os_memory_write(writer.os, "public/failed.html", "old");
  os_memory_write(writer.os, "static/copy.css", "copy");
  os_memory_fail(writer.os, OS_MEMORY_OP_WRITE_DATA, "public/failed.html", ENOSPC);
  struct OutputCacheRow row = {.relative_path = S("kept.html"), .attributes = S("x=2"), .len = 5};
  const struct OutputCache cache = {.rows = &row, .row_count = 1};
  static const char* const paths[] = {"public/written.html", "public/kept.html", "public/same.html",
                                      "public/copy.css"};
  ptrdiff_t call_count = 0;
  struct Log log = test_log();
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(paths) / sizeof(paths[0])); i++) {
    TEST_CHECK(output_writer_produce(&writer, str_from_cstr(paths[i]), &cache, make_new,
                                     &call_count, test_arena(), test_arena(),
                                     S("failed to write page"), &log) == 0);
  }
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(output_writer_produce(&writer, S("public/failed.html"), &cache, make_new, &call_count,
                                   test_arena(), test_arena(), S("failed to write page"),
                                   &log) == -1);
  TEST_CHECK(
      test_log_is(&log, "failed to write page: %s ('public/failed.html')\n", strerror(ENOSPC)));
  TEST_CHECK(call_count == 3);
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(paths) / sizeof(paths[0])); i++) {
    os_memory_fail(writer.os, OS_MEMORY_OP_STAT, paths[i], EACCES);
  }

  log = test_log();
  TEST_CHECK(output_cache_write(&writer, S(CACHE_PATH), NULL, 0, test_arena(), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(writer.os, CACHE_PATH,
                               "output cache v1\n"
                               "output\tcopy.css\tlen=4\t4\n"
                               "output\tkept.html\tx=2\t5\n"
                               "output\tsame.html\tx=3\t3\n"
                               "output\twritten.html\tx=1\t3\n"));
}

// A cache loads back the rows and facts it was written with, and finds neither a path nor a key it
// does not hold.
static void test_load_round_trips_a_written_cache(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_built_writer(&writer, &manifest, false, &arena);
  struct Log log = test_log();
  TEST_CHECK(output_cache_write(&writer, S(CACHE_PATH), FACTS, 2, test_arena(), &log) == 0);

  struct Arena perm = test_arena();
  struct OutputCache cache;
  struct Str reason = {0};
  TEST_CHECK(load(writer.os, &perm, &cache, &reason) == BUILD_CACHE_FILE_LOADED);
  TEST_CHECK(cache.row_count == 2 && cache.fact_count == 2);
  const struct OutputCacheRow* row = output_cache_find(&cache, S("b.webp"));
  TEST_ASSERT(row != NULL);
  if (row == NULL) {
    return;
  }
  TEST_CHECK(str_eq(row->relative_path, S("b.webp")));
  TEST_CHECK(str_eq(row->attributes, S("src=2 width=640")));
  TEST_CHECK(row->len == 5);
  TEST_CHECK(output_cache_find(&cache, S("a.html")) != NULL);
  TEST_CHECK(output_cache_find(&cache, S("c.html")) == NULL);
  TEST_CHECK(output_cache_find(&cache, S("z.html")) == NULL);
  const struct OutputCacheFact* fact = output_cache_find_fact(&cache, S("photos/z.jpg"));
  TEST_ASSERT(fact != NULL);
  if (fact == NULL) {
    return;
  }
  TEST_CHECK(str_eq(fact->value, S("w=4000 h=3000")));
  TEST_CHECK(output_cache_find_fact(&cache, S("photos/b.jpg")) == NULL);
}

// A path, attributes, a key or a value that holds a tab or a newline gets no row, so the file still
// parses and every other row is kept.
static void test_write_leaves_out_a_field_with_a_tab_or_newline(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  TEST_CHECK(manifest_add(&manifest, S(CACHE_PATH), S("cache"), &arena, NULL) == 0);
  add_output(&manifest, "tab\tname.html", "x=1", &arena);
  add_output(&manifest, "tab.html", "x=1\ty=2", &arena);
  add_output(&manifest, "ok.html", "x=1", &arena);
  struct OutputWriter writer = {
      .manifest = &manifest, .os = os_memory(), .output_dir = S("public"), .perm = &arena};
  struct Log log = test_log();
  struct Str output_path = {0};
  while (manifest_next_output_path(&manifest, &output_path)) {
    TEST_CHECK(output_writer_write(&writer, output_path, (struct OsSource){.bytes = S("x")},
                                   S("failed to write output"), &log) == 0);
  }
  const struct OutputCacheFact facts[] = {
      {.key = S("a\nb"), .value = S("v")},
      {.key = S("c"), .value = S("v\tw")},
      {.key = S("d"), .value = S("")},
  };
  TEST_CHECK(output_cache_write(&writer, S(CACHE_PATH), facts, 3, test_arena(), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(writer.os, CACHE_PATH,
                               "output cache v1\n"
                               "output\tok.html\tx=1\t1\n"
                               "fact\td\t\n"));
  struct Arena perm = test_arena();
  struct OutputCache cache;
  struct Str reason = {0};
  TEST_CHECK(load(writer.os, &perm, &cache, &reason) == BUILD_CACHE_FILE_LOADED);
}

// A dry run writes no cache, and marks the cache's path written.
static void test_dry_run_writes_no_cache(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_built_writer(&writer, &manifest, true, &arena);
  os_memory_write(writer.os, CACHE_PATH, "old");
  struct Log log = test_log();
  TEST_CHECK(output_cache_write(&writer, S(CACHE_PATH), FACTS, 2, test_arena(), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(writer.os, CACHE_PATH, "old"));
  TEST_CHECK(manifest_find_mark(&manifest, S(CACHE_PATH)) == MANIFEST_MARK_WRITTEN);
}

// A missing cache is the usual first build, and loads as an empty cache.
static void test_load_gives_an_empty_cache_for_a_missing_file(void) {
  struct Arena perm = test_arena();
  struct OutputCache cache = {.row_count = 1};
  struct Str reason = {0};
  TEST_CHECK(load(os_memory(), &perm, &cache, &reason) == BUILD_CACHE_FILE_MISSING);
  TEST_CHECK(cache.row_count == 0 && cache.fact_count == 0);
}

// An interrupted build leaves the cache of the build before it, which the next build uses. It makes
// again an output that the interrupted build cut short, whose size no longer matches its row, and
// one that it rewrote whole under new attributes, here with the same size, and keeps the rest.
static void test_interrupted_build_leaves_a_cache_that_keeps_only_whole_outputs(void) {
  struct Os* os = os_memory();
  os_memory_write(os, CACHE_PATH,
                  "output cache v1\n"
                  "output\tcut.html\tx=1\t3\n"
                  "output\tkept.html\tx=2\t3\n"
                  "output\trewritten.html\tx=3\t3\n");
  os_memory_write(os, "public/cut.html", "ne");
  os_memory_write(os, "public/kept.html", "old");
  os_memory_write(os, "public/rewritten.html", "new");
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  TEST_CHECK(manifest_add(&manifest, S(CACHE_PATH), S("cache"), &arena, NULL) == 0);
  add_output(&manifest, "cut.html", "x=1", &arena);
  add_output(&manifest, "kept.html", "x=2", &arena);
  add_output(&manifest, "rewritten.html", "x=4", &arena);
  struct OutputWriter writer = {
      .manifest = &manifest, .os = os, .output_dir = S("public"), .perm = &arena};
  struct Arena perm = test_arena();
  struct OutputCache cache;
  struct Str reason = {0};
  TEST_CHECK(load(os, &perm, &cache, &reason) == BUILD_CACHE_FILE_LOADED);

  static const char* const paths[] = {"public/cut.html", "public/kept.html",
                                      "public/rewritten.html"};
  ptrdiff_t call_count = 0;
  struct Log log = test_log();
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(paths) / sizeof(paths[0])); i++) {
    TEST_CHECK(output_writer_produce(&writer, str_from_cstr(paths[i]), &cache, make_new,
                                     &call_count, test_arena(), test_arena(),
                                     S("failed to write page"), &log) == 0);
  }
  TEST_CHECK(output_cache_write(&writer, S(CACHE_PATH), NULL, 0, test_arena(), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(call_count == 2);
  TEST_CHECK(os_memory_is_file(os, "public/cut.html", "new"));
  TEST_CHECK(os_memory_is_file(os, "public/kept.html", "old"));
  TEST_CHECK(os_memory_is_file(os, "public/rewritten.html", "new"));
  TEST_CHECK(os_memory_is_file(os, CACHE_PATH,
                               "output cache v1\n"
                               "output\tcut.html\tx=1\t3\n"
                               "output\tkept.html\tx=2\t3\n"
                               "output\trewritten.html\tx=4\t3\n"));
}

// A file of an older version or of another kind is unusable, whatever its rows, and loads as an
// empty cache.
static void test_load_rejects_another_format(void) {
  static const char* const texts[] = {
      "output cache v0\noutput\ta.html\tblob=1\t3\n",
      "fram build cache v1\n",
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(texts) / sizeof(texts[0])); i++) {
    TEST_CASE(texts[i]);
    struct Os* os = os_memory();
    os_memory_write(os, CACHE_PATH, texts[i]);
    struct Arena perm = test_arena();
    struct OutputCache cache;
    struct Str reason = {0};
    TEST_CHECK(load(os, &perm, &cache, &reason) == BUILD_CACHE_FILE_UNUSABLE);
    TEST_CHECK(str_eq(reason, S("output cache is not in the current format")));
    TEST_MSG("reason: '%.*s'", STR_FMT(reason));
    TEST_CHECK(cache.row_count == 0 && cache.fact_count == 0);
  }
}

// A row of an unknown kind, one with a field missing, empty or extra, a size that is no number, an
// output after a fact, a row out of order or twice, and a last row cut short are each reported at
// their line, and the cache loads empty.
static void test_load_rejects_a_row_that_does_not_parse(void) {
  static const char* const cases[][2] = {
      {"output cache v1\nother\ta\n", "output cache does not parse at line 2"},
      {"output cache v1\noutput\n", "output cache does not parse at line 2"},
      {"output cache v1\noutput\ta.html\tblob=1\n", "output cache does not parse at line 2"},
      {"output cache v1\noutput\t\tblob=1\t3\n", "output cache does not parse at line 2"},
      {"output cache v1\noutput\ta.html\t\t3\n", "output cache does not parse at line 2"},
      {"output cache v1\noutput\ta.html\tblob=1\t-3\n", "output cache does not parse at line 2"},
      {"output cache v1\noutput\ta.html\tblob=1\t3\tx\n", "output cache does not parse at line 2"},
      {"output cache v1\nfact\tk\n", "output cache does not parse at line 2"},
      {"output cache v1\nfact\tk\tv\tw\n", "output cache does not parse at line 2"},
      {"output cache v1\nfact\tk\tv\noutput\ta.html\tblob=1\t3\n",
       "output cache does not parse at line 3"},
      {"output cache v1\noutput\tb.html\tx=1\t1\noutput\ta.html\tx=1\t1\n",
       "output cache does not parse at line 3"},
      {"output cache v1\noutput\ta.html\tx=1\t1\noutput\ta.html\tx=1\t1\n",
       "output cache does not parse at line 3"},
      {"output cache v1\nfact\tk\tv\nfact\tk\tw\n", "output cache does not parse at line 3"},
      {"output cache v1\nfact\tk\tv", "output cache does not parse at line 2"},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    TEST_CASE(cases[i][0]);
    struct Os* os = os_memory();
    os_memory_write(os, CACHE_PATH, cases[i][0]);
    struct Arena perm = test_arena();
    struct OutputCache cache;
    struct Str reason = {0};
    TEST_CHECK(load(os, &perm, &cache, &reason) == BUILD_CACHE_FILE_UNUSABLE);
    TEST_CHECK(str_eq(reason, str_from_cstr(cases[i][1])));
    TEST_MSG("reason: '%.*s'", STR_FMT(reason));
    TEST_CHECK(cache.row_count == 0 && cache.fact_count == 0);
  }
}

TEST_LIST = {
    {"write records each written or kept output and each fact",
     test_write_records_each_written_or_kept_output_and_each_fact},
    {"write takes each size from its mark", test_write_takes_each_size_from_its_mark},
    {"load round trips a written cache", test_load_round_trips_a_written_cache},
    {"write leaves out a field with a tab or newline",
     test_write_leaves_out_a_field_with_a_tab_or_newline},
    {"dry run writes no cache", test_dry_run_writes_no_cache},
    {"load gives an empty cache for a missing file",
     test_load_gives_an_empty_cache_for_a_missing_file},
    {"interrupted build leaves a cache that keeps only whole outputs",
     test_interrupted_build_leaves_a_cache_that_keeps_only_whole_outputs},
    {"load rejects another format", test_load_rejects_another_format},
    {"load rejects a row that does not parse", test_load_rejects_a_row_that_does_not_parse},
    {NULL, NULL},
};
