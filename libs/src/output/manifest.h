#ifndef APPS_OUTPUT_MANIFEST_H
#define APPS_OUTPUT_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>

#include "core/str.h"

struct Arena;

/** One recorded output path and the source that claims it. Defined in `manifest.c`. */
struct ManifestEntry;

/** What the build has done with one recorded output path. */
enum ManifestMark {
  /** Nothing yet. A path still pending after the writes is one the output writer never saw. */
  MANIFEST_MARK_PENDING,

  /** Written, or in a dry run, would be written. */
  MANIFEST_MARK_WRITTEN,

  /** Left as it is, because it is fresh. */
  MANIFEST_MARK_KEPT,
};

/**
 * What the manifest records about how one output is made, besides its path and its label. A zeroed
 * one declares no inputs and is no copy.
 */
struct ManifestOutput {
  /**
   * Text that names every input the output is made from, such as `blob=3f2a mode=100644`, and that
   * holds no tab or newline. The output writer keeps an output whose attributes equal those its
   * output cache recorded, so only an output that is expensive to make and whose inputs can be
   * named exactly declares them. Empty for an output that declares no inputs, which is always made.
   */
  struct Str attributes;

  /** Input file the output is a copy of, or empty for an output the build makes. */
  struct Str copy_source_path;

  /**
   * Size in bytes of `copy_source_path` in the stat its attributes were made from, which the output
   * writer records as the size of a copy it streams. `0` gives no size, so the output cache stats a
   * copy of an empty source, or one recorded without its source's size, instead.
   */
  ptrdiff_t copy_source_len;
};

/**
 * Insertion-ordered set of intended build output paths, each mapped to its producing source. A
 * zeroed manifest is empty and ready to add to.
 */
struct Manifest {
  /**
   * Root of a hash trie over the entries, keyed by the ASCII-folded output path. Managed
   * internally: `struct ManifestEntry` is incomplete here and no accessor exposes it.
   */
  struct ManifestEntry* root;

  /** First entry recorded. Entries link to the next in insertion order. Managed internally. */
  struct ManifestEntry* head;

  /** Last entry recorded, which the next one links after. Managed internally. */
  struct ManifestEntry* tail;

  /** Number of recorded output paths. */
  ptrdiff_t count;
};

/**
 * @brief Records `output_path` and the source that produces it, rejecting a path already claimed
 *        under ASCII case folding.
 *
 * The manifest copies both strings into `arena`, so the arguments need not outlive the call.
 * `manifest_add` reports a path already present as a duplicate and leaves the manifest unchanged.
 *
 * The manifest keeps its entries in insertion order, the order `manifest_next_output_path` visits
 * them in.
 *
 * Duplicate means equal after folding ASCII `A-Z` to `a-z`, so `About/index.html` and
 * `about/index.html` collide on every platform, as they would on a case-insensitive filesystem.
 * Every other byte compares exactly: the manifest does not attempt locale-dependent folding or
 * Unicode normalization. A `0` is therefore not proof that the path can be created: two outputs
 * where one is a `/`-delimited prefix of the other, like a file `a/b` beside `a/b/c`, are each
 * recorded here yet still collide on disk. `manifest_find_prefix_collision` catches that pair once
 * every path is recorded.
 *
 * @param manifest                  Manifest to append to. Must not be `NULL`.
 * @param output_path               Filesystem output path to record.
 * @param source_label              Diagnostic label for the source producing `output_path`.
 * @param arena                     Arena that owns the manifest's entries and copies. Every add to
 *                                  one manifest must use the same arena. Must not be `NULL`.
 * @param source_label_existing_out Receives the label of the earlier claim on `-1`. Ignored when
 *                                  `NULL`. The label is borrowed from the manifest and lives as
 *                                  long as `arena`.
 * @return `0` when the path was recorded, or `-1` when it was already claimed and the manifest is
 *         unchanged.
 */
int manifest_add(struct Manifest* manifest,
                 struct Str output_path,
                 struct Str source_label,
                 struct Arena* arena,
                 struct Str* source_label_existing_out) __attribute__((nonnull(1, 4)));

/**
 * @brief Records `output_path`, the source that produces it, and how it is made, as `manifest_add`
 *        records a path.
 *
 * The manifest copies every string into `arena`.
 *
 * @param manifest                  Manifest to append to. Must not be `NULL`.
 * @param output_path               Filesystem output path to record.
 * @param source_label              Diagnostic label for the source producing `output_path`.
 * @param output                    How the output is made. Must not be `NULL`.
 * @param arena                     Arena that owns the manifest's entries and copies, as for
 *                                  `manifest_add`. Must not be `NULL`.
 * @param source_label_existing_out Receives the label of the earlier claim on `-1`. Ignored when
 *                                  `NULL`.
 * @return `0` when the path was recorded, or `-1` when it was already claimed and the manifest is
 *         unchanged.
 */
int manifest_add_output(struct Manifest* manifest,
                        struct Str output_path,
                        struct Str source_label,
                        const struct ManifestOutput* output,
                        struct Arena* arena,
                        struct Str* source_label_existing_out) __attribute__((nonnull(1, 4, 5)));

/**
 * A pair of recorded output paths where one is a `/`-delimited prefix of the other under ASCII case
 * folding, so both cannot exist on disk: the shorter names a file. The longer needs that same name
 * as a directory.
 */
struct ManifestPrefixCollision {
  /** The shorter path, recorded as a file output. */
  struct Str ancestor_path;

  /** Diagnostic label of the source that produced `ancestor_path`. */
  struct Str ancestor_label;

  /** The longer path, which needs `ancestor_path` to be a directory. */
  struct Str descendant_path;

  /** Diagnostic label of the source that produced `descendant_path`. */
  struct Str descendant_label;
};

/**
 * @brief Finds the next recorded output path that has a `/`-delimited prefix recorded as another.
 *
 * `manifest_add` rejects only paths equal under ASCII case folding. This catches the other way two
 * outputs collide on disk: a file path that is also a directory prefix of a second output, like
 * `a/b` beside `a/b/c`. The prefix match folds ASCII case the same way, so `Dir/Sub` collides with
 * `dir/sub/index.html`. Run it after every output path is recorded. It examines each path's
 * ancestor prefixes, so it reports such a pair whichever order the two were added in.
 *
 * Each call resumes after the descendant of the pair `collision` holds, so a loop that starts from
 * a zeroed `collision` and calls this until it returns `false` visits every path that nests under
 * another once, paired with its shortest recorded ancestor.
 *
 * The reported strings are borrowed from the manifest and live as long as its arena.
 *
 * @param manifest  Manifest to scan. Must not be `NULL`.
 * @param collision The pair the previous call found, or zeroed for the first call. Receives the
 *                  next pair on a hit, and is untouched on a miss. Must not be `NULL`.
 * @return `true` and fills `collision` when another prefix collision exists, or `false` when every
 *         remaining recorded path can coexist on disk.
 */
bool manifest_find_prefix_collision(const struct Manifest* manifest,
                                    struct ManifestPrefixCollision* collision)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Finds the recorded output path after `output_path`, in the order the paths were recorded.
 *
 * A loop that starts from a zeroed `output_path` and calls this until it returns `false` visits
 * every recorded path once. The path is borrowed from the manifest and lives as long as its arena.
 *
 * @param manifest    Manifest to read. Must not be `NULL`.
 * @param output_path The path the previous call found, or zeroed for the first call. Receives the
 *                    next path on a hit, and is untouched on a miss. Must not be `NULL`.
 * @return `true` and fills `output_path` when a path was recorded after it, or `false` when none
 *         was.
 */
bool manifest_next_output_path(const struct Manifest* manifest, struct Str* output_path)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Marks the recorded output path equal to `output_path` under ASCII case folding, with the
 *        size of the file now at the path.
 *
 * It changes the entry's mark and size and nothing else, so jobs running at once may each mark a
 * path of their own while the others look theirs up, and the size is written by the thread that
 * writes the mark.
 *
 * @param manifest    Manifest whose entry is marked. Must not be `NULL`.
 * @param output_path Path to mark.
 * @param mark        Mark the entry receives.
 * @param len         Size in bytes of the file now at `output_path`, or `-1` when the caller does
 *                    not know it, as for `MANIFEST_MARK_PENDING`.
 * @return `0` when the path is recorded, or `-1` when it is not and nothing was marked.
 */
int manifest_mark(struct Manifest* manifest,
                  struct Str output_path,
                  enum ManifestMark mark,
                  ptrdiff_t len) __attribute__((nonnull(1)));

/**
 * @brief Finds the mark of the recorded output path equal to `output_path` under ASCII case
 *        folding.
 *
 * @param manifest    Manifest to read. Must not be `NULL`.
 * @param output_path Path whose mark is read.
 * @return The path's mark, or `MANIFEST_MARK_PENDING` when it is unmarked or not recorded.
 */
enum ManifestMark manifest_find_mark(const struct Manifest* manifest, struct Str output_path)
    __attribute__((nonnull(1)));

/**
 * @brief Finds the size that the mark of the recorded output path equal to `output_path` under
 *        ASCII case folding recorded.
 *
 * @param manifest    Manifest to read. Must not be `NULL`.
 * @param output_path Path whose size is read.
 * @return The size in bytes, or `-1` when the path is pending, was marked without a size, or is not
 *         recorded.
 */
ptrdiff_t manifest_find_len(const struct Manifest* manifest, struct Str output_path)
    __attribute__((nonnull(1)));

/**
 * @brief Finds how the recorded output path equal to `output_path` under ASCII case folding is
 *        made.
 *
 * What it reads never changes once the path is recorded, so jobs running at once may each read it
 * while they mark their paths.
 *
 * @param manifest    Manifest to read. Must not be `NULL`.
 * @param output_path Path to find.
 * @param output_out  Receives how the output is made, with strings borrowed from the manifest, when
 *                    the path is recorded. Must not be `NULL`.
 * @return `true` when the path is recorded, or `false` when it is not and `output_out` is
 *         untouched.
 */
bool manifest_find_output(const struct Manifest* manifest,
                          struct Str output_path,
                          struct ManifestOutput* output_out) __attribute__((nonnull(1, 3)));

#endif
