# Architecture

`surge` is a C17 static site generator for git repositories. It reads a TOML config, the repositories that the config names, Mustache templates, static files and release directories, and writes the site to a directory that it owns. This document describes the code. [README.md](README.md) describes usage, the configuration and the templates. [BUILD.md](../../BUILD.md) describes the CMake build and the tests, and [STYLE.md](../../STYLE.md) describes the code style.

## Layout

- [src/app/](src/app/): The entry point `main`, the `cli_spec` that describes the app to the `cli` library, and the commands.
- [src/build/](src/build/): The build pipeline, the scan and pages phases, the page data trees, documents, releases, and the outputs of the clone export.
- [src/domain/](src/domain/): The configuration, the repository model, the map from a path to a grammar, text and URL helpers, and the tool name and version.
- [src/vcs/](src/vcs/): The only code that includes `<git2.h>`: repositories, refs, history, trees, diffs and the clone pack.
- [tests/](tests/): The fixture repositories in [tests/repos/](tests/repos/), the fixture sites in [tests/fixtures/](tests/fixtures/), their expected output in [tests/expected/](tests/expected/), and the clone and mise tests.
- [cmake/](cmake/): The golden test driver, and the scripts that make and check the fixture repositories.
- [vendor.cmake](vendor.cmake): The build of `copt`, `tomlc17`, `mustache4c`, `acutest` and libgit2, from the root [vendor/](../../vendor/) directory. It includes [libs/src/markup/vendor.cmake](../../libs/src/markup/vendor.cmake) for `md4c`, `org4c` and tree-sitter.
- [libs/](../../libs/): The shared libraries that `surge` links: `cli`, `core`, `os`, `runtime`, `formats`, `markup`, `output` and `templates`, and `testing` and `os_memory` for the unit tests.

First-party dependencies point inward:

- `app` can depend on every other directory.
- `build` can depend on `vcs` and `domain`.
- `vcs` can depend on `domain`.
- `domain` depends on no other first-party directory.
- Every directory can depend on `libs/`, and no library depends on `surge`.

A module can skip a layer, and modules in one directory can depend on each other. A first-party include starts at the `src/` root or at a library's include root, such as `#include "vcs/repository.h"` or `#include "core/str.h"`. The model that the pages read, [repo_model.h](src/domain/repo_model.h), holds no libgit2 type. Object ids are hex text, and trees, logs and diffs are arrays in arenas.

## Build pipeline

`cmd_build_execute` owns the `BuildState`, runs the first phase, and calls `run_build_phases` for the others. On every path, it then removes the packs that the scan wrote below `TMPDIR`, and releases the templates. An out-of-memory exit or a signal leaves those packs in `TMPDIR`.

### Phases

1. **Load inputs** (`load_build_inputs`): Load the config, check the input directories, set up libgit2, open each repository to find its git directory, reject an `output_dir` that overlaps an input, and list and stat the templates and static files.
2. **Load templates** (`load_templates`): Read and compile every template that the `[templates]` table names, each once, and every partial, without the release templates when no repo has a release project.
3. **Load the output cache** (`load_recipe_id`, `load_output_cache`): Make the page recipe's id, and read `_surge/output-cache.tsv` unless `--force` is set.
4. **Scan** (`scan_repos`): On the worker pool, read each repository into a `RepoScan`, with the author time and LLM flag of every commit of its default branch, and write a changed clone pack below `TMPDIR`.
5. **Manifest** (`populate_output_manifest`): Record every output path with its attributes, and reject every conflict.
6. **Prepare output** (`prepare_output_dir`): Create `output_dir`, the first write.
7. **Keep previous packs** (`keep_previous_packs`): Keep the pack that a changed clone export replaces, for one more build.
8. **Pages** (`write_repo_pages`): On the worker pool, make or keep every page, feed and raw copy of every repo.
9. **Clone exports** (`write_clone_exports`): Copy each new pack and idx, then make `HEAD`, `info/refs` and `objects/info/packs`, and `_headers` when `headers_file` is set.
10. **Releases** (`write_release_outputs`): Make or keep the release pages, version lists and assets of each repo.
11. **Site pages** (`write_site_pages`): Make or keep the site index, `index.html`, and the not-found page, at `not_found_path`.
12. **Static files** (`copy_static_files`): Copy or keep each static file.
13. **Write the output cache** (`write_output_cache`): Write a row for each output that the build wrote or kept, also when an earlier write phase failed.
14. **Remove stale outputs** (`prune_stale_outputs`): Remove every entry below `output_dir` that is not an output, only when every write succeeded.
15. **Report the dry run** (`output_writer_report`): In a dry run only, print the `write`, `keep` and `remove` lines.

A phase before the first write stops the build when it fails, after it reports all of its errors. After the first write, the first failed phase is the last one that writes outputs. The pages phase and the releases phase report every output that fails before they stop. The output is the same, byte for byte, for any `-w`.

No phase writes to `stderr` itself. Every fallible function records its errors in the log that its caller gives, and `build_run` prints the errors and then the warnings.

## Modules

The header of each module documents its interface.

- [src/app/main.c](src/app/main.c): `main`, which calls `cli_dispatch` with the POSIX host, and the `surge` version of `os_oom`.
- [cli_spec](src/app/cli_spec.h): The description, commands and version text that the `cli` library reads.
- [cmd_build](src/app/cmd_build.h): The arena sizes of a build, and the build pipeline.
- [cmd_config](src/app/cmd_config.h): Loads the config and prints it resolved.
- [repo_scan](src/build/repo_scan.h): The scan phase.
- [repo_pages](src/build/repo_pages.h): The pages phase.
- [contributions](src/build/contributions.h): Counts the commits of every default branch into the site index's contributions calendar, each commit once.
- [export_writer](src/build/export_writer.h): The outputs of the clone export, and `_headers`.
- [release_scan](src/build/release_scan.h): The release projects, releases and assets of a repo, and their warnings.
- [release_writer](src/build/release_writer.h): The release outputs of a repo.
- [readme](src/build/readme.h): Finds and renders the documents of a directory, and resolves their links.
- [highlight_diff](src/build/highlight_diff.h): Highlights the two sides of a changed file for its diff.
- [page_context](src/build/page_context.h), [page_paths](src/build/page_paths.h): The maps that every page shares, and the output paths and links of the pages of a branch.
- [page_index](src/build/page_index.h), [page_tree](src/build/page_tree.h), [page_blob](src/build/page_blob.h), [page_log](src/build/page_log.h), [page_commit](src/build/page_commit.h), [page_refs](src/build/page_refs.h), [page_release](src/build/page_release.h), [page_feed](src/build/page_feed.h): The data tree of each kind of page. [README.md](README.md#data-trees) lists their keys.
- [template_value](src/build/template_value.h): The data tree of a page, and its render.
- [site_config](src/domain/site_config.h): Loads, validates and prints `SiteConfig`.
- [repo_model](src/domain/repo_model.h): The types that the scan fills and the pages read.
- [grammar_path](src/domain/grammar_path.h): Maps a file path to a language name.
- [semver](src/domain/semver.h): Parses and orders Semantic Versioning versions.
- [timestamp](src/domain/timestamp.h): Formats a git time as RFC 3339, and gives its local day and that day's date.
- [trailer](src/domain/trailer.h): The names in a commit's trailers, its co-authors, and the LLM tools that its trailers name.
- [tree_path](src/domain/tree_path.h): Resolves a path against a directory of a branch's tree, and finds the file, symlink or directory at a path, for the links of documents and symlinks.
- [url_segment](src/domain/url_segment.h): Percent-encodes a path segment.
- [surge_version](src/domain/surge_version.h): The tool name and version, which CMake sets.
- [repository](src/vcs/repository.h): The libgit2 setup, opening a repository, reading a blob, object ids, and `repository_fail`.
- [refset](src/vcs/refset.h): Loads and checks every branch and tag, and formats `info/refs`.
- [revision](src/vcs/revision.h): The default branch, commits and their trailers, what the contributions calendar reads of a commit, tag notes, a branch's log, and every commit that a ref reaches.
- [filetree](src/vcs/filetree.h): Walks and checks a branch's tree.
- [changeset](src/vcs/changeset.h): Diffs a commit against its first parent, with the caps.
- [pack_export](src/vcs/pack_export.h): Writes the clone pack and its idx, and formats `objects/info/packs`.

## Shared libraries

[libs/ARCHITECTURE.md](../../libs/ARCHITECTURE.md) describes these parts, which `surge` uses without change:

- The arenas, the perm and scratch of each thread, counted strings and the log: see [core](../../libs/ARCHITECTURE.md#core).
- The build's one memory reservation and the worker count under an address-space limit: see [runtime](../../libs/ARCHITECTURE.md#runtime).
- The out-of-memory report: see [Out-of-memory report](../../libs/ARCHITECTURE.md#out-of-memory-report).
- The host interface `os.h`, and the POSIX and in-memory hosts: see [os](../../libs/ARCHITECTURE.md#os).
- The manifest, the output writer, the prune and the output cache: see [output](../../libs/ARCHITECTURE.md#output).
- The worker pool and the jobs: see [runtime](../../libs/ARCHITECTURE.md#runtime).

`surge` adds these parts of its own:

- Each output declares its inputs as attributes, such as `recipe=`, `map=`, `refs=`, `tree=`, `blob=`, `commit=` and `releases=`. [README.md](README.md#output-cache) lists what each output depends on.
- The page recipe is the generator string, the grammars, the site keys, `llm_trailer_keys` and `llm_trailer_values`, every limit but `pack_len_max`, every name of the `[templates]` table, and the path, size and modification time of every template. A code change to what a page holds needs a new version before a release, because the version is the only part of the recipe that a code change moves.
- A failure inside libgit2 reads `<operation>: <libgit2 message> (in repo '<repo>')`, and only `repository_fail` writes it. A message about a branch ends with `(in repo '<repo>', branch '<branch>')`.

## Design decisions

Read a decision before you change its design, and make sure that the change deals with each of its reasons.

### The phases are in this order

The templates load before the scan, so a template that does not compile stops the build before it reads a repository. The output cache loads before the scan, because the scan decides whether a pack is current, and writes a new pack before the manifest needs its name. The scan walks every log and every reachable commit, because the manifest needs every page path before the first write. The release pages run on the main thread after the clone exports, because they read no git object. The cost is that the scan holds every repository's trees, logs and notes in memory until the build ends.

### The scan reads every commit of each default branch

The contributions calendar needs the author time and the trailers of every commit that a default branch reaches. Each scan job reads them on the worker pool, into a `Contribution` per commit of `RepoModel`, and `contributions_tally` merges the repos only when the site index is made. So the index is a render of what the scan found, like every other page, and no repository opens on the main thread. The revwalk of a log reads only commit headers, so the cost is one commit lookup for each commit of each default branch on every build, also one that keeps the index, and a parse of its trailers when `llm_trailer_values` names a value. On a synthetic repository of 100,000 commits, the scan took 0.69 s with no value and 0.75 s with one, against 0.59 s without the calendar, in a build of about 10 s that kept every page.

### A perm and a scratch for each worker

Each pool worker has its own perm and scratch for both parallel phases. A scan job builds in a copy of its worker's perm, and stores the copy back only when the repo scanned completely, so a failed repo leaves nothing in the arena. A page job never stores its perm back. Separate arenas need no lock. The cost is that every `RepoScan` stays in its worker's perm until the build ends, so the peak memory grows with the trees, logs and notes of every repo.

### Repositories are never shared between threads

Each scan job opens its repository and closes it at the end. In the pages phase, each worker has one repository slot. A job opens its repo in the slot only when it reads a git object, and keeps it open for the worker's next job of the same repo. The jobs are in repo order, so a worker mostly reuses one repository. The cost is one open repository, with its mapped packs, for each thread.

### Out of memory exits the process

When an arena cannot grow, or an allocation of libgit2 or another vendored library fails, `surge` calls `os_oom`. Its version in [src/app/main.c](src/app/main.c) calls `cli_oom_exit`, which prints the errors logged so far and `surge: out of memory` with the repo or file that the thread worked on, and exits with status 1. `surge` is a CLI tool whose output directory is easy to make again, so no call site checks for an allocation failure. The cost is that the report shows no warnings, so the next build prints them again. [libs/ARCHITECTURE.md](../../libs/ARCHITECTURE.md#out-of-memory-report) describes the report.

### libgit2 outside `os.h`

`surge` reads repositories through libgit2 and never starts the `git` command. libgit2 reads objects, packs, refs and attributes in the process, so a worker reads every blob and commit of a job from one open repository. A tool that starts `git show` for each blob and each commit pays one process each. The cost is that `vcs/` makes its own system calls, so its tests run on the POSIX host over real fixture repositories. `REPOSITORY_MWINDOW_FILE_LIMIT` bounds the mapped pack files across the process.

### libgit2 setup ignores every git config

`repository_setup` runs once before any worker starts, because libgit2's options apply to the process. It checks the libgit2 version and thread support, routes libgit2's allocations to `os_oom` on failure, sets every config search path to `""`, and turns owner validation off. So no user or system config changes a page. The cost is that `safe.directory` cannot allow a repository that another user owns, so owner validation must be off, as the trust model accepts.

### Trust model

The repositories are the user's own and their contents are trusted, so `surge` renders documents with `libs/markup` unchanged. Raw HTML stays enabled, unresolved link targets stay as written, and `raw/` holds every blob up to `raw_len_max`, SVG included. `surge` writes no Content Security Policy, because the templates are the user's. sosig makes the same decision in `libs/src/markup/markdown.c`, so both apps render Markdown and Org one way. The checks that protect the output tree stay on: the overlap check, the manifest, the name checks of `filetree_load`, and a prune that never follows a symlink. Reversing this decision needs a renderer with `MD_FLAG_NOHTML` and a scheme allowlist.

### Bad refs are errors, bad tree names are warnings

`refset_load` refuses invalid, non-UTF-8, case-colliding and `/`-prefix refs, because the pages of each one would collide, or a filesystem would refuse them. A tree entry gets a warning, a row without a link, and no pages instead, because a trusted repository can hold names such as the Linux kernel's `xt_CONNMARK.h` and `xt_connmark.h`. `filetree_load` reads the tree breadth first with `git_tree_lookup`, and checks a directory's names before it queues the subdirectories, so it never enters a directory without a link. The cost is that a page cannot show a file with such a name.

### Every map holds every key

mustache4c looks up a name that a map does not hold in the enclosing scopes. So every map holds every key of its schema, with empty text or `false` for an absent value, and a missing key cannot show an outer value, such as the repo's `name` in an entry. The HTML that `surge` makes reaches a template only in `*_html` keys, and the render repairs every text to valid UTF-8 for XML 1.0. So the data trees hold names and messages as git stores their bytes. A blob's lines and a diff's rows are one HTML fragment, so a page of many lines costs the render one interpolation.

### Diffs highlight whole blobs

`highlight_diff_lines` highlights the whole old and new blob of a changed file once each, and `highlight_diff_file` gives each diff line the fragment of its side. A line inside a comment or a string that starts above its hunk keeps its color, which a highlight of each diff line alone does not do. The cost is that the first build highlights both sides of every changed file. Later builds keep those pages.

### The clone pack is stable and outlives its refs by one build

`pack_export_build` feeds a revwalk from every exported ref's commit, and adds each ref's own object, because a tag can name a tag, a tree or a blob. libgit2 1.9.7 with one pack thread writes the same pack for the same objects, so the golden test compares packs byte for byte. When the refs change, the manifest still claims the previous pack, unlisted, and `keep_previous_packs` keeps it for one build. So a client with a stale cached `info/refs` can finish its clone. `export_writer_write` writes the pack and the idx before the files that name them, so a client never reads an `info/refs` whose objects are missing. The prune removes only what the manifest does not claim, so no glob ever selects a pack to delete. The cost is one extra pack on the host for one build.

### Releases have their own id

`release_scan_id` hashes what the release outputs read, and those outputs declare it as `releases=`. A new, changed or removed asset, or a release directory for an old tag, then makes the release outputs again, and a moved branch keeps them. The `repo` attribute cannot carry these ids, because it holds the default branch's newest date, which every commit moves.

### libgit2 built by upstream's CMake

[vendor.cmake](vendor.cmake) adds libgit2 with `add_subdirectory`, with options that keep it small and offline. Upstream's configure checks find the platform's `stat` fields, `futimens`, `qsort_r` variant, `iconv` and entropy source on macOS and Linux `musl`, under both Zig toolchains. A hand-written config header would need an `#if` branch for each, and a wrong define still compiles. [BUILD.md](../../BUILD.md#surge-vendored-dependencies) lists the options.

## Tests

[BUILD.md](../../BUILD.md#surge-tests) describes how CTest runs the tests.

- **Unit tests.** Most modules have an acutest file. The six `vcs/` suites and `cmd_build` read real fixture repositories through libgit2, so they run on the POSIX host. `page_context`, `page_index`, `release_scan`, `release_writer`, `repo_pages` and `repo_scan` have no suite of their own, and the golden tests run them.
- **Fixture repositories.** [tests/repos/](tests/repos/) holds hand-written `git fast-import` streams with fixed dates, so their object ids never change. [cmake/surge_make_fixture_repos.cmake](cmake/surge_make_fixture_repos.cmake) replays them and adds what `git fast-import` refuses, such as nested tags, hostile tree names, an empty repository, and refs that collide in case or by prefix.
- **Golden tests.** [cmake/surge_run_golden.cmake](cmake/surge_run_golden.cmake) builds [tests/fixtures/site/](tests/fixtures/site/) with one, two and eight workers, compares the output with [tests/expected/site/](tests/expected/site/) byte for byte, and checks what dry runs write, keep and remove after it changes the inputs. Each `tests/fixtures/refuse-*` site must fail with the exact `stderr` in `tests/expected/refuse-*/stderr.txt`. The refusals are case-colliding branches, a ref prefix, a project `path` that names no directory, a `releases_dir` that is a file, static entries named like a repo, and an empty `templates_dir`.
- **Clone and mise tests.** [tests/clone_test.py](tests/clone_test.py) clones every `<r>.git` over dumb HTTP from a host that answers a missing path with 404. It then checks the corrupt-object errors of a clone from a host that answers with 200. [tests/mise_test.py](tests/mise_test.py) installs the fixture's `alpha` project with mise, and checks that a tampered `SHA256SUMS` or archive fails.
- **Threads.** `ci-tsan` runs the golden test with 1, 2 and 8 workers under ThreadSanitizer, with libgit2 and tree-sitter instrumented and no suppressions.
