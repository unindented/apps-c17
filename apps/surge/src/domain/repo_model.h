#ifndef SURGE_REPO_MODEL_H
#define SURGE_REPO_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/str.h"
#include "domain/semver.h"

struct SurgeRepo;

/** What a ref is: a branch below `refs/heads/` or a tag below `refs/tags/`. */
enum RefKind {
  /** A branch, which gets tree, blob, raw and log pages. */
  REF_KIND_BRANCH,

  /** A tag, which gets a row on the tags page and a release page when it has one. */
  REF_KIND_TAG,
};

/** One branch or tag of a repository. */
struct Ref {
  /** Full name, such as `refs/heads/feature/x`. */
  struct Str name;

  /** Name below `refs/heads/` or `refs/tags/`, such as `feature/x`. */
  struct Str short_name;

  /** Whether it is a branch or a tag. */
  enum RefKind kind;

  /** Hex id of the object the ref points at: a commit, or for an annotated tag, the tag object. */
  struct Str oid;

  /**
   * Hex id of the object the ref peels to through every tag, or empty when the ref points at no tag
   * object. `info/refs` lists it on a `^{}` line.
   */
  struct Str peeled_oid;

  /** Hex id of the commit the ref peels to, or empty when it peels to a tree or a blob. */
  struct Str commit_oid;
};

/** The exported refs of one repository: its branches and its tags, sorted by full name. */
struct Refset {
  /** Refs, sorted bytewise by `name`. */
  struct Ref* refs;

  /** Number of entries in `refs`. */
  ptrdiff_t count;
};

/** What the pages show of a commit a ref names. */
struct CommitSummary {
  /** Hex id of the commit, or empty when the ref peels to no commit. */
  struct Str oid;

  /** First line of the message. */
  struct Str subject;

  /** Author's name. */
  struct Str author;

  /** RFC 3339 date of the commit, from its committer time. */
  struct Str date;

  /**
   * The signature of a signed commit, PGP, X.509 or SSH, from its `gpgsig` header with each line's
   * leading space removed, or empty. Nothing checks it.
   */
  struct Str signature;

  /**
   * The LLM tools that the message's trailers name, as `trailer_llm_tools` lists them for the
   * config's `llm_trailer_keys` and `llm_trailer_values`. The commit is LLM-assisted when there is
   * one.
   */
  struct Str* llm_tools;

  /** Number of entries in `llm_tools`. */
  ptrdiff_t llm_tool_count;
};

/** One trailer of a commit message, such as `Signed-off-by: Ada Lovelace <ada@example.org>`. */
struct Trailer {
  /** Key as the message writes it, such as `Co-Authored-By`. */
  struct Str key;

  /**
   * Value as the message writes it, address included, its continuation lines joined with one space
   * and its white space trimmed.
   */
  struct Str value;
};

/** What the site index's contributions calendar reads of one commit of a default branch. */
struct Contribution {
  /** Hex id of the commit, which counts it once across repos. */
  struct Str oid;

  /** Author time in seconds since the Unix epoch. */
  int64_t time;

  /** Offset of the author time from UTC, in minutes, as git recorded it. */
  int offset_minutes;

  /** Whether its trailers mark it LLM-assisted. */
  bool is_llm_assisted;
};

/** What a commit page shows of its commit. */
struct CommitDetail {
  /** Id, subject, author's name, committer date, signature and LLM tools. */
  struct CommitSummary summary;

  /** The message after its subject line and the blank lines after it, its trailers included. */
  struct Str body;

  /** The trailers of the message's last paragraph, as git finds them, in message order. */
  struct Trailer* trailers;

  /** Number of entries in `trailers`. */
  ptrdiff_t trailer_count;

  /** RFC 3339 date of the author's time. */
  struct Str author_date;

  /** Committer's name. */
  struct Str committer;

  /** Hex ids of the parents, in order, the first the one a diff is against. */
  struct Str* parents;

  /** Number of entries in `parents`. */
  ptrdiff_t parent_count;
};

/** What a release page shows of its tag. */
struct TagNote {
  /**
   * The annotated tag's message without its signature, or empty for a lightweight tag.
   */
  struct Str message;

  /**
   * The signature at the end of a signed tag's message, PGP, X.509 or SSH, as the tag holds it, or
   * empty. Nothing checks it.
   */
  struct Str signature;

  /**
   * RFC 3339 date: the tagger's of an annotated tag, else the committer date of the commit the tag
   * names, or empty when it names none.
   */
  struct Str date;

  /** The time of `date` in seconds since the Unix epoch, which orders the tags, or `0` for none. */
  int64_t time;

  /** The tagger's name of an annotated tag, or empty. */
  struct Str tagger;
};

/** What a tree entry is, from its mode. */
enum TreeEntryKind {
  /** A directory, which gets a tree page. */
  TREE_ENTRY_DIR,

  /** A regular file, which gets a blob page and a raw copy. */
  TREE_ENTRY_FILE,

  /** An executable file, which gets a blob page and a raw copy. */
  TREE_ENTRY_EXECUTABLE,

  /** A symlink, a row with its target text and a link to its target, which gets no page. */
  TREE_ENTRY_SYMLINK,

  /** A gitlink, a submodule's commit, a row that links nowhere. */
  TREE_ENTRY_GITLINK,
};

/** One entry of a directory of a branch's tree. */
struct TreeEntry {
  /** Name as the tree holds it. */
  struct Str name;

  /** Path from the root of the tree, the names joined by `/`. */
  struct Str path;

  /** What the entry is. */
  enum TreeEntryKind kind;

  /** Hex id of the entry's object: a tree, a blob, or a gitlink's commit. */
  struct Str oid;

  /** Size of a file or a symlink's blob in bytes, `0` for a directory or a gitlink. */
  ptrdiff_t len;

  /** A symlink's target, or empty for another kind. */
  struct Str link_target;

  /**
   * Whether the entry gets pages. A name a page cannot use, one that collides with another in its
   * directory after ASCII case folding, and one whose pages exceed an output-path limit get a row
   * without a link, and a directory of those gets no tree page and none of what is below it.
   */
  bool is_linked;
};

/** One directory of a branch's tree that gets a tree page. */
struct TreeDir {
  /** Path from the root of the tree, empty for the root. */
  struct Str path;

  /** Hex id of the directory's tree. */
  struct Str oid;

  /** The entries, directories first and then the rest, each group in byte order of name. */
  struct TreeEntry* entries;

  /** Number of entries in `entries`. */
  ptrdiff_t entry_count;
};

/** The tree of one branch: every directory that gets a page, and the paths links resolve to. */
struct BranchTree {
  /** The branch. */
  const struct Ref* branch;

  /** Directories in breadth-first order, the root first. */
  struct TreeDir* dirs;

  /** Number of entries in `dirs`. */
  ptrdiff_t dir_count;

  /** Every linked file, sorted bytewise by path, for link resolution. */
  const struct TreeEntry** files;

  /** Number of entries in `files`. */
  ptrdiff_t file_count;

  /** Every symlink of a directory with a tree page, sorted bytewise by path, for link
      resolution. */
  const struct TreeEntry** symlinks;

  /** Number of entries in `symlinks`. */
  ptrdiff_t symlink_count;

  /** Path of every linked directory but the root, sorted bytewise, for link resolution. */
  struct Str* dir_paths;

  /** Number of entries in `dir_paths`. */
  ptrdiff_t dir_path_count;
};

/** The history of one branch, newest first, as its log pages list it. */
struct BranchLog {
  /** The branch. */
  const struct Ref* branch;

  /** Hex ids of the commits the branch reaches, in `git log --date-order` order, newest first. */
  struct Str* commits;

  /** Number of entries in `commits`. */
  ptrdiff_t commit_count;
};

/** How a commit changed one file against its first parent. */
enum ChangeStatus {
  /** The file is new. */
  CHANGE_ADDED,

  /** The file is gone. */
  CHANGE_DELETED,

  /** The file's bytes or mode changed. */
  CHANGE_MODIFIED,

  /** The file moved, with the same bytes. */
  CHANGE_RENAMED,

  /** The entry changed kind, such as a file that became a symlink. */
  CHANGE_TYPE_CHANGED,
};

/** One line of a diff. */
struct DiffLine {
  /** `+` for an addition, `-` for a deletion, ` ` for context, or `\` for "no newline at end". */
  char origin;

  /** Number of the line in the old file, from 1, or `0` for an addition. */
  ptrdiff_t old_line;

  /** Number of the line in the new file, from 1, or `0` for a deletion. */
  ptrdiff_t new_line;

  /** Text of the line, without its newline. */
  struct Str text;
};

/** One hunk of a file's diff. */
struct DiffHunk {
  /** The hunk's header, such as `@@ -1,3 +1,4 @@ int main(void)`, without its newline. */
  struct Str header;

  /** Index of the hunk's first line in its file's `lines`. */
  ptrdiff_t line_index;

  /** Number of the hunk's lines. */
  ptrdiff_t line_count;
};

/** One file a commit changed. */
struct ChangedFile {
  /** How it changed. */
  enum ChangeStatus status;

  /** Path before the commit, empty for an added file. */
  struct Str old_path;

  /** Path after the commit, empty for a deleted file. */
  struct Str new_path;

  /** For a renamed file, how alike its two sides are, in percent, 100 for an exact rename, and 0
      for any other file. */
  ptrdiff_t similarity;

  /** Hex id of the blob before the commit, empty for an added file or a gitlink. */
  struct Str old_oid;

  /** Hex id of the blob after the commit, empty for a deleted file or a gitlink. */
  struct Str new_oid;

  /** Size of the blob before the commit in bytes, for a file whose diff the page shows, else 0. */
  ptrdiff_t old_len;

  /** Size of the blob after the commit in bytes, for a file whose diff the page shows, else 0. */
  ptrdiff_t new_len;

  /** Whether a side's blob is over `blob_len_max`, so no lines show. */
  bool is_too_large;

  /** Whether git judges either side binary, when neither is too large, so no lines show. */
  bool is_binary;

  /** Whether its diff is shown, which the diff caps decide in tree order. */
  bool is_shown;

  /** Whether the line or byte cap cut its diff short. */
  bool is_truncated;

  /** Lines added, counted over the lines shown. */
  ptrdiff_t additions;

  /** Lines deleted, counted over the lines shown. */
  ptrdiff_t deletions;

  /** The hunks shown. */
  struct DiffHunk* hunks;

  /** Number of entries in `hunks`. */
  ptrdiff_t hunk_count;

  /** The lines shown, every hunk's in order. */
  struct DiffLine* lines;

  /** Number of entries in `lines`. */
  ptrdiff_t line_count;
};

/** A commit's changes against its first parent, or against nothing for a root commit. */
struct Changeset {
  /**
   * The changed files, in the changed-files tree's order: at each level directories first, then
   * files, each bytewise by name, a file sorted by its new path, or its old one when deleted.
   */
  struct ChangedFile* files;

  /** Number of entries in `files`. */
  ptrdiff_t file_count;

  /** Lines added over every file shown. */
  ptrdiff_t additions;

  /** Lines deleted over every file shown. */
  ptrdiff_t deletions;

  /** Whether a cap left a file out or cut one short. */
  bool is_truncated;
};

/** One asset of a release: a regular file in its directory below `releases_dir`. */
struct ReleaseAsset {
  /** Name of the file, which its download keeps. */
  struct Str name;

  /** Path of the file, which the build copies. */
  struct Str source_path;

  /** Size of the file in bytes, which the output cache records. */
  ptrdiff_t len;

  /** Modification time in whole seconds since the Unix epoch, which the output cache records. */
  int64_t mtime_s;

  /** Nanoseconds within `mtime_s`. */
  int64_t mtime_ns;
};

/** One release: a tag that has a directory below its repo's `releases_dir`. */
struct Release {
  /** The tag. */
  const struct Ref* tag;

  /** The tag without its project's prefix, or for a repo's own project without a leading `v`. */
  struct Str version;

  /** Whether `version` parses as `MAJOR.MINOR.PATCH[-PRE]`, so `versions.txt` can list it. */
  bool is_semver;

  /** The parsed `version` when `is_semver`, which views it, else zeroed. */
  struct SemVer semver;

  /** Index of the release's project in `release_projects`, or `-1` when no project's prefix
      fits. */
  ptrdiff_t project_index;

  /** The tag's message, rendered as the notes, and its date. */
  struct TagNote note;

  /** The regular files of the directory, in byte order of name. */
  struct ReleaseAsset* assets;

  /** Number of entries in `assets`. */
  ptrdiff_t asset_count;

  /** Whether one of `assets` is `SHA256SUMS`. */
  bool has_sums;
};

/**
 * One release project: a project with a `tag_prefix`, or for a repo with `releases_dir` and no such
 * project, the repo itself.
 */
struct ReleaseProject {
  /** Name, the project's or the repo's, which its pages' directory takes. */
  struct Str name;

  /** Prefix of its tags, or empty for a repo's own project, whose tags may lead with `v`. */
  struct Str tag_prefix;

  /**
   * Its releases: those listed in `versions.txt` first, newest first by precedence, then the others
   * in byte order of tag.
   */
  const struct Release** releases;

  /** Number of entries in `releases`. */
  ptrdiff_t release_count;

  /** Number of releases at the start of `releases` that `versions.txt` lists. */
  ptrdiff_t version_count;

  /** Whether its pages show how to install it with mise, from the config's `mise`. */
  bool is_mise;
};

/**
 * What the scan of one repo found, which every page of the repo and the site index read. No field
 * holds a libgit2 type, so the pages need no `<git2.h>`.
 */
struct RepoModel {
  /** The repo's table in the config. */
  const struct SurgeRepo* config;

  /** Description: the config's, else the repository's `description` file, else empty. */
  struct Str description;

  /** Name of the default branch below `refs/heads/`, which an empty repository does not have
      yet. */
  struct Str default_branch;

  /** The default branch, or `NULL` for an empty repository. */
  const struct Ref* default_ref;

  /** RFC 3339 date of the default branch's newest commit, or empty for an empty repository. */
  struct Str modified;

  /** The branches and tags. */
  struct Refset refset;

  /** The commit each ref of `refset` peels to, in its order, zeroed for a ref that peels to
      none. */
  struct CommitSummary* ref_commits;

  /** The note of each tag of `refset`, in its order, zeroed for a branch. */
  struct TagNote* ref_notes;

  /** The tree of each branch that names a commit, in the order of the branches in `refset`. */
  struct BranchTree* trees;

  /** Number of entries in `trees`. */
  ptrdiff_t tree_count;

  /** The log of each branch that names a commit, in the order of `trees`. */
  struct BranchLog* logs;

  /** Number of entries in `logs`, which is `tree_count`. */
  ptrdiff_t log_count;

  /**
   * Every commit the default branch reaches, in the order of its log, for the contributions
   * calendar.
   */
  struct Contribution* contributions;

  /** Number of entries in `contributions`. */
  ptrdiff_t contribution_count;

  /** Hex id of every commit a branch or a tag reaches, sorted bytewise, each once. */
  struct Str* commits;

  /** Number of entries in `commits`. */
  ptrdiff_t commit_count;

  /** The releases, in byte order of tag. */
  struct Release* releases;

  /** Number of entries in `releases`. */
  ptrdiff_t release_count;

  /** The release projects: the repo's projects with a `tag_prefix`, in config order, or the repo's
      own. */
  struct ReleaseProject* release_projects;

  /** Number of entries in `release_projects`. */
  ptrdiff_t release_project_count;

  /** Common git directory of the repository, as `Repository` holds it, which no output may
      enter. */
  struct Str git_dir;

  /** Work tree of the repository, or empty for a bare one. */
  struct Str work_dir;
};

#endif
