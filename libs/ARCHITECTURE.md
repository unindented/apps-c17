# Architecture

The `libs/` project holds the libraries that the apps share. Each library is a CMake target with one directory below `libs/`. An app links the libraries that it uses, and the library code compiles with the build profiles of that app. This document describes the libraries, their modules, their dependencies, and how their tests run. Each app's ARCHITECTURE.md describes the modules that are the app's own, and each app's STYLE.md covers the code style.

## Layout

- [CMakeLists.txt](CMakeLists.txt) and [CMakePresets.json](CMakePresets.json): The project entry point and its presets. An app adds the project with `add_subdirectory(../../libs ${CMAKE_BINARY_DIR}/libs EXCLUDE_FROM_ALL SYSTEM)`, so a library builds only when the app links it. Configured on its own, the project builds every library, its tests, its analysis and its lint.
- [vendor.cmake](vendor.cmake): The vendored targets that the libraries and their tests use when the project is configured on its own: `acutest` and `tomlc17`, from the root [vendor/](../vendor/) directory. An app that adds the project gives these targets from its own `vendor.cmake`.
- [shared/](shared/), [core/](core/), [testing/](testing/), [runtime/](runtime/), [os_memory/](os_memory/), [formats/](formats/) and [site/](site/): One directory for each library, with the library's sources, its `test_*.c` files and its `CMakeLists.txt`.

## Libraries

| Library | Contents | Linked by |
| --- | --- | --- |
| [`shared`](shared/README.md) | The arena allocator, counted strings, growable buffers and the error log. | sosig, fram, cwrap |
| [`core`](#core) | ASCII classification, error text, exit codes, number parsing, paths, path lists and text helpers. | sosig, fram, cwrap |
| [`testing`](#testing) | The support that every unit test uses, and the `os_oom` of the unit tests. | the unit tests of every app and of the other libraries |
| [`runtime`](#runtime) | The host interface, its POSIX implementation `os_posix`, the out-of-memory report, the worker pool and the directory walk. | sosig, fram |
| [`os_memory`](#os_memory) | The in-memory implementation of the host interface. | the unit tests of sosig, fram and the libraries |
| [`formats`](#formats) | HTML escaping and the TOML helpers. | sosig, fram |
| [`site`](#site) | The output manifest, the build job, output paths and the output writer. | sosig, fram |

cwrap has a smaller host interface of its own in its `src/runtime/`, and links neither `runtime` nor `os_memory`.

### Dependency rules

- `core` depends on `shared`.
- `testing` and `runtime` depend on `core` and `shared`.
- `os_memory` depends on `runtime` and `testing`.
- `formats` depends on `core`, `shared` and the vendored `tomlc17`. The project adds `formats` only when the `vendor_tomlc17` target exists, so an app that vendors no tomlc17, such as cwrap, builds without it.
- `site` depends on `core`, `runtime` and `shared`.
- No library depends on an app.

A library include names the library and the module, for example `#include "core/parse.h"` or `#include "site/manifest.h"`. Each library has an include root of its own: `libs_add_include_root` makes a directory in the build tree that holds one link, named for the library, to the library's directory, and gives the target that directory as its include root. `shared` has its own include root in the source tree, `shared/include/`, as in `#include "shared/str.h"`. A target therefore reaches the headers of the libraries that it links and of no other library. A header guard is `APPS_<LIB>_<MODULE>_H`, for example `APPS_CORE_PARSE_H`. An app's own `src/<layer>/` holds only modules that no library it links has, so an app's include never resolves to a different file than the library's. An app's header can have the same path as a header of a library that the app does not link, as cwrap's `runtime/os.h` has.

### Build profiles and analysis

Each library target links the `apps_build_project` profile of the project that builds it, and the test-only targets link `apps_build_tests`. An app's sanitizer build therefore instruments the library code, and `shared`'s arena poisoning runs in the app's ASan build. `SYSTEM` on `add_subdirectory` keeps the app's clang-tidy to the app's own code. The app's cppcheck reads the headers of the libraries that the app lists in `APPS_LIBRARIES`, so that it can expand their macros, and it also checks the inline functions of these headers. Only the top-level `libs/` project runs clang-tidy and cppcheck on the library code, and only it has the `lint` and `format` targets for the library sources. Each library appends its sources to the lint list.

### Tests

Only the top-level `libs/` project builds the library tests, and only when `APPS_BUILD_TESTING` is on. An app that adds the project never registers them. `libs_add_unit_test(<library> <module>)` compiles `test_<module>.c` into the executable `<library>_unit_<module>` and registers it as the CTest case `<library>.<module>`. The test links the library, `apps_build_tests`, acutest and the libraries that the call names. With `OS posix`, the test links `os_posix`. With `OS memory`, it links `os_memory` and compiles with `APPS_TEST_OS_MEMORY` set to 1. `NAME` gives the executable and the case another name, so that one source can build more than one test, as the `os` contract tests do.

## core

- [ascii](core/ascii.h): Locale-independent ASCII byte classification and case folding.
- [error](core/error.h): Turns an `errno` into text, and is thread-safe. Diagnostics themselves go to `shared/log.h`, as `printf` formats that the compiler checks.
- [exit_code](core/exit_code.h): Every command of an app returns an `enum ExitCode`: `EXIT_CODE_OK` (0), `EXIT_CODE_FAILURE` (1), or `EXIT_CODE_USAGE` (2).
- [parse](core/parse.h): Parsing of text into scalar values.
- [path](core/path.h): Path construction and validation, the filename and output-path limits, and `path_basename_without_extension`. `path_is_below` finds the part of a path below a root by text comparison.
- [path_list](core/path_list.h): A growable list of path strings.
- [text](core/text.h): Text normalization, matching and validation, including the slug that `text_slugify` makes and the `NUL` check `text_is_nul_free`.

## testing

- [test_support](testing/test_support.h): The helpers that several `test_*.c` files need: an arena and a log for each test, checks of a log's text and of formatted text (`test_log_is`, `test_text_is`), the out-of-memory check `test_is_oom`, a seeded random generator (`struct TestRng`), a printable copy of any bytes (`test_printable`), fixture files and directories (`init_fixture_dir`, `write_fixture_file`, `remove_fixture_tree`), the working directory, environment variables, and the capture of a standard stream.

`testing` is an object library, so its `os_oom` is always linked in: a static archive member would be pulled only by a reference that comes before it on the link line, and the reference comes from the arena in `shared`, which comes after it. Its `os_oom` exits with its own status, so `test_is_oom` can tell an out-of-memory exit apart from a crash or an ordinary exit. The seed of `test_rng` is `TEST_RNG_SEED_DEFAULT`, or the value of the `APPS_TEST_SEED` environment variable, so a failure that one seed finds can be replayed. `test_support.c` defines `TEST_NO_MAIN`, so the `main` function and the run state of acutest stay in the translation unit of each test executable.

## runtime

- [os](runtime/os.h): The host interface. Every read, write, directory creation, removal, listing, `stat` and print that an app does goes through `os_read`, `os_write`, `os_mkdirs`, `os_remove`, `os_remove_dir`, `os_list`, `os_stat` and `os_print`. Each function that accesses the host takes a `struct Os*`.
  - `os_read` takes a size limit from each caller, and checks it before any read or allocation. It rejects a file that is not a regular file, and a file that changes size during the read. It also takes an `enum OsReadKind`: with `OS_READ_TEXT`, it rejects a file that contains a `NUL` byte, and with `OS_READ_BYTES`, it accepts any byte, as an image or a video needs.
  - `os_write` writes either bytes or the contents of a file that it streams, and it creates parent directories. It overwrites its destination in place, through a symlink or a hard link, and the write is neither atomic nor synced. A new file gets mode `0666` reduced by the process umask.
  - `os_stat` reports identity, type, size, and modification time.
  - `os_list` closes a directory before it returns. It classifies each directory entry, and for a symlink it classifies the target. It skips dangling links, and directory entries that were removed after it read them.
  - `os_print` carries every byte that an app prints, so a test on the in-memory host sees all of it. One call writes its text as a whole. A caller that prints from more than one thread holds its own lock around the call.
  - `os_oom` is the one other member. `shared` declares it, and each program defines it: an app in its `app/main.c`, and `testing` for the unit tests.
- [fs](runtime/fs.h): The POSIX implementation of `os.h`, in the object library `os_posix`. An app's executable and every unit test registered with `OS posix` link it. Its `struct Os` has no state.
- [memory](runtime/memory.h): Holds what the out-of-memory report prints: the command's log, which `memory_oom_set_log` names, and, for each thread, the file that the thread works on and its job's log, which `memory_oom_set_job` names. `memory_oom_report` takes the name of the program, which leads its last line, as in `sosig: out of memory`.
- [pool](runtime/pool.h): Runs indexed jobs through a pthread pool of bounded size. It completes every job, even when one job fails, and joins every worker before it returns. Each job gets the index of the worker that runs it, so the job can use that worker's arenas. The module also gets the default worker count from the number of online CPUs.
- [walk](runtime/walk.h): Finds and sorts the matching regular files in a directory tree. It uses only `os_list` and `os_stat`, so it behaves the same on every host. It follows symlinked directories, but walks each directory once, by device and inode. It skips a cycle or a second alias of a directory. A caller can name one directory to exclude. The walk visits directory entries in byte order, and visits each symlinked directory only after every real directory, so it lists the path without a symlink when such a path exists. Only one directory is open at a time.

An executable selects its host when it links. A link-time choice needs no table of function pointers, because no executable needs two hosts. `os_posix` and `os_memory` are object libraries, because `runtime` and an app's library refer to them, and a single-pass linker skips a static archive that comes before the code that uses it on the link line.

## os_memory

- [os_memory](os_memory/os_memory.h): The in-memory implementation of `runtime/os.h` for unit tests: an in-memory tree with symlinks, hard links, a modification-time counter, injected failures, and captured output. A lock guards the tree, so tests that run workers can use it.

Each property that a test checks against the in-memory host must have a contract test. [runtime/test_os.c](runtime/test_os.c) compiles once against each host, as `runtime.os_posix` and `runtime.os_memory`, and both executables must pass, so every property that a test relies on in the in-memory host is also true on POSIX.

## formats

- [html](formats/html.h): HTML escaping of untrusted text for generated markup.
- [toml](formats/toml.h): TOML parsing, text validation at the boundary where parsed bytes become owned text, and the conversion and formatting of dates and datetimes. `toml_require_known_keys` is the shared check for unknown keys. It reports each table key that is not in the list of known keys that a loader gives, and it reports a key that contains a `NUL` byte without its name, but with the name of its table.

## site

- [job](site/job.h): `job_run` connects the parallel phases of a build to the worker pool. It gives each job a log of its own, in the share of the phase's scratch that the job's worker owns. It reports progress under a lock, and merges the logs of the failed jobs. When the lines of two jobs differ only after the text that the caller names, the two jobs count as one failure. Each job writes its results into its own slot, which the caller owns.
- [manifest](site/manifest.h): An insertion-ordered map from `output_path` to `source_label`. A build makes it before it writes an output file. A 4-ary hash trie, with the ASCII-folded path as its key, lets `manifest_add` reject a duplicate. Each trie level uses the next two bits of the path's 64-bit FNV-1a hash, so a lookup goes down O(log n) levels in expectation, and the trie never moves a manifest entry that it placed. Two paths that differ only in ASCII case are duplicates. `manifest_find_prefix_collision` finds a path that is a `/`-delimited prefix of another path. Each manifest entry also holds a mark, which is pending until the output writer marks the path as written or kept. A mark changes no trie link, so parallel jobs mark their own paths at the same time.
- [output_path](site/output_path.h): Reports a generated output path that exceeds either output-path limit, with the same wording for every producer, and builds the `../` prefix from an output file back to the output root (`output_path_to_root`).
- [output_writer](site/output_writer.h): Makes every change that a build makes under the output directory: the writes, the creation of the output directory, and the prune's removals. Each write or keep marks its path in the manifest. The writer fails on a path that the manifest does not name, because the same build's prune would remove that path. In a dry run, the writer makes no change and records the removals, and `output_writer_report` lists its marks and removals.
