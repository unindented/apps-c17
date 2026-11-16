#include "build/repo_scan.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>

#include "build/page_context.h"
#include "build/release_scan.h"
#include "core/arena.h"
#include "core/ascii.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/path.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "domain/site_config.h"
#include "domain/trailer.h"
#include "os/oom.h"
#include "os/os.h"
#include "output/manifest.h"
#include "output/output_cache.h"
#include "runtime/job.h"
#include "vcs/filetree.h"
#include "vcs/pack_export.h"
#include "vcs/refset.h"
#include "vcs/repository.h"
#include "vcs/revision.h"

/** Hex digits of the id of a repo's `repo` map that every page of the repo declares. */
enum { REPO_SCAN_MAP_ID_LEN = 16 };

/** Largest `description` file of a repository read, in bytes. git's own is one line. */
enum { REPO_SCAN_DESCRIPTION_LEN_MAX = 64 * 1024 };

/** What `git init` writes to `description`, which names nothing, so no page shows it. */
#define REPO_SCAN_DESCRIPTION_DEFAULT \
  S("Unnamed repository; edit this file 'description' to name the repository.")

/** Shared state of the scan jobs, read-only while they run but for each job's own slot. */
struct RepoScanJobs {
  /** Loaded configuration. */
  const struct SiteConfig* site_config;

  /** Output cache an earlier build left. */
  const struct OutputCache* cache;

  /** Whether the build is a dry run, which makes no pack. */
  bool is_dry_run;

  /** Host the output is inspected on. */
  struct Os* os;

  /** One slot per repo, each written only by its job. */
  struct RepoScan* scans;
};

/**
 * @brief Scans one repo. This is the `JobFn` of `repo_scan_run`.
 *
 * @param index    Index of the repo.
 * @param arenas   Arenas of the worker, whose perm keeps what the job finds. Must not be `NULL`.
 * @param log      Log of the job. Must not be `NULL`.
 * @param userdata `struct RepoScanJobs*`. Must not be `NULL`.
 * @return `0` on success, or `-1` on failure.
 */
static int repo_scan_job(ptrdiff_t index, struct JobArenas* arenas, struct Log* log, void* userdata)
    __attribute__((nonnull(2, 3, 4)));

/**
 * @brief Reads what the repo's pages show of it besides its trees: its default branch, the commit
 *        of each ref, the date of its newest commit and its description.
 *
 * @param scan         Slot of the repo, whose refs are loaded. Must not be `NULL`.
 * @param repo_index   Index of the repo in the config.
 * @param repository   Open repository. Must not be `NULL`.
 * @param llm_trailers Which trailers mark a commit LLM-assisted. Must not be `NULL`.
 * @param os           Host the `description` file is read on. Must not be `NULL`.
 * @param perm         Arena that owns what the job keeps. Must not be `NULL`.
 * @param scratch      Arena for the `description` file's path and bytes.
 * @param log          Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` on failure.
 */
static int repo_scan_job_model(struct RepoScan* scan,
                               ptrdiff_t repo_index,
                               const struct Repository* repository,
                               const struct TrailerMatch* llm_trailers,
                               struct Os* os,
                               struct Arena* perm,
                               struct Arena scratch,
                               struct Log* log) __attribute__((nonnull(1, 3, 4, 5, 6, 8)));

/**
 * @brief Reads the repository's `description` file, which may be missing, as the description to
 *        show when the config gives none.
 *
 * @param git_dir         Common git directory of the repository.
 * @param os              Host the file is read on. Must not be `NULL`.
 * @param perm            Arena that owns the description. Must not be `NULL`.
 * @param scratch         Arena for the file's path and bytes, apart from `perm`.
 * @param description_out Receives the description, empty when the file is missing or holds git's
 *                        default. Must not be `NULL`.
 * @param log             Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the file exists but cannot be read.
 */
static int repo_scan_job_model_read_description(struct Str git_dir,
                                                struct Os* os,
                                                struct Arena* perm,
                                                struct Arena scratch,
                                                struct Str* description_out,
                                                struct Log* log)
    __attribute__((nonnull(2, 3, 5, 6)));

/**
 * @brief Walks the tree and the log of every branch that names a commit, checking the tree's names,
 *        in the order of the branches, warns of a branch that names none, and lists every commit a
 *        ref reaches.
 *
 * @param scan       Slot of the repo, whose refs are loaded. Must not be `NULL`.
 * @param repository Open repository. Must not be `NULL`.
 * @param perm       Arena that owns the trees. Must not be `NULL`.
 * @param log        Log that receives the warnings and the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when a tree cannot be read.
 */
static int repo_scan_job_branches(struct RepoScan* scan,
                                  const struct Repository* repository,
                                  struct Arena* perm,
                                  struct Log* log) __attribute__((nonnull(1, 2, 3, 4)));

/**
 * @brief Requires each project's `path` to be a directory of the default branch with a tree page.
 *
 * An empty repository has no default branch yet, so nothing is checked.
 *
 * @param scan       Slot of the repo, whose branches are walked. Must not be `NULL`.
 * @param repo_index Index of the repo in the config, which the diagnostic's key names.
 * @param log        Log that receives an error per project. Must not be `NULL`.
 * @return `0` when every project's path is such a directory, or `-1` otherwise.
 */
static int repo_scan_job_projects(const struct RepoScan* scan,
                                  ptrdiff_t repo_index,
                                  struct Log* log) __attribute__((nonnull(1, 3)));

/**
 * @brief Decides the clone export of one repo: keeps the pack in the output when it is current, or
 *        writes a new one below `TMPDIR`.
 *
 * @param scan       Slot of the repo, whose model is complete. Must not be `NULL`.
 * @param repository Open repository. Must not be `NULL`.
 * @param jobs       Shared state of the scan. Must not be `NULL`.
 * @param perm       Arena that owns what the job keeps. Must not be `NULL`.
 * @param log        Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the pack cannot be written.
 */
static int repo_scan_job_export(struct RepoScan* scan,
                                const struct Repository* repository,
                                const struct RepoScanJobs* jobs,
                                struct Arena* perm,
                                struct Log* log) __attribute__((nonnull(1, 2, 3, 4, 5)));

/** What the output cache recorded of one repo's clone export, with the pack still in the output. */
struct CachedExport {
  /** Hex name of the pack. */
  struct Str pack_id;

  /** Id of the `info/refs` the pack was made for. */
  struct Str info_refs_id;

  /** Row of the pack. */
  const struct OutputCacheRow* pack_row;

  /** Row of the pack's idx. */
  const struct OutputCacheRow* idx_row;
};

/**
 * @brief Finds what the output cache recorded of one repo's clone export: its fact, and the rows of
 *        its pack and idx, which must both be in the output with the sizes the rows record.
 *
 * @param jobs       Shared state of the scan. Must not be `NULL`.
 * @param repo_name  Name of the repo.
 * @param perm       Arena for the paths. Must not be `NULL`.
 * @param cached_out Receives what the cache recorded, whose strings point into the cache. Must not
 *                   be `NULL`.
 * @return `true` when the cache records a pack that is still in the output.
 */
static bool repo_scan_job_export_find(const struct RepoScanJobs* jobs,
                                      struct Str repo_name,
                                      struct Arena* perm,
                                      struct CachedExport* cached_out)
    __attribute__((nonnull(1, 3, 4)));

/**
 * @brief Reports whether the output holds `file_path` with the size `len`.
 *
 * @param file_path File to inspect.
 * @param len       Size the file must have.
 * @param os        Host the output is on. Must not be `NULL`.
 * @return `true` when the file is there with that size.
 */
static bool is_present_with_len(struct Str file_path, ptrdiff_t len, struct Os* os)
    __attribute__((nonnull(3)));

/**
 * @brief Returns the id of what the repo's pages read besides its refs: its place in the config,
 *        which orders the site index, its description, its default branch, the date of the default
 *        branch's newest commit and its projects.
 *
 * @param scan       Slot of the repo, whose model is complete. Must not be `NULL`.
 * @param repo_index Index of the repo in the config.
 * @param arena      Arena that owns the id. Must not be `NULL`.
 * @return The id.
 */
static struct Str repo_scan_job_repo_id(const struct RepoScan* scan,
                                        ptrdiff_t repo_index,
                                        struct Arena* arena) __attribute__((nonnull(1, 3)));

/**
 * @brief Reports whether `manifest` marks both the pack `pack_id` of a repo's clone export and its
 *        idx written or kept.
 *
 * @param manifest   Manifest of the build. Must not be `NULL`.
 * @param output_dir Output directory the manifest's paths are below.
 * @param repo_name  Name of the repo.
 * @param pack_id    Hex name of the pack.
 * @param scratch    Arena for the paths.
 * @return `true` when both are marked.
 */
static bool is_pack_marked(const struct Manifest* manifest,
                           struct Str output_dir,
                           struct Str repo_name,
                           struct Str pack_id,
                           struct Arena scratch) __attribute__((nonnull(1)));

int repo_scan_locate(const struct SiteConfig* site_config,
                     struct Arena* perm,
                     struct RepoScan* scans,
                     struct Log* log) {
  int rc = 0;
  for (ptrdiff_t i = 0; i < site_config->repo_count; i++) {
    const struct SurgeRepo* repo = &site_config->repos[i];
    scans[i].model.config = repo;
    struct Repository repository = {0};
    if (repository_open(&repository, repo->path, repo->name, perm, log) != 0) {
      rc = -1;
      continue;
    }
    scans[i].model.git_dir = repository.git_dir;
    scans[i].model.work_dir = repository.work_dir;
    repository_close(&repository);
  }
  return rc;
}

int repo_scan_run(const struct SiteConfig* site_config,
                  const struct OutputCache* cache,
                  bool is_dry_run,
                  ptrdiff_t worker_count,
                  struct JobArenas* worker_arenas,
                  bool is_verbose,
                  struct Os* os,
                  struct Arena scratch,
                  struct RepoScan* scans,
                  struct Log* log) {
  struct RepoScanJobs jobs = {
      .site_config = site_config,
      .cache = cache,
      .is_dry_run = is_dry_run,
      .os = os,
      .scans = scans,
  };
  return job_run(site_config->repo_count, worker_count, worker_arenas, repo_scan_job, &jobs,
                 S("scanning repos"), (struct Str){0}, is_verbose, os, scratch, log);
}

struct OutputCacheFact repo_scan_export_fact(const struct RepoScan* scan,
                                             const struct Manifest* manifest,
                                             struct Str output_dir,
                                             struct Arena* arena) {
  const struct RepoExport* export = &scan->export;
  const struct Str repo_name = scan->model.config->name;
  struct Str pack_id = export->previous_pack_id;
  struct Str info_refs_id = export->previous_info_refs_id;
  if (export->pack_id.len > 0 &&
      is_pack_marked(manifest, output_dir, repo_name, export->pack_id, *arena)) {
    pack_id = export->pack_id;
    info_refs_id = export->info_refs_id;
  }
  if (pack_id.len == 0) {
    return (struct OutputCacheFact){0};
  }
  struct Buf value = {.arena = arena};
  buf_printf(&value, "pack=%.*s refs=%.*s", STR_FMT(pack_id), STR_FMT(info_refs_id));
  return (struct OutputCacheFact){
      .key = str_concat(scan->model.config->name, S(".git"), arena),
      .value = buf_str(&value),
  };
}

const struct BranchTree* repo_scan_default_tree(const struct RepoModel* model) {
  for (ptrdiff_t i = 0; i < model->tree_count; i++) {
    if (model->trees[i].branch == model->default_ref) {
      return &model->trees[i];
    }
  }
  return NULL;
}

void repo_scan_remove_packs(const struct RepoScan* scans, ptrdiff_t scan_count) {
  for (ptrdiff_t i = 0; i < scan_count; i++) {
    pack_export_remove(&scans[i].export.built);
  }
}

static int repo_scan_job(ptrdiff_t index,
                         struct JobArenas* arenas,
                         struct Log* log,
                         void* userdata) {
  const struct RepoScanJobs* jobs = userdata;
  struct RepoScan* scan = &jobs->scans[index];
  const struct SurgeRepo* repo = scan->model.config;
  oom_set_job(repo->name, log);
  // What the job keeps goes in a copy of the worker's perm, which it stores back only on success,
  // so a failed repo leaves nothing behind.
  struct Arena perm = arenas->perm;
  struct Repository repository = {0};
  if (repository_open(&repository, repo->path, repo->name, &perm, log) != 0) {
    return -1;
  }
  int rc = -1;
  if (refset_load(&scan->model.refset, &repository, &perm, log) != 0 ||
      repo_scan_job_model(scan, index, &repository, &jobs->site_config->llm_trailers, jobs->os,
                          &perm, arenas->scratch, log) != 0 ||
      release_scan_load(&scan->model, jobs->os, &perm, arenas->scratch, log) != 0 ||
      repo_scan_job_branches(scan, &repository, &perm, log) != 0 ||
      repo_scan_job_projects(scan, index, log) != 0 ||
      repo_scan_job_export(scan, &repository, jobs, &perm, log) != 0) {
    goto cleanup;
  }
  scan->repo_id = repo_scan_job_repo_id(scan, index, &perm);
  // Every page of the repo declares it, so a prefix of it keeps the output cache short, as the page
  // recipe's does.
  scan->repo_map_id = str_head(
      repository_text_id(page_context_repo_text(&scan->model, &perm), &perm), REPO_SCAN_MAP_ID_LEN);
  scan->releases_id = release_scan_id(&scan->model, &perm);
  arenas->perm = perm;
  rc = 0;

cleanup:
  if (rc != 0) {
    // The paths of a pack the job wrote are in the perm it does not store back, which the worker's
    // next job reuses, so the pack goes now, while they still hold.
    pack_export_remove(&scan->export.built);
    scan->export.built = (struct PackExport){0};
  }
  repository_close(&repository);
  return rc;
}

static int repo_scan_job_model(struct RepoScan* scan,
                               ptrdiff_t repo_index,
                               const struct Repository* repository,
                               const struct TrailerMatch* llm_trailers,
                               struct Os* os,
                               struct Arena* perm,
                               struct Arena scratch,
                               struct Log* log) {
  struct RepoModel* model = &scan->model;
  const struct SurgeRepo* repo = model->config;
  struct Arena key_scratch = scratch;
  struct Buf key = {.arena = &key_scratch};
  buf_printf(&key, "repo[%td].default_branch", repo_index);
  if (revision_resolve_default_branch(repository, &model->refset, repo->default_branch,
                                      buf_str(&key), perm, &model->default_branch,
                                      &model->default_ref, log) != 0) {
    return -1;
  }
  // The commit of each ref is what its row on the refs pages shows, and the default branch's dates
  // the repo. The note of each tag is what its feed entry and its release show.
  model->ref_commits = arena_new(perm, model->refset.count, struct CommitSummary);
  model->ref_notes = arena_new(perm, model->refset.count, struct TagNote);
  for (ptrdiff_t i = 0; i < model->refset.count; i++) {
    const struct Ref* ref = &model->refset.refs[i];
    if (ref->commit_oid.len > 0 &&
        revision_commit_summary(repository, ref->commit_oid, llm_trailers, perm,
                                &model->ref_commits[i], log) != 0) {
      return -1;
    }
    if (ref->kind == REF_KIND_TAG &&
        revision_tag_note(repository, ref, perm, &model->ref_notes[i], log) != 0) {
      return -1;
    }
  }
  if (model->default_ref != NULL) {
    model->modified = model->ref_commits[model->default_ref - model->refset.refs].date;
  }
  // An empty description that the config sets shows none, so it hides the `description` file.
  if (repo->has_description) {
    model->description = repo->description;
    return 0;
  }
  return repo_scan_job_model_read_description(model->git_dir, os, perm, scratch,
                                              &model->description, log);
}

static int repo_scan_job_model_read_description(struct Str git_dir,
                                                struct Os* os,
                                                struct Arena* perm,
                                                struct Arena scratch,
                                                struct Str* description_out,
                                                struct Log* log) {
  *description_out = (struct Str){0};
  const struct Str path = path_join(git_dir, S("description"), &scratch);
  struct OsStat st;
  if (os_stat(os, path, &st) != 0 && errno == ENOENT) {
    return 0;
  }
  struct Str text = {0};
  if (os_read(os, path, REPO_SCAN_DESCRIPTION_LEN_MAX, OS_READ_BYTES, &scratch, &text,
              S("failed to read repo description"), log) != 0) {
    return -1;
  }
  while (text.len > 0 && ascii_is_space((unsigned char)text.data[text.len - 1])) {
    text = str_head(text, text.len - 1);
  }
  if (str_eq(text, REPO_SCAN_DESCRIPTION_DEFAULT)) {
    return 0;
  }
  *description_out = str_clone(text, perm);
  return 0;
}

static int repo_scan_job_branches(struct RepoScan* scan,
                                  const struct Repository* repository,
                                  struct Arena* perm,
                                  struct Log* log) {
  struct RepoModel* model = &scan->model;
  ptrdiff_t branch_count = 0;
  for (ptrdiff_t i = 0; i < model->refset.count; i++) {
    branch_count += model->refset.refs[i].kind == REF_KIND_BRANCH;
  }
  model->trees = arena_new(perm, branch_count, struct BranchTree);
  model->logs = arena_new(perm, branch_count, struct BranchLog);
  for (ptrdiff_t i = 0; i < model->refset.count; i++) {
    const struct Ref* ref = &model->refset.refs[i];
    if (ref->kind != REF_KIND_BRANCH) {
      continue;
    }
    // A branch may name a tree or a blob, which has no history and no commit to walk the tree of.
    if (ref->commit_oid.len == 0) {
      log_warn(log, "branch names no commit, so it has no pages: '%.*s' (in repo '%.*s')",
               STR_FMT(ref->short_name), STR_FMT(model->config->name));
      continue;
    }
    struct BranchLog* branch_log = &model->logs[model->log_count++];
    branch_log->branch = ref;
    if (filetree_load(&model->trees[model->tree_count++], repository, ref, perm, log) != 0 ||
        revision_walk_log(repository, ref, perm, &branch_log->commits, &branch_log->commit_count,
                          log) != 0) {
      return -1;
    }
  }
  // Every commit a ref reaches gets a page, a tag's as well as a branch's.
  return revision_list_commits(repository, &model->refset, perm, &model->commits,
                               &model->commit_count, log);
}

static int repo_scan_job_projects(const struct RepoScan* scan,
                                  ptrdiff_t repo_index,
                                  struct Log* log) {
  const struct RepoModel* model = &scan->model;
  const struct BranchTree* tree = repo_scan_default_tree(model);
  if (tree == NULL) {
    return 0;
  }
  int rc = 0;
  const struct SurgeRepo* repo = model->config;
  for (ptrdiff_t i = 0; i < repo->project_count; i++) {
    const struct Str path = repo->projects[i].path;
    bool is_found = false;
    for (ptrdiff_t j = 0; j < tree->dir_count && !is_found; j++) {
      is_found = str_eq(tree->dirs[j].path, path);
    }
    if (!is_found) {
      rc = log_error(log,
                     "config key 'repo[%td].project[%td].path' names no directory with a tree page "
                     "on the default branch: '%.*s' (in repo '%.*s', branch '%.*s')",
                     repo_index, i, STR_FMT(path), STR_FMT(repo->name),
                     STR_FMT(model->default_branch));
    }
  }
  return rc;
}

static int repo_scan_job_export(struct RepoScan* scan,
                                const struct Repository* repository,
                                const struct RepoScanJobs* jobs,
                                struct Arena* perm,
                                struct Log* log) {
  struct RepoExport* export = &scan->export;
  const struct RepoModel* model = &scan->model;
  const struct Str repo_name = model->config->name;
  struct Buf head = {.arena = perm};
  buf_printf(&head, "ref: refs/heads/%.*s\n", STR_FMT(model->default_branch));
  export->head = buf_str(&head);
  export->info_refs = refset_format_info_refs(&model->refset, perm);
  export->info_refs_id = repository_text_id(export->info_refs, perm);

  struct CachedExport cached = {0};
  const bool has_cached = repo_scan_job_export_find(jobs, repo_name, perm, &cached);
  if (has_cached && str_eq(cached.info_refs_id, export->info_refs_id)) {
    // The same refs make the same pack, so the one in the output stays. The pack it replaced has
    // stayed one build already, so it goes now.
    export->pack_id = cached.pack_id;
    export->pack_len = cached.pack_row->len;
    export->is_pack_kept = true;
    export->pack_attributes = cached.pack_row->attributes;
    export->idx_attributes = cached.idx_row->attributes;
  } else if (jobs->is_dry_run) {
    // A pack's name is the hash of its bytes, which a dry run does not make, so it names the pack
    // by the refs it would be made from.
    export->pack_id = model->refset.count > 0 ? export->info_refs_id : (struct Str){0};
  } else {
    if (pack_export_build(&export->built, repository, &model->refset, jobs->os, perm, log) != 0) {
      return -1;
    }
    export->pack_id = export->built.pack_id;
    export->pack_len = export->built.pack_len;
  }
  // The pack this one replaces stays one build, unlisted, for a client that read the old
  // `info/refs`, when it is still in the output with its idx.
  if (!export->is_pack_kept && has_cached && !str_eq(cached.pack_id, export->pack_id)) {
    export->previous_pack_id = cached.pack_id;
    export->previous_info_refs_id = cached.info_refs_id;
    export->previous_pack_attributes = cached.pack_row->attributes;
    export->previous_idx_attributes = cached.idx_row->attributes;
  }
  export->packs = pack_export_format_packs(export->pack_id, perm);
  // A host refuses a file over its limit, which a clone needs whole, so a large pack only warns.
  const ptrdiff_t pack_len_max = jobs->site_config->limits.pack_len_max;
  if (export->pack_id.len > 0 && export->pack_len > pack_len_max) {
    const struct Str pack_path =
        pack_export_output_path(repo_name, export->pack_id, S("pack"), perm);
    log_warn(log,
             "clone export pack exceeds max pack length (%td bytes) at %td bytes, so a host with "
             "that file limit cannot serve it: '%.*s' (in repo '%.*s')",
             pack_len_max, export->pack_len, STR_FMT(pack_path), STR_FMT(repo_name));
  }
  return 0;
}

static bool repo_scan_job_export_find(const struct RepoScanJobs* jobs,
                                      struct Str repo_name,
                                      struct Arena* perm,
                                      struct CachedExport* cached_out) {
  const struct OutputCacheFact* fact =
      output_cache_find_fact(jobs->cache, str_concat(repo_name, S(".git"), perm));
  if (fact == NULL) {
    return false;
  }
  const struct StrCut fields = str_cut(fact->value, ' ');
  if (!str_startswith(fields.head, S("pack=")) || !str_startswith(fields.tail, S("refs="))) {
    return false;
  }
  cached_out->pack_id = str_tail(fields.head, 5);
  cached_out->info_refs_id = str_tail(fields.tail, 5);
  const struct Str extensions[] = {S("pack"), S("idx")};
  const struct OutputCacheRow** rows[] = {&cached_out->pack_row, &cached_out->idx_row};
  for (ptrdiff_t i = 0; i < 2; i++) {
    const struct Str relative =
        pack_export_output_path(repo_name, cached_out->pack_id, extensions[i], perm);
    *rows[i] = output_cache_find(jobs->cache, relative);
    if (*rows[i] == NULL ||
        !is_present_with_len(path_join(jobs->site_config->output_dir, relative, perm),
                             (*rows[i])->len, jobs->os)) {
      return false;
    }
  }
  return true;
}

static bool is_present_with_len(struct Str file_path, ptrdiff_t len, struct Os* os) {
  struct OsStat st;
  return os_stat(os, file_path, &st) == 0 && !st.is_dir && st.len == len;
}

static struct Str repo_scan_job_repo_id(const struct RepoScan* scan,
                                        ptrdiff_t repo_index,
                                        struct Arena* arena) {
  // Each field ends in a `NUL`. Only the description, read as bytes, can hold one, so two different
  // sets of fields hash the same text only when a `NUL` in one description lines up with the fields
  // after the other's.
  const struct RepoModel* model = &scan->model;
  const struct SurgeRepo* repo = model->config;
  struct Buf text = {.arena = arena};
  buf_printf(&text, "%td", repo_index);
  buf_append_char(&text, '\0');
  const struct Str fields[] = {model->description, model->default_branch, model->modified};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(fields) / sizeof(fields[0])); i++) {
    buf_append(&text, fields[i]);
    buf_append_char(&text, '\0');
  }
  for (ptrdiff_t i = 0; i < repo->project_count; i++) {
    const struct SurgeProject* project = &repo->projects[i];
    const struct Str project_fields[] = {project->name, project->path, project->description,
                                         project->tag_prefix};
    for (ptrdiff_t j = 0; j < (ptrdiff_t)(sizeof(project_fields) / sizeof(project_fields[0]));
         j++) {
      buf_append(&text, project_fields[j]);
      buf_append_char(&text, '\0');
    }
  }
  return repository_text_id(buf_str(&text), arena);
}

static bool is_pack_marked(const struct Manifest* manifest,
                           struct Str output_dir,
                           struct Str repo_name,
                           struct Str pack_id,
                           struct Arena scratch) {
  const struct Str extensions[] = {S("pack"), S("idx")};
  for (ptrdiff_t i = 0; i < 2; i++) {
    const struct Str relative =
        pack_export_output_path(repo_name, pack_id, extensions[i], &scratch);
    if (manifest_find_mark(manifest, path_join(output_dir, relative, &scratch)) ==
        MANIFEST_MARK_PENDING) {
      return false;
    }
  }
  return true;
}
