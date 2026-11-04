# Architecture

`sosig` reads a TOML config, Markdown and Org content, and Mustache templates, and writes the generated files to a directory that the site owns. This document describes the directories, the build pipeline, the modules and the design decisions of `sosig`. [README.md](README.md) covers usage, [libs/ARCHITECTURE.md](../../libs/ARCHITECTURE.md) covers the shared libraries, [BUILD.md](../../BUILD.md) covers the CMake build, and [STYLE.md](../../STYLE.md) covers the code style.

## Layout

- [src/app/](src/app/): The process entry point `main`, the `cli_spec` that describes `sosig` to the `cli` library, and the commands.
- [src/build/](src/build/): The build phases that are specific to `sosig`: entries, pages, aggregates and feeds, the output plan, and the template data.
- [src/domain/](src/domain/): The site config, the content entry, frontmatter, permalinks, and the tool name and version.
- [tests/](tests/): The golden tests. Each fixture site in [tests/fixtures/](tests/fixtures/) has a directory of expected output in [tests/expected/](tests/expected/):
  - [site_file_permalink](tests/fixtures/site_file_permalink/): Markdown, Org, links between denote notes, and templates, with the default permalink.
  - [site_index_permalink](tests/fixtures/site_index_permalink/): A permalink that publishes a directory index for each entry.
  - [site_nested_sections](tests/fixtures/site_nested_sections/): Nested sections, source names with spaces, punctuation and non-ASCII characters, nested partials, aggregates and feeds in subdirectories, and `path_to_root` at each depth.
- [CMakeLists.txt](CMakeLists.txt) and [CMakePresets.json](CMakePresets.json): The project entry point and its CMake configurations.
- [cmake/](cmake/): The golden test driver. The root [cmake/](../../cmake/) directory holds the build profiles, quality tools, packaging and cross toolchains of every app.
- [vendor.cmake](vendor.cmake): The build of the vendored `copt`, `tomlc17`, `mustache4c` and `acutest`, from the root [vendor/](../../vendor/) directory. It includes [libs/src/markup/vendor.cmake](../../libs/src/markup/vendor.cmake), which builds `md4c`, `org4c`, and tree-sitter with its grammars.
- [libs/](../../libs/): The shared libraries that `sosig` links: `cli`, `core`, `os`, `runtime`, `formats`, `markup`, `output` and `templates`, and for the unit tests `testing` and `os_memory`. They hold every part that does not depend on what `sosig` does.

### Dependency rules

- `app` can depend on every other directory.
- `build` can depend on `domain`.
- `domain` depends on no other first-party directory.
- Every directory can depend on the libraries in `libs/`. No library depends on `sosig`.

A first-party include starts at the `src/` root or at the include root of a library, for example `#include "build/template_context.h"` or `#include "core/ascii.h"`. So each include names the directory or the library that it depends on.

## Modules

Each header documents the interface of its module.

- [app/main](src/app/main.c): Calls `cli_dispatch` with the POSIX host, and defines `os_oom`.
- [app/cli_spec](src/app/cli_spec.h): Tells the `cli` library the description, default config file, version text, grammar list and commands of `sosig`.
- [app/cmd_build](src/app/cmd_build.h): `sosig build`. It owns the `BuildState` and runs the phases below.
- [app/cmd_config](src/app/cmd_config.h): `sosig config`. It loads the config and prints it resolved.
- [build/entry_renderer](src/build/entry_renderer.h): The parse and convert phases, and the rules for links between content files.
- [build/output_manifest](src/build/output_manifest.h): The check of `output_dir` against the input roots, and the output plan with its conflict checks.
- [build/page_renderer](src/build/page_renderer.h): The page phase. It renders each entry through its content template.
- [build/site_writer](src/build/site_writer.h): Renders the aggregate and feed templates, and stops at the first one that fails.
- [build/template_context](src/build/template_context.h): The template data. It resolves each name when a render asks for it, and marks only `body_html` as HTML that a template can write unescaped.
- [domain/content_entry](src/domain/content_entry.h): The `ContentEntry` struct, the newest-first sort, and the newest change for `feed.modified`.
- [domain/frontmatter](src/domain/frontmatter.h): Splits a Markdown file at its `+++` fences, parses the TOML frontmatter, and reads the keywords at the top of an Org file.
- [domain/permalink](src/domain/permalink.h): Expands `{slug}` and `{section}` into the URL of an entry.
- [domain/site_config](src/domain/site_config.h): Loads, checks, resolves and prints the config.
- [domain/sosig_version](src/domain/sosig_version.h): The tool name and version, which CMake sets.

## Build pipeline

`cmd_build_run` gives the arena sizes of `sosig` to `build_run` from the `runtime` library. `build_run` reserves the memory, calls `cmd_build_execute`, prints the log, and releases the memory. `cmd_build_execute` runs `load_build_inputs`, and then `run_build_phases` runs the other phases in this order. No phase writes to `stderr` itself.

1. **Load inputs** (`load_build_inputs`): Load the config, and check that each input root is a usable directory. Reject an `output_dir` that is inside an input root or holds one. Walk `content_dir`, `templates_dir` and `static_dir`. A failure here stops the build.
2. **Parse entries** (`parse_content_entries`): In parallel, read each source, parse its frontmatter or Org keywords, and make its URL and output path.
3. **Collect** (`collect_content_entries`): Put the entries that are not drafts in one array, sorted newest first.
4. **Load templates** (`load_templates`): Read and compile every template that the build renders, and every partial, once.
5. **Manifest** (`populate_output_manifest`): Record every output path and reject each conflict.
6. **Load the output cache** (`load_output_cache`): Read `_sosig/output-cache.tsv`, unless the build has `--force`. A cache that is missing or not usable gives a verbose note, not an error.
7. **Convert bodies** (`convert_content_bodies`): In parallel, convert each body to HTML and resolve its links to other content files. If the log then holds an error, the build stops.
8. **Prepare output** (`prepare_output_dir`): Create `output_dir`. This is the first write.
9. **Render pages** (`render_content_pages`): In parallel, render each content page, and keep or write it.
10. **Write the site** (`write_generated_site`): On the main thread, render each aggregate and feed, then copy each static file and each asset.
11. **Write the output cache** (`write_output_cache`): Write the cache, also when phase 9 or 10 failed.
12. **Remove stale outputs** (`prune_stale_outputs`): Remove every path below `output_dir` that the build did not write. This phase runs only when every write succeeded.
13. **Report the dry run** (`report_dry_run`): In a dry run only, print the `write`, `keep` and `remove` lines.

## Shared libraries

These parts of `sosig` come from the libraries, and [libs/ARCHITECTURE.md](../../libs/ARCHITECTURE.md) describes them:

- The arenas, the memory model and the ASan poisoning: [Arenas](../../libs/ARCHITECTURE.md#arenas) and [Build memory](../../libs/ARCHITECTURE.md#build-memory).
- Counted strings, the log, its arena rule and its checked format strings: [Strings](../../libs/ARCHITECTURE.md#strings) and [Logs](../../libs/ARCHITECTURE.md#logs).
- The out-of-memory report: [os_oom](../../libs/ARCHITECTURE.md#os_oom) and [os](../../libs/ARCHITECTURE.md#os).
- The host interface and the in-memory host for unit tests: [os](../../libs/ARCHITECTURE.md#os).
- The output manifest, the output writer that makes every change below `output_dir`, the output cache and the prune: [output](../../libs/ARCHITECTURE.md#output).
- The worker pool, the parallel jobs and the directory walk: [runtime](../../libs/ARCHITECTURE.md#runtime).
- Markdown, Org and syntax highlighting: [markup](../../libs/ARCHITECTURE.md#markup).
- The template set and its renders: [templates](../../libs/ARCHITECTURE.md#templates).

## Design decisions

Read a decision before you change its design, and make sure that the change deals with each of its reasons.

### Parse every entry before any body converts

**Decision.** The build parses every entry in one parallel phase, sorts the entries, and converts the bodies in a second parallel phase. A third parallel phase renders the pages. **Why.** A link to another content file needs the URL of that file, and no URL exists until every entry is parsed. A content template can iterate `content_entries`, which needs every entry parsed and sorted. **Cost.** Each source stays in memory from its parse to the end of the build, so the peak memory of a build grows by about the total size of the sources.

### Arenas belong to workers, not to entries

**Decision.** Each worker has its own perm and scratch, and a job uses the arenas of the worker that runs it. A parse job builds its entry in a copy of the perm, and stores the copy back only when it fills its slot. Each job writes only its own slot and its own log. **Why.** Separate arenas need no lock. A draft or a failed source leaves nothing in the arena. Thousands of entries, each with an arena for the largest entry, would need too much address space. **Cost.** A worker keeps every entry that it parsed until the build ends.

### Arena sizes come from the limits

**Decision.** Each thread's perm is `BUILD_PERM_LEN` (4 GiB), its scratch is `BUILD_SCRATCH_LEN` (3 GiB), and the log is `BUILD_LOG_LEN` (256 MiB). `cmd_build.c` derives the perm from the render limit, and its `_Static_assert`s check the scratch against the content, render and highlight limits. After a page is written, `arena_discard` gives back its pages. **Why.** An input that the limits accept fits, and no estimate from the inputs is necessary. Without the discard, a large page would stay resident in every worker after its write. **Cost.** Org lines with code references in a short `-l` label format need up to 32 bytes of scratch for each byte, so more than about 88 MiB of such lines ends in the out-of-memory report. The next page faults its pages in again, which costs far less than its render.

### Report every error before the first write

**Decision.** The config load stops at its first error, and an error in the load inputs phase stops the build. After any other error, the build runs each later phase that still has something to check, up to the convert phase. Then it stops before it creates `output_dir` if the log holds an error. After the first write, a page that fails to render does not stop the other pages, and the prune reports each failure and goes on. A parallel phase reports the distinct job errors up to a cap, and a count of the others, as [Logs](../../libs/ARCHITECTURE.md#logs) describes. The log keeps phase order, and job order within a phase, so the order does not change with the worker count. **Why.** A user sees every parse error, output conflict and broken link in one run. **Cost.** A build that fails early still runs the later checks.

### Plan every output before any write

**Decision.** The manifest phase records every output path, and rejects each conflict, before the build renders a page or creates `output_dir`. **Why.** A rejected build leaves the filesystem as it was. An output that would overwrite an input fails before it can destroy the file. A duplicate path fails before the build spends time on renders. **Cost.** The build stats every static file and asset before it renders a page.

### Compare roots and inputs by identity

**Decision.** The checks of `output_dir` against the input roots, and of each output against the input files, compare device and inode values, not path text. **Why.** These checks find a different spelling of a path, a symlink, a hard link, and a case-insensitive path to the same file or directory. A sibling such as `contentx` does not match `content`. **Cost.** A missing root has no identity, so the checks skip it. A root that is bind-mounted inside `output_dir` passes the check, and the prune empties it.

### Reject `NUL` bytes in input

**Decision.** `os_read` with `OS_READ_TEXT` rejects a content file, config file or template that holds a `NUL` byte. The TOML helpers reject a string or key that holds a decoded `NUL`. **Why.** No input format gives a `NUL` byte a meaning, and a `%.*s` diagnostic stops at one. **Cost.** A file with a `NUL` byte fails the build, also where the byte would do no harm. The string type holds a `NUL` as an ordinary byte and does not need these checks, so you can remove them with the tests that pin them.

### Out of memory exits the process

**Decision.** `os_oom` in [main.c](src/app/main.c) calls `cli_oom_exit` with the name `sosig`. The report names the file of the thread that ran out. A parse, convert or page job names its source. A main-thread phase names each aggregate or feed template, each static file and asset that it copies, and the output cache. **Why.** It is easy to make the output again, no call site checks for allocation failure, and the user needs the file name to find the input that failed. **Cost.** Under a low `ulimit -v`, a large input ends in the out-of-memory report and not in a sized refusal.
