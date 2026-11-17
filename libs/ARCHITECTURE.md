# Architecture

The `libs/` project holds the libraries that the apps share. This document is the one description of the shared machinery and the reasons for its design. The headers document each function. Each app's ARCHITECTURE.md describes the app's own modules, [STYLE.md](../STYLE.md) the code style, and [BUILD.md](../BUILD.md) the CMake builds.

## Layout

- [CMakeLists.txt](CMakeLists.txt) and [CMakePresets.json](CMakePresets.json): The project and its presets. An app adds the project with `add_subdirectory(../../libs ${CMAKE_BINARY_DIR}/libs EXCLUDE_FROM_ALL SYSTEM)`, so a library builds only when the app links it.
- [vendor.cmake](vendor.cmake): The vendored targets of a standalone configure. An app gives these targets from its own `vendor.cmake`.
- `src/<library>/`: One directory for each library, with its sources, its `test_*.c` files and its `CMakeLists.txt`. [src/os/](src/os/) also holds the two hosts as libraries of their own.

## Libraries

| Library | Contents | Linked by |
| --- | --- | --- |
| [`core`](#core) | The arena, counted strings, growable buffers, the error log, and small primitives: ASCII, error text, exit codes, number parsing, paths, text, URL checks and UTF-8. | sosig, fram, cwrap, surge |
| [`testing`](#testing) | The helpers of the unit tests, and their `os_oom`. | the unit tests |
| [`os`](#os) | The host interface and the out-of-memory report, with the POSIX host `os_posix` and the in-memory host `os_memory`. | sosig, fram, cwrap, surge |
| [`runtime`](#runtime) | `build_run`, the worker pool, the jobs of a parallel phase, the check of the input directories, the directory walk, and `--verbose` output. | sosig, fram, surge |
| [`cli`](#cli) | The command line of the site generators. | sosig, fram, surge |
| [`formats`](#formats) | HTML escaping, front matter, TOML and the config helpers, RFC 3339 dates, tab-separated fields, and JSON strings. | sosig, fram, surge |
| [`markup`](#markup) | Markdown and Org to HTML, and the highlighting of code blocks with tree-sitter. | sosig, surge |
| [`output`](#output) | The output manifest, the output writer, the prune and the output cache. | sosig, fram, surge |
| [`templates`](#templates) | The template set of a build and its renders. | sosig, fram, surge |

### Where the shared code lives

The libraries hold the code that more than one app uses in the same form, because that code does not depend on what a program does. Each program defines its own `os_oom` and sizes its own arenas. The cost is that a comment in a library must use words that fit every app, as [STYLE.md](../STYLE.md#comments) asks.

### Dependency rules

- `testing` and `os` depend on `core`. `os_memory` also depends on `testing`.
- `runtime` and `output` depend on `core` and `os`.
- `cli` depends on `core`, `os`, `runtime` and copt.
- `formats` depends on `core`, `os` and tomlc17.
- `markup` depends on `core`, `formats`, md4c, org4c, and tree-sitter with its grammars.
- `templates` depends on `core`, `os`, `formats` and mustache4c.
- No library depends on an app.

The project adds `cli`, `formats`, `markup` and `templates` only when the targets that they need exist. cwrap vendors no tomlc17, so its build has no `formats`, `markup` or `templates`. fram includes no `src/markup/vendor.cmake`, so its build has no `markup`.

An include names the library and the module, as in `#include "core/parse.h"`, and a header guard is `APPS_<LIB>_<MODULE>_H`. `libs_add_include_root` gives each library an include root that holds one link to the library's directory, so a target reaches only the headers of the libraries that it links. An app's own `src/<layer>/` holds no module that a linked library also has.

### Build profiles and analysis

Each library links the `apps_build_project` profile of the project that builds it, so an app's sanitizer build instruments the library code. `SYSTEM` keeps the app's clang-tidy on the app's own code. The app's cppcheck reads the headers of the libraries in `APPS_LIBRARIES` to expand their macros, and so it also checks their inline functions. Only the top-level `libs/` project analyzes and lints the library sources.

### Tests

Only the top-level `libs/` project builds the library tests. `libs_add_unit_test(<library> <module>)` registers `test_<module>.c` as the CTest case `<library>.<module>`, linked with the library, `apps_build_tests`, acutest and the libraries in `LIBRARIES`. `OS posix` links `os_posix`, and `OS memory` links `os_memory` and sets `APPS_TEST_OS_MEMORY` to 1. A test without `OS` links no host. `NAME` lets one source build two tests, as the `os` contract tests do.

## core

The memory modules build on each other in this order:

1. An arena allocates memory.
2. A string is a view of that memory.
3. A buffer grows a string in an arena.
4. A log is a buffer of error messages.

The headers document the small primitives. Every command returns an `enum ExitCode`: `EXIT_CODE_OK` (0), `EXIT_CODE_FAILURE` (1) or `EXIT_CODE_USAGE` (2). [error](src/core/error.h) turns an `errno` into text on any thread.

### `os_oom`

When an allocation does not fit, the arena calls `os_oom`, which must not return. `core/arena.h` declares it, and each program defines it once, because only the program knows what to print and which exit status to use:

```c
_Noreturn void os_oom(void) {
  fputs("myprogram: out of memory\n", stderr);
  _Exit(EXIT_FAILURE);
}
```

sosig, fram and surge call `cli_oom_exit` with their name. cwrap calls `oom_report` and then `_Exit`. The unit tests use the `os_oom` of `testing`, except the tests of the arena, strings, buffers and log, which use [src/core/test_oom.h](src/core/test_oom.h).

### Arenas

[`core/arena.h`](src/core/arena.h) defines `struct Arena`, which allocates in sequence from one region and never returns `NULL`. `arena_reserve` reserves only address space, and the arena commits pages in steps of one mebibyte as allocations reach them. A large reservation therefore costs memory only for the pages that it uses, also under strict overcommit. `arena_reserve_halving` reserves a power-of-two fraction of the size when the system refuses the full size. A test can lower `arena_reserve_len_max` to simulate `ulimit -v`. An arena over memory that the caller owns has a `NULL` `limit`.

A function that returns data takes `struct Arena* perm`. A function that needs only temporaries takes `struct Arena scratch` by value, so its allocations are free again when it returns:

```c
struct Str render_title(struct Str name, struct Arena* perm, struct Arena scratch);
```

To use an arena correctly:

- Never assume that two allocations are adjacent. To grow one in place, use `arena_grow` or `arena_extend`.
- Never let two threads allocate from one arena. Give each thread its own region with `arena_split`.
- Never grow a buffer or a log in an arena that a callee gets by value.
- Give back the pages of a large buffer that a worker no longer needs with `arena_discard`.

#### Memory: arenas only

All first-party memory comes from arenas, and the code never frees one allocation alone. No comment needs to say which code frees a buffer, and the bounds check is one comparison that cannot overflow. An arena is one region, so a callee that allocates in a by-value copy leaks nothing. The cost is that memory comes back only with the whole arena, or through `arena_discard`.

#### Address sanitizer coverage in arenas

Under the address sanitizer, an arena over a reserved region poisons the pages that it commits, unpoisons only the bytes of each allocation, and leaves a poisoned gap before each allocation. ASan cannot see an overflow inside one large mapping, so without the poisoning the debug preset loses most of its overflow checks. The limits are these:

- An arena over memory that the caller owns has no gap and no poison.
- The arena does not poison a scratch again when its function returns, so ASan does not find a later use of that memory.
- A job's copy of its worker's arenas poisons the free space again when it commits, so a job does not read what an earlier job left there.
- `arena_release` leaves the shadow memory to the sanitizer's `munmap` interceptor. To unpoison the whole region would make an eighth of the reservation resident.

### Strings

[`core/str.h`](src/core/str.h) defines `struct Str`, a pointer and a signed `ptrdiff_t` length, which goes by value. A substring is a view of the original bytes, and the zero value is the empty string. A view is valid only while its bytes exist, so copy it with `str_clone` before they go away.

#### Counted strings with signed lengths

Every first-party string is a `struct Str`, and every length and count is a `ptrdiff_t`. A length changes to `size_t` only at a libc, POSIX or vendored call, with an assertion that it is not negative. A C string exists only where such an API needs one, and `str_cstr` makes it. Subtraction cannot wrap, and a `NUL` is an ordinary byte, so no code calls `strlen` on owned text. The cost is a conversion at each such call. The apps reject a `NUL` in text input as a product decision, and the string type does not force it. If that decision changes, remove the `NUL` checks in both hosts and the contract tests that pin them.

### Buffers

[`core/buf.h`](src/core/buf.h) defines `struct Buf`, which grows a string in an arena from many appends. While the buffer is the last allocation in its arena, it grows in place, so a large output needs no copies. Pass its arena by pointer while it grows, and call `buf_str` after the last append, because an earlier view does not see later appends.

### Logs

[`core/log.h`](src/core/log.h) defines `struct Log`: error lines with a count, and warning lines with a count. `log_error` records a line and returns `-1`, so a fallible function can end with `return log_error(log, ...);`. `log_error_in` adds ` (in '<file>')`. `log_warn` does not change the error count, so a run with only warnings succeeds. A program prints the warnings after the errors. Each thread has its own log, and `log_append` merges the logs in a fixed order, so the output does not depend on the threads.

#### A growable log, and every error in one run

A fallible function whose caller needs a message takes `struct Log* log` last, and a callback type puts its `void* userdata` after the log. The log grows with each error and has no fixed size. A config load stops at its first error. After any other error, a command continues while its next step still has something to check, and stops before its first write if the log holds an error. A parallel phase records errors in job order, so the order does not change with the worker count. It prints at most `JOB_ERROR_REPORT_COUNT_MAX` (20) distinct job errors and a count of the others. An app can stop earlier, as surge's build does.

#### A log must not grow in a borrowed arena

A log lives in an arena that no callee gets by value: an arena of its own, or one that all its users get by pointer. A `Buf` grows in place, so a callee with a by-value copy of the arena allocates the bytes that the log grows into. The two then overwrite each other, with no crash and no ASan report. [STYLE.md](../STYLE.md#memory-and-resources) states the rule. Treat a break of the rule as a bug.

#### Log messages use checked format strings

A log call takes one `printf` format, as in `log_error(log, "failed to read template: %s ('%.*s')", reason, STR_FMT(template_path))`. Each function that formats a message has the `format(printf, ...)` attribute, so the compiler checks its arguments, with `-Wformat=2` on. Each message is one sentence in the source, so `grep` finds it. The costs and limits are these:

- `STR_FMT` evaluates its argument twice, so give it a variable.
- `%.*s` stops at a `NUL`, and a string longer than `INT_MAX` bytes fails an assertion.
- An `off_t` prints with `%jd` and a cast to `intmax_t`.
- A variadic function starts its `va_list` again for each `vsnprintf` pass, and never passes it to a formatter that copies it with `va_copy`. On AArch64 Linux, the Clang analyzer, versions 22 and 23, reports a false `va_end` finding on such a copy.

The tests `core.format_compile.*` check that the compiler rejects a call that does not match its format, as [BUILD.md](../BUILD.md#format-checks-in-core) lists.

## testing

[test_support](src/testing/test_support.h) holds the helpers of the unit tests: an arena and a log for each test, exact checks of text, `test_is_oom`, the seeded generator `struct TestRng`, fixture files, and the capture of a standard stream. `testing` is an object library, so its `os_oom` is always linked in. A static archive member would come in only through a reference earlier on the link line, and the reference comes from `core`, which comes later. `test_is_oom` runs its code in a child process, where `os_oom` exits with `TEST_OOM_EXIT_STATUS`. Outside `test_is_oom`, `os_oom` fails the test. A random test starts from `TEST_RNG_SEED`, so it reaches the same inputs on every run.

## os

[os/os.h](src/os/os.h) is the host interface. Every read, write, directory change, listing, `stat` and print of an app goes through it. These properties of the interface matter to its callers:

- `os_read` takes a size limit from each caller, and checks it before it reads. With `OS_READ_TEXT` it rejects a `NUL`, and with `OS_READ_BYTES` it accepts any byte. `os_read_stdin` reads standard input up to the caller's limit, and rejects a `NUL`.
- `os_write` writes bytes or streams a file, creates the parent directories, and replaces a file or link at the destination without following it. The new file gets mode `0666` reduced by the process umask.
- `os_replace` replaces a file atomically, through a synced temporary sibling and a rename.
- `os_print` carries every byte that an app prints, so a test sees it all. A caller that prints from several threads holds its own lock.
- A path that holds a `NUL` fails with `EINVAL`, and a path that ends in `/` must name a directory.

[os_posix](src/os/os_posix.h) is the POSIX host, with no state. [os_memory](src/os/os_memory.h) is the in-memory host for unit tests: a tree with symlinks, hard links, a modification-time counter, injected failures and captured output, under a lock. Running other programs is not part of the interface.

### The `os` interface

All host access goes through `os.h`, and the link step selects one host for each executable. Tests of walks, manifests, pages, writes and prunes then need no temporary directories. A link-time choice needs no table of function pointers, because no executable needs two hosts. Both hosts are object libraries, because a single-pass linker skips a static archive that comes before its user. The cost is the contract tests. [src/os/test_os.c](src/os/test_os.c) runs against both hosts as `os.contract_posix` and `os.contract_memory`, so the in-memory host cannot accept what POSIX rejects, such as `file/` as a file. Each property that a test relies on in the in-memory host needs a case there. `os_write` is neither atomic nor synced, so an interrupted build can leave a truncated file, which the next build repairs. That costs less than a temporary file for every output.

### Out-of-memory report

[oom](src/os/oom.h) holds what the report prints. `oom_set_log` names the command's log, and `oom_set_job` names the file and the job log of the calling thread. `build_run` names the build's log, `cli_dispatch` names the log of `config`, and each job names its file when it starts. `oom_report` prints the command's log, the job log, and then `<program>: out of memory`, with ` (in '<file>')` when the thread named a file. It allocates nothing.

#### Out of memory exits the process

When an arena cannot commit pages, when the system refuses the smallest reservation, or when a vendored library's allocation fails, the program calls `os_oom`. The wrappers of tomlc17, md4c, org4c, mustache4c and tree-sitter map each such failure to `os_oom`. The one exception is the `calloc` of the Bash grammar scanner's fixed-size state. In sosig, fram and surge, `cli_oom_exit` takes a lock, so the first of two failing workers prints its report and exits while the second waits. Then it prints the report and exits with `EXIT_CODE_FAILURE` through `_Exit`, which runs no `atexit` handler that another thread could use. The apps are command-line tools whose output is easy to make again, so no call site checks for allocation failure, and the report names the input that failed. The cost is that an input that does not fit ends the run.

## runtime

- [build_run](src/runtime/build_run.h): Reserves a build's memory, runs the build, prints the errors and then the warnings, and releases the memory.
- [pool](src/runtime/pool.h): Runs indexed jobs on a bounded pthread pool. It completes every job, also after a failure, and joins every worker before it returns. Each job gets the index of its worker.
- [job](src/runtime/job.h): `job_run` runs one parallel phase. Each job gets the perm and scratch of its worker, a log of its own, and a result slot that the caller owns. `job_run` merges the errors and warnings in job order, and jobs whose lines differ only in the job's own name count once. A job that reads a source of `JOB_SOURCE_DISCARD_LEN_MIN` (16 MiB) or more gives back its pages with `arena_discard` after it uses them.
- [read_root](src/runtime/read_root.h): Checks each directory that a build reads, and reports every failure in one run.
- [walk](src/runtime/walk.h): Lists the matching files of a tree in byte order, through `os_list` and `os_stat`. It follows symlinked directories but walks each directory once, by device and inode, and it prefers the path without a symlink.

### Build memory

`build_run` reserves a build's memory once, before the build reads any input, as one block. It splits the block into the arenas that the app's `BuildRunLens` sizes: the log, a perm and a scratch for each thread, and in the main thread's scratch a log share for each worker and one for `job_run`. Each app derives the sizes from its input limits, so the reservation depends only on the worker count. That count is `-w`, or one for each online CPU, and at most `BUILD_RUN_WORKER_PER_CPU_MAX` (4) for each CPU. If the system refuses the block, `build_run` halves the workers down to one, and then `arena_reserve_halving` halves the block and every arena in it.

Sizes from the limits hold every input that the limits accept, except an input shape that an app names as an exception. Fewer workers keep every arena at full size, so the limit that a build needs does not grow with `-w`. Strict overcommit charges every committed page, and Linux ignores `MAP_NORESERVE` under it, so the arenas commit on demand. The cost is that under a `ulimit -v` below the full block, a large input ends in the out-of-memory report and not in a refusal that names the limit. The build never writes wrong bytes.

## cli

The command line of sosig, fram and surge. cwrap has its own. Each app describes itself in a `CliSpec`: its description, the noun of what `build` builds, its default config file, its version line and grammar list, and its `build` and `config` commands. `cli_parse` gives a command, its options, and one of four actions: version, help, run or error. It never prints and never exits. `cli_dispatch` runs the action, and runs `config` in a reservation of its own. `-c`, `-h` and `-V` apply to every command.

## formats

- [frontmatter](src/formats/frontmatter.h): Finds TOML front matter between `+++` lines, or YAML between `---` lines.
- [html](src/formats/html.h): Escapes untrusted text for HTML.
- [json](src/formats/json.h): Writes a JSON string, repaired as a page is, for the JSON files that an app writes without a template.
- [rfc3339](src/formats/rfc3339.h): `RFC3339_EPOCH`, for a date that Atom requires and a value lacks. Every date that the apps write spells a zero offset `Z`.
- [toml](src/formats/toml.h): Reads a config of at most `TOML_CONFIG_LEN_MAX` (1 MiB), parses TOML, and holds the config readers and printers of every app, so the apps word each config diagnostic the same way. `toml_require_known_keys` reports each unknown key.
- [tsv](src/formats/tsv.h): Escapes a field of a tab-separated line, so a field cannot split its line and two texts never give the same field.

## markup

- [body_html](src/markup/body_html.h): The HTML that both renderers share: headings with anchors, footnotes, task checkboxes, admonitions, math, code blocks and the escaping of link targets. The `LinkTargets` callback gets the target of every link and image, and can write a URL of its own. With `is_raw_html_linked`, it also gets the targets of raw HTML, as surge asks and sosig does not.
- [highlight](src/markup/highlight.h): Wraps each token that the grammar's query captures in a `<span class="tok-…">`. `APPS_GRAMMARS` selects the grammars, as [BUILD.md](../BUILD.md#grammars) describes.
- [markdown](src/markup/markdown.h) and [org](src/markup/org.h): Parse Markdown with md4c and Org with org4c, and write the HTML.

The modules know nothing about content files, and have no log.

### Highlighting: shared queries, memory per code block

`highlight` compiles each grammar's query at its first lookup, under a lock, and keeps it until the process exits. Each code block gets a new parser and query cursor. While it highlights a block, tree-sitter allocates from the writer's scratch, through a thread-local pointer, with a free list for each size. At other times it uses `malloc`, and a failure calls `os_oom`. The writer writes a block plain if its parse takes more than `HIGHLIGHT_MEMORY_LEN_MAX` (128 MiB), or if its tree has an error node with more than 4096 children. A query that needs more memory or work than the block's length allows leaves the rest plain. The reasons are these:

- A query takes milliseconds to compile, and a parser almost nothing, so no job carries state for each worker. A site that names no language compiles no query.
- To free the queries at exit could race a worker that still highlights when `os_oom` ends the process.
- Allocation from each worker's scratch lets a parse scale across threads. The free lists stop text that a grammar cannot parse from filling the scratch.
- Unclosed or deeply nested brackets can take seconds, and the limits catch only such blocks.
- Each converter splits a region of twice the limit off its scratch for its code blocks, so the app's scratch size covers it.
- A plain code block is an exception to the rule to fail at a limit, because a build that fails over colors helps no one.

The cost is that an Org block of mostly code references with a short `-l` label format can need more scratch than sosig's `BUILD_SCRATCH_LEN`, as its definition explains.

## output

- [manifest](src/output/manifest.h): Maps each output path to the source that makes it, before the build writes anything. Two paths that differ only in ASCII case collide, and so does a path that is a directory prefix of another. Each entry holds the output's attributes and a mark, which records whether the build wrote or kept the path, and the size of the file. Parallel jobs mark their own paths at the same time.
- [manifest_builder](src/output/manifest_builder.h): Fills the manifest and refuses an output that would hurt an input. It rejects an `output_dir` inside an input root, or one that holds a root or the config. It rejects an output past a path limit, below a root, through a symlink, or on an input file.
- [output_path](src/output/output_path.h): Reports an output path past `OUTPUT_PATH_RELATIVE_LEN_MAX` or `FILENAME_LEN_MAX` in the same words for every producer.
- [output_writer](src/output/output_writer.h): Makes every change below the output directory. It fails on a path that the manifest does not name. A dry run makes no change, and `output_writer_report` lists what a real run would write, keep and remove.
- [output_pruner](src/output/output_pruner.h): Removes each entry that the manifest does not name and that holds no output, after the last write. It compares device and inode, so on a case-insensitive filesystem it keeps the old spelling of a current output. It never goes into a symlink.
- [output_cache](src/output/output_cache.h): One row for each output that declares attributes, and one for each fact that an app records about an input. A cache of another version, an empty one, or one cut short is unusable, as [build_cache_file](src/output/build_cache_file.h) checks, and the build then makes every output.

`output_writer_produce` is the chokepoint that every output goes through, from any job. It keeps an output without making it when the cache records the attributes that the manifest declares, and the file has the recorded size. Otherwise it makes the output and writes it only if its bytes changed. It streams a copy from its source. `is_forced` keeps nothing. An app declares attributes only for an output that is expensive to make and whose inputs it can name exactly. `output_cache_write` writes the cache from the marks after the last output, also after a failure. An interrupted build leaves the previous cache. That is safe, because a truncated file has another size than its row, and a rewritten one has new attributes. Two writes bypass the chokepoint: the cache file, because a cache cannot vouch for itself, and surge's previous clone pack, which `output_writer_keep` keeps for one more build.

## templates

[template_set](src/templates/template_set.h) loads the templates of a build and renders them, for every page, aggregate and feed. `template_set_load` compiles each page template and every `.html` file of `partials/` whose name is an identifier, before the build writes anything. It reports every template that fails in one run. A set holds at most `TEMPLATE_PARTIAL_COUNT_MAX` partials, of at most `TEMPLATE_FILE_LEN_MAX` (4 MiB) each. `template_set_render` renders with mustache4c over a `struct TemplateData`, the callbacks that give the app's data. The workers render from one set at the same time.

A render escapes every interpolation for HTML. A triple mustache or an ampersand tag fails unless the app marked the node as HTML, so a value that the app did not make cannot add markup. The render passes all text that it writes through `utf8_repair_for_xml`, so no output holds invalid UTF-8 or a character that XML 1.0 forbids. A render stops at `TEMPLATE_RENDER_EXPANSION_COUNT_MAX` partial expansions and at `TEMPLATE_RENDER_OUTPUT_LEN_MAX` bytes. A failure names its template, and ends with `(while rendering '<output>')`.
