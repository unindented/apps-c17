#ifndef SURGE_PAGE_REFS_H
#define SURGE_PAGE_REFS_H

#include <stddef.h>
#include <stdint.h>

#include "core/str.h"

struct Arena;
struct CommitSummary;
struct RepoModel;
struct SiteConfig;
struct TemplateValue;

/** Name of a branches page's template. */
#define PAGE_REFS_BRANCHES_TEMPLATE S("branches.html")

/** Name of a tags page's template. */
#define PAGE_REFS_TAGS_TEMPLATE S("tags.html")

/** Output path of a branches page below the repo's directory. */
#define PAGE_REFS_BRANCHES_PAGE S("branches/index.html")

/** Output path of a tags page below the repo's directory. */
#define PAGE_REFS_TAGS_PAGE S("tags/index.html")

/** A tag or a release, with the time and the name that order a list of them newest first. */
struct PageRefsDated {
  /** Index of the tag in the refset, or of the release in the repo's releases. */
  ptrdiff_t index;

  /** Seconds since the Unix epoch, `0` for none. */
  int64_t time;

  /** Name of the tag, which breaks a tie. */
  struct Str name;
};

/**
 * @brief Returns the data tree of a repo's branches page, `<r>/branches/index.html`.
 *
 * Besides `site`, `generator`, `path_to_root` and `repo`, it holds `branches`, one map per branch
 * in byte order of name, with `name`, `is_default`, `author` and `date` (those of its commit),
 * `tree_url`, `commits_url`, and the `commit` the branch names (`oid`, `short_oid`, `subject`,
 * `author`, `date` and `url`). A branch that names a tree or a blob has an empty `author`, `date`
 * and links, and a `commit` of `false`.
 *
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param repo        What the scan found for the repo. Must not be `NULL`.
 * @param arena       Arena that owns the tree. Must not be `NULL`.
 * @return The data tree.
 */
struct TemplateValue* page_refs_branches_value(const struct SiteConfig* site_config,
                                               const struct RepoModel* repo,
                                               struct Arena* arena)
    __attribute__((nonnull(1, 2, 3), returns_nonnull));

/**
 * @brief Returns the data tree of a repo's tags page, `<r>/tags/index.html`.
 *
 * Besides `site`, `generator`, `path_to_root` and `repo`, it holds `tags`, one map per tag, in the
 * order of `page_refs_tags_newest`, with `name`, `is_annotated`, `author` (as
 * `page_refs_tag_author` gives it), `date` (the RFC 3339 date that orders the tag, or empty for
 * none), `signature` (a signed tag's signature, or empty), `signature_kind`, `release_url` (its
 * release's page, or empty when it has none), and the `commit` the tag peels to, as the branches
 * page has it, `false` for a tag of a tree or a blob.
 *
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param repo        What the scan found for the repo. Must not be `NULL`.
 * @param arena       Arena that owns the tree. Must not be `NULL`.
 * @return The data tree.
 */
struct TemplateValue* page_refs_tags_value(const struct SiteConfig* site_config,
                                           const struct RepoModel* repo,
                                           struct Arena* arena)
    __attribute__((nonnull(1, 2, 3), returns_nonnull));

/**
 * @brief Returns the `commit` map of a ref's commit, as the refs pages and the repo home show it:
 *        `oid`, `short_oid`, `subject`, `author`, `date`, `signature` (a signed commit's signature,
 *        or empty), `signature_kind` (as `page_refs_signature_kind` names it) and `url`.
 *
 * @param repo         What the scan found for the repo. Must not be `NULL`.
 * @param commit       The commit, zeroed for none.
 * @param path_to_root Prefix from the page back to the output root.
 * @param arena        Arena that owns the map. Must not be `NULL`.
 * @return The map, or `false` for no commit, so a section over it enters only for a commit. A
 *         caller that adds fields to the map passes a commit.
 */
struct TemplateValue* page_refs_commit(const struct RepoModel* repo,
                                       struct CommitSummary commit,
                                       struct Str path_to_root,
                                       struct Arena* arena)
    __attribute__((nonnull(1, 4), returns_nonnull));

/**
 * @brief Returns a repo's tags newest first, as the tags page and the tag feed list them.
 *
 * A tag's time is its tagger's, else its commit's. A tie, and a tag with neither, which comes last,
 * sort by name in byte order.
 *
 * @param repo      What the scan found for the repo. Must not be `NULL`.
 * @param arena     Arena that owns the array. Must not be `NULL`.
 * @param count_out Receives the number of tags. Must not be `NULL`.
 * @return The tags, `*count_out` of them.
 */
struct PageRefsDated* page_refs_tags_newest(const struct RepoModel* repo,
                                            struct Arena* arena,
                                            ptrdiff_t* count_out)
    __attribute__((nonnull(1, 2, 3), returns_nonnull));

/**
 * @brief Sorts tags or releases newest first, a tie and an item of time `0`, which comes last, by
 *        name in byte order.
 *
 * @param items Items to sort. Must not be `NULL` unless `count` is `0`.
 * @param count Number of entries in `items`.
 */
void page_refs_sort_newest(struct PageRefsDated* items, ptrdiff_t count);

/**
 * @brief Returns the author of a tag: its tagger, else the author of its commit.
 *
 * @param repo  What the scan found for the repo. Must not be `NULL`.
 * @param index Index of the tag in the refset.
 * @return The author, or empty for neither.
 */
struct Str page_refs_tag_author(const struct RepoModel* repo, ptrdiff_t index)
    __attribute__((nonnull(1)));

/**
 * @brief Names the kind of a signature by the line that opens it, as git tells them apart: `PGP`
 *        for `-----BEGIN PGP SIGNATURE-----` or `-----BEGIN PGP MESSAGE-----`, `X.509` for
 *        `-----BEGIN SIGNED MESSAGE-----` and `SSH` for `-----BEGIN SSH SIGNATURE-----`.
 *
 * @param signature A signature, as a tag or a commit holds it.
 * @return The kind, or empty for no signature or one of another kind.
 */
struct Str page_refs_signature_kind(struct Str signature);

#endif
