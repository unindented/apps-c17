# Architecture

The `libs/` project holds the libraries that the apps share. Each library is a CMake target with one directory below `libs/src/`. An app links the libraries that it uses, and the library code compiles with the build profiles of that app. This document describes the libraries, their modules, their dependencies, and how their tests run. Each app's ARCHITECTURE.md describes the modules that are the app's own, [STYLE.md](../STYLE.md) covers the code style of every project, and [BUILD.md](../BUILD.md) covers the CMake builds.

## Layout

- [CMakeLists.txt](CMakeLists.txt) and [CMakePresets.json](CMakePresets.json): The project entry point and its presets. An app adds the project with `add_subdirectory(../../libs ${CMAKE_BINARY_DIR}/libs EXCLUDE_FROM_ALL SYSTEM)`, so a library builds only when the app links it. Configured on its own, the project builds every library, its tests, its analysis and its lint.
- [vendor.cmake](vendor.cmake): The vendored targets that the libraries and their tests use when the project is configured on its own: `acutest`, `copt`, `mustache4c` and `tomlc17`, from the root [vendor/](../vendor/) directory, and the targets of [src/markup/vendor.cmake](src/markup/vendor.cmake). An app that adds the project gives these targets from its own `vendor.cmake`, which includes `libs/src/markup/vendor.cmake` when the app links `markup`.
- [src/core/](src/core/), [src/testing/](src/testing/), [src/os/](src/os/), [src/runtime/](src/runtime/), [src/cli/](src/cli/), [src/formats/](src/formats/), [src/markup/](src/markup/), [src/output/](src/output/) and [src/templates/](src/templates/): One directory for each library, except `src/os/`, which also holds the two implementations of its interface as libraries of their own, with the library's sources, its `test_*.c` files and its `CMakeLists.txt`.

## Libraries

| Library | Contents | Linked by |
| --- | --- | --- |
| [`core`](#core) | The arena allocator, counted strings, growable buffers and the error log, and ASCII classification, error text, exit codes, number parsing, paths, path lists, text helpers, URL checks and UTF-8 validation and repair. | sosig, fram, cwrap, surge |
| [`testing`](#testing) | The support that every unit test uses, and the `os_oom` of the unit tests. | the unit tests of every app and of the other libraries |
| [`os`](#os) | The host interface and the out-of-memory report, with the POSIX host `os_posix` and the in-memory host `os_memory`. | sosig, fram, cwrap and surge, and `os_memory` for their unit tests and those of the libraries |
| [`runtime`](#runtime) | `build_run`, which runs a build in its memory and prints its log, the worker pool, the jobs of a build phase that run on it, the check of the directories that a build reads, the directory walk, and `verbose_printf`, which prints a command's status lines. | sosig, fram, surge |
| [`cli`](#cli) | The command line of the site generators: its parse, its help and version text, and its dispatch to the app's commands. | sosig, fram, surge |
| [`formats`](#formats) | HTML escaping, the TOML helpers and the escaping of a tab-separated field. | sosig, fram, surge |
| [`markup`](#markup) | Markdown and Org to HTML, and the syntax highlighting of code blocks with tree-sitter. | sosig, surge |
| [`output`](#output) | The output manifest and its builder, the output-path checks, the output writer, the prune, the output cache, and the read of the output cache file. | sosig, fram, surge |
| [`templates`](#templates) | The template set of a build and its renders. | sosig, fram, surge |

### Dependency rules

- `testing` and `os` depend on `core`. `os_memory` also depends on `testing`.
- `runtime` depends on `core` and `os`.
- `cli` depends on `core`, `os`, `runtime` and the vendored copt, which it links privately. The project adds `cli` only when the `vendor_copt` target exists.
- `formats` depends on `core`, `os`, for the read of a config file, and the vendored `tomlc17`. The project adds `formats` only when the `vendor_tomlc17` target exists, so an app that vendors no tomlc17, such as cwrap, builds without it.
- `markup` depends on `core`, `formats` and the targets of `src/markup/vendor.cmake`: md4c, org4c, and tree-sitter with its grammars. The project adds `markup` only when these targets and `formats` exist.
- `output` depends on `core` and `os`.
- `templates` depends on `core`, `os`, `formats` and the vendored mustache4c. The project adds `templates` only when `formats` and the `vendor_mustache` target exist.
- No library depends on an app.

A library include names the library and the module, for example `#include "core/parse.h"` or `#include "output/manifest.h"`. Each library has an include root of its own: `libs_add_include_root` makes a directory in the build tree that holds one link, named for the library, to the library's directory in `src/`, and gives the target that directory as its include root. A target therefore reaches the headers of the libraries that it links and of no other library. A header guard is `APPS_<LIB>_<MODULE>_H`, for example `APPS_CORE_PARSE_H`. An app's own `src/<layer>/` holds only modules that no library it links has, so an app's include never resolves to a different file than the library's.

### Build profiles and analysis

Each library target links the `apps_build_project` profile of the project that builds it, and the test-only targets link `apps_build_tests`. An app's sanitizer build therefore instruments the library code, and `core`'s arena poisoning runs in the app's ASan build. `SYSTEM` on `add_subdirectory` keeps the app's clang-tidy to the app's own code. The app's cppcheck reads the headers of the libraries that the app lists in `APPS_LIBRARIES`, so that it can expand their macros, and it also checks the inline functions of these headers. Only the top-level `libs/` project runs clang-tidy and cppcheck on the library code, and only it has the `lint` and `format` targets for the library sources. The lint targets check the sources of every library and test target.

### Tests

Only the top-level `libs/` project builds the library tests, and only when `APPS_BUILD_TESTING` is on. An app that adds the project never registers them. `libs_add_unit_test(<library> <module>)` compiles `test_<module>.c` into the executable `<library>_unit_<module>` and registers it as the CTest case `<library>.<module>`. The test links the library, `apps_build_tests`, acutest and the libraries that the call names. With `OS posix`, the test links `os_posix`. With `OS memory`, it links `os_memory` and compiles with `APPS_TEST_OS_MEMORY` set to 1. `NAME` gives the executable and the case another name, so that one source can build more than one test, as the `os` contract tests do.

## core

The library has two kinds of modules. The memory modules build on each other in this order:

1. An arena allocates memory.
2. A string is a view of that memory.
3. A buffer grows a string in an arena.
4. A log is a buffer of error messages.

The other modules are small primitives:

- [asan](src/core/asan.h): `SHARED_ASAN` is `1` in a translation unit that the address sanitizer instruments, and `0` otherwise, and the header includes the sanitizer interface when it is `1`, so the arena and its tests compile the gaps and the poisoning only in an ASan build.
- [ascii](src/core/ascii.h): Locale-independent ASCII byte classification and case folding.
- [error](src/core/error.h): Turns an `errno` into text, and is thread-safe. Diagnostics themselves go to `core/log.h`, as `printf` formats that the compiler checks.
- [exit_code](src/core/exit_code.h): Every command of an app returns an `enum ExitCode`: `EXIT_CODE_OK` (0), `EXIT_CODE_FAILURE` (1), or `EXIT_CODE_USAGE` (2).
- [parse](src/core/parse.h): Parsing of text into scalar values.
- [path](src/core/path.h): Path construction and validation, the filename and output-path limits, and `path_basename_without_extension`. `path_is_below` finds the part of a path below a root by text comparison. `path_file_dir` and `path_resolve` resolve a relative path that a config file names against the file's directory, as each app's `-c` asks, and `path_trim_trailing_slashes` gives a directory or a URL from a config one spelling without its trailing `/`.
- [path_list](src/core/path_list.h): A growable list of path strings.
- [text](src/core/text.h): Text normalization, matching and validation, including the slug that `text_slugify` makes and the `NUL` check `text_is_nul_free`.
- [url](src/core/url.h): The checks of a base URL that each app's config applies: `url_is_http_with_host` asks for an `http://` or `https://` scheme in any letter case and a host, and `url_has_path_breaking_byte` finds a `?`, a `#`, a space or a control byte, which would break a path joined after the URL.
- [utf8](src/core/utf8.h): `utf8_is_valid` checks UTF-8, and `utf8_repair_for_xml` replaces each invalid sequence with U+FFFD and drops the characters that XML 1.0 forbids. Each app that renders templates repairs every render with it, so no page or feed holds text that a browser or a feed reader rejects, whatever bytes a value came from.

### `os_oom`

When an allocation does not fit in an arena, the arena calls `os_oom`. This function must not return. The library does not define `os_oom`, because only the application knows which message to print and which exit status to use when it runs out of memory. Each program that links the library must define `os_oom` once:

```c
_Noreturn void os_oom(void) {
  fputs("myprogram: out of memory\n", stderr);
  _Exit(EXIT_FAILURE);
}
```

sosig, fram and surge define it as one call to `cli_oom_exit` from the `cli` library, with the program's name. The unit tests that link `testing` use the `os_oom` of `testing`. The tests of the arena, strings, buffers and log in `core` link no `testing`, so [src/core/test_oom.h](src/core/test_oom.h) defines their `os_oom` and `test_is_oom`, with the same `TEST_OOM_EXIT_STATUS` as `testing`.

### Arenas

[`core/arena.h`](src/core/arena.h) defines `struct Arena`, which allocates memory in sequence from one region. If a program frees each allocation separately, it must decide for each pointer who owns it and when to free it, and each allocation can fail. An arena does not free allocations one at a time, and its allocation functions never return `NULL`. If an allocation does not fit, the arena calls [`os_oom`](#os_oom). Data lives as long as its region, unless the program allocated the data through a copy of the arena and then dropped that copy.

`arena_reserve` reserves only address space. The arena makes pages usable when allocations reach them, so a large reservation costs memory only for the pages that allocations reach, even on a system with strict overcommit. An arena can also use a static array as its region. In that case, leave `limit` as `NULL`:

```c
struct Arena arena;
if (arena_reserve(1 << 30, &arena) != 0) {
  // report the error and stop; nothing has been reserved
}

static char storage[1 << 16];
struct Arena fixed = {.beg = storage, .end = storage + sizeof storage};
```

| Call | Effect |
| --- | --- |
| `arena_reserve(len, &arena)` | Reserves `len` bytes of address space. If the system refuses the reservation, it returns `-1` and sets `errno`. |
| `arena_reserve_halving(len, &arena)` | Reserves `len` bytes. If the system refuses the full size, for example because of `ulimit -v`, it reserves a power-of-two fraction of `len`. Returns the number of times that it halved the size. |
| `arena_split(&arena, len)` | Removes the first `len` bytes from the free space of `arena`, and returns a new arena over them, for example one arena for each thread. |
| `arena_release(region)` | Gives a whole reserved region back to the system. |
| `arena_new(&arena, count, Type)` | Returns zeroed, aligned storage for `count` objects of `Type`. |
| `arena_alloc(&arena, count, size, align)` | Returns zeroed storage for `count` objects, with a size and an alignment in bytes. |
| `arena_grow(&arena, data, &capacity, size, align)` | Doubles the capacity of a growable array. If the array is the last allocation, it grows in place. |
| `arena_extend(&arena, count, size)` | Returns bytes directly after the last allocation, and does not zero them. |
| `arena_discard(&arena, data, len)` | Gives the pages of a large buffer back to the system while the arena stays in use. The buffer loses its bytes, but the program can still use the buffer. |

To simulate an address-space limit, a test can lower `arena_reserve_len_max`.

Pass an arena by pointer for data that the caller keeps, and by value for scratch memory. A function that returns data takes `struct Arena* perm` and allocates its result in that arena. A function that needs only scratch memory takes `struct Arena scratch` by value. The function allocates through its own copy of the bounds, so all of its allocations are free again when it returns:

```c
struct Str render_title(struct Str name, struct Arena* perm, struct Arena scratch);
```

To use an arena correctly:

- Never assume that two allocations are next to each other. In a build with the address sanitizer, the arena puts a gap between allocations. To grow an allocation in place, use `arena_grow` or `arena_extend`.
- Never let two threads allocate from the same arena. Give each thread its own region with `arena_split`.
- Never grow a buffer or a log in an arena that you also pass by value to another function. That function allocates the same bytes that the buffer grows into, so the two overwrite each other's data without a crash.
- With the address sanitizer, an arena over a reserved region poisons all bytes that are not part of an allocation, so the sanitizer reports a read or write past the end of an allocation. When a function returns, the arena cannot poison the scratch memory of that function again, so the sanitizer does not report a later use of that memory.

### Strings

[`core/str.h`](src/core/str.h) defines `struct Str`, which holds a pointer and a signed `ptrdiff_t` length. A C string marks its end with a `NUL`, so a substring needs a copy. The lengths of C strings are unsigned, so `a - b` can wrap to a very large value. A substring of a `struct Str` is a view of the original bytes, a `NUL` is an ordinary byte, and arithmetic on lengths cannot wrap. The zero value, `(struct Str){0}`, is the empty string.

| Call | Effect |
| --- | --- |
| `S("literal")` | Makes a string from a literal. |
| `STR_INIT("literal")` | Initializes a string in a static table. |
| `str_from_cstr(cstr)` | Wraps a C string that comes from outside the program. |
| `str_head(s, n)`, `str_tail(s, n)` | Return a view of the first `n` bytes, or of the bytes after them. |
| `str_cut(s, c)` | Splits `s` at the first `c` into a `head` view and a `tail` view. Sets `is_found` if `s` contains `c`. |
| `str_eq`, `str_eq_folded`, `str_compare`, `str_startswith`, `str_endswith` | Compare two strings. `str_eq_folded` ignores the case of ASCII letters. |
| `str_clone(s, arena)`, `str_concat(head, tail, arena)` | Copy the bytes into an arena. |
| `str_cstr(s, arena)` | Copies the bytes into an arena and adds a terminator, for an interface that needs a C string. |
| `STR_FMT(s)` | Expands to the two arguments that `%.*s` takes: the length as an `int`, and the data. |

The view functions do not copy bytes. To walk a delimited list, call `str_cut` on each tail in turn:

```c
struct StrCut cut = {.tail = S("a,b,c")};
while (cut.tail.len > 0) {
  cut = str_cut(cut.tail, ',');
  // cut.head is "a", then "b", then "c"
}
```

To use a string correctly:

- Pass a `struct Str` by value. Its size is two machine words.
- A view is valid only while the bytes that it points to exist. Before those bytes go away, copy the view with `str_clone`.
- Convert a length to `size_t` only where you call a libc, POSIX, or vendored function. At that point, check that the length is not negative.
- Make a C string with `str_cstr` only where an interface needs one, such as a path for a system call. A `NUL` inside the string ends the C string early. If the input can contain a `NUL`, check for it first.
- Print a string with `%.*s` and `STR_FMT(s)`. `STR_FMT` evaluates its argument twice, so pass a variable, not an expression with side effects. `%.*s` stops at the first `NUL`, and does not print the bytes after it.

### Buffers

[`core/buf.h`](src/core/buf.h) defines `struct Buf`, which grows a string in an arena. A program often builds output, such as a rendered page or a message, from many small appends, and does not know the total size in advance. While the buffer is the last allocation in its arena, the buffer grows in place, so a large output usually needs no copies. Start a buffer with the arena that it grows in, append to it, and then read it back as a string:

```c
struct Buf out = {.arena = perm};
buf_append(&out, S("<h1>"));
buf_printf(&out, "%.*s", STR_FMT(title));
buf_append_char(&out, '\n');
struct Str html = buf_str(&out);
```

| Call | Effect |
| --- | --- |
| `buf_append(&buffer, s)` | Appends a string. |
| `buf_append_char(&buffer, c)` | Appends one byte. |
| `buf_printf(&buffer, format, ...)` | Appends formatted text. It never truncates the text. |
| `buf_vprintf_measured(&buffer, text_len, format, args)` | Appends the text of a `va_list`. The caller measures the text with `vsnprintf` and gives the length as `text_len`. A function that formats its own arguments, as `buf_printf` does, calls it in its second pass. |
| `buf_str(&buffer)` | Returns all bytes in the buffer as a string. |
| `buf_cstr(&buffer)` | Adds a terminator, which `len` does not count, and returns the buffer as a C string, for an interface that needs one. |

To use a buffer correctly:

- The buffer's arena must outlive the buffer. While the buffer grows, pass its arena by pointer, never by value.
- If you take a pointer into the buffer and the buffer then grows, the pointer still reads the old bytes, but it does not see later appends. Call `buf_str` after the last append.
- `buf_printf` and `buf_vprintf_measured` have the `format` attribute. Compile with `-Wformat`, so that the compiler checks each format against its arguments.

### Logs

[`core/log.h`](src/core/log.h) defines `struct Log`, which holds a buffer of error lines and a count of the errors, and a separate buffer of warning lines and a count of the warnings. A fixed-size error buffer truncates long messages and holds only the first error. With a log, a program can do these things:

- Continue after an error.
- Report all of the errors together.
- Check `log.errors` before it does an action that it cannot undo.

Give the log its own arena, record errors with `printf` formats, and check the count before the first action that the program cannot undo:

```c
struct Log log = {.buf = {.arena = &log_arena}};

if (load_config(path, &perm, scratch, &log) != 0) {
  // load_config called log_error and returned its -1
}
if (log.errors > 0) {
  fputs(buf_cstr(&log.buf), stderr);
  return EXIT_FAILURE;
}
```

| Call | Effect |
| --- | --- |
| `log_error(log, format, ...)` | Records one line and returns `-1`. This lets a function that can fail end with `return log_error(log, "failed to read '%.*s': %s", STR_FMT(path), cause);`. |
| `log_error_in(log, file, format, ...)` | Records one line about an error inside `file`, adds ` (in '<file>')` to the end of the line, and returns `-1`. |
| `log_warn(log, format, ...)` | Records one warning line, which starts with `warning: `, in the warning buffer, and counts it in `warnings`. The error count does not change, so a run with only warnings succeeds. |
| `log_append(log, source)` | Copies the error and warning lines of `source` into `log`, and adds both counts of `source` to those of `log`. For example, after the workers finish, copy the log of each worker into the main log. `log_append_errors` and `log_append_warnings` copy one half. |

A wrapper that adds context formats its own message in scratch memory. Then it records the message with `log_error(log, "%.*s", ...)`.

To use a log correctly:

- Make the log the last parameter of a function that can fail. When the function records an error, return `-1`.
- Put the log in an arena that you never pass by value. The simplest choice is a region that only the log uses.
- Give each thread its own log. After the threads finish, merge the logs with `log_append` in a fixed order, so that the output does not depend on the order in which the threads run.
- Print the warnings after the errors, so the errors that stopped a run come first. The warning buffer grows in the arena of the error buffer, which the first warning gives it.
- `log_error`, `log_error_in` and `log_warn` have the `format` attribute. Give the same attribute to each wrapper that formats its own arguments.

## testing

- [test_support](src/testing/test_support.h): The helpers that several `test_*.c` files need: an arena and a log for each test, checks of a log's text and of formatted text (`test_log_is`, `test_text_is`), the out-of-memory check `test_is_oom`, a seeded random generator (`struct TestRng`), a printable copy of any bytes (`test_printable`), fixture files and directories (`init_fixture_dir`, `write_fixture_file`, `remove_fixture_tree`), the working directory, environment variables, and the capture of a standard stream.

`testing` is an object library, so its `os_oom` is always linked in: a static archive member would be pulled only by a reference that comes before it on the link line, and the reference comes from the arena in `core`, which comes after it. Its `os_oom` exits with its own status, so `test_is_oom` can tell an out-of-memory exit apart from a crash or an ordinary exit. The seed of `test_rng` is `TEST_RNG_SEED_DEFAULT`, or the value of the `APPS_TEST_SEED` environment variable, so a failure that one seed finds can be replayed. `test_support.c` defines `TEST_NO_MAIN`, so the `main` function and the run state of acutest stay in the translation unit of each test executable.

## os

- [os](src/os/os.h): The host interface. Every read, read of standard input, write, replacement, directory creation, removal, listing, `stat` and print that an app does goes through `os_read`, `os_read_stdin`, `os_write`, `os_replace`, `os_mkdirs`, `os_remove`, `os_remove_dir`, `os_list`, `os_stat` and `os_print`. Each function that accesses the host takes a `struct Os*`.
  - `os_read` takes a size limit from each caller, and checks it before any read or allocation. It rejects a file that is not a regular file, and a file that changes size during the read. It also takes an `enum OsReadKind`: with `OS_READ_TEXT`, it rejects a file that contains a `NUL` byte, and with `OS_READ_BYTES`, it accepts any byte, as an image or a video needs.
  - `os_read_stdin` reads standard input to its end, as cwrap does. It rejects input longer than the caller's limit, and stops reading one byte past that limit, so the limit also bounds its memory. It rejects a `NUL` byte, as `os_read` does with `OS_READ_TEXT`.
  - `os_write` writes either bytes or the contents of a file that it streams, and it creates parent directories. It removes a file, symlink or hard link at its destination without following it and creates a new file there, and the write is neither atomic nor synced. A new file gets mode `0666` reduced by the process umask.
  - `os_replace` atomically replaces a file that only the user has, such as a source file that cwrap rewrites in place. It writes and syncs a sibling temporary file and renames it over the destination, so a reader sees either the previous file or the new file whole, and a failed write leaves the destination untouched. It follows a symlink, creates no parent directory, keeps the permission bits of an existing file, and breaks a hard link to it.
  - `os_stat` reports identity, type, size, and modification time.
  - `os_list` closes a directory before it returns. It classifies each directory entry, and for a symlink it classifies the target. It skips dangling links, and directory entries that were removed after it read them.
  - `os_print` carries every byte that an app prints, so a test on the in-memory host sees all of it. One call writes its text as a whole. A caller that prints from more than one thread holds its own lock around the call.
  - `os_oom` is the one other member. `core` declares it, and each program defines it: an app in its `app/main.c`, `testing` for the unit tests, and `core/test_oom.h` for the tests of `core` that link no `testing`.
- [os_posix](src/os/os_posix.h): The POSIX implementation of `os.h`, in the object library `os_posix`. An app's executable and every unit test registered with `OS posix` link it. Its `struct Os` has no state.
- [os_memory](src/os/os_memory.h): The in-memory implementation of `os.h` for unit tests, in the object library `os_memory`: an in-memory tree with symlinks, hard links, a modification-time counter, injected failures, and captured output. A lock guards the tree, so tests that run workers can use it.
- [oom](src/os/oom.h): Holds what the out-of-memory report prints: the command's log, which `oom_set_log` names, and, for each thread, the file that the thread works on and its job's log, which `oom_set_job` names. `oom_report` takes the name of the program, which leads its last line, as in `sosig: out of memory`.

An executable selects its host when it links. A link-time choice needs no table of function pointers, because no executable needs two hosts. `os_posix` and `os_memory` are object libraries, because `os` and an app's library refer to them, and a single-pass linker skips a static archive that comes before the code that uses it on the link line.

Each property that a test checks against the in-memory host must have a contract test. [src/os/test_os.c](src/os/test_os.c) compiles once against each host, as `os.contract_posix` and `os.contract_memory`, and both executables must pass, so every property that a test relies on in the in-memory host is also true on POSIX.

## runtime

- [build_run](src/runtime/build_run.h): `build_run` resolves a build's worker count, at most `BUILD_RUN_WORKER_PER_CPU_MAX` (four) per online CPU, reserves one block for every arena the app sizes, halving the workers before the arenas under an address-space limit, as [Build memory](#build-memory) describes, runs the app's build in it, prints the errors and then the warnings, and releases the block.
- [job](src/runtime/job.h): `job_run` connects the parallel phases of a build to the worker pool. It gives each job a log of its own, in the share of the phase's scratch that the job's worker owns. It reports progress under a lock, merges the errors of the failed jobs, and merges the warnings of every job. When the lines of two jobs differ only after the text that the caller names, the two jobs count as one failure, or as one job with warnings. Each job writes its results into its own slot, which the caller owns.
- [pool](src/runtime/pool.h): Runs indexed jobs through a pthread pool of bounded size. It completes every job, even when one job fails, and joins every worker before it returns. Each job gets the index of the worker that runs it, so the job can use that worker's arenas. The module also gets the default worker count from the number of online CPUs.
- [read_root](src/runtime/read_root.h): `read_root_require` checks that each directory a build reads exists and is a directory, skips one whose config key the load rejected, accepts an optional one that does not exist, and reports every one that fails in one run, with its config key, its cause and its path.
- [verbose](src/runtime/verbose.h): `verbose_printf` prints a command's formatted status line to `stderr` in one `os_print` call, when `--verbose` enabled it.
- [walk](src/runtime/walk.h): Finds and sorts the matching regular files in a directory tree. It uses only `os_list` and `os_stat`, so it behaves the same on every host. It follows symlinked directories, but walks each directory once, by device and inode. It skips a cycle or a second alias of a directory. A caller can name one directory to exclude. The walk visits directory entries in byte order, and visits each symlinked directory only after every real directory, so it lists the path without a symlink when such a path exists. Only one directory is open at a time. `walk_stat_paths` stats each listed file for the output cache's rows.

[src/runtime/test_walk_posix.c](src/runtime/test_walk_posix.c) runs the walk on the POSIX host, for the failures that only a real filesystem gives.

### Build memory

**Decision.** `build_run` reserves a build's memory once, before the build reads any input, as one block of address space, and splits it into the arenas that the app's `BuildRunLens` sizes: the log, a perm and a scratch for the main thread and for each worker, and in the main thread's scratch a log share for each worker and one for `job_run`. Each app derives the sizes from the limits that bound what an arena holds, so the worker count is the only input that the reservation depends on: the `-w` count, or one per online CPU, at most `BUILD_RUN_WORKER_PER_CPU_MAX` (four) per online CPU. If the system refuses the block, `build_run` halves the workers, down to one, and then `arena_reserve_halving` halves the block and every arena with it. An arena commits pages only when an allocation reaches them, one mebibyte past what it needs.

**Reasons.**

- Sizes from the limits hold every input that the limits accept, apart from any input shape that an app names beside its sizes as needing more, so a build without an address-space limit runs out of memory only on such an input or when the system does. An estimate from the inputs would help only under `ulimit -v`, and could be wrong in both directions.
- Strict overcommit charges every committed page, and Linux ignores `MAP_NORESERVE` under it, so an arena commits on demand.
- On macOS a reservation costs about 1 µs per GiB to reserve and again to release, a fraction of a millisecond at these sizes.
- Fewer workers keep every arena at full size, so the limit that a build needs does not grow with `-w`.

**Cost.** Under a `ulimit -v` below the full reservation, the arenas hold less than the limits allow, so a large input ends in the out-of-memory report and not in a sized refusal. The build still never writes wrong bytes.

## cli

The command line that sosig, fram and surge share. Each app describes itself in a `CliSpec`: its description, the noun of what `build` builds, its default config file, its version line and grammar list, and its `build` and `config` commands. cwrap has a command line of its own.

- [cli](src/cli/cli.h): `cli_parse` parses the command line into a command, the options, and one of four actions: version, help, run, or error. It never prints and never exits. `cli.c` is the only translation unit that defines `COPT_IMPL`. `-c`, `-h` and `-V` apply to every command, and each command lists the other flags that it accepts. `cli_print_version` and `cli_print_usage` format the app's texts from its spec.
- [cli_dispatch](src/cli/cli_dispatch.h): `cli_dispatch` calls `cli_parse`. For the run action, it calls the app's `build` with the parsed `BuildOptions`, or runs `config` in a reservation of its own through the app's `config_print`, and prints the load's errors when it fails. For each other action, it prints one text through `os_print`: the diagnostic for the rejected command line, the `--help` text, or the `--version` text, formatted in a reservation that it releases when it returns. `cli_oom_exit` is what each program's `os_oom` calls: it prints the out-of-memory report under a lock, so two workers that run out at once print one, and ends the process with `_Exit`.

## formats

- [frontmatter](src/formats/frontmatter.h): `frontmatter_find` finds the front matter at the start of a Markdown file, TOML between `+++` lines or YAML between `---` lines, by its fences alone, and reports its kind, its text and the body after it. sosig parses the TOML of its content files, and surge shows either kind as a code block.
- [html](src/formats/html.h): HTML escaping of untrusted text for generated markup.
- [rfc3339](src/formats/rfc3339.h): `RFC3339_EPOCH`, the Unix epoch in UTC, which surge and sosig write for a date that Atom requires and a value lacks. Every date the apps write spells a zero offset `Z`.
- [toml](src/formats/toml.h): The read of a config file, at most `TOML_CONFIG_LEN_MAX` (1 MiB), with `toml_load_config`, TOML parsing, text validation at the boundary where parsed bytes become owned text, and the conversion and formatting of dates and datetimes. `toml_require_known_keys` is the shared check for unknown keys. It reports each table key that is not in the list of known keys that a loader gives, and it reports a key that contains a `NUL` byte without its name, but with the name of its table. The config readers and printers that every app's config loader shares are here too, so the apps word each config diagnostic and print each config value the same way: `toml_copy_string` copies a string key, `toml_copy_strings` an array of strings, `toml_copy_bool` an optional boolean key, `toml_copy_dir` copies a directory key, trims its trailing `/`, rejects an empty one and resolves it against the config's directory, and `toml_copy_size` copies a bounded integer key under the policy of a `struct TomlSizeKey`, which also says whether the key is required and whether its bound is a length in bytes. Each names the key after the dotted prefix of its table. `toml_print_key_string`, `toml_print_key_string_optional` and `toml_print_key_string_array` write the `key = value` lines of a printed config, and quote each string with `toml_print_string`, which writes a TOML basic string with the short escapes where TOML has them. `toml_copy_base_url` copies a site's base URL, checks that it is an absolute `http` URL with a host and no byte that breaks a path, and drops its trailing `/`.
- [tsv](src/formats/tsv.h): `tsv_escape_append` writes a text as a field of a tab-separated line, with each `\`, tab and newline escaped, so that the field cannot split its line and two different texts never give the same field. surge and sosig write the texts of the page recipe of their output caches with it.

## markup

- [body_html](src/markup/body_html.h): The markup that the Markdown and Org renderers share, written in one place for both: headings, with the `id` of each and its anchor link beside it in a `<div class="heading">`, which `body_html_headings_write` inserts once the body is written, because a heading's text, and an Org headline's `CUSTOM_ID`, come only after its opening tag, footnotes, task checkboxes, admonitions, math, code blocks that `highlight` highlights, the code lines of a block with line numbers or code references, and the escaping of link targets. It also defines `LinkTargets`, a callback that both renderers give the target of every link and image. The callback can write a URL of its own for a target, or decline, and then the renderer writes the target itself. With `is_raw_html_linked`, `body_html_raw_html_write` gives it the targets of raw HTML too: each file that an element embeds, such as an `<img>`'s `src` and `srcset`, a `<video>`'s `src` and `poster` or an `<iframe>`'s `src`, as an image, and the `href` of each `<a>` and `<area>` as a link, as surge asks for its READMEs and sosig does not. The modules know nothing about content files.
- [highlight](src/markup/highlight.h): Syntax highlighting of a code block with tree-sitter. `highlight_grammar_find` finds the grammar for a language name among the grammars that `APPS_GRAMMARS` compiles in. [src/markup/vendor.cmake](src/markup/vendor.cmake) declares these grammars in a header that it generates, with the highlight queries from [src/markup/queries/](src/markup/queries/). `highlight_grammar_list` names those grammars. `highlight_write` writes the code with a `<span class="tok-…">` around each token that the grammar's highlight query captures. It writes the code plain, only escaped, when there is no grammar or when the code block goes past a memory limit. Its line callbacks run around each line, and the closing one writes the line end, so markup around a line can hold it.
- [markdown](src/markup/markdown.h): `markdown_to_html` parses Markdown with md4c and its extension set, and writes the HTML through its own callbacks. It gives each link and image destination, as written, to the `LinkTargets` callback of its caller. md4c can split one raw HTML tag across text runs, so the renderer gathers consecutive runs of raw HTML and writes them together through `body_html_raw_html_write`.
- [org](src/markup/org.h): `org_to_html` parses Org with org4c, its flags, and the file's `#+OPTIONS:` lines, and writes the HTML through its own callbacks. It gives the org4c type and the path of each link and image to the `LinkTargets` callback of its caller. A `file` link without a description shows the URL that the callback wrote. org4c gives an export block line by line, so the renderer gathers consecutive export text and writes it together through `body_html_raw_html_write`, to the buffer it went to.

The modules escape text with `formats/html.h`.

### Highlighting: shared queries, memory per code block

**Decision.**

1. `highlight` compiles each grammar's highlight query at the grammar's first lookup, under a lock. The query lives until the process exits, and every worker reads it.
2. For each code block, the writer makes a new tree-sitter parser and query cursor, and deletes them and the tree before it returns.
3. While the writer highlights a code block, tree-sitter allocates from the scratch that the writer gets. A thread-local variable points the allocator at that scratch. The allocator puts a freed allocation on a free list for its exact size, and all of these allocations go away with the scratch. At other times, for example when `highlight` compiles a query, the allocator calls `malloc`, and calls `os_oom` when `malloc` fails.
4. If the parse of a code block takes more than `HIGHLIGHT_MEMORY_LEN_MAX` (128 MiB), or its tree has an error node with more than 4096 children, the writer writes the code block plain, as for a language with no grammar. If a query would take more memory, or more work than the length of its code block allows, the writer writes the rest of the code block plain.

**Reasons.**

- Query compilation takes 2 to 15 ms, so it occurs once. A new parser and cursor take less than a microsecond to make, so each code block gets its own, and no job or pool carries state for each worker.
- Tree-sitter recovers from tens of thousands of unclosed brackets with one error node that has that many children, and its query steps over the later siblings of a node at each of them. It also follows a match at every level of brackets nested 65,536 deep or more. Each of these cases takes seconds on a code block of a few hundred KB. A well-formed wide node, such as a long table of numbers, is as fast as other code, and real code takes at most three work reports per KiB, so the bounds catch only such code blocks.
- The queries are immutable after compilation, and a site that names no language compiles none. If the code freed them at exit, the free could race a worker that still highlights when `os_oom` ends the process. They are the one tree-sitter allocation that no call frees.
- With musl's `malloc`, a parse on eight threads is no faster than on one thread. Allocation from each worker's scratch makes it scale, and when the scratch runs out, the result is the arena's `os_oom`.
- If `free` did nothing, text that a grammar cannot parse could commit memory up to the limit before the writer writes the code block plain. The free lists reuse each allocation for the next allocation of the same size.
- The limit bounds the scratch that one code block can use. Each converter splits a region of twice the limit off its scratch for its code blocks, so that region, like every arena, gets its size from a limit, and sosig's `BUILD_SCRATCH_LEN` counts it, together with the code that a converter holds until a code block ends.
- A plain code block is an exception to the rule to fail immediately at a limit, because a build that fails over a code block's colors helps no one, and `markup` has no log.

**Cost.** An Org code block that is almost all code references with a short `-l` label format is the one input that the limits accept and `BUILD_SCRATCH_LEN` does not hold, as the definition of sosig's `BUILD_SCRATCH_LEN` explains.

## output

- [build_cache_file](src/output/build_cache_file.h): `build_cache_file_load` reads an output cache and says whether it is missing, loaded, or unusable, with the reason for the verbose log. `build_cache_file_parse` checks the file around its rows, which each app parses itself: an empty file, which an interrupted write leaves, a first line other than the one the build writes, and a last row cut short of its newline, and it reports a row that does not parse by its line.
- [manifest_builder](src/output/manifest_builder.h): Fills a build's manifest and refuses an output that would hurt an input. `manifest_builder_check_output_dir` compares `output_dir` with each input root that the app labels, before any walk, and rejects one inside a root or one that holds a root or the config. `manifest_builder_add` rejects an output past a path limit, at or below a root, through a symlink too, or on an input file, and a duplicate, each with a line of its own. `manifest_builder_add_output` records an output with its attributes, `manifest_builder_add_template_list` records a config list of templates, `manifest_builder_add_copy` and `manifest_builder_add_copies` record copies whose attributes are each source's path below its root, size and modification time, with each source's size for the output cache, and `manifest_builder_finish` reports an output that nests under another. Every app builds its roots and its input list, and adds its own outputs.
- [manifest](src/output/manifest.h): An insertion-ordered map from `output_path` to `source_label`. A build makes it before it writes an output file. A 4-ary hash trie, with the ASCII-folded path as its key, lets `manifest_add` reject a duplicate. Each trie level uses the next two bits of the path's 64-bit FNV-1a hash, so a lookup goes down O(log n) levels in expectation, and the trie never moves a manifest entry that it placed. Two paths that differ only in ASCII case are duplicates. `manifest_find_prefix_collision` finds a path that is a `/`-delimited prefix of another path. Each manifest entry also holds a mark, which is pending until the output writer marks the path as written or kept, and the size of the file that the mark leaves, when the writer knows it. A mark changes no trie link, so parallel jobs mark their own paths at the same time. An entry also holds how its output is made: its attributes, the text that names its inputs, and for a copy its source and the source's size.
- [output_cache](src/output/output_cache.h): The output cache that `output_writer_produce` reads: one row per output that declares attributes, with its attributes and its size, and one row per fact that an app records about an input. `output_cache_load` rejects a file of another version as unusable. `output_cache_write` writes it from the manifest's marks after the last output, also after a failure, because only what the build wrote or kept gets a row. Until then the previous cache stays, so an interrupted build leaves it, and that is safe: a file that the build cut short has another size than its row, and one that it rewrote whole has new attributes. Each row takes its size from the mark, and stats only an output whose mark recorded none, such as one that an app kept without `output_writer_produce`.
- [output_path](src/output/output_path.h): Reports a generated output path that exceeds either output-path limit, with the same wording for every producer, and builds the `../` prefix from an output file back to the output root (`output_path_to_root`).
- [output_pruner](src/output/output_pruner.h): `output_pruner_prune` removes every directory entry below the output directory that the manifest does not name and that no output is below, after a build writes every output. It compares device and inode values, not names, so on a case-insensitive filesystem it keeps the old spelling of a current output. It removes a symlink as a link and never goes into one. It reports each entry that it cannot list or remove once, and continues. Every removal goes through the output writer, so a dry run records it instead. [src/output/test_output_pruner_case.c](src/output/test_output_pruner_case.c) runs it on the POSIX host, for the case-insensitive filesystem that only a real host has.
- [output_writer](src/output/output_writer.h): Makes every change that a build makes under the output directory: the writes, the creation of the output directory, and the prune's removals. Each write or keep marks its path in the manifest. The writer fails on a path that the manifest does not name, because the same build's prune would remove that path. In a dry run, the writer makes no change and records the removals, and `output_writer_report` lists its marks and removals.

`output_writer_produce` is the call that every output goes through, from any job. It keeps an output without making it when the output declares attributes, the cache records the same ones, and the file has the recorded size. Otherwise it makes the output, keeps a file whose bytes are unchanged, and writes any other. A copy is streamed instead of made. Each mark records the size of the file that it leaves: the size the cache row records, the size of the made bytes, or for a copy the size in its source's stat, so the cache write stats no output that went through it. `is_forced` keeps nothing, and a dry run never makes an output whose attributes do not match. A build keeps an output only when making it is expensive and its inputs can be named exactly; such an output declares attributes in the manifest, and every other output declares none, so it is always made and written only when its bytes change. Surge's previous clone pack bypasses it: `output_writer_keep` keeps that pack for one more build, because nothing in the build can make it. The output cache bypasses it too: `output_cache_write` writes the file directly, because a cache cannot vouch for itself.

## templates

- [template_set](src/templates/template_set.h): The templates of a build and their renders, which every app's pages, aggregates and feeds go through. `template_set_load` reads and compiles each page template that the app names and every `.html` file of `partials/` whose name is an identifier, once, before a build writes anything, and reports every template that cannot be read or does not compile in one run. A set holds at most `TEMPLATE_PARTIAL_COUNT_MAX` partials, and reads each file up to `TEMPLATE_FILE_LEN_MAX`. `template_set_render` renders one page template of the set with mustache4c over a `struct TemplateData`, the callbacks through which the app gives its data: a root, a child by name, a child by index, and the text of a node, with whether it is HTML. The set is read-only once loaded, so the workers render from it at once.

A render escapes every interpolation for HTML, and fails a triple mustache or an ampersand tag of a node whose text is not HTML, so a value that the app did not make cannot add markup. Its two output callbacks repair every text that they write, a value or literal template text, with `utf8_repair_for_xml`, so no output holds an invalid UTF-8 sequence or a character that XML 1.0 forbids. A render stops at `TEMPLATE_RENDER_EXPANSION_COUNT_MAX` partial expansions, which ends a partial that includes itself, and at its output bound, `TEMPLATE_RENDER_OUTPUT_LEN_MAX` outside the tests. A failure names the template that it is in, and ends with `(while rendering '<output>')`, the output that the render is for.
