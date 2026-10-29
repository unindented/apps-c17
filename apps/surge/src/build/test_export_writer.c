#include <acutest.h>
#include <stdbool.h>
#include <stddef.h>

#include "build/export_writer.h"
#include "build/repo_scan.h"
#include "core/arena.h"
#include "core/log.h"
#include "core/path_list.h"
#include "core/str.h"
#include "domain/site_config.h"
#include "os/os.h"
#include "os/os_memory.h"
#include "output/manifest.h"
#include "output/manifest_builder.h"
#include "output/output_cache.h"
#include "output/output_writer.h"
#include "testing/test_support.h"

/** Attributes the cache recorded for the kept pack. */
#define PACK_ATTRIBUTES "src=pack-abc.pack len=4 mtime=0.000000000"

/** Attributes the cache recorded for the kept pack's idx. */
#define IDX_ATTRIBUTES "src=pack-abc.idx len=3 mtime=0.000000000"

/** A clone export, its manifest and its writer, below `public` on the in-memory host. */
struct ExportFixture {
  /** Configuration of the repo `r`. */
  struct SurgeRepo repo;

  /** Scan of `r`, whose pack `abc` the scan kept. */
  struct RepoScan scan;

  /** Manifest of the export's outputs. */
  struct Manifest manifest;

  /** Writer of the build. */
  struct OutputWriter writer;

  /** Rows the cache recorded for the pack and its idx, sorted by path. */
  struct OutputCacheRow rows[2];

  /** Cache that holds `rows`. */
  struct OutputCache cache;

  /** Arena that owns the manifest's entries. */
  struct Arena perm;
};

/**
 * @brief Fills `fixture` with an export whose pack `abc` the scan kept, records its outputs, and
 *        writes the pack and its idx in the output with the sizes the cache recorded.
 *
 * @param fixture Fixture to fill. Must not be `NULL`.
 */
static void export_fixture_init(struct ExportFixture* fixture) {
  *fixture = (struct ExportFixture){
      .repo = {.name = S("r")},
      .rows =
          {
              {.relative_path = S("r.git/objects/pack/pack-abc.idx"),
               .attributes = S(IDX_ATTRIBUTES),
               .len = 3},
              {.relative_path = S("r.git/objects/pack/pack-abc.pack"),
               .attributes = S(PACK_ATTRIBUTES),
               .len = 4},
          },
      .perm = test_arena(),
  };
  fixture->cache = (struct OutputCache){.rows = fixture->rows, .row_count = 2};
  fixture->scan.model.config = &fixture->repo;
  fixture->scan.export = (struct RepoExport){
      .head = S("ref: refs/heads/main\n"),
      .info_refs = S("refs\n"),
      .packs = S("P pack-abc.pack\n"),
      .pack_id = S("abc"),
      .pack_len = 4,
      .is_pack_kept = true,
      .pack_attributes = S(PACK_ATTRIBUTES),
      .idx_attributes = S(IDX_ATTRIBUTES),
  };
  struct Os* os = os_memory();
  os_memory_write(os, "public/r.git/objects/pack/pack-abc.pack", "PACK");
  os_memory_write(os, "public/r.git/objects/pack/pack-abc.idx", "IDX");
  struct ManifestBuilder builder;
  const struct PathList inputs = {0};
  manifest_builder_init(&builder, &fixture->manifest, S("public"), NULL, 0, &inputs, os,
                        &fixture->perm);
  struct Log log = test_log();
  TEST_CHECK(export_writer_register(&builder, &fixture->scan, test_arena(), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  fixture->writer = (struct OutputWriter){
      .manifest = &fixture->manifest, .os = os, .output_dir = S("public"), .perm = &fixture->perm};
}

// A pack the scan kept, still in the output with the size the cache recorded, is kept with its idx
// and left untouched, and the small files that name it are written.
static void test_write_keeps_a_current_pack(void) {
  struct ExportFixture fixture;
  export_fixture_init(&fixture);
  struct Log log = test_log();
  TEST_CHECK(export_writer_write(&fixture.writer, S("public"), &fixture.scan, &fixture.cache,
                                 test_arena(), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(manifest_find_mark(&fixture.manifest, S("public/r.git/objects/pack/pack-abc.pack")) ==
             MANIFEST_MARK_KEPT);
  TEST_CHECK(manifest_find_mark(&fixture.manifest, S("public/r.git/objects/pack/pack-abc.idx")) ==
             MANIFEST_MARK_KEPT);
  TEST_CHECK(
      os_memory_is_file(fixture.writer.os, "public/r.git/objects/pack/pack-abc.pack", "PACK"));
  TEST_CHECK(os_memory_is_file(fixture.writer.os, "public/r.git/info/refs", "refs\n"));
  TEST_CHECK(
      os_memory_is_file(fixture.writer.os, "public/r.git/objects/info/packs", "P pack-abc.pack\n"));
  TEST_CHECK(os_memory_is_file(fixture.writer.os, "public/r.git/HEAD", "ref: refs/heads/main\n"));
}

// A pack the scan kept that changed size before the export phase, as another process writing the
// output during the build would leave it, cannot be made again, since nothing of this build made
// it. The write fails with the pack's path, and writes neither its idx nor the files that name it.
static void test_write_fails_for_a_kept_pack_that_changed(void) {
  struct ExportFixture fixture;
  export_fixture_init(&fixture);
  os_memory_write(fixture.writer.os, "public/r.git/objects/pack/pack-abc.pack", "PACK!");
  struct Log log = test_log();
  TEST_CHECK(export_writer_write(&fixture.writer, S("public"), &fixture.scan, &fixture.cache,
                                 test_arena(), &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to keep clone pack: changed during the build "
                         "('public/r.git/objects/pack/pack-abc.pack')\n"));
  TEST_CHECK(manifest_find_mark(&fixture.manifest, S("public/r.git/objects/pack/pack-abc.idx")) ==
             MANIFEST_MARK_PENDING);
  TEST_CHECK(!os_memory_has_path(fixture.writer.os, "public/r.git/info/refs"));
}

TEST_LIST = {
    {"write keeps a current pack", test_write_keeps_a_current_pack},
    {"write fails for a kept pack that changed", test_write_fails_for_a_kept_pack_that_changed},
    {NULL, NULL},
};
