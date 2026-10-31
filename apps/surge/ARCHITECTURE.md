# Architecture

`surge` is a C17 static site generator for git repositories. It reads a TOML config, the repositories that the config names, Mustache templates, static files and release directories, and it writes the generated site to a directory that the site owns. This document describes the code: the directories and their dependency rules, the build pipeline, the job of each module, how it reads repositories, the output cache, the manifest, the clone export, highlighting, releases and feeds, the page data, the conventions that every module follows, the design decisions behind them, and the tests. [README.md](README.md) covers usage, the configuration and the templates, [BUILD.md](../../BUILD.md) covers the CMake build, and [STYLE.md](../../STYLE.md) covers the code style.

## Layout

- [src/app/](src/app/): The process entry point `main`, the `cli_spec` that describes the app to the `cli` library, and the commands.
- [src/build/](src/build/): Build orchestration, the scan and pages phases, the page data trees, templates, documents, releases, the clone export's outputs, and manifest population.
- [src/domain/](src/domain/): The configuration, the repository model, the mapping of a path to a grammar, text and URL helpers, and the tool name and version. Nothing here includes `<git2.h>`.
- [src/vcs/](src/vcs/): The only directory whose code includes `<git2.h>`: opening repositories, refs, history, trees, diffs and the clone pack.
- [tests/](tests/): The golden tests, the clone test and the mise test:
  - [tests/repos/](tests/repos/) holds the fixture repositories as `git fast-import` streams, with a `<name>.refs` file that pins the refs and ids of each.
  - [tests/fixtures/site/](tests/fixtures/site/) is a complete site over four of them: a config, a template set, static files and release directories. [tests/expected/site/](tests/expected/site/) is its output, and [tests/expected/site.warnings.txt](tests/expected/site.warnings.txt) its warnings.
  - Each `tests/fixtures/refuse-*` site is one that the build must refuse, with the exact `stderr` in `tests/expected/refuse-*/stderr.txt`.
  - [clone_test.py](tests/clone_test.py) and [mise_test.py](tests/mise_test.py) clone and install from the served output.
- [CMakeLists.txt](CMakeLists.txt) and [CMakePresets.json](CMakePresets.json): The project entry point and the supported CMake configurations.
- [cmake/](cmake/): The golden test driver and the scripts that make and check the fixture repositories. The root [cmake/](../../cmake/) directory holds the CMake profiles, quality tools, packaging, and reusable cross toolchains that every app uses.
- [libs/](../../libs/): The libraries that `surge` links: `cli`, `core`, `os`, `runtime`, `formats`, `markup`, `output` and `templates`, and for the unit tests `testing` and `os_memory`. [libs/ARCHITECTURE.md](../../libs/ARCHITECTURE.md) describes their modules.
- [vendor.cmake](vendor.cmake): The build of the bundled dependencies, whose sources are in the root [vendor/](../../vendor/) directory: `copt`, `tomlc17`, `mustache4c`, `acutest` and libgit2. It includes [libs/src/markup/vendor.cmake](../../libs/src/markup/vendor.cmake), which builds `md4c`, `org4c`, and `tree-sitter` with its grammars, for the `markup` library.

### Dependency rules

First-party dependencies point inward:

- `app` can depend on every other directory.
- `build` can depend on `vcs` and `domain`.
- `vcs` can depend on `domain`.
- `domain` depends on no other first-party directory.
- Every directory can depend on the libraries in `libs/`, and no library depends on `surge`.

A module can skip layers, as `cmd_build` does when it calls `repository_setup`, and modules in the same directory can depend on each other. Vendored code is outside this hierarchy. A first-party include starts at the `src/` root or at the include root of a library, for example `#include "vcs/repository.h"` or `#include "core/str.h"`.

Only `vcs/` includes `<git2.h>`. The model that the pages read, [repo_model.h](src/domain/repo_model.h), holds no libgit2 type: object ids are hex text, and trees, logs and diffs are arrays in arenas. `vcs/` is also the one directory with host access outside `os.h`, as [Host access](#host-access) describes.

## Terms

These words have one meaning everywhere in this document:

- **Arena.** A `struct Arena` from `core`. An arena never frees an individual allocation.
- **Perm.** A thread's arena for data that lives longer than the function that makes it. A function takes it by pointer, as `struct Arena* perm`.
- **Scratch.** A thread's arena for temporary data. A function takes it by value, as `struct Arena scratch`, so its allocations are free again when the function returns.
- **Log.** A `struct Log`: a growable buffer with one line for each error and an error count, and a second buffer for warnings with a warning count.
- **Warning.** A line that `log_warn` records, which starts with `warning: `. A warning never fails a build.
- **Block.** The one reservation that `surge build` makes when it starts, which it splits into its arenas.
- **Repo.** One `[[repo]]` table of the config. Its **repository** is the git repository at its `path`.
- **Scan.** The parallel phase that reads each repository into a `RepoScan`: its refs, trees, logs, notes, releases and clone export.
- **Page recipe.** Everything that every page depends on besides its own inputs. Every page declares its id.
- **Attributes.** The text that names an output's inputs in the manifest, such as `blob=<oid> mode=100644`. The output writer keeps an output whose attributes equal those that the output cache recorded.
- **Keep.** To leave an existing output in place without making it again, because the output cache shows that its inputs did not change, or because the output already holds the bytes that the build would write. The output writer marks a kept output in the manifest, as it marks a written output.

## Entry point and commands

[main.c](src/app/main.c) defines `main` and the `surge` version of `os_oom`. `main` only calls `cli_dispatch` with the POSIX host, so the unit tests can link the code that maps an action to an exit code. These are the other modules in `src/app`:

- [cli_spec](src/app/cli_spec.h): `cli_spec_init` tells the [cli](../../libs/src/cli/cli.h) library what surge is: its description and noun, its default config file, its version text, and its `build` and `config` commands. `main` hands it to `cli_dispatch`, which parses the command line, runs the command or prints the version, the help or the diagnostic, and reserves the memory of the text it prints.
- [cmd_build](src/app/cmd_build.h): `cmd_build_run` gives `build_run` from the `runtime` library its arena sizes, and `build_run` reserves the build's memory once, as one block of address space. `build_run` calls `cmd_build_execute`, prints the build's errors and then its warnings to `stderr` in one `os_print` call each, and releases the block. `cmd_build_execute` runs the [build pipeline](#build-pipeline).
- [cmd_config](src/app/cmd_config.h): `cmd_config_print` loads the config that `-c` names, `surge.toml` by default, and prints the resolved config, in the memory that `cli_dispatch` reserves for the `config` command.

## Build pipeline

`cmd_build_execute` owns the `BuildState`. It runs the first phase itself, and `run_build_phases` runs the others. Whatever the outcome, it then removes every pack that the scan wrote below `TMPDIR`, and releases what mustache4c allocated for the templates.

### Phases

1. **Load inputs** (`load_build_inputs`): `site_config_load` reads and validates the config that `-c` names, `surge.toml` by default. `read_root_require` requires `templates_dir` to be a directory, and `static_dir` and each `releases_dir` to be one when they exist. A rejected directory key stops the build here, because its default names a directory that the user did not choose. A directory that fails its check still lets the repos open and the overlap check run, so one run reports both, and the build stops before the walks. `repository_setup` sets libgit2 up for the process, as [Reading repositories](#reading-repositories) describes. `repo_scan_locate` opens each repository on the main thread to learn its git directory and its work tree, and closes it. A repository that does not open stops the build after this step. `manifest_builder_check_output_dir` then rejects an `output_dir` that overlaps an input root, as [The manifest](#the-manifest) describes, and any config error stops the build after it, so one run reports both. Last, the phase walks `templates_dir` and `static_dir`, each walk excluding `output_dir`, and stats every file for the page recipe and the copies.
2. **Load templates** (`load_templates`): `template_set_load` from the `templates` library reads and compiles every page template that the site needs and every partial once, before the build scans a repository, and reports every template that cannot be read or does not compile in one run. It leaves out the release templates when no repo has a release project. The set is read-only from then on, so every worker renders from it.
3. **Load the output cache** (`load_recipe_id`, `load_output_cache`): The build makes the id of the page recipe, and reads `_surge/output-cache.tsv` below `output_dir` with `output_cache_load`, unless `--force` tells it to ignore the file. A missing or unusable file gives an empty output cache and a verbose note, never an error.
4. **Scan** (`scan_repos` -> `repo_scan_run`): `job_run` runs one job for each repo. A job opens the repository, loads its refs with `refset_load`, resolves the default branch, reads the commit that each ref names and the note of each tag, and reads the description. `release_scan_load` then finds the release projects and the releases. For each branch that names a commit, `filetree_load` walks its tree and checks every name, and `revision_walk_log` lists its log. `revision_list_commits` lists every commit that a ref reaches. Last, the job decides the clone export: it keeps the pack in the output when the output cache vouches for it, or writes a new pack below `TMPDIR`, except in a dry run, and warns when the pack is over `pack_len_max`. A scan failure stops the build before the manifest, after every job ran, so one run reports the failures of every repo.
5. **Manifest** (`populate_output_manifest`): The build records every output path with its attributes, in this order: the clone exports, the pages of each repo, `_headers`, the release outputs, `index.html` and `404.html`, the static files, and the output cache last. It rejects every conflict that [The manifest](#the-manifest) lists, and reports all of them. If the log then holds an error, the build stops, before it has written anything.
6. **Prepare output** (`prepare_output_dir`): The build creates `output_dir`, its first write. From this point, every output goes through `output_writer_produce`, which keeps it or makes it, and makes no change in a dry run.
7. **Pages** (`write_repo_pages` -> `repo_pages_write`): A second `job_run` phase makes or keeps the pages of every repo: for each repo one job for its home, its branches and tags pages and its tag and release feeds, then for each branch one job for each directory, with the directory's tree page and its files' blob pages and raw copies, and one job for each log page, with the commit feed beside the first, and then one job for each 16 commit pages. The jobs are in repo order, so a worker's next job is mostly of the repo that it has open. A failed page is a diagnostic for that page, and the phase still writes the other pages. A verbose build then notes, for each repo, the links that named no path and the blobs without a raw copy.
8. **Clone exports** (`write_clone_exports` -> `export_writer_write`): On the main thread, the build streams each new pack and idx into `output_dir`, makes `HEAD`, `info/refs` and `objects/info/packs`, keeps whatever is current, and makes `_headers` when `headers_file` is set.
9. **Releases** (`write_release_outputs` -> `release_writer_write`): On the main thread, the build writes or keeps each repo's releases page, project pages, `versions.txt` and `versions.json`, release pages and assets. It goes on to the next repo after a failure.
10. **Site pages** (`write_site_pages`): The build renders `index.html` and `404.html`, or keeps them.
11. **Static files** (`copy_static_files`): The build copies each static file into `output_dir`, at its path below `static_dir`, or keeps a copy whose source has the size and time that the output cache recorded.
12. **Write the output cache** (`write_output_cache`): After the other outputs, and also when one of their phases failed, the build writes a row for each output that it wrote or kept and that declares attributes, and a fact for each clone pack. Until then the previous output cache stays, so an interrupted build leaves it, and that cache keeps no output that the build cut short or rewrote.
13. **Remove stale outputs** (`prune_stale_outputs` -> `output_pruner_prune`): The build removes every directory entry below `output_dir` that is not an output and is not a directory that an output is below. The prune never follows a symlink.
14. **Report the dry run** (`output_writer_report`): Only in a dry run, the build prints a `write` or `keep` line for each output in manifest order, and then a `remove` line for each path that the prune would remove, sorted.

Output is byte for byte the same for any `-w`. After the first write, a failed phase is the last one that writes outputs, except in the pages phase and the release phase, which report every output that fails. The output cache is still written, and the prune does not run.

### Why the phases are in this order

- The templates load first, and a template that fails to load still lets the scan and the manifest run, so one run reports every template error with theirs, before the first write.
- The output cache loads before the scan, and the scan writes a changed pack. The name of a pack is the hash of its content, which the manifest needs before the first write, and whether the pack is current depends on the output cache. A dry run makes no pack, so it names a changed one by the id of its `info/refs`.
- The scan walks each branch's log and lists every commit that a ref reaches, because the manifest needs the path of every log page and commit page.
- The scan job of each repo reads its release directories. A release is a tag with a directory, so the scan needs the refs that it loads, and the tags' notes come from the repository that the job has open. The first phase only checks that each `releases_dir` that exists is a directory.
- The repo homes render in the pages phase, on the worker pool. A home reads its documents' blobs, and the worker that renders it already has the repository open for the repo's other jobs.
- The release pages, version lists and assets are written on the main thread after the clone exports. They read no git object, the scan already read every tag's note, and a repo has few of them.
- The main-thread phases run in the order clone exports, releases, site pages, static files.
- The revwalk of the clone pack starts from each exported ref's commit, so the pack covers exactly the refs that `info/refs` lists.

### Errors, warnings and status output

No phase writes a diagnostic to `stderr` itself. The one exception is the out-of-memory report, which `os_oom` prints through `oom_report` before the process exits. Every fallible function records its errors in the log that its caller gives, and `build_run` prints the build's errors and then its warnings in one `os_print` call each. [Errors and diagnostics](#errors-and-diagnostics) gives the form of the messages.

Until the first write, the build continues after an error wherever the next phase still has something to check. One run reports every bad config key, every repository that does not open, every scan failure, and every output collision.

`job_run` merges the warnings of every job in job order, and the errors of the failed jobs, each with the same de-duplication and the same cap of `JOB_ERROR_REPORT_COUNT_MAX` (20) distinct job logs. A build with warnings and no errors exits with status 0.

Verbose status goes through `os_print` too. The main thread writes a status line between phases, and each worker writes its progress line, `\r<label> n/total`, in one call under the lock that `job.c` owns.

## Modules

Each item below links to the module's header, which documents its interface. The `src/app` modules are in [Entry point and commands](#entry-point-and-commands).

### Build (`src/build`)

- [repo_scan](src/build/repo_scan.h): The scan phase. `repo_scan_locate` opens each repository on the main thread for the overlap check. `repo_scan_run` scans each repo on the worker pool into a `RepoScan`: the `RepoModel` that the pages read, the `RepoExport` of the clone export, and the ids of what its pages read, its `repo` map and its releases.
- [repo_pages](src/build/repo_pages.h): The pages phase. It registers every page and raw copy of every repo with the manifest, with its attributes, plans the jobs, and makes or keeps each output through `output_writer_produce`. It reads the blobs that a blob page, a raw copy, a document or a highlighted diff needs, and only for an output that it makes.
- [export_writer](src/build/export_writer.h): The clone export's outputs and `_headers`: their manifest paths and attributes, and their writes and keeps.
- [release_scan](src/build/release_scan.h): The release projects of a repo, its releases and their assets, versions and warnings, and the id of the releases, which their outputs declare.
- [release_writer](src/build/release_writer.h): The release outputs of a repo: their manifest paths and attributes, and their writes and keeps.
- [readme](src/build/readme.h): Finds the documents of a directory, and for the root also in its `docs/`, renders them with `markup`, and resolves their links against the branch's paths through a `LinkTargets` callback.
- [highlight_diff](src/build/highlight_diff.h): Highlights a whole blob into one HTML fragment for each line, and picks the fragment of each diff line by its side.
- [page_context](src/build/page_context.h), [page_paths](src/build/page_paths.h): The root map of every page (`site`, `generator` and `path_to_root`), the `repo` and `ref` maps, the clone URL, and the output paths and relative links of the per-ref pages, with each name segment percent-encoded in a link.
- [page_index](src/build/page_index.h), [page_tree](src/build/page_tree.h), [page_blob](src/build/page_blob.h), [page_log](src/build/page_log.h), [page_commit](src/build/page_commit.h), [page_refs](src/build/page_refs.h), [page_release](src/build/page_release.h), [page_feed](src/build/page_feed.h): One module for each kind of page. Each builds the data tree of its page, and its header lists every key. `page_blob` writes the lines of a blob, and `page_commit` the rows of a diff, as HTML in C. `page_release` also writes `versions.txt`, `versions.json` and the mise example.
- [template_value](src/build/template_value.h): The data tree of a page: maps, lists, text, true and false, built in an arena before the render and read-only during it. `template_value_render` renders one page template of the build's `TemplateSet` over a tree, through the `templates` library.

### Domain (`src/domain`)

- [site_config](src/domain/site_config.h): Defines `SiteConfig` and its load and print operations. The load rejects a key outside the schema at every level, validates names, limits and paths, resolves each path against the config's directory, and never stores a rejected value.
- [repo_model](src/domain/repo_model.h): The types that the scan fills and the pages read: refs, commit summaries and details, tag notes, branch trees, logs, changesets, releases and release projects.
- [grammar_path](src/domain/grammar_path.h): Maps a file's path to the language name that `highlight_grammar_find` takes.
- [semver](src/domain/semver.h): Parses `MAJOR.MINOR.PATCH[-PRE]` and orders versions by Semantic Versioning precedence.
- [timestamp](src/domain/timestamp.h): Formats a git time and offset as RFC 3339 in that offset.
- [url_segment](src/domain/url_segment.h): Percent-encodes each byte of a path segment outside RFC 3986's unreserved set.
- [surge_version](src/domain/surge_version.h): The tool name and version, which CMake sets when it configures `surge`.

### VCS (`src/vcs`)

- [repository](src/vcs/repository.h): libgit2's setup, opening and closing a repository, reading a blob, the hex text of an object id, the git blob id of any text, and `repository_fail`.
- [refset](src/vcs/refset.h): Loads every branch and tag, peels each tag to the object that it names, refuses invalid, case-colliding and prefix refs, and formats `info/refs`.
- [revision](src/vcs/revision.h): Resolves the default branch, reads commit summaries and details and tag notes, walks a branch's log, and lists every commit that a ref reaches.
- [filetree](src/vcs/filetree.h): Walks a branch's tree, one directory at a time, into one sorted entry list for each directory, and checks every name before it becomes a path.
- [changeset](src/vcs/changeset.h): Diffs a commit against its first parent and applies the caps.
- [pack_export](src/vcs/pack_export.h): Writes the clone pack and its idx below `TMPDIR`, and formats `objects/info/packs`.

### Libraries

`surge` renders Markdown and Org with the `markup` library, which also highlights blobs, diff sides and code blocks, and escapes HTML and parses TOML with `formats`. It takes the host interface, the POSIX host and the out-of-memory report from `os`, the worker pool, the jobs, the directory walk and the check of the input directories from `runtime`, the manifest and its builder, the output writer, the prune and the output cache from `output`, its template set and renders from `templates`, its command line from `cli`, and its small primitives from `core`, among them the UTF-8 repair of every render. [libs/ARCHITECTURE.md](../../libs/ARCHITECTURE.md) describes their modules.

## Reading repositories

`repository_setup` runs once, on the main thread, before any worker starts, because libgit2's options are process-wide. It checks that the linked libgit2 is the version that the headers name (1.9) and that it has thread support, installs an allocator over `malloc` that calls `os_oom` when an allocation fails, sets every config search path level to `""`, turns owner validation off, and limits the mapped pack files to `REPOSITORY_MWINDOW_FILE_LIMIT` (128), so that a build with one worker for each CPU stays within a descriptor limit of 256 on up to about 120 CPUs. The empty search paths mean that neither the user's nor the system's git config changes what a build reads, and libgit2 reads `safe.directory` only from those configs, so with them empty no config could allow a repository that another user owns. Owner validation is off for that reason, as the [trust model](#trust-model) decides.

`repository_open` opens a repository exactly at its path, with no search of the parent directories and no `GIT_DIR`. A commit page's diff still reads `$GIT_DIR/info/attributes` and a work tree's `.gitattributes`, which decide what is binary and the hunk headers, so a kept commit page goes stale when they change, and `--force` makes it again. The repository's `diff.renameLimit` is not read: the exact-match rename detection tries every source for each target, up to 10000, so it finds every exact rename of a commit of up to 10000 changed files, as git does, while a larger one costs its targets times 10000 compares, not its square.

`filetree_load` reads each directory with `git_tree_lookup`, breadth first, rather than with `git_tree_walk`, which descends before it has seen a directory's siblings. A directory's names and collisions are checked before its subdirectories are queued, so a directory without a link is never entered. A gitlink's entry is a row, and its object is never read.

## Output cache

The output cache is `_surge/output-cache.tsv` in `output_dir`, which the `output` library's `output_cache` reads and writes, so deleting `output_dir` resets everything. Each output declares its inputs as attributes in the manifest, and `output_writer_produce` keeps an output whose attributes equal its row's and whose file has the row's size. An output without attributes is made every build, and kept when its bytes are unchanged: `HEAD`, `info/refs`, `objects/info/packs` and `_headers`. A text that an output reads, such as `info/refs` or a log page's commits, is named by its git blob id, from `repository_text_id`.

| Output | Attributes |
| --- | --- |
| Every page and feed | `recipe=` the page recipe, and `map=` the id of `page_context_repo_text` for a repo's pages, then its own: |
| Home, branches page | `repo=` the repo's place in the config, description, default branch, newest date and projects, `refs=` the id of `info/refs` |
| Tags page | as the home, and `releases=` |
| `tags.atom`, `releases.atom` | `feed=` every tag with its object and the releases, or `releases=` |
| Tree page | `tree=`, `root=` when a document is Markdown or Org, `commit=` for the branch's root |
| Blob page | `blob=`, `mode=`, `root=` when the file is Markdown or Org |
| Log page, commit feed | `log=` the page count and each commit with its refs, or `feed=` the newest commits |
| Commit page | `commit=` |
| Release page, version list | `releases=` the release projects and each release's tag, version and assets; a version list has no `map=` |
| `index.html`, `404.html` | `repos=` every repo's `repo=` and `refs=` for the index |
| Raw copy | `blob=` only |
| Static file, release asset | the source's `len=` and `mtime=`, as `manifest_builder_add_copy` gives them |
| Pack, idx | its size as a copy, or for a kept pack the attributes that the output cache recorded |

The page recipe is the generator string with the version, the grammars, the site keys and limits of the config but `pack_len_max`, and the path, size and modification time of every file below `templates_dir`. A change to what a page holds needs a new version before a release, because the version is what makes every page again. The fact `<repo>.git` holds `pack=<id> refs=<id>`, the current pack and the `info/refs` it was made for, which the scan reads.

`--force` reads no output cache and sets the writer's `is_forced`, which keeps nothing.

## The manifest

Every output path goes into the `output` library's manifest before the first write, through `manifest_builder_add`. Each output has a label that a collision message names, such as `<repo>:<oid>` for a commit page, `<repo>:<branch>` for a log page or a commit feed, and `<repo>:<tag>` or `<repo>:<project>` for a release output, so a message about two outputs of one repo names both.

### Overlap with the inputs

In the first phase, `manifest_builder_check_output_dir` compares, by device and inode, `output_dir` or its nearest existing ancestor, and each of its ancestors, with each input root: `templates_dir`, `static_dir`, and the git directory, the work tree and the `releases_dir` of each repo. The git directory is the common one, `git_repository_commondir`, so for a linked worktree it is the main repository's, which holds the objects and refs. It also checks the other way round, for each root and the directory of the config. This rejects an `output_dir` inside an input, and an `output_dir` that holds one, such as `output_dir = "/srv/git"` with a repository at `/srv/git/foo.git`, before any walk. A missing root is skipped. The walks of `templates_dir` and `static_dir` exclude `output_dir`.

### Collision policy

The build is refused, with a message that names both sides, for:

- two repos or two projects of one repo whose names collide after ASCII case folding, a reserved name, and two overlapping tag prefixes, in the config load
- a top-level static entry named like a repo or its `<repo>.git` after ASCII case folding, once for each entry, whose files then stay out of the manifest
- two branches whose names, or the directories above them, collide after case folding, a ref that is a `/`-prefix of another, a ref name that git calls invalid, and one that is not UTF-8, in `refset_load`, which checks every ref before it stops
- in the manifest, two outputs at one path after ASCII case folding, an output that one path must hold both as a file and as a directory, such as the branches `x` and `x.atom` of one repo, an output at or below an input root or over an input file, and an output path over `OUTPUT_PATH_RELATIVE_LEN_MAX` (1024) or with a segment over `FILENAME_LEN_MAX` (255)

A tree entry has its own policy, because a vetted repository can hold the Linux kernel's `xt_CONNMARK.h` and `xt_connmark.h`, and a crafted object can hold any name. `filetree_load` gives an entry a row without a link, no pages and no raw copy, and one warning that names every path involved, for: a name that a page cannot use, names of one directory that collide after ASCII case folding, with no winner picked, and a name whose pages would pass an output-path limit. Only linked entries reach the manifest, so the manifest's path limit applies to the outputs that are not listing rows, such as a branch's root tree page. A release outside every project, a version that does not parse, a repeated version and a release without `SHA256SUMS` are warnings of the release scan.

The prune is `output_pruner_prune`. It compares device and inode values, never follows a symlink, and runs only when every write succeeded.

## Clone export

The clone export is built in two steps: the scan decides it and writes a changed pack below `TMPDIR`, and the main thread later streams it into `output_dir`.

- `refset_load` loads `refs/heads/*` and `refs/tags/*`, skips symbolic refs, and peels each tag. `refset_format_info_refs` writes `info/refs` sorted by refname, with a `^{}` line after each annotated tag, as `git update-server-info` writes it.
- `HEAD` is `ref: refs/heads/<default branch>`. An empty repository has no branch, so its `HEAD` names the branch that the repository's `HEAD` names, which does not exist yet, with an empty `info/refs`, no pack, and an empty `objects/info/packs`.
- `pack_export_build` feeds a revwalk from the commit of every exported ref into `git_packbuilder_insert_walk`, and passes each ref's own object to `git_packbuilder_insert_recur`, because a revwalk skips a ref that does not peel to a commit, and a tag can name a tag, a tree or a blob. It runs the pack builder with one thread, writes the pack and the idx into a new directory below `TMPDIR`, or `/tmp`, and names the pack with `git_packbuilder_name`. The export never changes the source repository, and never publishes its `config` or hooks.
- **Pack stability.** libgit2 1.9.7 writes the same pack for the same objects with one thread. Across a cold build, a `--force` build with `-w 1` and one with `-w 8`, every pack and idx of the fixtures was identical, so the golden test compares the packs and idx files byte for byte, and checks them with `git verify-pack` and a strict `git index-pack` as well.
- **Keep.** The scan keeps the pack in the output when the export's fact records the same `info/refs` id, and the pack and idx are there with the sizes of their rows. Otherwise it writes a new pack, whose name the manifest records before the first write, or in a dry run names it by the `info/refs` id without making it. `export_writer_write` streams a new pack and idx from the temporary directory through `output_writer_produce`, as copies, so the manifest, a dry run and the prune see them, and keeps `HEAD`, `info/refs` and `objects/info/packs` when the output holds their bytes. It writes the pack and the idx before the files that name them, so a client never reads an `info/refs` whose objects are not there yet. A change of the default branch alone writes only `HEAD`.
- **Retention.** When a build replaces a pack, the manifest claims the pack that the fact names, unlisted, and `output_writer_keep` keeps it before the first page, without `output_writer_produce`, because no input of this build could make it again, so a client that read a stale cached `info/refs` can finish its clone. It declares the attributes that the output cache recorded for it, and the fact names it until the new pack is written, so a build that fails before then keeps it for the next build. The build after the one that wrote the new pack drops it. No glob ever selects a pack to delete: the prune removes what the manifest does not claim. A `--force` build reads no output cache, so it retains no previous pack.
- `cmd_build_execute` removes every temporary directory on every path. An out-of-memory exit or a signal leaves them in `TMPDIR`, as fram's video frames are left.
- `_headers` sets `Cache-Control: no-cache` on every export's `HEAD`, `info/refs` and `objects/info/packs`, with one splat rule each, and `public, max-age=31536000, immutable` on each repo's packs. Its paths start with the path of `base_url`. Cloudflare Pages reads 100 rules with one splat each, so a site of up to 97 repos fits.

## Highlighting

`libs/markup` vendors the tree-sitter runtime and grammars, embeds their queries, evaluates the query predicates in C, closes each span at a line end, and falls back to escaped text past its memory and work limits. `surge` adds three things:

- **Languages from paths.** `grammar_path_language` maps an exact file name, such as `Makefile` or `CMakeLists.txt`, or else an extension in any ASCII case, such as `.c` or `.TSX`, to the language name that `highlight_grammar_find` takes. A language whose grammar the binary does not link, such as `make`, `cmake` or `dockerfile` today, finds no grammar, and the file is escaped. Nothing reads the file's bytes, so the same path always gets the same language.
- **Blob pages.** `page_blob_lines` calls `highlight_write` once for each blob, and the line callbacks write each line as a code line of `body_html`, whose number links to `#L<n>`. The page's lines reach the template as one pre-escaped fragment, `source_html`, so a page of 100,000 lines costs one interpolation, within the render's limits.
- **Diffs.** `highlight_diff_lines` highlights the whole old blob and the whole new blob of a changed file once each, into one fragment for each line, in the job's scratch. `highlight_diff_file` gives each diff line the fragment of its side by line number: the old side for a deletion, and the new side for an addition or a context line. It copies only the shown lines' fragments into the page's perm. A line past its side's lines is escaped alone. This keeps a line inside a multi-line comment or string right, which highlighting each diff line alone does not. `highlight_commit` and `highlight_side` in `repo_pages.c` read the blobs, and highlight only when the commit's whole diff is under its caps, for a path that names a grammar, and for a blob under `blob_len_max`. A gitlink side has no blob, so it is never read or highlighted.

Both sides of every changed file roughly double the first build's time for the commit pages, and later builds keep those pages.

## Releases and feeds

The scan reads the note of every tag, `ref_notes`, through `revision_tag_note`: an annotated tag's message, tagger and date, or for a lightweight tag the date of its commit. `release_scan_load` then runs in the same job. It finds the release projects, stats `<releases_dir>/<tag>/` for each tag, lists the regular files of each release directory with `os_list` and `os_stat`, and gives each release its tag's note, so it reads no git object itself. It parses each version with `semver_parse`, sorts each project's releases, and records the warnings. `release_scan_id` hashes what the release outputs read into the `releases` row, so a new, changed or removed asset, or a release directory added for an old tag, makes the release outputs again, and a moved branch keeps them. The `repo` row could not carry these ids, because it holds the default branch's date, which every new commit moves.

`release_writer` runs on the main thread. It registers and writes the releases page, the project pages, `versions.txt` and `versions.json`, the release pages and the assets. A page or a version list is kept when the templates and the `releases` row are fresh and the file exists. An asset is streamed from `releases_dir` with `OS_SOURCE_FILE`, with no size limit, and is kept when the `releases` row is fresh and the output has the asset's size. `page_release_mise_toml` builds the mise example of a project with `mise` from the newest listed release that is not a pre-release, because mise's `latest` passes over a version with a `-PRE`. The release templates are compiled only when a repo has a release project.

The feeds are templates, `commits.atom`, `tags.atom` and `releases.atom`, rendered in the pages phase: a branch's commit feed in the job of its first log page, and the tag and release feeds in the repo's own job. `page_feed` builds their data trees with absolute links below `base_url`, because a reader may show an entry apart from the feed. A feed or an entry without a date gets the Unix epoch. `tags.atom` and `releases.atom` keep the newest `feed_count` entries, as the commit feed does.

## Templates and page data

A page's data is a `TemplateValue` tree that the page module builds in C before the render. mustache4c looks up a name that a map does not hold in the enclosing scopes, so every map holds every key of its schema, with empty text or `false` where a value is absent. Otherwise a missing key would silently show an outer value, such as the repo's `name` where an entry had none. Each page's header in `src/build/page_*.h` lists its keys, and [README.md](README.md#data-trees) gives the same schema to template authors.

The HTML that `surge` makes itself, the rendered documents, the blob lines and the diff rows, reaches the template as pre-escaped fragments in `*_html` keys, which a template writes with `{{{…}}}`. Only such a key renders unescaped, and an unescaped tag of any other key fails the render. The render repairs every text it writes with `utf8_repair_for_xml`, literal template text included, so the data trees hold names, refs and messages as git stores their bytes, and no page or feed holds an invalid sequence or a character that XML 1.0 forbids. The lines of a blob or a diff are rows that C writes, so a page of many lines costs the render one interpolation.

## Cross-cutting conventions

Every module follows these rules. When a design decision is behind a rule, the link goes to its reasons and costs.

### Memory

- Every first-party allocation comes from an arena, and each thread has a perm and a scratch. The build's log has an arena of its own. A function that keeps what it allocates takes the perm by pointer, and a function that needs only temporaries takes the scratch by value, so its temporaries are gone when it returns. `template_value_render` takes the arena that owns the page by pointer, where nothing else of the render allocates, and the render's scratch by value. The `page_*_value` builders take the perm that owns the tree by pointer, and a scratch by value when they need temporaries, such as the anchors of a diff line. See [Memory: arenas only](../sosig/ARCHITECTURE.md#memory-arenas-only) and [Memory: a perm and a scratch for each worker](#memory-a-perm-and-a-scratch-for-each-worker).
- A command reserves its memory once, when it starts. It splits the block into arenas of fixed sizes, and an arena commits pages only when its allocations get to them. See [Memory: reserve, then commit on demand](#memory-reserve-then-commit-on-demand).
- An allocation that does not fit calls `os_oom`, which reports and exits, so no call site checks for allocation failure. The vendored libraries that still use `malloc`, libgit2 among them, also end in `os_oom`. See [Out of memory exits the process](#out-of-memory-exits-the-process).
- Under the address sanitizer, two arena allocations are never adjacent, so code that extends the last allocation in place uses `arena_extend`. See [Address sanitizer coverage in arenas](../sosig/ARCHITECTURE.md#address-sanitizer-coverage-in-arenas).
- A log's arena is never an arena that a callee gets by value, so the log never grows into bytes that a callee uses. See [A log must not grow in a borrowed arena](../sosig/ARCHITECTURE.md#a-log-must-not-grow-in-a-borrowed-arena).

### Threads and ownership

Two jobs that run at the same time never share an arena, a log or a repository. Every job reads the same immutable data: the config, every `RepoScan`, the `TemplateSet`, the output cache, and the manifest's attributes. mustache4c only reads a compiled template while it renders, so every worker renders from the one `TemplateSet`. The highlighter's grammars and compiled queries are shared, and load once under a lock in `markup`. Each job writes only its own job log, its own slot of the notes, and the manifest marks of the outputs that it writes or keeps. The manifest makes sure that no two jobs write the same file. `pool_run` joins every worker before the main thread reads any result. See [Memory: a perm and a scratch for each worker](#memory-a-perm-and-a-scratch-for-each-worker).

A `git_repository` is never shared between threads. Each scan job opens its repository and closes it when it ends. In the pages phase, `repo_pages_write` gives each worker one `RepoPagesWorker` slot, which only that worker touches, found from the worker's `JobArenas` pointer as `arenas - worker_arenas`. A job opens its repo in its worker's slot only when it reads a git object, and keeps it open for the worker's next job of the same repo. A job of another repo closes it and opens its own. Because the jobs are in repo order, a worker mostly reuses one repository. After `job_run` joins the workers, the main thread closes every slot's repository.

### Strings and text input

Every first-party string is a counted `struct Str` with a signed `ptrdiff_t` length, and every length and count is a `ptrdiff_t`. An object id is hex text in a `struct Str`. See [Counted strings with signed lengths](../sosig/ARCHITECTURE.md#counted-strings-with-signed-lengths).

`surge` reads the config, the templates and the partials with `OS_READ_TEXT`, which rejects an embedded `NUL`. It reads a repository's `description` file with `OS_READ_BYTES` and keeps its bytes as they are, less the white space at its end. The names, refs, messages and blobs that libgit2 reads keep the bytes that git stores, and the render repairs every text that it writes, as [Templates and page data](#templates-and-page-data) describes. The repair drops a `NUL`, so no page or feed holds one, and a raw copy holds its blob's bytes as they are. `toml_datum_is_text` rejects a TOML string that contains a decoded `NUL`. `toml_require_known_keys` rejects a TOML key that contains a `NUL`, before it names the key. A path that contains a `NUL` fails at the `os` boundary with `EINVAL`, because a system call would read the path only up to that byte.

### Errors and diagnostics

- A fallible action whose caller needs a message beyond success or failure takes a final `struct Log* log`. It records every error that it finds in that log, and never truncates one. A message is one `printf` format that the compiler checks. See [A growable log, and every error in one run](../sosig/ARCHITECTURE.md#a-growable-log-and-every-error-in-one-run) and [Log messages use checked format strings](../sosig/ARCHITECTURE.md#log-messages-use-checked-format-strings).
- A filesystem action also takes the `operation` of the caller, such as `failed to read config`, and logs the whole message, because only the action knows which path failed. A failure inside a file names the file as `(in '<file>')` through `log_error_in`.
- A failure inside libgit2 on an open repository reads `<operation>: <libgit2 message> (in repo '<repo>')`, and only `repository_fail` writes it. The setup of libgit2 and a repository that does not open have their own messages. Every one keeps libgit2's own message. A message about a repository ends with `(in repo '<repo>')`, and a message about a branch with `(in repo '<repo>', branch '<branch>')`. `(in '<file>')` is kept for the file that holds a failure.
- A warning records, through `log_warn`, a problem that the build works around, and says what the build does about it after `, so`: `tree paths collide after case folding, so none of them has pages: ...`.
- An action, and a producer that can fail, return `0` or `-1`, and the producer writes its result through an `_out` parameter. A producer that cannot fail returns its result directly, for example a `struct Str` by value. A predicate returns `bool`. Running out of memory is never a return value. [STYLE.md](../../STYLE.md#function-signatures) gives the full rules.
- A first-party diagnostic is lowercase prose. It names the failed operation first, puts literal names and values in single quotes, and puts a value after the cause. Messages from the system or from a vendored library, libgit2 among them, use their original capitalization and punctuation. [STYLE.md](../../STYLE.md#errors-and-diagnostics) gives the full rules.

### Paths and limits

- **Path safety.** The config load checks each repo and project name, and `filetree_load` checks every name of a tree before it becomes a path, as [Collision policy](#collision-policy) describes. A link percent-encodes each name segment with `url_segment`. Three checks cover symlink aliases that already exist, each at its own boundary: the identity checks of the input roots, the exclusion of `output_dir` from the walks, and the identity checks of the inputs: the config, the templates, the static files and the release assets.
- **Input size limits.** Each caller of `os_read` gives a limit for the kind of file that it reads. The read checks the limit before it allocates, so it never loads a file that is too large. `TOML_CONFIG_LEN_MAX` in the `formats` library is 1 MiB for the config. `TEMPLATE_FILE_LEN_MAX` in the `templates` library is 4 MiB for each template and each partial. `OUTPUT_CACHE_FILE_LEN_MAX` in the `output` library is 256 MiB, and a larger output cache is an empty one. `REPO_SCAN_DESCRIPTION_LEN_MAX` in `repo_scan` is 64 KiB for a repository's `description` file. libgit2 reads a blob whole, and the config's `blob_len_max` bounds a blob that a page shows, and `raw_len_max` a blob that a raw copy holds. `os_write` streams a static file, a release asset and a pack from the file itself, so none of them has a size limit. An output that is made is compared with the file in the output in the scratch half that its make used, which holds the largest page, 256 MiB, many times over. A raw copy's blob gets a region of its own, and its comparison the rest of the job's 3 GiB scratch, which holds two blobs of the 1 GiB that bounds `raw_len_max`.
- **Path limits.** The manifest checks each output path against two limits, through `output_path_check_limits`. `OUTPUT_PATH_RELATIVE_LEN_MAX` (1024) limits the whole relative path, and `FILENAME_LEN_MAX` (255) limits each path segment. A tree entry whose pages would pass a limit gets a warning and no pages instead, as [Collision policy](#collision-policy) describes.

### Host access

No code in `src/` outside `vcs/` makes a system call on a path or prints through `stdio`: only the POSIX host, `os/os_posix.c` in `libs/`, does. libgit2 opens repositories and reads their objects with its own system calls, and `pack_export` writes the clone pack through libgit2 into a directory that it makes with `mkdtemp` below `TMPDIR`, and removes with `unlink` and `rmdir`. Everything else goes through `os.h`: the config, the templates, the static files, a repository's `description` file, the release directories, every output, and the copy of a pack from its temporary directory into `output_dir`. fram's child processes and video frames are the other exception in the repository. The link step selects the host for each executable. A new unit test links the in-memory host, unless it reads a repository through libgit2. See [libgit2 outside `os.h`](#libgit2-outside-osh) and [The `os` interface](../sosig/ARCHITECTURE.md#the-os-interface).

## Design decisions

Each decision below states what the code does and why, and some decisions also state what they cost. Read the decision before you change one of these designs, and make sure that the change deals with each of its reasons. The decisions come in groups, in this order: memory, repositories, and the build of libgit2. The decisions that `surge` shares with sosig through `libs/`, arenas only, the address sanitizer coverage in arenas, counted strings, the growable log, the log that must not grow in a borrowed arena, checked format strings, the `os` interface and where the shared code lives, are in [sosig's ARCHITECTURE.md](../sosig/ARCHITECTURE.md#design-decisions).

### Memory: a perm and a scratch for each worker

**Decision.** Each pool worker has its own perm and its own scratch, as one `JobArenas`, which lives across every job and both parallel phases that the worker runs. Its perm keeps what must outlive a job, and its scratch holds one job's temporaries.

- A scan job builds everything that it finds in a copy of its worker's perm, and stores the copy back as the worker's perm only when the repo scanned completely. The paths of a pack that the job wrote are in that copy, so a failed job removes the pack before it returns, while the paths still hold.
- A page job never stores its copy of the perm back. It builds a page's data tree in a copy of the perm, renders the page into a region of its scratch, writes it, and drops both. The only thing that a page job keeps in its worker's perm is the paths of a repository that it opens, which a later job of the same repo still reads. After a write, `render_page` gives back the page's memory with `arena_discard`, a blob page or a commit page gives back its data tree's memory too, because highlighting and rendering made it, and a job gives back a blob of `JOB_SOURCE_DISCARD_LEN_MIN` (16 MiB) or more after its outputs are written, so a worker does not keep its largest page or blob resident.
- The main-thread phases render into copies of the main thread's scratch. A page grows in an arena that the render's scratch does not overlap, so the output grows in place: `write_site_page` gives each of them half of what is left, so a halved reservation halves both.

**Reasons.**

- A bump arena is not thread-safe, and separate arenas need no lock.
- A failed repo leaves nothing in the arena.

**Cost.** Every `RepoScan` stays in the perm of the worker that scanned it until the build ends, because the pages phase and the main-thread phases read it, so the peak memory of a build grows with the trees, logs and notes of every repo.

### Memory: reserve, then commit on demand

**Decision.** `cmd_build_run` has `build_run` reserve the build's memory once, as one block of address space, and split it into arenas of the sizes it gives:

- the build's log, `BUILD_LOG_LEN` (256 MiB), which no other code allocates from
- the main thread's perm, `BUILD_PERM_LEN` (16 times `TEMPLATE_RENDER_OUTPUT_LEN_MAX`, 4 GiB)
- the main thread's scratch, `BUILD_SCRATCH_LEN` (3 GiB), and one `BUILD_LOG_LEN` share of job logs for each worker and one more
- a perm and a scratch of the same sizes for each worker, as one `JobArenas` each

Under an address-space limit, `build_run` halves the workers and then the block, as [libs/ARCHITECTURE.md](../../libs/ARCHITECTURE.md#build-memory) describes.

### Out of memory exits the process

**Decision.** When an arena cannot commit more pages, when the system refuses even the smallest reservation, or when the allocation of a vendored library fails, the program calls `os_oom`. Each program defines `os_oom`. The `surge` version, in [src/app/main.c](src/app/main.c), calls `cli_oom_exit` from [cli/cli_dispatch.h](../../libs/src/cli/cli_dispatch.h) with the name `surge`, and `cli_oom_exit` does these steps:

1. It takes a lock, so the first of two workers that run out at once prints its report and exits while the second waits.
2. It prints the report of `oom_report` from [os/oom.h](../../libs/src/os/oom.h): the errors in the command's log and in the thread's job log, then `surge: out of memory` and the name of what the thread was working on, as `(in '<name>')`. The report prints errors only, so a scan's warnings are not shown when a build runs out of memory, and the next build prints them again.
3. It exits with `EXIT_CODE_FAILURE` through `_Exit`, which runs no `atexit` handler that another thread could be using.

`oom_set_log` and `oom_set_job` name what the report prints. `build_run` from [runtime/build_run.h](../../libs/src/runtime/build_run.h) names the build's log with `oom_set_log`, as `cli_dispatch` names the log of the `config` command. A job names what it works on and its log with `oom_set_job` when it starts: a scan or page job names its repo. A main-thread phase names what it writes, with no job log, as it goes: the repo of each clone export, `_headers`, the repo of each repo's releases, each site page, each static file that it copies, and the output cache.

**Reasons.**

- `surge` is a CLI tool, and it is easy to make its output directory again.
- No call site checks for allocation failure.
- The report names the repo that the thread was working on. A user needs that name to find the input that failed.

**Vendored code.** libgit2 allocates through the allocator that `repository_setup` installs with `GIT_OPT_SET_ALLOCATOR`, which calls `os_oom` when `malloc` fails, so its out-of-memory ends in the same report. tomlc17, md4c, org4c, mustache4c and tree-sitter end in `os_oom` as they do in sosig.

### Trust model

**Decision.** The repositories are the user's own and their contents are vetted, so `surge` treats them as sosig treats its content. READMEs, documents, release notes, and Markdown and Org blobs render with `libs/markup` unchanged: raw HTML stays enabled, the targets of its embedded files and links resolve as the markup's do, and link targets that `readme` does not resolve pass through as they are written. `raw/` holds every blob up to `raw_len_max`, SVG included, on the site's origin. `surge` writes no Content Security Policy, because the templates are the user's. libgit2's owner validation is off, so a repository that another user owns publishes.

**Reasons.**

- The repositories are the user's own, and their contents are vetted, so a page has no untrusted author.
- `libs/markup` records the same trust decision for sosig in `libs/src/markup/markdown.c`, so both apps render Markdown and Org in one way.
- With every config search path empty, no `safe.directory` could allow a repository at `/srv/git` owned by `git` while the build runs as its user.

**What stays.** The checks that protect the output tree stay on, because a vetted repository can still hold long or case-colliding paths, and a crafted object can hold any name: the overlap check, the manifest, the name checks of `filetree_load`, and the prune that never follows a symlink. Reversing this decision means a Markdown renderer with `MD_FLAG_NOHTML` and a scheme allowlist.

### libgit2 outside `os.h`

**Decision.** `surge` reads repositories through libgit2, and never starts the `git` command. libgit2 makes its own system calls, and `vcs/` is the one directory that calls it. The tests of `vcs/` run on the POSIX host, over real fixture repositories.

**Reason.** libgit2 reads objects, packs, refs and attributes in the process, so a worker reads every blob and commit of a job from one open repository. A tool that reads through the `git` command, as gitmal does, starts one `git show` for each blob and each commit.

**Cost.** Each thread holds one open repository at a time, and the descriptors of the mapped packs. `REPOSITORY_MWINDOW_FILE_LIMIT` bounds the mapped packs across the process.

### libgit2 built by upstream's CMake

**Decision.** [vendor.cmake](vendor.cmake) adds libgit2 1.9.7 with `add_subdirectory`, after it forces the options that keep the build small and offline: no shared library, tests, CLI, examples or fuzzers, no HTTPS, SSH or GSSAPI, the bundled zlib, threads on, and the libc's POSIX regex. The same build serves the debug presets, CI and the Zig `musl` release presets, with no `pkg-config` and no `find_package`.

**Reasons.** libgit2's configure checks find the platform's nanosecond `stat` field, `futimens`, the `qsort_r` variant, `iconv` and the entropy source on macOS and Linux `musl`. A hand-written config header would need `#if` branches for each, and a wrong define still compiles. The checks work under both Zig toolchains, so no hand-written config header is needed.

**Settings.** Each object library of libgit2 links `apps_build_vendor`, so the sanitizer builds instrument it, and compiles with `-w`. The build of libgit2 turns `CMAKE_LINK_LIBRARIES_ONLY_TARGETS` off, because libgit2 links `rt` by name, and turns off the lookups of OpenSSL and GSSAPI, so a cross build never finds the build host's libraries. CPack installs only the `surge_runtime` component, because libgit2's own `install()` rules would put `libgit2.a` and its headers in the archive.

## Testing

### Unit tests

Most modules have an acutest file, registered with `apps_add_unit_test`. The suites that reach a host through `os/os.h` run on the in-memory host, and the others link the POSIX one without using it. The six suites of `vcs/`, `changeset`, `filetree`, `pack_export`, `refset`, `repository` and `revision`, and `cmd_build`, read real repositories through libgit2, so they run on the POSIX host, after `surge.fixture_repos_setup` makes the fixture repositories from their streams. `cmd_build` builds sites of the golden fixture's templates and the `showcase` repository in the system's temporary directory. Without `git`, the configure step leaves these suites out. `export_writer`, `page_context`, `page_index`, `release_scan`, `release_writer`, `repo_pages` and `repo_scan` have no suite of their own: the golden tests run them end to end.

### Fixture repositories

The fixture repositories are small, hand-written `git fast-import` streams in [tests/repos/](tests/repos/), with fixed dates, so their object ids are the same on every run. [cmake/surge_make_fixture_repos.cmake](cmake/surge_make_fixture_repos.cmake) replays them with `GIT_CONFIG_NOSYSTEM=1`, a `HOME` of its own, a pinned hash and ref format, and `core.ignorecase` off, and adds what `git fast-import` refuses: a nested tag and a tag of a tree, a signed commit that the hostile fixture's tag `signed` names, tree entries named `..`, `.GIT` and `Git~1` or with a control byte, an empty repository, two branches that differ only in case, and a branch `feature` next to `feature/x` in a hand-written `packed-refs`. `surge.fixture_repos` checks that each repository holds exactly the refs and ids of its `<name>.refs`, and runs a strict `git fsck` on those that hold no hostile object.

Together the repositories cover: a Markdown README with raw HTML and relative links, an Org document, the other document kinds in the root and in `docs/`, PNG and SVG images and a binary file, a subdirectory with its own README, a history longer than one log page, names with spaces, `&`, `%`, `#`, `+` and `ñ`, case-colliding tree paths, a name over `FILENAME_LEN_MAX`, hostile names, a symlink and a gitlink, a merge, a rename, a deletion, a file that becomes a symlink, a diff of two hunks, a root commit, a diff over the caps, a hunk inside a comment that starts above it, branches `main` and `feature/x`, lightweight, annotated and nested tags, an empty repository, a bare repository and a working copy, non-UTF-8 and control characters in messages, and a monorepo with two projects, one with releases.

### Golden tests

[cmake/surge_run_golden.cmake](cmake/surge_run_golden.cmake) runs each fixture site:

1. It copies the site into a scratch tree, makes the fixture repositories there, and puts a stale page, a stale directory tree and a stale pack in the output, which the first build must remove. It points `TMPDIR` at a scratch directory.
2. It builds the site three times: with `-w 1`, with `-w 2` over the output cache, and with `--force -w 8`. The first build's `stderr` must be the warnings in `tests/expected/site.warnings.txt`, and the later builds must not change a generated file.
3. A dry run must then keep every output, the packs included, and write only the output cache. `TMPDIR` must be empty.
4. It compares the output with `tests/expected/site/` byte for byte, the packs included, after it replaces `@PROJECT_VERSION@` in each expected text file. It finds the names on each side through the filesystem, because a sync tool can rewrite the Unicode normalization of a checked-out name, which macOS matches either way.
5. It checks each pack with `git verify-pack` and a strict `git index-pack`, and each `info/refs` against `git for-each-ref` of its source.
6. It runs the clone test with `python3`, and the mise test with `python3` and mise.
7. It moves a branch with a new commit. A dry run must write only what the move decides: the branch's log pages and feed, the new commit page, the page of the branch's root, the repo's home and refs pages, the site index, the clone export's changed files and new pack, and the output cache. After a build that fails at a page, the next build must keep the previous pack, and the build after it must remove it.
8. It adds a release directory for an old tag and removes another. A dry run must write the repo's release outputs, its tag and release feeds and the tags page, remove the removed release's outputs, and keep everything else.
9. Then, as they change the site's inputs: a `--force` dry run must keep nothing, and a dry run after a partial changes may keep no page or feed outside `<r>.git/`.
10. Next, it sets `blob_len_max` below `raw_len_max` and gives `base_url` a path. The raw copy of a blob between the two limits must hold the blob's bytes, after a build and again after the copy is removed and the next build keeps the blob's page, the page must say that the blob is too large, and every rule of `_headers` must start with the path.
11. Last, it changes one blob. A dry run must write only what the change decides: its blob page and raw copy, the tree pages above it, the pages that show Markdown or Org, the pages that a new commit moves, and the clone export. It must keep every other output.

A site whose expected directory holds only `stderr.txt` is a refusal: one build must exit with status 1 with exactly that text, and create no `public/`. The refusal sites are case-colliding branches, a ref prefix, a project `path` that names no directory of the default branch, a `releases_dir` that is a file, and static entries named like a repo.

### Clone and mise tests

[tests/clone_test.py](tests/clone_test.py) serves the output on two localhost ports, from threads, so no process outlives the test. The first server answers a missing path with 404, and every `<r>.git` must clone with the branches and tags of its source, and pass `git fsck` but `hostile`'s. The second answers a missing path with 200 and an HTML page, as Cloudflare Pages does without a top-level `404.html`. With git 2.56, a clone from it reports the page as a corrupt object for each loose object that git asks for, then finds the objects in the pack and finishes, so the test checks for those errors rather than for a failed clone.

[tests/mise_test.py](tests/mise_test.py) copies the output, serves it, reads the mise example from the fixture's `alpha` project page with the site's `base_url` swapped for the server's, and keeps `HOME`, the four `MISE_*` directories and mise's config files in its scratch tree. `mise ls-remote` must list `0.1.0` and `0.2.0` and pass over `0.10.0-rc.1`, `mise lock` must record a `SHA256SUMS` digest, and `mise exec -- alpha` must print `alpha 0.2.0`. A tampered `SHA256SUMS` behind a fresh lock, and a tampered archive behind a good lock, must each fail with a checksum mismatch.

### Threads

`ci-tsan` runs the golden test with `-w 1`, `-w 2` and `-w 8` under ThreadSanitizer, over highlighted blob pages and commit pages in three repos, with the first load of a grammar raced by several workers. libgit2 and tree-sitter are instrumented, and the build has no ThreadSanitizer suppressions, so a race reported in either is fixed in the code.
