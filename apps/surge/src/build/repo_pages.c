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
#include "domain/build_cache.h"
#include "domain/grammar_path.h"
#include "domain/repo_model.h"
#include "domain/site_config.h"
#include "markup/highlight.h"
#include "os/oom.h"
#include "os/os.h"
#include "output/manifest_builder.h"
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

  /** Host the jobs check outputs on. */
  struct Os* os;

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

/**
 * @brief Walks the page rows `repo_pages_append_rows` appends, or only counts them, so a count and
 *        the rows always agree.
 *
 * @param cache       Cache whose `count` grows by one per row, and which receives the rows unless
 *                    only counting. Must not be `NULL`.
 * @param site_config Configuration of the build. Must not be `NULL`.
 * @param scans       What the scan found, one slot per repo of `site_config`. Must not be `NULL`.
 * @param arena       Arena that owns the rows' names and ids, or `NULL` to count them only.
 */
static void visit_rows(struct BuildCache* cache,
                       const struct SiteConfig* site_config,
                       const struct RepoScan* scans,
                       struct Arena* arena) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Records the paths of one directory's tree page and of its files' blob pages and raw
 *        copies.
 *
 * @param builder     Manifest being built. Must not be `NULL`.
 * @param repo        What the scan found for the repo. Must not be `NULL`.
 * @param tree        Tree of the branch. Must not be `NULL`.
 * @param dir         The directory. Must not be `NULL`.
 * @param raw_len_max Largest blob given a raw copy.
 * @param scratch     Arena for each path before the manifest copies it.
 * @param log         Log that receives every refusal. Must not be `NULL`.
 * @return `0` when every path was recorded, or `-1` otherwise.
 */
static int repo_pages_register_dir(struct ManifestBuilder* builder,
                                   const struct RepoModel* repo,
                                   const struct BranchTree* tree,
                                   const struct TreeDir* dir,
                                   ptrdiff_t raw_len_max,
                                   struct Arena scratch,
                                   struct Log* log) __attribute__((nonnull(1, 2, 3, 4, 7)));

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
 * @brief Writes a repo's home and its branches and tags pages, or keeps each one.
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
 * @brief Renders and writes one file's blob page.
 *
 * @param run       The run. Must not be `NULL`.
 * @param job       The job of the file's directory. Must not be `NULL`.
 * @param entry     The file. Must not be `NULL`.
 * @param data      The blob's bytes, which the raw copy can have read for a blob over
 *                  `blob_len_max`, or empty when nothing read them.
 * @param is_binary Whether `git_blob_is_binary` judges the blob binary, or `false` when nothing
 *                  read it. The page ignores it for a blob over `blob_len_max`.
 * @param arenas    Arenas of the worker. Must not be `NULL`.
 * @param scratch   Arena past the blob's bytes, for the page's temporaries and its HTML.
 * @param log       Log of the job. Must not be `NULL`.
 * @return `0` on success, or `-1` when the render or the write fails.
 */
static int repo_pages_job_dir_file_page(const struct RepoPagesRun* run,
                                        const struct RepoPageJob* job,
                                        const struct TreeEntry* entry,
                                        struct Str data,
                                        bool is_binary,
                                        struct JobArenas* arenas,
                                        struct Arena scratch,
                                        struct Log* log) __attribute__((nonnull(1, 2, 3, 6, 8)));

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
 * @brief Renders a page and writes it.
 *
 * @param run           The run. Must not be `NULL`.
 * @param template_name Name of the page's template.
 * @param value         Data tree of the page. Must not be `NULL`.
 * @param output        The page's output. Must not be `NULL`.
 * @param page_arena    Region the page grows in, where nothing else allocates, given back once the
 *                      page is written.
 * @param scratch       Arena for the render's temporaries.
 * @param log           Log of the job. Must not be `NULL`.
 * @return `0` on success, or `-1` when the render or the write fails.
 */
static int render_page(const struct RepoPagesRun* run,
                       struct Str template_name,
                       const struct TemplateValue* value,
                       const struct RepoPagesOutput* output,
                       struct Arena page_arena,
                       struct Arena scratch,
                       struct Log* log) __attribute__((nonnull(1, 3, 4, 7)));

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
 * @brief Reports whether the build cache vouches for an output that still exists.
 *
 * @param run          The run. Must not be `NULL`.
 * @param repo_index   Index of the output's repo in the config.
 * @param output       The output. Must not be `NULL`.
 * @param key          Ids of what the output reads now, as `repo_pages_append_rows` records them.
 * @param is_templated Whether the output is a page, which its templates and its repo's `repo` map
 *                     also decide.
 * @param len          Size the output must have, or `-1` for any.
 * @return `true` when the output can be kept.
 */
static bool is_kept(const struct RepoPagesRun* run,
                    ptrdiff_t repo_index,
                    const struct RepoPagesOutput* output,
                    struct Str key,
                    bool is_templated,
                    ptrdiff_t len) __attribute__((nonnull(1, 3)));

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
 * @brief Returns the ids a tree page reads: its tree, the branch's root tree when one of its
 *        documents is Markdown or Org, whose links resolve against every path of the branch, and
 *        for the root's page the branch's commit, whose summary it shows.
 *
 * @param tree         Tree of the branch. Must not be `NULL`.
 * @param dir          The directory. Must not be `NULL`.
 * @param blob_len_max Largest blob read for a page.
 * @param arena        Arena that owns the key. Must not be `NULL`.
 * @return The key.
 */
static struct Str tree_key(const struct BranchTree* tree,
                           const struct TreeDir* dir,
                           ptrdiff_t blob_len_max,
                           struct Arena* arena) __attribute__((nonnull(1, 2, 4)));

/**
 * @brief Returns the ids a blob page reads: its blob, its mode, and the branch's root tree when it
 *        is Markdown or Org.
 *
 * @param tree  Tree of the branch. Must not be `NULL`.
 * @param entry The file. Must not be `NULL`.
 * @param arena Arena that owns the key. Must not be `NULL`.
 * @return The key.
 */
static struct Str blob_key(const struct BranchTree* tree,
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
 * @param index Index of the page, as `repo_page` takes it.
 * @return The template's name.
 */
static struct Str repo_page_template(ptrdiff_t index);

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
                        struct Arena scratch,
                        struct Log* log) {
  int rc = 0;
  for (ptrdiff_t i = 0; i < site_config->repo_count; i++) {
    const struct RepoModel* repo = &scans[i].model;
    for (ptrdiff_t j = 0; j < REPO_PAGES_REPO_PAGE_COUNT; j++) {
      struct Arena path_scratch = scratch;
      const struct RepoPagesOutput output =
          repo_output(site_config, repo, repo_page(j), &path_scratch);
      if (manifest_builder_add(builder, output.relative_path, output.label, path_scratch, log) !=
          0) {
        rc = -1;
      }
    }
    const ptrdiff_t feed_count = repo_feed_count(repo);
    for (ptrdiff_t j = 0; j < feed_count; j++) {
      struct Arena path_scratch = scratch;
      const struct RepoPagesOutput output =
          repo_output(site_config, repo, repo_feed_page(j), &path_scratch);
      if (manifest_builder_add(builder, output.relative_path, output.label, path_scratch, log) !=
          0) {
        rc = -1;
      }
    }
    for (ptrdiff_t j = 0; j < repo->tree_count; j++) {
      const struct BranchTree* tree = &repo->trees[j];
      for (ptrdiff_t k = 0; k < tree->dir_count; k++) {
        if (repo_pages_register_dir(builder, repo, tree, &tree->dirs[k],
                                    site_config->limits.raw_len_max, scratch, log) != 0) {
          rc = -1;
        }
      }
      const ptrdiff_t page_count =
          page_log_count(repo->logs[j].commit_count, site_config->log_page_size);
      for (ptrdiff_t k = 0; k < page_count; k++) {
        struct Arena path_scratch = scratch;
        const struct RepoPagesOutput output = log_output(site_config, repo, tree, k, &path_scratch);
        if (manifest_builder_add(builder, output.relative_path, output.label, path_scratch, log) !=
            0) {
          rc = -1;
        }
      }
      // A branch with log pages has a commit feed.
      if (page_count > 0) {
        struct Arena path_scratch = scratch;
        const struct RepoPagesOutput output = feed_output(site_config, repo, tree, &path_scratch);
        if (manifest_builder_add(builder, output.relative_path, output.label, path_scratch, log) !=
            0) {
          rc = -1;
        }
      }
    }
    for (ptrdiff_t j = 0; j < repo->commit_count; j++) {
      struct Arena path_scratch = scratch;
      const struct RepoPagesOutput output =
          commit_output(site_config, repo, repo->commits[j], &path_scratch);
      if (manifest_builder_add(builder, output.relative_path, output.label, path_scratch, log) !=
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
      .os = os,
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

ptrdiff_t repo_pages_count_rows(const struct SiteConfig* site_config,
                                const struct RepoScan* scans) {
  struct BuildCache counted = {0};
  visit_rows(&counted, site_config, scans, NULL);
  return counted.count;
}
void repo_pages_append_rows(struct BuildCache* cache,
                            const struct SiteConfig* site_config,
                            const struct RepoScan* scans,
                            struct Arena* arena) {
  visit_rows(cache, site_config, scans, arena);
}
static void visit_rows(struct BuildCache* cache,
                       const struct SiteConfig* site_config,
                       const struct RepoScan* scans,
                       struct Arena* arena) {
  const ptrdiff_t blob_len_max = site_config->limits.blob_len_max;
  for (ptrdiff_t i = 0; i < site_config->repo_count; i++) {
    const struct RepoModel* repo = &scans[i].model;
    // The tag feed, and the release feed of a repo with release projects.
    const ptrdiff_t feed_count = repo_feed_count(repo);
    for (ptrdiff_t j = 0; j < feed_count; j++) {
      if (arena == NULL) {
        cache->count++;
        continue;
      }
      cache->rows[cache->count++] = (struct BuildCacheRow){
          .kind = BUILD_CACHE_KIND_PAGE,
          .name = repo_output(site_config, repo, repo_feed_page(j), arena).relative_path,
          .id = j == 0 ? tags_feed_key(&scans[i], arena) : scans[i].releases_id,
      };
    }
    for (ptrdiff_t j = 0; j < repo->tree_count; j++) {
      const struct BranchTree* tree = &repo->trees[j];
      for (ptrdiff_t k = 0; k < tree->dir_count; k++) {
        const struct TreeDir* dir = &tree->dirs[k];
        if (arena == NULL) {
          cache->count++;
        } else {
          cache->rows[cache->count++] = (struct BuildCacheRow){
              .kind = BUILD_CACHE_KIND_PAGE,
              .name = ref_output(site_config, repo, tree, PAGE_PATH_TREE, dir->path, arena)
                          .relative_path,
              .id = tree_key(tree, dir, blob_len_max, arena),
          };
        }
        for (ptrdiff_t m = 0; m < dir->entry_count; m++) {
          const struct TreeEntry* entry = &dir->entries[m];
          if (!filetree_entry_has_pages(entry)) {
            continue;
          }
          const bool has_raw = entry->len <= site_config->limits.raw_len_max;
          if (arena == NULL) {
            cache->count += has_raw ? 2 : 1;
            continue;
          }
          cache->rows[cache->count++] = (struct BuildCacheRow){
              .kind = BUILD_CACHE_KIND_PAGE,
              .name = ref_output(site_config, repo, tree, PAGE_PATH_BLOB, entry->path, arena)
                          .relative_path,
              .id = blob_key(tree, entry, arena),
          };
          if (has_raw) {
            cache->rows[cache->count++] = (struct BuildCacheRow){
                .kind = BUILD_CACHE_KIND_PAGE,
                .name = ref_output(site_config, repo, tree, PAGE_PATH_RAW, entry->path, arena)
                            .relative_path,
                .id = entry->oid,
            };
          }
        }
      }
      const ptrdiff_t page_count =
          page_log_count(repo->logs[j].commit_count, site_config->log_page_size);
      // The log pages, and the commit feed of a branch that has them.
      if (arena == NULL) {
        cache->count += page_count + (page_count > 0);
        continue;
      }
      for (ptrdiff_t k = 0; k < page_count; k++) {
        cache->rows[cache->count++] = (struct BuildCacheRow){
            .kind = BUILD_CACHE_KIND_PAGE,
            .name = log_output(site_config, repo, tree, k, arena).relative_path,
            .id = log_key(repo, &repo->logs[j], k, site_config->log_page_size, arena),
        };
      }
      if (page_count > 0) {
        cache->rows[cache->count++] = (struct BuildCacheRow){
            .kind = BUILD_CACHE_KIND_PAGE,
            .name = feed_output(site_config, repo, tree, arena).relative_path,
            .id = commits_feed_key(&repo->logs[j], site_config->feed_count, arena),
        };
      }
    }
  }
}

static int repo_pages_register_dir(struct ManifestBuilder* builder,
                                   const struct RepoModel* repo,
                                   const struct BranchTree* tree,
                                   const struct TreeDir* dir,
                                   ptrdiff_t raw_len_max,
                                   struct Arena scratch,
                                   struct Log* log) {
  int rc = 0;
  // Only the relative paths go to the manifest, so the config the outputs need is not read here.
  struct Arena path_scratch = scratch;
  struct Buf label = {.arena = &path_scratch};
  buf_printf(&label, "%.*s:%.*s:%.*s", STR_FMT(repo->config->name),
             STR_FMT(tree->branch->short_name), STR_FMT(dir->path));
  const struct Str dir_label = buf_str(&label);
  // The path is made before the call, so the copy of the scratch the add gets starts past it: C
  // leaves the order in which a call evaluates its arguments unspecified.
  const struct Str dir_page = page_path(PAGE_PATH_TREE, repo->config->name,
                                        tree->branch->short_name, dir->path, &path_scratch);
  if (manifest_builder_add(builder, dir_page, dir_label, path_scratch, log) != 0) {
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
    const enum PagePathKind kinds[] = {PAGE_PATH_BLOB, PAGE_PATH_RAW};
    for (ptrdiff_t j = 0; j < 2; j++) {
      if (kinds[j] == PAGE_PATH_RAW && entry->len > raw_len_max) {
        continue;
      }
      struct Arena add_scratch = entry_scratch;
      const struct Str path = page_path(kinds[j], repo->config->name, tree->branch->short_name,
                                        entry->path, &add_scratch);
      if (manifest_builder_add(builder, path, file_label, add_scratch, log) != 0) {
        rc = -1;
      }
    }
  }
  return rc;
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
  const struct RepoPagesInputs* inputs = run->inputs;
  const struct RepoModel* repo = &inputs->scans[job->repo_index].model;
  const bool is_fresh = inputs->is_templates_fresh && inputs->is_repo_fresh[job->repo_index] &&
                        inputs->is_repo_map_fresh[job->repo_index];
  int rc = 0;
  for (ptrdiff_t i = 0; i < REPO_PAGES_REPO_PAGE_COUNT; i++) {
    // The tags page links each tag's release, so it is also made again when the releases change.
    const bool is_page_fresh =
        is_fresh && (i != REPO_PAGES_TAGS_INDEX || inputs->is_releases_fresh[job->repo_index]);
    struct Arena scratch = arenas->scratch;
    const struct RepoPagesOutput output =
        repo_output(inputs->site_config, repo, repo_page(i), &scratch);
    struct OsStat st;
    if (is_page_fresh && os_stat(run->os, output.file_path, &st) == 0 && !st.is_dir) {
      if (output_writer_keep(run->writer, output.file_path, log) != 0) {
        rc = -1;
      }
      continue;
    }
    // The data tree goes in a copy of the perm, and the page in a region of the scratch of its own.
    struct Arena perm = arenas->perm;
    const struct TemplateValue* value = NULL;
    if (repo_pages_job_repo_value(run, job, i, &output, arenas, &perm, scratch, &value, log) != 0) {
      rc = -1;
      continue;
    }
    struct Arena page_arena = split_half(&scratch);
    if (render_page(run, repo_page_template(i), value, &output, page_arena, scratch, log) != 0) {
      rc = -1;
    }
  }
  if (repo_pages_job_repo_feeds(run, job, arenas, log) != 0) {
    rc = -1;
  }
  return rc;
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
  const struct RepoPagesInputs* inputs = run->inputs;
  const struct SiteConfig* site_config = inputs->site_config;
  const struct RepoScan* scan = &inputs->scans[job->repo_index];
  const struct RepoModel* repo = &scan->model;
  int rc = 0;
  for (ptrdiff_t i = 0; i < repo_feed_count(repo); i++) {
    const bool is_tags = i == 0;
    struct Arena perm = arenas->perm;
    struct Arena scratch = arenas->scratch;
    const struct RepoPagesOutput output =
        repo_output(site_config, repo, repo_feed_page(i), &scratch);
    const struct Str key = is_tags ? tags_feed_key(scan, &scratch) : scan->releases_id;
    if (is_kept(run, job->repo_index, &output, key, true, -1)) {
      if (output_writer_keep(run->writer, output.file_path, log) != 0) {
        rc = -1;
      }
      continue;
    }
    const struct TemplateValue* value =
        is_tags ? page_feed_tags_value(site_config, repo, &perm)
                : page_feed_releases_value(site_config, repo, &perm, scratch);
    struct Arena page_arena = split_half(&scratch);
    if (render_page(run, is_tags ? PAGE_FEED_TAGS_TEMPLATE : PAGE_FEED_RELEASES_TEMPLATE, value,
                    &output, page_arena, scratch, log) != 0) {
      rc = -1;
    }
  }
  return rc;
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
  const struct RepoPagesInputs* inputs = run->inputs;
  const struct SiteConfig* site_config = inputs->site_config;
  const struct RepoModel* repo = &inputs->scans[job->repo_index].model;
  const struct BranchTree* tree = &repo->trees[job->tree_index];
  const struct TreeDir* dir = &tree->dirs[job->item_index];
  struct Arena scratch = arenas->scratch;
  const struct RepoPagesOutput output =
      ref_output(site_config, repo, tree, PAGE_PATH_TREE, dir->path, &scratch);
  const struct Str key = tree_key(tree, dir, site_config->limits.blob_len_max, &scratch);
  if (is_kept(run, job->repo_index, &output, key, true, -1)) {
    return output_writer_keep(run->writer, output.file_path, log);
  }
  // The root goes in the scratch, because the documents restart the perm past the repository's
  // paths when they open it, and the page in a region of the scratch of its own.
  const struct Str path_to_root = output_path_to_root(output.relative_path, &scratch);
  struct Arena perm = arenas->perm;
  struct TreeDocument* documents = NULL;
  ptrdiff_t document_count = 0;
  if (render_documents(run, job, tree, dir, path_to_root, arenas, &perm, scratch, &documents,
                       &document_count, log) != 0) {
    return -1;
  }
  const struct TemplateValue* value =
      page_tree_value(site_config, repo, tree, dir, false, documents, document_count, &perm);
  struct Arena page_arena = split_half(&scratch);
  return render_page(run, PAGE_TREE_TEMPLATE, value, &output, page_arena, scratch, log);
}

static int repo_pages_job_dir_file(const struct RepoPagesRun* run,
                                   const struct RepoPageJob* job,
                                   const struct TreeEntry* entry,
                                   struct JobArenas* arenas,
                                   struct Log* log) {
  const struct RepoPagesInputs* inputs = run->inputs;
  const struct SiteConfig* site_config = inputs->site_config;
  const struct RepoModel* repo = &inputs->scans[job->repo_index].model;
  const struct BranchTree* tree = &repo->trees[job->tree_index];
  struct RepoPagesNotes* notes = &run->job_notes[job - run->jobs];
  struct Arena scratch = arenas->scratch;
  const struct RepoPagesOutput blob =
      ref_output(site_config, repo, tree, PAGE_PATH_BLOB, entry->path, &scratch);
  const struct RepoPagesOutput raw =
      ref_output(site_config, repo, tree, PAGE_PATH_RAW, entry->path, &scratch);
  const bool has_raw = entry->len <= site_config->limits.raw_len_max;
  const bool is_too_large = entry->len > site_config->limits.blob_len_max;
  notes->raw_skipped_count += !has_raw;
  const bool is_blob_kept =
      is_kept(run, job->repo_index, &blob, blob_key(tree, entry, &scratch), true, -1);
  const bool is_raw_kept =
      has_raw && is_kept(run, job->repo_index, &raw, entry->oid, false, entry->len);
  // The blob is read when an output that is not kept shows its bytes.
  struct Str data = {0};
  bool is_binary = false;
  if ((!is_blob_kept && !is_too_large) || (has_raw && !is_raw_kept)) {
    const struct Repository* repository = NULL;
    if (repo_pages_open(run, job->repo_index, arenas, &repository, log) != 0) {
      return -1;
    }
    if (repository_read_blob(repository, entry->oid, &scratch, &data, &is_binary, log) != 0) {
      return -1;
    }
  }
  int rc = 0;
  if (has_raw) {
    rc = is_raw_kept
             ? output_writer_keep(run->writer, raw.file_path, log)
             : output_writer_write(run->writer, raw.file_path, (struct OsSource){.bytes = data},
                                   S("failed to write raw copy"), log);
  }
  if (rc == 0 && is_blob_kept) {
    rc = output_writer_keep(run->writer, blob.file_path, log);
  } else if (rc == 0) {
    rc = repo_pages_job_dir_file_page(run, job, entry, data, is_binary, arenas, scratch, log);
  }
  // A large blob's pages are given back once its outputs are written, as fram gives back a large
  // source, so the worker does not keep the largest blob of its jobs resident.
  if (data.len >= JOB_SOURCE_DISCARD_LEN_MIN) {
    arena_discard(&scratch, data.data, data.len);
  }
  return rc;
}

static int repo_pages_job_dir_file_page(const struct RepoPagesRun* run,
                                        const struct RepoPageJob* job,
                                        const struct TreeEntry* entry,
                                        struct Str data,
                                        bool is_binary,
                                        struct JobArenas* arenas,
                                        struct Arena scratch,
                                        struct Log* log) {
  const struct RepoPagesInputs* inputs = run->inputs;
  const struct SiteConfig* site_config = inputs->site_config;
  const struct RepoModel* repo = &inputs->scans[job->repo_index].model;
  const struct BranchTree* tree = &repo->trees[job->tree_index];
  const struct TreeDir* dir = &tree->dirs[job->item_index];
  const bool is_too_large = entry->len > site_config->limits.blob_len_max;
  const struct RepoPagesOutput blob =
      ref_output(site_config, repo, tree, PAGE_PATH_BLOB, entry->path, &scratch);
  // The rendered view and the data tree go in a copy of the perm taken after any open, and the page
  // gets a region of the scratch of its own, past the blob's bytes.
  struct Arena perm = arenas->perm;
  const char* perm_beg = perm.beg;
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
        .path_to_root = output_path_to_root(blob.relative_path, &perm),
        .dir_path = dir->path,
        .raw_len_max = site_config->limits.raw_len_max,
    };
    ptrdiff_t missing_count = 0;
    content.rendered_html = readme_render_markup(data, readme_is_org(entry->name), &place, &perm,
                                                 scratch, &missing_count);
    run->job_notes[job - run->jobs].missing_link_count += missing_count;
  }
  const struct TemplateValue* value =
      page_blob_value(site_config, repo, tree, entry, &content, &perm, scratch);
  struct Arena page_arena = split_half(&scratch);
  const int rc = render_page(run, PAGE_BLOB_TEMPLATE, value, &blob, page_arena, scratch, log);
  // The highlighted lines and the rendered view cost more to make than to fault in again, so their
  // pages are given back with the page's.
  arena_discard(&perm, perm_beg, perm.beg - perm_beg);
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
  const struct RepoPagesInputs* inputs = run->inputs;
  const struct SiteConfig* site_config = inputs->site_config;
  const struct RepoModel* repo = &inputs->scans[job->repo_index].model;
  const struct BranchTree* tree = &repo->trees[job->tree_index];
  const struct BranchLog* branch_log = &repo->logs[job->tree_index];
  const ptrdiff_t page_size = site_config->log_page_size;
  const ptrdiff_t page_count = page_log_count(branch_log->commit_count, page_size);
  const ptrdiff_t begin = job->item_index * page_size;
  const ptrdiff_t end =
      begin + page_size < branch_log->commit_count ? begin + page_size : branch_log->commit_count;
  struct Arena scratch = arenas->scratch;
  const struct RepoPagesOutput output =
      log_output(site_config, repo, tree, job->item_index, &scratch);
  const struct Str key = log_key(repo, branch_log, job->item_index, page_size, &scratch);
  if (is_kept(run, job->repo_index, &output, key, true, -1)) {
    return output_writer_keep(run->writer, output.file_path, log);
  }
  const struct Repository* repository = NULL;
  if (repo_pages_open(run, job->repo_index, arenas, &repository, log) != 0) {
    return -1;
  }
  // The summaries and the data tree go in a copy of the perm taken after the open.
  struct Arena perm = arenas->perm;
  struct CommitSummary* commits = arena_new(&perm, end - begin, struct CommitSummary);
  for (ptrdiff_t i = begin; i < end; i++) {
    if (revision_commit_summary(repository, branch_log->commits[i], &perm, &commits[i - begin],
                                log) != 0) {
      return -1;
    }
  }
  const struct TemplateValue* value = page_log_value(site_config, repo, tree, job->item_index,
                                                     page_count, commits, end - begin, &perm);
  struct Arena page_arena = split_half(&scratch);
  return render_page(run, PAGE_LOG_TEMPLATE, value, &output, page_arena, scratch, log);
}

static int repo_pages_job_log_feed(const struct RepoPagesRun* run,
                                   const struct RepoPageJob* job,
                                   struct JobArenas* arenas,
                                   struct Log* log) {
  const struct RepoPagesInputs* inputs = run->inputs;
  const struct SiteConfig* site_config = inputs->site_config;
  const struct RepoModel* repo = &inputs->scans[job->repo_index].model;
  const struct BranchTree* tree = &repo->trees[job->tree_index];
  const struct BranchLog* branch_log = &repo->logs[job->tree_index];
  struct Arena scratch = arenas->scratch;
  const struct RepoPagesOutput output = feed_output(site_config, repo, tree, &scratch);
  const struct Str key = commits_feed_key(branch_log, site_config->feed_count, &scratch);
  if (is_kept(run, job->repo_index, &output, key, true, -1)) {
    return output_writer_keep(run->writer, output.file_path, log);
  }
  const struct Repository* repository = NULL;
  if (repo_pages_open(run, job->repo_index, arenas, &repository, log) != 0) {
    return -1;
  }
  struct Arena perm = arenas->perm;
  const ptrdiff_t count = branch_log->commit_count < site_config->feed_count
                              ? branch_log->commit_count
                              : site_config->feed_count;
  struct CommitDetail* commits = arena_new(&perm, count, struct CommitDetail);
  for (ptrdiff_t i = 0; i < count; i++) {
    if (revision_commit_detail(repository, branch_log->commits[i], &perm, &commits[i], log) != 0) {
      return -1;
    }
  }
  const struct TemplateValue* value =
      page_feed_commits_value(site_config, repo, tree, commits, count, &perm);
  struct Arena page_arena = split_half(&scratch);
  return render_page(run, PAGE_FEED_COMMITS_TEMPLATE, value, &output, page_arena, scratch, log);
}

static int repo_pages_job_commits(const struct RepoPagesRun* run,
                                  const struct RepoPageJob* job,
                                  struct JobArenas* arenas,
                                  struct Log* log) {
  const struct RepoPagesInputs* inputs = run->inputs;
  const struct SiteConfig* site_config = inputs->site_config;
  const struct RepoModel* repo = &inputs->scans[job->repo_index].model;
  const ptrdiff_t end = job->item_index + REPO_PAGES_COMMIT_BATCH < repo->commit_count
                            ? job->item_index + REPO_PAGES_COMMIT_BATCH
                            : repo->commit_count;
  // Each commit is a page of its own, so one that fails leaves the others to report theirs.
  int rc = 0;
  for (ptrdiff_t i = job->item_index; i < end; i++) {
    struct Arena scratch = arenas->scratch;
    const struct Str commit_oid = repo->commits[i];
    const struct RepoPagesOutput output = commit_output(site_config, repo, commit_oid, &scratch);
    // A commit page reads only its commit and its repo's `repo` map, so it is kept while they, the
    // templates and the recipe are.
    struct OsStat st;
    if (inputs->is_templates_fresh && inputs->is_repo_map_fresh[job->repo_index] &&
        os_stat(run->os, output.file_path, &st) == 0 && !st.is_dir) {
      if (output_writer_keep(run->writer, output.file_path, log) != 0) {
        rc = -1;
      }
      continue;
    }
    const struct Repository* repository = NULL;
    if (repo_pages_open(run, job->repo_index, arenas, &repository, log) != 0) {
      return -1;
    }
    struct Arena perm = arenas->perm;
    const char* perm_beg = perm.beg;
    struct CommitDetail detail = {0};
    struct Changeset changeset = {0};
    if (revision_commit_detail(repository, commit_oid, &perm, &detail, log) != 0 ||
        changeset_load(&changeset, repository, commit_oid, &site_config->limits, &perm, log) != 0) {
      rc = -1;
      continue;
    }
    // Both sides of every file are highlighted only for a diff under its caps.
    const struct Str** fragments = NULL;
    if (!changeset.is_truncated &&
        highlight_commit(repository, &changeset, &perm, scratch, &fragments, log) != 0) {
      rc = -1;
      continue;
    }
    struct ChangedImage* images = NULL;
    if (read_commit_images(repository, &changeset, &site_config->limits, &perm, &images, log) !=
        0) {
      rc = -1;
      continue;
    }
    const struct TemplateValue* value = page_commit_value(site_config, repo, &detail, &changeset,
                                                          fragments, images, &perm, scratch);
    struct Arena page_arena = split_half(&scratch);
    if (render_page(run, PAGE_COMMIT_TEMPLATE, value, &output, page_arena, scratch, log) != 0) {
      rc = -1;
    }
    // The diff and its highlighted lines cost more to make than to fault in again, so their pages
    // are given back with the page's.
    arena_discard(&perm, perm_beg, perm.beg - perm_beg);
  }
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

static int render_page(const struct RepoPagesRun* run,
                       struct Str template_name,
                       const struct TemplateValue* value,
                       const struct RepoPagesOutput* output,
                       struct Arena page_arena,
                       struct Arena scratch,
                       struct Log* log) {
  struct Str html = {0};
  if (template_value_render(run->inputs->template_set, template_name, value, output->relative_path,
                            &page_arena, scratch, &html, log) != 0) {
    return -1;
  }
  const int rc =
      output_writer_write(run->writer, output->file_path, (struct OsSource){.bytes = html},
                          S("failed to write page"), log);
  // A large page's pages are given back rather than kept resident in the worker's region.
  arena_discard(&page_arena, html.data, html.len);
  return rc;
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

static bool is_kept(const struct RepoPagesRun* run,
                    ptrdiff_t repo_index,
                    const struct RepoPagesOutput* output,
                    struct Str key,
                    bool is_templated,
                    ptrdiff_t len) {
  if (is_templated &&
      !(run->inputs->is_templates_fresh && run->inputs->is_repo_map_fresh[repo_index])) {
    return false;
  }
  const struct BuildCacheRow* row =
      build_cache_find(run->inputs->cache, BUILD_CACHE_KIND_PAGE, output->relative_path);
  struct OsStat st;
  return row != NULL && str_eq(row->id, key) && os_stat(run->os, output->file_path, &st) == 0 &&
         !st.is_dir && (len < 0 || st.len == len);
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

static struct Str tree_key(const struct BranchTree* tree,
                           const struct TreeDir* dir,
                           ptrdiff_t blob_len_max,
                           struct Arena* arena) {
  const struct TreeEntry* documents[DOCUMENT_KIND_COUNT] = {0};
  (void)readme_find_documents(tree, dir, blob_len_max, documents);
  bool has_markup = false;
  for (ptrdiff_t i = 0; i < DOCUMENT_KIND_COUNT; i++) {
    has_markup = has_markup || (documents[i] != NULL && readme_is_markup(documents[i]->name));
  }
  struct Buf key = {.arena = arena};
  buf_append(&key, dir->oid);
  if (has_markup) {
    buf_append_char(&key, ',');
    buf_append(&key, tree->dirs[0].oid);
  }
  if (dir == &tree->dirs[0]) {
    buf_append(&key, S(",commit "));
    buf_append(&key, tree->branch->commit_oid);
  }
  return buf_str(&key);
}

static struct Str blob_key(const struct BranchTree* tree,
                           const struct TreeEntry* entry,
                           struct Arena* arena) {
  struct Buf key = {.arena = arena};
  buf_printf(&key, "%.*s,%s", STR_FMT(entry->oid),
             entry->kind == TREE_ENTRY_EXECUTABLE ? "100755" : "100644");
  if (readme_is_markup(entry->name)) {
    buf_printf(&key, ",%.*s", STR_FMT(tree->dirs[0].oid));
  }
  return buf_str(&key);
}

static struct Str repo_page(ptrdiff_t index) {
  const struct Str pages[] = {PAGE_TREE_HOME_PAGE, PAGE_REFS_BRANCHES_PAGE, PAGE_REFS_TAGS_PAGE};
  return pages[index];
}

static struct Str repo_page_template(ptrdiff_t index) {
  const struct Str templates[] = {PAGE_TREE_TEMPLATE, PAGE_REFS_BRANCHES_TEMPLATE,
                                  PAGE_REFS_TAGS_TEMPLATE};
  return templates[index];
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
