#include "build/repo_pages.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "build/highlight_diff.h"
#include "build/page_blob.h"
#include "build/page_commit.h"
#include "build/page_feed.h"
#include "build/page_log.h"
#include "build/page_paths.h"
#include "build/page_refs.h"
#include "build/page_tree.h"
#include "build/readme.h"
#include "build/repo_scan.h"
#include "build/template_value.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/path.h"
#include "core/str.h"
#include "core/text.h"
#include "domain/grammar_path.h"
#include "domain/repo_model.h"
#include "domain/site_config.h"
#include "markup/highlight.h"
#include "os/oom.h"
#include "os/os.h"
#include "output/manifest_builder.h"
#include "output/output_cache.h"
#include "output/output_path.h"
#include "output/output_writer.h"
#include "runtime/job.h"
#include "templates/template_set.h"
#include "vcs/changeset.h"
#include "vcs/filetree.h"
#include "vcs/repository.h"
#include "vcs/revision.h"

/** Number of each repo's own pages: its home and its branches and tags pages. */
enum { REPO_PAGES_REPO_PAGE_COUNT = 3 };

/** Index of the branches page among a repo's own pages, as `repo_page` orders them. */
enum { REPO_PAGES_BRANCHES_INDEX = 1 };

/** Index of the tags page among a repo's own pages, as `repo_page` orders them. */
enum { REPO_PAGES_TAGS_INDEX = 2 };

/** Longest hex object id `repository_text_id` returns, a SHA-256 id's. */
enum { REPO_PAGES_ID_LEN_MAX = 64 };

/** Commit pages one job renders, so a repo of many commits does not make as many jobs. */
enum { REPO_PAGES_COMMIT_BATCH = 16 };

/**
 * Bytes the region of a blob holds past the blob: `arena_split` rounds a region down to 16 bytes, a
 * sanitizer build pads an allocation by up to 23, and an allocation leaves a byte of its arena
 * free.
 */
enum { REPO_PAGES_BLOB_SLACK = 64 };

/** What one job of the pages phase writes. */
enum RepoPageJobKind {
  /** A repo's home and its branches and tags pages. */
  REPO_PAGE_JOB_REPO,

  /** One directory of a branch: its tree page, and its files' blob pages and raw copies. */
  REPO_PAGE_JOB_DIR,

  /** One log page of a branch. */
  REPO_PAGE_JOB_LOG,

  /** Up to `REPO_PAGES_COMMIT_BATCH` commit pages. */
  REPO_PAGE_JOB_COMMITS,
};

/** One job of the pages phase. */
struct RepoPageJob {
  /** What the job writes. */
  enum RepoPageJobKind kind;

  /** Index of the repo in the config. */
  ptrdiff_t repo_index;

  /** Index of the branch's tree and log in the repo's model, for a directory or a log page. */
  ptrdiff_t tree_index;

  /**
   * Index of the directory in the tree, of the page in the log, or of the first commit in the
   * repo's commits.
   */
  ptrdiff_t item_index;
};

/** The repository one worker has open, which its next job of the same repo reuses. */
struct RepoPagesWorker {
  /** Index of the open repo, or `-1` for none. */
  ptrdiff_t repo_index;

  /** The open repository. */
  struct Repository repository;
};

/** What every job of one run shares. */
struct RepoPagesRun {
  /** What the phase reads. */
  const struct RepoPagesInputs* inputs;

  /** Writer every output goes through. */
  const struct OutputWriter* writer;

  /** The workers' arenas, whose index names a job's worker. */
  const struct JobArenas* worker_arenas;

  /** One slot per worker, which only that worker touches. */
  struct RepoPagesWorker* workers;

  /** The jobs. */
  const struct RepoPageJob* jobs;

  /** What each job left out, one per job, which only that job writes. */
  struct RepoPagesNotes* job_notes;
};

/** What a job needs to write one page or copy. */
struct RepoPagesOutput {
  /** Output path relative to `output_dir`. */
  struct Str relative_path;

  /** Output path below `output_dir`. */
  struct Str file_path;

  /** Name the manifest and a failure give the output's source. */
  struct Str label;
};

/** A file's blob, which its raw copy and its page share, read by whichever needs it first. */
struct RepoPagesBlob {
  /** Region of the job's scratch that the bytes go in, the blob's size and a little more. */
  struct Arena arena;

  /** The blob's bytes, once read. */
  struct Str data;

  /** Whether `git_blob_is_binary` judges the blob binary, once read. */
  bool is_binary;

  /** Whether the blob was read. */
  bool is_read;
};

/** What makes one output of a job, for `output_writer_produce`. */
struct RepoPagesMake {
  /** The run. */
  const struct RepoPagesRun* run;

  /** The job. */
  const struct RepoPageJob* job;

  /** Arenas of the worker. */
  struct JobArenas* arenas;

  /** The output. */
  const struct RepoPagesOutput* output;

  /** Index of one of a repo's own pages or feeds, or of a commit in the repo's commits. */
  ptrdiff_t index;

  /** The file of a blob page or a raw copy, else `NULL`. */
  const struct TreeEntry* entry;

  /** The blob of a blob page or a raw copy, else `NULL`. */
  struct RepoPagesBlob* blob;

  /** The page the make rendered, which `produce_page` gives back once it is written. */
  struct Str bytes;

  /** The region the page grew in, past the page. */
  struct Arena bytes_arena;
};

/**
 * @brief Records the paths of one directory's tree page and of its files' blob pages and raw
 *        copies, with their attributes.
 *
 * @param builder     Manifest being built. Must not be `NULL`.
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param scan        What the scan found for the repo. Must not be `NULL`.
 * @param tree        Tree of the branch. Must not be `NULL`.
 * @param dir         The directory. Must not be `NULL`.
 * @param recipe_id   Id of the page recipe.
 * @param scratch     Arena for each path and its attributes before the manifest copies them.
 * @param log         Log that receives every refusal. Must not be `NULL`.
 * @return `0` when every path was recorded, or `-1` otherwise.
 */
static int repo_pages_register_dir(struct ManifestBuilder* builder,
                                   const struct SiteConfig* site_config,
                                   const struct RepoScan* scan,
                                   const struct BranchTree* tree,
                                   const struct TreeDir* dir,
                                   struct Str recipe_id,
                                   struct Arena scratch,
                                   struct Log* log) __attribute__((nonnull(1, 2, 3, 4, 5, 8)));

/**
 * @brief Records one page with the attributes every page of its repo declares, `recipe=<id>` and
 *        `map=<id>`, followed by `inputs`.
 *
 * @param builder   Manifest being built. Must not be `NULL`.
 * @param scan      What the scan found for the page's repo. Must not be `NULL`.
 * @param output    The page. Must not be `NULL`.
 * @param recipe_id Id of the page recipe.
 * @param inputs    Attributes of the page's own inputs, such as `commit=<oid>`.
 * @param scratch   Arena for the attributes.
 * @param log       Log that receives the refusal. Must not be `NULL`.
 * @return `0` when the path was recorded, or `-1` otherwise.
 */
static int repo_pages_register_page(struct ManifestBuilder* builder,
                                    const struct RepoScan* scan,
                                    const struct RepoPagesOutput* output,
                                    struct Str recipe_id,
                                    struct Str inputs,
                                    struct Arena scratch,
                                    struct Log* log) __attribute__((nonnull(1, 2, 3, 7)));

/**
 * @brief Runs one job of the pages phase. This is the `JobFn` of `repo_pages_write`.
 *
 * @param index    Index of the job.
 * @param arenas   Arenas of the worker running the job. Must not be `NULL`.
 * @param log      Log of the job. Must not be `NULL`.
 * @param userdata `struct RepoPagesRun*`. Must not be `NULL`.
 * @return `0` when every output of the job was written or kept, or `-1` otherwise.
 */
static int repo_pages_job(ptrdiff_t index,
                          struct JobArenas* arenas,
                          struct Log* log,
                          void* userdata) __attribute__((nonnull(2, 3, 4)));

/**
 * @brief Writes a repo's home, its branches and tags pages and its feeds, or keeps each one.
 *
 * @param run    The run. Must not be `NULL`.
 * @param job    The job. Must not be `NULL`.
 * @param arenas Arenas of the worker. Must not be `NULL`.
 * @param log    Log of the job. Must not be `NULL`.
 * @return `0` on success, or `-1` when a render or a write fails.
 */
static int repo_pages_job_repo(const struct RepoPagesRun* run,
                               const struct RepoPageJob* job,
                               struct JobArenas* arenas,
                               struct Log* log) __attribute__((nonnull(1, 2, 3, 4)));

/**
 * @brief Makes one of a repo's own pages, as `OutputWriterMakeFn`.
 *
 * @param perm      Arena the page grows in. Must not be `NULL`.
 * @param scratch   Arena for the documents' and the render's temporaries.
 * @param bytes_out Receives the page. Must not be `NULL`.
 * @param log       Log of the job. Must not be `NULL`.
 * @param userdata  The `struct RepoPagesMake`, whose `index` names the page.
 * @return `0` on success, or `-1` when a document cannot be read or the render fails.
 */
static int repo_pages_job_repo_make(struct Arena* perm,
                                    struct Arena scratch,
                                    struct Str* bytes_out,
                                    struct Log* log,
                                    void* userdata) __attribute__((nonnull(1, 3, 4, 5)));

/**
 * @brief Builds the data tree of one of a repo's own pages, reading the home's documents.
 *
 * @param run        The run. Must not be `NULL`.
 * @param job        The job. Must not be `NULL`.
 * @param page_index Index of the page, as `repo_page` takes it.
 * @param output     The page's output. Must not be `NULL`.
 * @param arenas     Arenas of the worker. Must not be `NULL`.
 * @param perm       Arena that owns the data tree and the home's documents, set past the
 *                   repository's paths when this opens it. Must not be `NULL`.
 * @param scratch    Arena for the home's root and the documents' temporaries.
 * @param value_out  Receives the data tree. Must not be `NULL`.
 * @param log        Log of the job. Must not be `NULL`.
 * @return `0` on success, or `-1` when the repository or a document cannot be read.
 */
static int repo_pages_job_repo_value(const struct RepoPagesRun* run,
                                     const struct RepoPageJob* job,
                                     ptrdiff_t page_index,
                                     const struct RepoPagesOutput* output,
                                     struct JobArenas* arenas,
                                     struct Arena* perm,
                                     struct Arena scratch,
                                     const struct TemplateValue** value_out,
                                     struct Log* log) __attribute__((nonnull(1, 2, 4, 5, 6, 8, 9)));

/**
 * @brief Writes a repo's tag feed, and its release feed when it has release projects, or keeps each
 *        one.
 *
 * @param run    The run. Must not be `NULL`.
 * @param job    The job. Must not be `NULL`.
 * @param arenas Arenas of the worker. Must not be `NULL`.
 * @param log    Log of the job. Must not be `NULL`.
 * @return `0` on success, or `-1` when a render or a write fails.
 */
static int repo_pages_job_repo_feeds(const struct RepoPagesRun* run,
                                     const struct RepoPageJob* job,
                                     struct JobArenas* arenas,
                                     struct Log* log) __attribute__((nonnull(1, 2, 3, 4)));

/**
 * @brief Makes a repo's tag feed or release feed, as `OutputWriterMakeFn`.
 *
 * @param perm      Arena the feed grows in. Must not be `NULL`.
 * @param scratch   Arena for the render's temporaries.
 * @param bytes_out Receives the feed. Must not be `NULL`.
 * @param log       Log of the job. Must not be `NULL`.
 * @param userdata  The `struct RepoPagesMake`, whose `index` names the feed.
 * @return `0` on success, or `-1` when the render fails.
 */
static int repo_pages_job_repo_feeds_make(struct Arena* perm,
                                          struct Arena scratch,
                                          struct Str* bytes_out,
                                          struct Log* log,
                                          void* userdata) __attribute__((nonnull(1, 3, 4, 5)));

/**
 * @brief Writes one directory's tree page and its files' blob pages and raw copies, or keeps each
 *        one.
 *
 * @param run    The run. Must not be `NULL`.
 * @param job    The job. Must not be `NULL`.
 * @param arenas Arenas of the worker. Must not be `NULL`.
 * @param log    Log of the job. Must not be `NULL`.
 * @return `0` on success, or `-1` when a read, a render or a write fails.
 */
static int repo_pages_job_dir(const struct RepoPagesRun* run,
                              const struct RepoPageJob* job,
                              struct JobArenas* arenas,
                              struct Log* log) __attribute__((nonnull(1, 2, 3, 4)));

/**
 * @brief Writes one directory's tree page, with its documents, or keeps it.
 *
 * @param run    The run. Must not be `NULL`.
 * @param job    The job. Must not be `NULL`.
 * @param arenas Arenas of the worker. Must not be `NULL`.
 * @param log    Log of the job. Must not be `NULL`.
 * @return `0` on success, or `-1` when a document cannot be read, or the render or the write fails.
 */
static int repo_pages_job_dir_tree(const struct RepoPagesRun* run,
                                   const struct RepoPageJob* job,
                                   struct JobArenas* arenas,
                                   struct Log* log) __attribute__((nonnull(1, 2, 3, 4)));

/**
 * @brief Makes one directory's tree page, with its documents, as `OutputWriterMakeFn`.
 *
 * @param perm      Arena the page grows in. Must not be `NULL`.
 * @param scratch   Arena for the documents' and the render's temporaries.
 * @param bytes_out Receives the page. Must not be `NULL`.
 * @param log       Log of the job. Must not be `NULL`.
 * @param userdata  The `struct RepoPagesMake`.
 * @return `0` on success, or `-1` when a document cannot be read or the render fails.
 */
static int repo_pages_job_dir_tree_make(struct Arena* perm,
                                        struct Arena scratch,
                                        struct Str* bytes_out,
                                        struct Log* log,
                                        void* userdata) __attribute__((nonnull(1, 3, 4, 5)));

/**
 * @brief Writes one file's blob page and raw copy, or keeps each one.
 *
 * @param run    The run. Must not be `NULL`.
 * @param job    The job of the file's directory. Must not be `NULL`.
 * @param entry  The file. Must not be `NULL`.
 * @param arenas Arenas of the worker. Must not be `NULL`.
 * @param log    Log of the job. Must not be `NULL`.
 * @return `0` on success, or `-1` when a read, a render or a write fails.
 */
static int repo_pages_job_dir_file(const struct RepoPagesRun* run,
                                   const struct RepoPageJob* job,
                                   const struct TreeEntry* entry,
                                   struct JobArenas* arenas,
                                   struct Log* log) __attribute__((nonnull(1, 2, 3, 4, 5)));

/**
 * @brief Reads a file's blob into its region, unless a make of the file read it already.
 *
 * @param make Make of one of the file's outputs, whose `blob` receives the bytes. Must not be
 *             `NULL`.
 * @param log  Log of the job. Must not be `NULL`.
 * @return `0` on success, or `-1` when the repository or the blob cannot be read.
 */
static int repo_pages_job_dir_file_read(const struct RepoPagesMake* make, struct Log* log)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Makes one file's raw copy, its blob's bytes, as `OutputWriterMakeFn`.
 *
 * @param perm      Unused, as the bytes stay in the blob's region.
 * @param scratch   Unused.
 * @param bytes_out Receives the bytes. Must not be `NULL`.
 * @param log       Log of the job. Must not be `NULL`.
 * @param userdata  The `struct RepoPagesMake` of the copy.
 * @return `0` on success, or `-1` when the blob cannot be read.
 */
static int repo_pages_job_dir_file_raw_make(struct Arena* perm,
                                            struct Arena scratch,
                                            struct Str* bytes_out,
                                            struct Log* log,
                                            void* userdata) __attribute__((nonnull(1, 3, 4, 5)));

/**
 * @brief Makes one file's blob page, as `OutputWriterMakeFn`.
 *
 * A blob over `blob_len_max` is not read, as its page shows none of it.
 *
 * @param perm      Arena the page grows in. Must not be `NULL`.
 * @param scratch   Arena for the page's temporaries.
 * @param bytes_out Receives the page. Must not be `NULL`.
 * @param log       Log of the job. Must not be `NULL`.
 * @param userdata  The `struct RepoPagesMake` of the page.
 * @return `0` on success, or `-1` when the blob cannot be read or the render fails.
 */
static int repo_pages_job_dir_file_page_make(struct Arena* perm,
                                             struct Arena scratch,
                                             struct Str* bytes_out,
                                             struct Log* log,
                                             void* userdata) __attribute__((nonnull(1, 3, 4, 5)));

/**
 * @brief Writes one log page of a branch, and for its first page the branch's commit feed, or keeps
 *        each one.
 *
 * @param run    The run. Must not be `NULL`.
 * @param job    The job. Must not be `NULL`.
 * @param arenas Arenas of the worker. Must not be `NULL`.
 * @param log    Log of the job. Must not be `NULL`.
 * @return `0` on success, or `-1` when a commit cannot be read, or a render or a write fails.
 */
static int repo_pages_job_log(const struct RepoPagesRun* run,
                              const struct RepoPageJob* job,
                              struct JobArenas* arenas,
                              struct Log* log) __attribute__((nonnull(1, 2, 3, 4)));

/**
 * @brief Writes one log page of a branch, or keeps it.
 *
 * @param run    The run. Must not be `NULL`.
 * @param job    The job. Must not be `NULL`.
 * @param arenas Arenas of the worker. Must not be `NULL`.
 * @param log    Log of the job. Must not be `NULL`.
 * @return `0` on success, or `-1` when a commit cannot be read, or the render or the write fails.
 */
static int repo_pages_job_log_page(const struct RepoPagesRun* run,
                                   const struct RepoPageJob* job,
                                   struct JobArenas* arenas,
                                   struct Log* log) __attribute__((nonnull(1, 2, 3, 4)));

/**
 * @brief Makes one log page of a branch, as `OutputWriterMakeFn`.
 *
 * @param perm      Arena the page grows in. Must not be `NULL`.
 * @param scratch   Arena for the make's and the render's temporaries.
 * @param bytes_out Receives the page. Must not be `NULL`.
 * @param log       Log of the job. Must not be `NULL`.
 * @param userdata  The `struct RepoPagesMake`.
 * @return `0` on success, or `-1` when a commit cannot be read or the render fails.
 */
static int repo_pages_job_log_page_make(struct Arena* perm,
                                        struct Arena scratch,
                                        struct Str* bytes_out,
                                        struct Log* log,
                                        void* userdata) __attribute__((nonnull(1, 3, 4, 5)));

/**
 * @brief Writes a branch's commit feed, its newest `feed_count` commits, or keeps it.
 *
 * @param run    The run. Must not be `NULL`.
 * @param job    The job of the branch's first log page. Must not be `NULL`.
 * @param arenas Arenas of the worker. Must not be `NULL`.
 * @param log    Log of the job. Must not be `NULL`.
 * @return `0` on success, or `-1` when a commit cannot be read, or the render or the write fails.
 */
static int repo_pages_job_log_feed(const struct RepoPagesRun* run,
                                   const struct RepoPageJob* job,
                                   struct JobArenas* arenas,
                                   struct Log* log) __attribute__((nonnull(1, 2, 3, 4)));

/**
 * @brief Makes a branch's commit feed, as `OutputWriterMakeFn`.
 *
 * @param perm      Arena the page grows in. Must not be `NULL`.
 * @param scratch   Arena for the make's and the render's temporaries.
 * @param bytes_out Receives the page. Must not be `NULL`.
 * @param log       Log of the job. Must not be `NULL`.
 * @param userdata  The `struct RepoPagesMake`.
 * @return `0` on success, or `-1` when a commit cannot be read or the render fails.
 */
static int repo_pages_job_log_feed_make(struct Arena* perm,
                                        struct Arena scratch,
                                        struct Str* bytes_out,
                                        struct Log* log,
                                        void* userdata) __attribute__((nonnull(1, 3, 4, 5)));

/**
 * @brief Writes a batch of commit pages, or keeps each one.
 *
 * @param run    The run. Must not be `NULL`.
 * @param job    The job. Must not be `NULL`.
 * @param arenas Arenas of the worker. Must not be `NULL`.
 * @param log    Log of the job. Must not be `NULL`.
 * @return `0` on success, or `-1` when a commit cannot be read or diffed, or a render or a write
 *         fails.
 */
static int repo_pages_job_commits(const struct RepoPagesRun* run,
                                  const struct RepoPageJob* job,
                                  struct JobArenas* arenas,
                                  struct Log* log) __attribute__((nonnull(1, 2, 3, 4)));

/**
 * @brief Makes one commit page, with its diff, as `OutputWriterMakeFn`.
 *
 * @param perm      Arena the page grows in. Must not be `NULL`.
 * @param scratch   Arena for the make's and the render's temporaries.
 * @param bytes_out Receives the page. Must not be `NULL`.
 * @param log       Log of the job. Must not be `NULL`.
 * @param userdata  The `struct RepoPagesMake`, whose `index` names the commit.
 * @return `0` on success, or `-1` when the commit cannot be read or diffed, or the render fails.
 */
static int repo_pages_job_commits_make(struct Arena* perm,
                                       struct Arena scratch,
                                       struct Str* bytes_out,
                                       struct Log* log,
                                       void* userdata) __attribute__((nonnull(1, 3, 4, 5)));

/**
 * @brief Highlights both sides of each file of a commit whose diff shows lines, each side's blob
 *        whole, in the language its path names, and keeps the fragments of the lines the diff
 *        shows.
 *
 * @param repository    Open repository. Must not be `NULL`.
 * @param changeset     The commit's changes. Must not be `NULL`.
 * @param perm          Arena that owns the fragments. Must not be `NULL`.
 * @param scratch       Arena for the sides, the blobs and tree-sitter's memory.
 * @param fragments_out Receives one entry per file, the HTML of each of its lines, or `NULL` for a
 *                      file that shows no lines or whose paths name no grammar. Must not be `NULL`.
 * @param log           Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when a blob cannot be read.
 */
static int highlight_commit(const struct Repository* repository,
                            const struct Changeset* changeset,
                            struct Arena* perm,
                            struct Arena scratch,
                            const struct Str*** fragments_out,
                            struct Log* log) __attribute__((nonnull(1, 2, 3, 5, 6)));

/**
 * @brief Reads the sides of each changed image that a commit page embeds: in the files' order, the
 *        old side and then the new, while their sizes stay within `limits.image_len_max` in all.
 *
 * A file shows an image when the page shows its diff, its path has an image extension, and its type
 * did not change, as `page_commit_value` judges it. A side that would pass the budget is left out,
 * and a later, smaller one may still fit.
 *
 * @param repository Open repository. Must not be `NULL`.
 * @param changeset  The commit's changes. Must not be `NULL`.
 * @param limits     The build's limits. Must not be `NULL`.
 * @param perm       Arena that owns the bytes. Must not be `NULL`.
 * @param images_out Receives one entry per file, with empty sides for one that shows no image. Must
 *                   not be `NULL`.
 * @param log        Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when a blob cannot be read.
 */
static int read_commit_images(const struct Repository* repository,
                              const struct Changeset* changeset,
                              const struct SurgeLimits* limits,
                              struct Arena* perm,
                              struct ChangedImage** images_out,
                              struct Log* log) __attribute__((nonnull(1, 2, 3, 4, 5, 6)));

/**
 * @brief Highlights one side of a file's diff.
 *
 * @param repository    Open repository. Must not be `NULL`.
 * @param oid           Hex id of the side's blob, empty for a side with no blob.
 * @param path          Path of the side, whose language names the grammar.
 * @param perm          Arena that owns the fragments. Must not be `NULL`.
 * @param scratch       Arena for the blob and tree-sitter's memory.
 * @param fragments_out Receives the fragments, or `NULL` for no blob or no grammar. Must not be
 *                      `NULL`.
 * @param count_out     Receives the number of fragments. Must not be `NULL`.
 * @param log           Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the blob cannot be read.
 */
static int highlight_side(const struct Repository* repository,
                          struct Str oid,
                          struct Str path,
                          struct Arena* perm,
                          struct Arena scratch,
                          const struct Str** fragments_out,
                          ptrdiff_t* count_out,
                          struct Log* log) __attribute__((nonnull(1, 4, 6, 7, 8)));

/**
 * @brief Returns the repository of `repo_index`, opening it in the worker's slot when the slot
 *        holds another.
 *
 * @param run            The run. Must not be `NULL`.
 * @param repo_index     Index of the repo in the config.
 * @param arenas         Arenas of the worker, whose perm owns the repository's paths. Must not be
 *                       `NULL`.
 * @param repository_out Receives the open repository. Must not be `NULL`.
 * @param log            Log of the job. Must not be `NULL`.
 * @return `0` on success, or `-1` when the repository cannot be opened.
 */
static int repo_pages_open(const struct RepoPagesRun* run,
                           ptrdiff_t repo_index,
                           struct JobArenas* arenas,
                           const struct Repository** repository_out,
                           struct Log* log) __attribute__((nonnull(1, 3, 4, 5)));

/**
 * @brief Keeps or makes and writes a page through `output_writer_produce`, with half of `scratch`
 *        for the page to grow in and half for the make's temporaries and the read of the page in
 *        the output, and gives back the page's memory once it is written.
 *
 * @param make    Make of the page. Must not be `NULL`.
 * @param fn      Makes the page. Must not be `NULL`.
 * @param scratch Arena the page's regions come from.
 * @param log     Log of the job. Must not be `NULL`.
 * @return `0` on success, or `-1` when the make or the write fails.
 */
static int produce_page(struct RepoPagesMake* make,
                        OutputWriterMakeFn fn,
                        struct Arena scratch,
                        struct Log* log) __attribute__((nonnull(1, 2, 4)));

/**
 * @brief Renders a page into `perm`, and records it in `make`, so `produce_page` gives it back.
 *
 * @param make          Make of the page, whose output names it. Must not be `NULL`.
 * @param template_name Name of the page's template.
 * @param value         Data tree of the page. Must not be `NULL`.
 * @param perm          Region the page grows in, where nothing else allocates. Must not be `NULL`.
 * @param scratch       Arena for the render's temporaries.
 * @param bytes_out     Receives the page. Must not be `NULL`.
 * @param log           Log of the job. Must not be `NULL`.
 * @return `0` on success, or `-1` when the render fails.
 */
static int render_page(struct RepoPagesMake* make,
                       struct Str template_name,
                       const struct TemplateValue* value,
                       struct Arena* perm,
                       struct Arena scratch,
                       struct Str* bytes_out,
                       struct Log* log) __attribute__((nonnull(1, 3, 4, 6, 7)));

/**
 * @brief Finds and renders the documents that the page of `dir` shows, as `readme_find_documents`
 *        finds them.
 *
 * @param run           The run. Must not be `NULL`.
 * @param job           The job. Must not be `NULL`.
 * @param tree          Tree of the branch. Must not be `NULL`.
 * @param dir           Directory of the page, one of `tree`'s. Must not be `NULL`.
 * @param path_to_root  Prefix from the page back to the output root, which must not live in `perm`.
 * @param arenas        Arenas of the worker. Must not be `NULL`.
 * @param perm          Arena that owns the documents. When this opens the repository, which commits
 *                      its paths to the worker's perm, it is set to start past them first. Must not
 *                      be `NULL`.
 * @param scratch       Arena for each render's temporaries.
 * @param documents_out Receives the documents, in kind order. Must not be `NULL`.
 * @param count_out     Receives the number of documents. Must not be `NULL`.
 * @param log           Log of the job. Must not be `NULL`.
 * @return `0` on success, or `-1` when the repository or a document cannot be read.
 */
static int render_documents(const struct RepoPagesRun* run,
                            const struct RepoPageJob* job,
                            const struct BranchTree* tree,
                            const struct TreeDir* dir,
                            struct Str path_to_root,
                            struct JobArenas* arenas,
                            struct Arena* perm,
                            struct Arena scratch,
                            struct TreeDocument** documents_out,
                            ptrdiff_t* count_out,
                            struct Log* log) __attribute__((nonnull(1, 2, 3, 4, 6, 7, 9, 10, 11)));

/**
 * @brief Returns the output of a per-ref page or copy.
 *
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param repo        What the scan found for the repo. Must not be `NULL`.
 * @param tree        Tree of the branch. Must not be `NULL`.
 * @param kind        Kind of output.
 * @param path        Path in the tree, empty for the root.
 * @param arena       Arena that owns the paths. Must not be `NULL`.
 * @return The output.
 */
static struct RepoPagesOutput ref_output(const struct SiteConfig* site_config,
                                         const struct RepoModel* repo,
                                         const struct BranchTree* tree,
                                         enum PagePathKind kind,
                                         struct Str path,
                                         struct Arena* arena) __attribute__((nonnull(1, 2, 3, 6)));

/**
 * @brief Returns the output of one of a repo's own pages.
 *
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param repo        What the scan found for the repo. Must not be `NULL`.
 * @param page        Path of the page below the repo's directory.
 * @param arena       Arena that owns the paths. Must not be `NULL`.
 * @return The output.
 */
static struct RepoPagesOutput repo_output(const struct SiteConfig* site_config,
                                          const struct RepoModel* repo,
                                          struct Str page,
                                          struct Arena* arena) __attribute__((nonnull(1, 2, 4)));

/**
 * @brief Returns the output of a log page.
 *
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param repo        What the scan found for the repo. Must not be `NULL`.
 * @param tree        Tree of the branch. Must not be `NULL`.
 * @param page_index  Index of the page.
 * @param arena       Arena that owns the paths. Must not be `NULL`.
 * @return The output.
 */
static struct RepoPagesOutput log_output(const struct SiteConfig* site_config,
                                         const struct RepoModel* repo,
                                         const struct BranchTree* tree,
                                         ptrdiff_t page_index,
                                         struct Arena* arena) __attribute__((nonnull(1, 2, 3, 5)));

/**
 * @brief Returns the output of a commit page.
 *
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param repo        What the scan found for the repo. Must not be `NULL`.
 * @param commit_oid  Hex id of the commit.
 * @param arena       Arena that owns the paths. Must not be `NULL`.
 * @return The output.
 */
static struct RepoPagesOutput commit_output(const struct SiteConfig* site_config,
                                            const struct RepoModel* repo,
                                            struct Str commit_oid,
                                            struct Arena* arena) __attribute__((nonnull(1, 2, 4)));

/**
 * @brief Returns the id of what a log page reads: the number of pages, and each of its commits with
 *        the refs that name it.
 *
 * @param repo       What the scan found for the repo. Must not be `NULL`.
 * @param branch_log Log of the branch. Must not be `NULL`.
 * @param page_index Index of the page.
 * @param page_size  Commits a page lists.
 * @param arena      Arena that owns the id. Must not be `NULL`.
 * @return The id.
 */
static struct Str log_key(const struct RepoModel* repo,
                          const struct BranchLog* branch_log,
                          ptrdiff_t page_index,
                          ptrdiff_t page_size,
                          struct Arena* arena) __attribute__((nonnull(1, 2, 5)));

/**
 * @brief Returns the output of a branch's commit feed.
 *
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param repo        What the scan found for the repo. Must not be `NULL`.
 * @param tree        Tree of the branch. Must not be `NULL`.
 * @param arena       Arena that owns the paths. Must not be `NULL`.
 * @return The output.
 */
static struct RepoPagesOutput feed_output(const struct SiteConfig* site_config,
                                          const struct RepoModel* repo,
                                          const struct BranchTree* tree,
                                          struct Arena* arena) __attribute__((nonnull(1, 2, 3, 4)));

/**
 * @brief Returns the id of what a branch's commit feed reads: its newest `feed_count` commits, each
 *        of which decides its entry.
 *
 * @param branch_log Log of the branch. Must not be `NULL`.
 * @param feed_count Most commits the feed lists.
 * @param arena      Arena that owns the id. Must not be `NULL`.
 * @return The id.
 */
static struct Str commits_feed_key(const struct BranchLog* branch_log,
                                   ptrdiff_t feed_count,
                                   struct Arena* arena) __attribute__((nonnull(1, 3)));

/**
 * @brief Returns the id of what a repo's tag feed reads: each tag and the object it names, which
 *        decide its note and its commit, and the repo's releases, which its links name.
 *
 * @param scan  Scanned repo. Must not be `NULL`.
 * @param arena Arena that owns the id. Must not be `NULL`.
 * @return The id.
 */
static struct Str tags_feed_key(const struct RepoScan* scan, struct Arena* arena)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Returns the id of a key's text, which lies in `scratch`, a copy of `perm` past its next
 *        bytes, in `perm`.
 *
 * @param text    The key's text.
 * @param perm    Arena that owns the id. Must not be `NULL`.
 * @param scratch Copy of `perm` that holds `text`.
 * @return The id.
 */
static struct Str key_id(struct Str text, struct Arena* perm, struct Arena scratch)
    __attribute__((nonnull(2)));

/**
 * @brief Returns the attributes of what a tree page reads: `tree=` its tree, `root=` the branch's
 *        root tree when one of its documents is Markdown or Org or one of its entries is a symlink,
 *        whose links resolve against every path of the branch, and for the root's page `commit=`
 *        the branch's commit, whose summary it shows.
 *
 * @param tree         Tree of the branch. Must not be `NULL`.
 * @param dir          The directory. Must not be `NULL`.
 * @param blob_len_max Largest blob read for a page.
 * @param arena        Arena that owns the attributes. Must not be `NULL`.
 * @return The attributes.
 */
static struct Str tree_attributes(const struct BranchTree* tree,
                                  const struct TreeDir* dir,
                                  ptrdiff_t blob_len_max,
                                  struct Arena* arena) __attribute__((nonnull(1, 2, 4)));

/**
 * @brief Returns the attributes of what a blob page reads: `blob=` its blob, `mode=` its mode, and
 *        `root=` the branch's root tree when it is Markdown or Org.
 *
 * @param tree  Tree of the branch. Must not be `NULL`.
 * @param entry The file. Must not be `NULL`.
 * @param arena Arena that owns the attributes. Must not be `NULL`.
 * @return The attributes.
 */
static struct Str blob_attributes(const struct BranchTree* tree,
                                  const struct TreeEntry* entry,
                                  struct Arena* arena) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Returns one of a repo's own pages below its directory.
 *
 * @param index Index of the page: the home, then the branches and the tags pages.
 * @return The page's path.
 */
static struct Str repo_page(ptrdiff_t index);

/**
 * @brief Returns the template of one of a repo's own pages.
 *
 * @param site_config Configuration whose `[templates]` table names the template. Must not be
 *                    `NULL`.
 * @param index       Index of the page, as `repo_page` takes it.
 * @return The template's name.
 */
static struct Str repo_page_template(const struct SiteConfig* site_config, ptrdiff_t index)
    __attribute__((nonnull(1)));

/**
 * @brief Returns the number of a repo's own feeds: its tag feed, and its release feed when it has
 *        release projects.
 *
 * @param repo What the scan found for the repo. Must not be `NULL`.
 * @return The number of feeds, which `repo_feed_page` takes an index below.
 */
static ptrdiff_t repo_feed_count(const struct RepoModel* repo) __attribute__((nonnull(1)));

/**
 * @brief Returns the output path below the repo's directory of one of a repo's own feeds.
 *
 * @param index Index of the feed: `0` for the tag feed, `1` for the release feed.
 * @return The path.
 */
static struct Str repo_feed_page(ptrdiff_t index);

/**
 * @brief Lists the jobs of the pages phase: for each repo its own pages, then for each branch one
 *        job per directory and one per log page, then one per `REPO_PAGES_COMMIT_BATCH` commit
 *        pages.
 *
 * @param inputs    What the phase reads. Must not be `NULL`.
 * @param perm      Arena that owns the jobs. Must not be `NULL`.
 * @param count_out Receives the number of jobs. Must not be `NULL`.
 * @return The jobs.
 */
static struct RepoPageJob* plan_jobs(const struct RepoPagesInputs* inputs,
                                     struct Arena* perm,
                                     ptrdiff_t* count_out) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Splits off half of what is left of `arena`, up to its limit.
 *
 * @param arena Arena to split. Must not be `NULL`.
 * @return The region, the first half of the free space.
 */
static struct Arena split_half(struct Arena* arena) __attribute__((nonnull(1)));

int repo_pages_register(struct ManifestBuilder* builder,
                        const struct SiteConfig* site_config,
                        const struct RepoScan* scans,
                        struct Str recipe_id,
                        struct Arena scratch,
                        struct Log* log) {
  int rc = 0;
  for (ptrdiff_t i = 0; i < site_config->repo_count; i++) {
    const struct RepoScan* scan = &scans[i];
    const struct RepoModel* repo = &scan->model;
    for (ptrdiff_t j = 0; j < REPO_PAGES_REPO_PAGE_COUNT; j++) {
      // The home and the refs pages show the repo and every ref, and the tags page links each tag's
      // release.
      struct Arena page_scratch = scratch;
      const struct RepoPagesOutput output =
          repo_output(site_config, repo, repo_page(j), &page_scratch);
      struct Buf inputs = {.arena = &page_scratch};
      buf_printf(&inputs, "repo=%.*s refs=%.*s", STR_FMT(scan->repo_id),
                 STR_FMT(scan->export.info_refs_id));
      if (j == REPO_PAGES_TAGS_INDEX) {
        buf_printf(&inputs, " releases=%.*s", STR_FMT(scan->releases_id));
      }
      const struct Str text = buf_str(&inputs);
      if (repo_pages_register_page(builder, scan, &output, recipe_id, text, page_scratch, log) !=
          0) {
        rc = -1;
      }
    }
    const ptrdiff_t feed_count = repo_feed_count(repo);
    for (ptrdiff_t j = 0; j < feed_count; j++) {
      struct Arena page_scratch = scratch;
      const struct RepoPagesOutput output =
          repo_output(site_config, repo, repo_feed_page(j), &page_scratch);
      const struct Str text =
          j == 0 ? str_concat(S("feed="), tags_feed_key(scan, &page_scratch), &page_scratch)
                 : str_concat(S("releases="), scan->releases_id, &page_scratch);
      if (repo_pages_register_page(builder, scan, &output, recipe_id, text, page_scratch, log) !=
          0) {
        rc = -1;
      }
    }
    for (ptrdiff_t j = 0; j < repo->tree_count; j++) {
      const struct BranchTree* tree = &repo->trees[j];
      for (ptrdiff_t k = 0; k < tree->dir_count; k++) {
        if (repo_pages_register_dir(builder, site_config, scan, tree, &tree->dirs[k], recipe_id,
                                    scratch, log) != 0) {
          rc = -1;
        }
      }
      const ptrdiff_t page_count =
          page_log_count(repo->logs[j].commit_count, site_config->log_page_size);
      for (ptrdiff_t k = 0; k < page_count; k++) {
        struct Arena page_scratch = scratch;
        const struct RepoPagesOutput output = log_output(site_config, repo, tree, k, &page_scratch);
        const struct Str key =
            log_key(repo, &repo->logs[j], k, site_config->log_page_size, &page_scratch);
        const struct Str text = str_concat(S("log="), key, &page_scratch);
        if (repo_pages_register_page(builder, scan, &output, recipe_id, text, page_scratch, log) !=
            0) {
          rc = -1;
        }
      }
      // A branch with log pages has a commit feed.
      if (page_count > 0) {
        struct Arena page_scratch = scratch;
        const struct RepoPagesOutput output = feed_output(site_config, repo, tree, &page_scratch);
        const struct Str key =
            commits_feed_key(&repo->logs[j], site_config->feed_count, &page_scratch);
        const struct Str text = str_concat(S("feed="), key, &page_scratch);
        if (repo_pages_register_page(builder, scan, &output, recipe_id, text, page_scratch, log) !=
            0) {
          rc = -1;
        }
      }
    }
    // A commit page reads only its commit and its repo's `repo` map.
    for (ptrdiff_t j = 0; j < repo->commit_count; j++) {
      struct Arena page_scratch = scratch;
      const struct RepoPagesOutput output =
          commit_output(site_config, repo, repo->commits[j], &page_scratch);
      const struct Str text = str_concat(S("commit="), repo->commits[j], &page_scratch);
      if (repo_pages_register_page(builder, scan, &output, recipe_id, text, page_scratch, log) !=
          0) {
        rc = -1;
      }
    }
  }
  return rc;
}

int repo_pages_write(const struct RepoPagesInputs* inputs,
                     const struct OutputWriter* writer,
                     ptrdiff_t worker_count,
                     struct JobArenas* worker_arenas,
                     bool is_verbose,
                     struct Os* os,
                     struct Arena* perm,
                     struct Arena scratch,
                     struct RepoPagesNotes** notes_out,
                     struct Log* log) {
  const struct SiteConfig* site_config = inputs->site_config;
  ptrdiff_t job_count = 0;
  struct RepoPageJob* jobs = plan_jobs(inputs, perm, &job_count);
  struct RepoPagesWorker* workers = arena_new(perm, worker_count, struct RepoPagesWorker);
  for (ptrdiff_t i = 0; i < worker_count; i++) {
    workers[i].repo_index = -1;
  }
  struct RepoPagesRun run = {
      .inputs = inputs,
      .writer = writer,
      .worker_arenas = worker_arenas,
      .workers = workers,
      .jobs = jobs,
      .job_notes = arena_new(perm, job_count, struct RepoPagesNotes),
  };
  const int rc = job_run(job_count, worker_count, worker_arenas, repo_pages_job, &run,
                         S("rendering repo pages"), S(TEMPLATE_RENDER_ATTRIBUTION), is_verbose, os,
                         scratch, log);
  // The pool has joined its workers, so their repositories close here.
  for (ptrdiff_t i = 0; i < worker_count; i++) {
    repository_close(&workers[i].repository);
  }
  struct RepoPagesNotes* notes = arena_new(perm, site_config->repo_count, struct RepoPagesNotes);
  for (ptrdiff_t i = 0; i < job_count; i++) {
    notes[jobs[i].repo_index].missing_link_count += run.job_notes[i].missing_link_count;
    notes[jobs[i].repo_index].raw_skipped_count += run.job_notes[i].raw_skipped_count;
  }
  *notes_out = notes;
  return rc;
}

static int repo_pages_register_dir(struct ManifestBuilder* builder,
                                   const struct SiteConfig* site_config,
                                   const struct RepoScan* scan,
                                   const struct BranchTree* tree,
                                   const struct TreeDir* dir,
                                   struct Str recipe_id,
                                   struct Arena scratch,
                                   struct Log* log) {
  const struct RepoModel* repo = &scan->model;
  int rc = 0;
  // Only the relative paths go to the manifest, so the config the outputs need is not read here.
  struct Arena dir_scratch = scratch;
  struct Buf label = {.arena = &dir_scratch};
  buf_printf(&label, "%.*s:%.*s:%.*s", STR_FMT(repo->config->name),
             STR_FMT(tree->branch->short_name), STR_FMT(dir->path));
  const struct Str dir_label = buf_str(&label);
  const struct RepoPagesOutput dir_page = {
      .relative_path = page_path(PAGE_PATH_TREE, repo->config->name, tree->branch->short_name,
                                 dir->path, &dir_scratch),
      .label = dir_label,
  };
  const struct Str dir_inputs =
      tree_attributes(tree, dir, site_config->limits.blob_len_max, &dir_scratch);
  if (repo_pages_register_page(builder, scan, &dir_page, recipe_id, dir_inputs, dir_scratch, log) !=
      0) {
    rc = -1;
  }
  for (ptrdiff_t i = 0; i < dir->entry_count; i++) {
    const struct TreeEntry* entry = &dir->entries[i];
    if (!filetree_entry_has_pages(entry)) {
      continue;
    }
    struct Arena entry_scratch = scratch;
    struct Buf entry_label = {.arena = &entry_scratch};
    buf_printf(&entry_label, "%.*s:%.*s:%.*s", STR_FMT(repo->config->name),
               STR_FMT(tree->branch->short_name), STR_FMT(entry->path));
    const struct Str file_label = buf_str(&entry_label);
    const struct RepoPagesOutput blob_page = {
        .relative_path = page_path(PAGE_PATH_BLOB, repo->config->name, tree->branch->short_name,
                                   entry->path, &entry_scratch),
        .label = file_label,
    };
    const struct Str blob_inputs = blob_attributes(tree, entry, &entry_scratch);
    if (repo_pages_register_page(builder, scan, &blob_page, recipe_id, blob_inputs, entry_scratch,
                                 log) != 0) {
      rc = -1;
    }
    // A raw copy holds its blob's bytes, and nothing of the templates or the config's pages.
    if (entry->len <= site_config->limits.raw_len_max) {
      const struct Str raw_path = page_path(PAGE_PATH_RAW, repo->config->name,
                                            tree->branch->short_name, entry->path, &entry_scratch);
      const struct Str raw_attributes = str_concat(S("blob="), entry->oid, &entry_scratch);
      if (manifest_builder_add_output(builder, raw_path, file_label, raw_attributes, entry_scratch,
                                      log) != 0) {
        rc = -1;
      }
    }
  }
  return rc;
}

static int repo_pages_register_page(struct ManifestBuilder* builder,
                                    const struct RepoScan* scan,
                                    const struct RepoPagesOutput* output,
                                    struct Str recipe_id,
                                    struct Str inputs,
                                    struct Arena scratch,
                                    struct Log* log) {
  struct Buf attributes = {.arena = &scratch};
  buf_printf(&attributes, "recipe=%.*s map=%.*s %.*s", STR_FMT(recipe_id),
             STR_FMT(scan->repo_map_id), STR_FMT(inputs));
  const struct Str text = buf_str(&attributes);
  return manifest_builder_add_output(builder, output->relative_path, output->label, text, scratch,
                                     log);
}

static int repo_pages_job(ptrdiff_t index,
                          struct JobArenas* arenas,
                          struct Log* log,
                          void* userdata) {
  const struct RepoPagesRun* run = userdata;
  const struct RepoPageJob* job = &run->jobs[index];
  const struct RepoModel* repo = &run->inputs->scans[job->repo_index].model;
  oom_set_job(repo->config->name, log);
  switch (job->kind) {
    case REPO_PAGE_JOB_REPO:
      return repo_pages_job_repo(run, job, arenas, log);
    case REPO_PAGE_JOB_DIR:
      return repo_pages_job_dir(run, job, arenas, log);
    case REPO_PAGE_JOB_LOG:
      return repo_pages_job_log(run, job, arenas, log);
    case REPO_PAGE_JOB_COMMITS:
      return repo_pages_job_commits(run, job, arenas, log);
  }
  return -1;
}

static int repo_pages_job_repo(const struct RepoPagesRun* run,
                               const struct RepoPageJob* job,
                               struct JobArenas* arenas,
                               struct Log* log) {
  const struct RepoModel* repo = &run->inputs->scans[job->repo_index].model;
  int rc = 0;
  for (ptrdiff_t i = 0; i < REPO_PAGES_REPO_PAGE_COUNT; i++) {
    struct Arena scratch = arenas->scratch;
    const struct RepoPagesOutput output =
        repo_output(run->inputs->site_config, repo, repo_page(i), &scratch);
    struct RepoPagesMake make = {
        .run = run, .job = job, .arenas = arenas, .output = &output, .index = i};
    if (produce_page(&make, repo_pages_job_repo_make, scratch, log) != 0) {
      rc = -1;
    }
  }
  if (repo_pages_job_repo_feeds(run, job, arenas, log) != 0) {
    rc = -1;
  }
  return rc;
}

static int repo_pages_job_repo_make(struct Arena* perm,
                                    struct Arena scratch,
                                    struct Str* bytes_out,
                                    struct Log* log,
                                    void* userdata) {
  struct RepoPagesMake* make = userdata;
  // The data tree goes in a copy of the worker's perm.
  struct Arena value_perm = make->arenas->perm;
  const struct TemplateValue* value = NULL;
  if (repo_pages_job_repo_value(make->run, make->job, make->index, make->output, make->arenas,
                                &value_perm, scratch, &value, log) != 0) {
    return -1;
  }
  return render_page(make, repo_page_template(make->run->inputs->site_config, make->index), value,
                     perm, scratch, bytes_out, log);
}

static int repo_pages_job_repo_value(const struct RepoPagesRun* run,
                                     const struct RepoPageJob* job,
                                     ptrdiff_t page_index,
                                     const struct RepoPagesOutput* output,
                                     struct JobArenas* arenas,
                                     struct Arena* perm,
                                     struct Arena scratch,
                                     const struct TemplateValue** value_out,
                                     struct Log* log) {
  const struct SiteConfig* site_config = run->inputs->site_config;
  const struct RepoModel* repo = &run->inputs->scans[job->repo_index].model;
  if (page_index == REPO_PAGES_BRANCHES_INDEX) {
    *value_out = page_refs_branches_value(site_config, repo, perm);
    return 0;
  }
  if (page_index == REPO_PAGES_TAGS_INDEX) {
    *value_out = page_refs_tags_value(site_config, repo, perm);
    return 0;
  }
  // The root goes in the scratch, because the documents restart the perm past the repository's
  // paths when they open it.
  const struct Str path_to_root = output_path_to_root(output->relative_path, &scratch);
  const struct BranchTree* tree = repo_scan_default_tree(repo);
  const struct TreeDir* dir = tree != NULL ? &tree->dirs[0] : NULL;
  struct TreeDocument* documents = NULL;
  ptrdiff_t document_count = 0;
  if (tree != NULL && render_documents(run, job, tree, dir, path_to_root, arenas, perm, scratch,
                                       &documents, &document_count, log) != 0) {
    return -1;
  }
  *value_out = page_tree_value(site_config, repo, tree, dir, true, documents, document_count, perm);
  return 0;
}

static int repo_pages_job_repo_feeds(const struct RepoPagesRun* run,
                                     const struct RepoPageJob* job,
                                     struct JobArenas* arenas,
                                     struct Log* log) {
  const struct RepoModel* repo = &run->inputs->scans[job->repo_index].model;
  int rc = 0;
  for (ptrdiff_t i = 0; i < repo_feed_count(repo); i++) {
    struct Arena scratch = arenas->scratch;
    const struct RepoPagesOutput output =
        repo_output(run->inputs->site_config, repo, repo_feed_page(i), &scratch);
    struct RepoPagesMake make = {
        .run = run, .job = job, .arenas = arenas, .output = &output, .index = i};
    if (produce_page(&make, repo_pages_job_repo_feeds_make, scratch, log) != 0) {
      rc = -1;
    }
  }
  return rc;
}

static int repo_pages_job_repo_feeds_make(struct Arena* perm,
                                          struct Arena scratch,
                                          struct Str* bytes_out,
                                          struct Log* log,
                                          void* userdata) {
  struct RepoPagesMake* make = userdata;
  const struct SiteConfig* site_config = make->run->inputs->site_config;
  const struct RepoModel* repo = &make->run->inputs->scans[make->job->repo_index].model;
  const bool is_tags = make->index == 0;
  struct Arena value_perm = make->arenas->perm;
  const struct TemplateValue* value =
      is_tags ? page_feed_tags_value(site_config, repo, &value_perm)
              : page_feed_releases_value(site_config, repo, &value_perm, scratch);
  return render_page(
      make, is_tags ? site_config->templates.tag_feed : site_config->templates.release_feed, value,
      perm, scratch, bytes_out, log);
}

static int repo_pages_job_dir(const struct RepoPagesRun* run,
                              const struct RepoPageJob* job,
                              struct JobArenas* arenas,
                              struct Log* log) {
  const struct RepoModel* repo = &run->inputs->scans[job->repo_index].model;
  const struct TreeDir* dir = &repo->trees[job->tree_index].dirs[job->item_index];
  // The tree page and each file's pages are their own, so one that fails leaves the others to
  // report theirs.
  int rc = repo_pages_job_dir_tree(run, job, arenas, log);
  for (ptrdiff_t i = 0; i < dir->entry_count; i++) {
    if (filetree_entry_has_pages(&dir->entries[i]) &&
        repo_pages_job_dir_file(run, job, &dir->entries[i], arenas, log) != 0) {
      rc = -1;
    }
  }
  return rc;
}

static int repo_pages_job_dir_tree(const struct RepoPagesRun* run,
                                   const struct RepoPageJob* job,
                                   struct JobArenas* arenas,
                                   struct Log* log) {
  const struct RepoModel* repo = &run->inputs->scans[job->repo_index].model;
  const struct BranchTree* tree = &repo->trees[job->tree_index];
  struct Arena scratch = arenas->scratch;
  const struct RepoPagesOutput output =
      ref_output(run->inputs->site_config, repo, tree, PAGE_PATH_TREE,
                 tree->dirs[job->item_index].path, &scratch);
  struct RepoPagesMake make = {.run = run, .job = job, .arenas = arenas, .output = &output};
  return produce_page(&make, repo_pages_job_dir_tree_make, scratch, log);
}

static int repo_pages_job_dir_tree_make(struct Arena* perm,
                                        struct Arena scratch,
                                        struct Str* bytes_out,
                                        struct Log* log,
                                        void* userdata) {
  struct RepoPagesMake* make = userdata;
  const struct SiteConfig* site_config = make->run->inputs->site_config;
  const struct RepoModel* repo = &make->run->inputs->scans[make->job->repo_index].model;
  const struct BranchTree* tree = &repo->trees[make->job->tree_index];
  const struct TreeDir* dir = &tree->dirs[make->job->item_index];
  // The root goes in the scratch, because the documents restart the perm past the repository's
  // paths when they open it.
  const struct Str path_to_root = output_path_to_root(make->output->relative_path, &scratch);
  struct Arena value_perm = make->arenas->perm;
  struct TreeDocument* documents = NULL;
  ptrdiff_t document_count = 0;
  if (render_documents(make->run, make->job, tree, dir, path_to_root, make->arenas, &value_perm,
                       scratch, &documents, &document_count, log) != 0) {
    return -1;
  }
  const struct TemplateValue* value =
      page_tree_value(site_config, repo, tree, dir, false, documents, document_count, &value_perm);
  return render_page(make, site_config->templates.tree, value, perm, scratch, bytes_out, log);
}

static int repo_pages_job_dir_file(const struct RepoPagesRun* run,
                                   const struct RepoPageJob* job,
                                   const struct TreeEntry* entry,
                                   struct JobArenas* arenas,
                                   struct Log* log) {
  const struct SiteConfig* site_config = run->inputs->site_config;
  const struct RepoModel* repo = &run->inputs->scans[job->repo_index].model;
  const struct BranchTree* tree = &repo->trees[job->tree_index];
  struct Arena scratch = arenas->scratch;
  const struct RepoPagesOutput blob_page =
      ref_output(site_config, repo, tree, PAGE_PATH_BLOB, entry->path, &scratch);
  const struct RepoPagesOutput raw =
      ref_output(site_config, repo, tree, PAGE_PATH_RAW, entry->path, &scratch);
  const bool has_raw = entry->len <= site_config->limits.raw_len_max;
  run->job_notes[job - run->jobs].raw_skipped_count += !has_raw;
  // The blob's bytes, which only an output that is made reads, get a region of their own first, so
  // the copy and the page share one read, and their regions follow it.
  struct RepoPagesBlob blob = {0};
  if (has_raw || entry->len <= site_config->limits.blob_len_max) {
    blob.arena = arena_split(&scratch, entry->len + REPO_PAGES_BLOB_SLACK);
  }
  int rc = 0;
  if (has_raw) {
    // The copy's bytes stay in the blob's region, so the whole scratch is left for the read of the
    // copy in the output.
    struct RepoPagesMake make = {
        .run = run, .job = job, .arenas = arenas, .output = &raw, .entry = entry, .blob = &blob};
    rc = output_writer_produce(run->writer, raw.file_path, run->inputs->cache,
                               repo_pages_job_dir_file_raw_make, &make, scratch, scratch,
                               S("failed to write raw copy"), log);
  }
  if (rc == 0) {
    struct RepoPagesMake make = {.run = run,
                                 .job = job,
                                 .arenas = arenas,
                                 .output = &blob_page,
                                 .entry = entry,
                                 .blob = &blob};
    rc = produce_page(&make, repo_pages_job_dir_file_page_make, scratch, log);
  }
  // A large blob's pages are given back once its outputs are written, as fram gives back a large
  // source, so the worker does not keep the largest blob of its jobs resident.
  if (blob.data.len >= JOB_SOURCE_DISCARD_LEN_MIN) {
    arena_discard(&blob.arena, blob.data.data, blob.data.len);
  }
  return rc;
}

static int repo_pages_job_dir_file_read(const struct RepoPagesMake* make, struct Log* log) {
  struct RepoPagesBlob* blob = make->blob;
  if (blob->is_read) {
    return 0;
  }
  const struct Repository* repository = NULL;
  if (repo_pages_open(make->run, make->job->repo_index, make->arenas, &repository, log) != 0 ||
      repository_read_blob(repository, make->entry->oid, &blob->arena, &blob->data,
                           &blob->is_binary, log) != 0) {
    return -1;
  }
  blob->is_read = true;
  return 0;
}

static int repo_pages_job_dir_file_raw_make(struct Arena* perm,
                                            struct Arena scratch,
                                            struct Str* bytes_out,
                                            struct Log* log,
                                            void* userdata) {
  (void)perm;
  (void)scratch;
  const struct RepoPagesMake* make = userdata;
  if (repo_pages_job_dir_file_read(make, log) != 0) {
    return -1;
  }
  *bytes_out = make->blob->data;
  return 0;
}

static int repo_pages_job_dir_file_page_make(struct Arena* perm,
                                             struct Arena scratch,
                                             struct Str* bytes_out,
                                             struct Log* log,
                                             void* userdata) {
  struct RepoPagesMake* make = userdata;
  const struct SiteConfig* site_config = make->run->inputs->site_config;
  const struct RepoModel* repo = &make->run->inputs->scans[make->job->repo_index].model;
  const struct BranchTree* tree = &repo->trees[make->job->tree_index];
  const struct TreeDir* dir = &tree->dirs[make->job->item_index];
  const struct TreeEntry* entry = make->entry;
  const bool is_too_large = entry->len > site_config->limits.blob_len_max;
  if (!is_too_large && repo_pages_job_dir_file_read(make, log) != 0) {
    return -1;
  }
  const struct Str data = make->blob->data;
  const bool is_binary = make->blob->is_binary;
  // The rendered view and the data tree go in a copy of the perm taken after any open.
  struct Arena value_perm = make->arenas->perm;
  const char* perm_beg = value_perm.beg;
  struct BlobContent content = {
      .data = !is_too_large ? data : (struct Str){0},
      .is_too_large = is_too_large,
      .is_binary = !is_too_large && is_binary,
      .has_raw = entry->len <= site_config->limits.raw_len_max,
  };
  if (!is_too_large && !is_binary && readme_is_markup(entry->name)) {
    const struct DocumentPlace place = {
        .tree = tree,
        .repo_name = repo->config->name,
        .path_to_root = output_path_to_root(make->output->relative_path, &value_perm),
        .dir_path = dir->path,
        .raw_len_max = site_config->limits.raw_len_max,
    };
    ptrdiff_t missing_count = 0;
    content.rendered_html = readme_render_markup(data, readme_is_org(entry->name), &place,
                                                 &value_perm, scratch, &missing_count);
    make->run->job_notes[make->job - make->run->jobs].missing_link_count += missing_count;
  }
  const struct TemplateValue* value =
      page_blob_value(site_config, repo, tree, entry, &content, &value_perm, scratch);
  const int rc =
      render_page(make, site_config->templates.blob, value, perm, scratch, bytes_out, log);
  // The highlighted lines and the rendered view cost more to make than to fault in again, so their
  // pages are given back with the page's.
  arena_discard(&value_perm, perm_beg, value_perm.beg - perm_beg);
  return rc;
}

static int repo_pages_job_log(const struct RepoPagesRun* run,
                              const struct RepoPageJob* job,
                              struct JobArenas* arenas,
                              struct Log* log) {
  const int page_rc = repo_pages_job_log_page(run, job, arenas, log);
  const int feed_rc = job->item_index == 0 ? repo_pages_job_log_feed(run, job, arenas, log) : 0;
  return page_rc != 0 || feed_rc != 0 ? -1 : 0;
}

static int repo_pages_job_log_page(const struct RepoPagesRun* run,
                                   const struct RepoPageJob* job,
                                   struct JobArenas* arenas,
                                   struct Log* log) {
  const struct RepoModel* repo = &run->inputs->scans[job->repo_index].model;
  struct Arena scratch = arenas->scratch;
  const struct RepoPagesOutput output = log_output(
      run->inputs->site_config, repo, &repo->trees[job->tree_index], job->item_index, &scratch);
  struct RepoPagesMake make = {.run = run, .job = job, .arenas = arenas, .output = &output};
  return produce_page(&make, repo_pages_job_log_page_make, scratch, log);
}

static int repo_pages_job_log_page_make(struct Arena* perm,
                                        struct Arena scratch,
                                        struct Str* bytes_out,
                                        struct Log* log,
                                        void* userdata) {
  struct RepoPagesMake* make = userdata;
  const struct RepoPageJob* job = make->job;
  const struct SiteConfig* site_config = make->run->inputs->site_config;
  const struct RepoModel* repo = &make->run->inputs->scans[job->repo_index].model;
  const struct BranchTree* tree = &repo->trees[job->tree_index];
  const struct BranchLog* branch_log = &repo->logs[job->tree_index];
  const ptrdiff_t page_size = site_config->log_page_size;
  const ptrdiff_t page_count = page_log_count(branch_log->commit_count, page_size);
  const ptrdiff_t begin = job->item_index * page_size;
  const ptrdiff_t end =
      begin + page_size < branch_log->commit_count ? begin + page_size : branch_log->commit_count;
  const struct Repository* repository = NULL;
  if (repo_pages_open(make->run, job->repo_index, make->arenas, &repository, log) != 0) {
    return -1;
  }
  // The summaries and the data tree go in a copy of the perm taken after the open.
  struct Arena value_perm = make->arenas->perm;
  struct CommitSummary* commits = arena_new(&value_perm, end - begin, struct CommitSummary);
  for (ptrdiff_t i = begin; i < end; i++) {
    if (revision_commit_summary(repository, branch_log->commits[i], &site_config->llm_trailers,
                                &value_perm, &commits[i - begin], log) != 0) {
      return -1;
    }
  }
  const struct TemplateValue* value = page_log_value(site_config, repo, tree, job->item_index,
                                                     page_count, commits, end - begin, &value_perm);
  return render_page(make, site_config->templates.commit_list, value, perm, scratch, bytes_out,
                     log);
}

static int repo_pages_job_log_feed(const struct RepoPagesRun* run,
                                   const struct RepoPageJob* job,
                                   struct JobArenas* arenas,
                                   struct Log* log) {
  const struct RepoModel* repo = &run->inputs->scans[job->repo_index].model;
  struct Arena scratch = arenas->scratch;
  const struct RepoPagesOutput output =
      feed_output(run->inputs->site_config, repo, &repo->trees[job->tree_index], &scratch);
  struct RepoPagesMake make = {.run = run, .job = job, .arenas = arenas, .output = &output};
  return produce_page(&make, repo_pages_job_log_feed_make, scratch, log);
}

static int repo_pages_job_log_feed_make(struct Arena* perm,
                                        struct Arena scratch,
                                        struct Str* bytes_out,
                                        struct Log* log,
                                        void* userdata) {
  struct RepoPagesMake* make = userdata;
  const struct RepoPageJob* job = make->job;
  const struct SiteConfig* site_config = make->run->inputs->site_config;
  const struct RepoModel* repo = &make->run->inputs->scans[job->repo_index].model;
  const struct BranchTree* tree = &repo->trees[job->tree_index];
  const struct BranchLog* branch_log = &repo->logs[job->tree_index];
  const struct Repository* repository = NULL;
  if (repo_pages_open(make->run, job->repo_index, make->arenas, &repository, log) != 0) {
    return -1;
  }
  struct Arena value_perm = make->arenas->perm;
  const ptrdiff_t count = branch_log->commit_count < site_config->feed_count
                              ? branch_log->commit_count
                              : site_config->feed_count;
  struct CommitDetail* commits = arena_new(&value_perm, count, struct CommitDetail);
  for (ptrdiff_t i = 0; i < count; i++) {
    if (revision_commit_detail(repository, branch_log->commits[i], &site_config->llm_trailers,
                               &value_perm, &commits[i], log) != 0) {
      return -1;
    }
  }
  const struct TemplateValue* value =
      page_feed_commits_value(site_config, repo, tree, commits, count, &value_perm);
  return render_page(make, site_config->templates.commit_feed, value, perm, scratch, bytes_out,
                     log);
}

static int repo_pages_job_commits(const struct RepoPagesRun* run,
                                  const struct RepoPageJob* job,
                                  struct JobArenas* arenas,
                                  struct Log* log) {
  const struct RepoModel* repo = &run->inputs->scans[job->repo_index].model;
  const ptrdiff_t end = job->item_index + REPO_PAGES_COMMIT_BATCH < repo->commit_count
                            ? job->item_index + REPO_PAGES_COMMIT_BATCH
                            : repo->commit_count;
  // Each commit is a page of its own, so one that fails leaves the others to report theirs.
  int rc = 0;
  for (ptrdiff_t i = job->item_index; i < end; i++) {
    struct Arena scratch = arenas->scratch;
    const struct RepoPagesOutput output =
        commit_output(run->inputs->site_config, repo, repo->commits[i], &scratch);
    struct RepoPagesMake make = {
        .run = run, .job = job, .arenas = arenas, .output = &output, .index = i};
    if (produce_page(&make, repo_pages_job_commits_make, scratch, log) != 0) {
      rc = -1;
    }
  }
  return rc;
}

static int repo_pages_job_commits_make(struct Arena* perm,
                                       struct Arena scratch,
                                       struct Str* bytes_out,
                                       struct Log* log,
                                       void* userdata) {
  struct RepoPagesMake* make = userdata;
  const struct SiteConfig* site_config = make->run->inputs->site_config;
  const struct RepoModel* repo = &make->run->inputs->scans[make->job->repo_index].model;
  const struct Str commit_oid = repo->commits[make->index];
  const struct Repository* repository = NULL;
  if (repo_pages_open(make->run, make->job->repo_index, make->arenas, &repository, log) != 0) {
    return -1;
  }
  struct Arena value_perm = make->arenas->perm;
  const char* perm_beg = value_perm.beg;
  struct CommitDetail detail = {0};
  struct Changeset changeset = {0};
  if (revision_commit_detail(repository, commit_oid, &site_config->llm_trailers, &value_perm,
                             &detail, log) != 0 ||
      changeset_load(&changeset, repository, commit_oid, &site_config->limits, &value_perm, log) !=
          0) {
    return -1;
  }
  // Both sides of every file are highlighted only for a diff under its caps.
  const struct Str** fragments = NULL;
  if (!changeset.is_truncated &&
      highlight_commit(repository, &changeset, &value_perm, scratch, &fragments, log) != 0) {
    return -1;
  }
  struct ChangedImage* images = NULL;
  if (read_commit_images(repository, &changeset, &site_config->limits, &value_perm, &images, log) !=
      0) {
    return -1;
  }
  const struct TemplateValue* value = page_commit_value(site_config, repo, &detail, &changeset,
                                                        fragments, images, &value_perm, scratch);
  const int rc =
      render_page(make, site_config->templates.commit, value, perm, scratch, bytes_out, log);
  // The diff and its highlighted lines cost more to make than to fault in again, so their pages are
  // given back with the page's.
  arena_discard(&value_perm, perm_beg, value_perm.beg - perm_beg);
  return rc;
}

static int highlight_commit(const struct Repository* repository,
                            const struct Changeset* changeset,
                            struct Arena* perm,
                            struct Arena scratch,
                            const struct Str*** fragments_out,
                            struct Log* log) {
  const struct Str** fragments = arena_new(perm, changeset->file_count, const struct Str*);
  for (ptrdiff_t i = 0; i < changeset->file_count; i++) {
    const struct ChangedFile* file = &changeset->files[i];
    if (!file->is_shown || file->is_binary || file->is_too_large || file->line_count == 0) {
      continue;
    }
    // The whole sides stay in the scratch, so the perm holds only the lines the diff caps allow.
    struct Arena side_scratch = scratch;
    struct Arena side_arena = split_half(&side_scratch);
    struct HighlightDiffSides sides = {0};
    if (highlight_side(repository, file->old_oid, file->old_path, &side_arena, side_scratch,
                       &sides.old_fragments, &sides.old_fragment_count, log) != 0 ||
        highlight_side(repository, file->new_oid, file->new_path, &side_arena, side_scratch,
                       &sides.new_fragments, &sides.new_fragment_count, log) != 0) {
      return -1;
    }
    if (sides.old_fragments != NULL || sides.new_fragments != NULL) {
      fragments[i] = highlight_diff_file(file, &sides, perm);
    }
  }
  *fragments_out = fragments;
  return 0;
}

static int read_commit_images(const struct Repository* repository,
                              const struct Changeset* changeset,
                              const struct SurgeLimits* limits,
                              struct Arena* perm,
                              struct ChangedImage** images_out,
                              struct Log* log) {
  struct ChangedImage* images = arena_new(perm, changeset->file_count, struct ChangedImage);
  ptrdiff_t budget = limits->image_len_max;
  for (ptrdiff_t i = 0; i < changeset->file_count; i++) {
    const struct ChangedFile* file = &changeset->files[i];
    const struct Str path = file->new_path.len > 0 ? file->new_path : file->old_path;
    if (!file->is_shown || file->status == CHANGE_TYPE_CHANGED || !page_blob_is_image(path)) {
      continue;
    }
    const struct {
      struct Str oid;
      ptrdiff_t len;
      struct Str* data;
    } sides[] = {
        {file->old_oid, file->old_len, &images[i].old_data},
        {file->new_oid, file->new_len, &images[i].new_data},
    };
    for (ptrdiff_t side = 0; side < 2; side++) {
      if (sides[side].oid.len == 0 || sides[side].len == 0 || sides[side].len > budget) {
        continue;
      }
      bool is_binary = false;
      if (repository_read_blob(repository, sides[side].oid, perm, sides[side].data, &is_binary,
                               log) != 0) {
        return -1;
      }
      budget -= sides[side].data->len;
    }
  }
  *images_out = images;
  return 0;
}

static int highlight_side(const struct Repository* repository,
                          struct Str oid,
                          struct Str path,
                          struct Arena* perm,
                          struct Arena scratch,
                          const struct Str** fragments_out,
                          ptrdiff_t* count_out,
                          struct Log* log) {
  *fragments_out = NULL;
  *count_out = 0;
  // Without a grammar, a line escaped alone is the same as its fragment, so nothing is read.
  const struct HighlightGrammar* grammar = highlight_grammar_find(grammar_path_language(path));
  if (oid.len == 0 || grammar == NULL) {
    return 0;
  }
  struct Str data = {0};
  bool is_binary = false;
  if (repository_read_blob(repository, oid, &scratch, &data, &is_binary, log) != 0) {
    return -1;
  }
  struct Str* fragments = NULL;
  highlight_diff_lines(data, grammar, perm, scratch, &fragments, count_out);
  *fragments_out = fragments;
  return 0;
}

static int repo_pages_open(const struct RepoPagesRun* run,
                           ptrdiff_t repo_index,
                           struct JobArenas* arenas,
                           const struct Repository** repository_out,
                           struct Log* log) {
  struct RepoPagesWorker* worker = &run->workers[arenas - run->worker_arenas];
  *repository_out = &worker->repository;
  if (worker->repo_index == repo_index) {
    return 0;
  }
  repository_close(&worker->repository);
  worker->repo_index = -1;
  const struct SurgeRepo* repo = &run->inputs->site_config->repos[repo_index];
  // The repository's paths outlive the job, in the worker's perm, which a later job of the same
  // repo still reads.
  if (repository_open(&worker->repository, repo->path, repo->name, &arenas->perm, log) != 0) {
    return -1;
  }
  worker->repo_index = repo_index;
  return 0;
}

static int produce_page(struct RepoPagesMake* make,
                        OutputWriterMakeFn fn,
                        struct Arena scratch,
                        struct Log* log) {
  struct Arena page_arena = split_half(&scratch);
  const int rc =
      output_writer_produce(make->run->writer, make->output->file_path, make->run->inputs->cache,
                            fn, make, page_arena, scratch, S("failed to write page"), log);
  // A large page's pages are given back rather than kept resident in the worker's region.
  if (make->bytes.len > 0) {
    arena_discard(&make->bytes_arena, make->bytes.data, make->bytes.len);
  }
  return rc;
}

static int render_page(struct RepoPagesMake* make,
                       struct Str template_name,
                       const struct TemplateValue* value,
                       struct Arena* perm,
                       struct Arena scratch,
                       struct Str* bytes_out,
                       struct Log* log) {
  if (template_value_render(make->run->inputs->template_set, template_name, value,
                            make->output->relative_path, perm, scratch, bytes_out, log) != 0) {
    return -1;
  }
  make->bytes = *bytes_out;
  make->bytes_arena = *perm;
  return 0;
}

static int render_documents(const struct RepoPagesRun* run,
                            const struct RepoPageJob* job,
                            const struct BranchTree* tree,
                            const struct TreeDir* dir,
                            struct Str path_to_root,
                            struct JobArenas* arenas,
                            struct Arena* perm,
                            struct Arena scratch,
                            struct TreeDocument** documents_out,
                            ptrdiff_t* count_out,
                            struct Log* log) {
  const struct RepoModel* repo = &run->inputs->scans[job->repo_index].model;
  const ptrdiff_t blob_len_max = run->inputs->site_config->limits.blob_len_max;
  *documents_out = NULL;
  *count_out = 0;
  const struct TreeEntry* entries[DOCUMENT_KIND_COUNT] = {0};
  const ptrdiff_t count = readme_find_documents(tree, dir, blob_len_max, entries);
  if (count == 0) {
    return 0;
  }
  const struct Repository* repository = NULL;
  if (repo_pages_open(run, job->repo_index, arenas, &repository, log) != 0) {
    return -1;
  }
  // The open may have committed to the worker's perm, so the documents start past it.
  *perm = arenas->perm;
  struct TreeDocument* documents = arena_new(perm, count, struct TreeDocument);
  ptrdiff_t index = 0;
  for (ptrdiff_t kind = 0; kind < DOCUMENT_KIND_COUNT; kind++) {
    const struct TreeEntry* entry = entries[kind];
    if (entry == NULL) {
      continue;
    }
    const struct Str dir_path = entry->path.len > entry->name.len
                                    ? str_head(entry->path, entry->path.len - entry->name.len - 1)
                                    : (struct Str){0};
    const struct DocumentPlace place = {
        .tree = tree,
        .repo_name = repo->config->name,
        .path_to_root = path_to_root,
        .dir_path = dir_path,
        .raw_len_max = run->inputs->site_config->limits.raw_len_max,
    };
    ptrdiff_t missing_count = 0;
    struct TreeDocument* document = &documents[index++];
    *document = (struct TreeDocument){.kind = (enum DocumentKind)kind, .entry = entry};
    if (readme_render(repository, entry, &place, perm, scratch, &document->rendered_html,
                      &missing_count, log) != 0) {
      return -1;
    }
    run->job_notes[job - run->jobs].missing_link_count += missing_count;
  }
  *documents_out = documents;
  *count_out = count;
  return 0;
}

static struct RepoPagesOutput ref_output(const struct SiteConfig* site_config,
                                         const struct RepoModel* repo,
                                         const struct BranchTree* tree,
                                         enum PagePathKind kind,
                                         struct Str path,
                                         struct Arena* arena) {
  const struct Str relative_path =
      page_path(kind, repo->config->name, tree->branch->short_name, path, arena);
  return (struct RepoPagesOutput){
      .relative_path = relative_path,
      .file_path = path_join(site_config->output_dir, relative_path, arena),
  };
}

static struct RepoPagesOutput repo_output(const struct SiteConfig* site_config,
                                          const struct RepoModel* repo,
                                          struct Str page,
                                          struct Arena* arena) {
  struct Buf relative = {.arena = arena};
  buf_printf(&relative, "%.*s/%.*s", STR_FMT(repo->config->name), STR_FMT(page));
  const struct Str relative_path = buf_str(&relative);
  return (struct RepoPagesOutput){
      .relative_path = relative_path,
      .file_path = path_join(site_config->output_dir, relative_path, arena),
      .label = repo->config->name,
  };
}

static struct RepoPagesOutput log_output(const struct SiteConfig* site_config,
                                         const struct RepoModel* repo,
                                         const struct BranchTree* tree,
                                         ptrdiff_t page_index,
                                         struct Arena* arena) {
  const struct Str relative_path =
      page_log_path(repo->config->name, tree->branch->short_name, page_index, arena);
  struct Buf label = {.arena = arena};
  buf_printf(&label, "%.*s:%.*s", STR_FMT(repo->config->name), STR_FMT(tree->branch->short_name));
  return (struct RepoPagesOutput){
      .relative_path = relative_path,
      .file_path = path_join(site_config->output_dir, relative_path, arena),
      .label = buf_str(&label),
  };
}

static struct RepoPagesOutput commit_output(const struct SiteConfig* site_config,
                                            const struct RepoModel* repo,
                                            struct Str commit_oid,
                                            struct Arena* arena) {
  const struct Str relative_path = page_commit_path(repo->config->name, commit_oid, arena);
  struct Buf label = {.arena = arena};
  buf_printf(&label, "%.*s:%.*s", STR_FMT(repo->config->name), STR_FMT(commit_oid));
  return (struct RepoPagesOutput){
      .relative_path = relative_path,
      .file_path = path_join(site_config->output_dir, relative_path, arena),
      .label = buf_str(&label),
  };
}

static struct Str log_key(const struct RepoModel* repo,
                          const struct BranchLog* branch_log,
                          ptrdiff_t page_index,
                          ptrdiff_t page_size,
                          struct Arena* arena) {
  // The text is hashed, so it goes in a copy of the arena and only the id stays.
  struct Arena scratch = *arena;
  struct Buf text = {.arena = &scratch};
  buf_printf(&text, "pages %td\n", page_log_count(branch_log->commit_count, page_size));
  const ptrdiff_t begin = page_index * page_size;
  for (ptrdiff_t i = begin; i < begin + page_size && i < branch_log->commit_count; i++) {
    buf_printf(&text, "commit %.*s\n", STR_FMT(branch_log->commits[i]));
    page_log_append_refs(&text, repo, branch_log->commits[i]);
  }
  return key_id(buf_str(&text), arena, scratch);
}

static struct RepoPagesOutput feed_output(const struct SiteConfig* site_config,
                                          const struct RepoModel* repo,
                                          const struct BranchTree* tree,
                                          struct Arena* arena) {
  const struct Str relative_path =
      page_feed_commits_path(repo->config->name, tree->branch->short_name, arena);
  struct Buf label = {.arena = arena};
  buf_printf(&label, "%.*s:%.*s", STR_FMT(repo->config->name), STR_FMT(tree->branch->short_name));
  return (struct RepoPagesOutput){
      .relative_path = relative_path,
      .file_path = path_join(site_config->output_dir, relative_path, arena),
      .label = buf_str(&label),
  };
}

static struct Str commits_feed_key(const struct BranchLog* branch_log,
                                   ptrdiff_t feed_count,
                                   struct Arena* arena) {
  struct Arena scratch = *arena;
  struct Buf text = {.arena = &scratch};
  buf_append(&text, S("feed\n"));
  for (ptrdiff_t i = 0; i < feed_count && i < branch_log->commit_count; i++) {
    buf_printf(&text, "commit %.*s\n", STR_FMT(branch_log->commits[i]));
  }
  return key_id(buf_str(&text), arena, scratch);
}

static struct Str tags_feed_key(const struct RepoScan* scan, struct Arena* arena) {
  struct Arena scratch = *arena;
  struct Buf text = {.arena = &scratch};
  buf_printf(&text, "releases %.*s\n", STR_FMT(scan->releases_id));
  const struct Refset* refset = &scan->model.refset;
  for (ptrdiff_t i = 0; i < refset->count; i++) {
    const struct Ref* ref = &refset->refs[i];
    if (ref->kind == REF_KIND_TAG) {
      buf_printf(&text, "tag %.*s %.*s\n", STR_FMT(ref->oid), STR_FMT(ref->name));
    }
  }
  return key_id(buf_str(&text), arena, scratch);
}

static struct Str key_id(struct Str text, struct Arena* perm, struct Arena scratch) {
  const struct Str id = repository_text_id(text, &scratch);
  // The id sits in the copy, where `perm`'s next bytes are, so it moves through the stack.
  assert(id.len <= REPO_PAGES_ID_LEN_MAX);
  char copy[REPO_PAGES_ID_LEN_MAX];
  memcpy(copy, id.data, (size_t)id.len);
  return str_clone((struct Str){copy, id.len}, perm);
}

static struct Str tree_attributes(const struct BranchTree* tree,
                                  const struct TreeDir* dir,
                                  ptrdiff_t blob_len_max,
                                  struct Arena* arena) {
  const struct TreeEntry* documents[DOCUMENT_KIND_COUNT] = {0};
  (void)readme_find_documents(tree, dir, blob_len_max, documents);
  bool has_links = false;
  for (ptrdiff_t i = 0; i < DOCUMENT_KIND_COUNT; i++) {
    has_links = has_links || (documents[i] != NULL && readme_is_markup(documents[i]->name));
  }
  for (ptrdiff_t i = 0; i < dir->entry_count; i++) {
    has_links = has_links || dir->entries[i].kind == TREE_ENTRY_SYMLINK;
  }
  struct Buf attributes = {.arena = arena};
  buf_printf(&attributes, "tree=%.*s", STR_FMT(dir->oid));
  if (has_links) {
    buf_printf(&attributes, " root=%.*s", STR_FMT(tree->dirs[0].oid));
  }
  if (dir == &tree->dirs[0]) {
    buf_printf(&attributes, " commit=%.*s", STR_FMT(tree->branch->commit_oid));
  }
  return buf_str(&attributes);
}

static struct Str blob_attributes(const struct BranchTree* tree,
                                  const struct TreeEntry* entry,
                                  struct Arena* arena) {
  struct Buf attributes = {.arena = arena};
  buf_printf(&attributes, "blob=%.*s mode=%s", STR_FMT(entry->oid),
             entry->kind == TREE_ENTRY_EXECUTABLE ? "100755" : "100644");
  if (readme_is_markup(entry->name)) {
    buf_printf(&attributes, " root=%.*s", STR_FMT(tree->dirs[0].oid));
  }
  return buf_str(&attributes);
}

static struct Str repo_page(ptrdiff_t index) {
  const struct Str pages[] = {PAGE_TREE_HOME_PAGE, PAGE_REFS_BRANCHES_PAGE, PAGE_REFS_TAGS_PAGE};
  return pages[index];
}

static struct Str repo_page_template(const struct SiteConfig* site_config, ptrdiff_t index) {
  const struct SurgeTemplates* templates = &site_config->templates;
  const struct Str names[] = {templates->tree, templates->branch_list, templates->tag_list};
  return names[index];
}

static ptrdiff_t repo_feed_count(const struct RepoModel* repo) {
  return repo->release_project_count > 0 ? 2 : 1;
}

static struct Str repo_feed_page(ptrdiff_t index) {
  return index == 0 ? PAGE_FEED_TAGS_PAGE : PAGE_FEED_RELEASES_PAGE;
}

static struct RepoPageJob* plan_jobs(const struct RepoPagesInputs* inputs,
                                     struct Arena* perm,
                                     ptrdiff_t* count_out) {
  const struct SiteConfig* site_config = inputs->site_config;
  // The jobs are counted on the first pass and written on the second.
  struct RepoPageJob* jobs = NULL;
  ptrdiff_t job_count = 0;
  for (ptrdiff_t pass = 0; pass < 2; pass++) {
    if (pass == 1) {
      jobs = arena_new(perm, job_count, struct RepoPageJob);
    }
    ptrdiff_t index = 0;
    for (ptrdiff_t i = 0; i < site_config->repo_count; i++) {
      const struct RepoModel* repo = &inputs->scans[i].model;
      if (jobs != NULL) {
        jobs[index] = (struct RepoPageJob){.kind = REPO_PAGE_JOB_REPO, .repo_index = i};
      }
      index++;
      for (ptrdiff_t j = 0; j < repo->tree_count; j++) {
        const ptrdiff_t dir_count = repo->trees[j].dir_count;
        const ptrdiff_t page_count =
            page_log_count(repo->logs[j].commit_count, site_config->log_page_size);
        for (ptrdiff_t k = 0; k < dir_count + page_count; k++) {
          if (jobs != NULL) {
            jobs[index] = (struct RepoPageJob){
                .kind = k < dir_count ? REPO_PAGE_JOB_DIR : REPO_PAGE_JOB_LOG,
                .repo_index = i,
                .tree_index = j,
                .item_index = k < dir_count ? k : k - dir_count,
            };
          }
          index++;
        }
      }
      for (ptrdiff_t k = 0; k < repo->commit_count; k += REPO_PAGES_COMMIT_BATCH) {
        if (jobs != NULL) {
          jobs[index] =
              (struct RepoPageJob){.kind = REPO_PAGE_JOB_COMMITS, .repo_index = i, .item_index = k};
        }
        index++;
      }
    }
    job_count = index;
  }
  *count_out = job_count;
  return jobs;
}

static struct Arena split_half(struct Arena* arena) {
  const char* region_end = arena->limit != NULL ? arena->limit : arena->end;
  return arena_split(arena, (region_end - arena->beg) / 2);
}
