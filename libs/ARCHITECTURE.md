# Architecture

The `libs/` project holds the libraries that the apps share. Each library is a CMake target with one directory below `libs/`. An app links the libraries that it uses, and the library code compiles with the build profiles of that app. This document describes the libraries, their modules, their dependencies, and how their tests run. Each app's ARCHITECTURE.md describes the modules that are the app's own, and each app's STYLE.md covers the code style.

## Layout

- [CMakeLists.txt](CMakeLists.txt) and [CMakePresets.json](CMakePresets.json): The project entry point and its presets. An app adds the project with `add_subdirectory(../../libs ${CMAKE_BINARY_DIR}/libs EXCLUDE_FROM_ALL SYSTEM)`, so a library builds only when the app links it. Configured on its own, the project builds every library, its tests, its analysis and its lint.
- [vendor.cmake](vendor.cmake): The vendored targets that the libraries and their tests use when the project is configured on its own: `acutest`, from the root [vendor/](../vendor/) directory. An app that adds the project gives these targets from its own `vendor.cmake`.
- [shared/](shared/), [core/](core/) and [testing/](testing/): One directory for each library, with the library's sources, its `test_*.c` files and its `CMakeLists.txt`.

## Libraries

| Library | Contents | Linked by |
| --- | --- | --- |
| [`shared`](shared/README.md) | The arena allocator, counted strings, growable buffers and the error log. | sosig, fram, cwrap |
| [`core`](#core) | ASCII classification, error text, exit codes, number parsing, paths, path lists and text helpers. | sosig, fram, cwrap |
| [`testing`](#testing) | The support that every unit test uses, and the `os_oom` of the unit tests. | the unit tests of every app and of the other libraries |

### Dependency rules

- `core` depends on `shared`.
- `testing` depends on `core` and `shared`.
- No library depends on an app.

A library include names the library and the module, for example `#include "core/parse.h"` or `#include "testing/test_support.h"`. Each library has an include root of its own: `libs_add_include_root` makes a directory in the build tree that holds one link, named for the library, to the library's directory, and gives the target that directory as its include root. `shared` has its own include root in the source tree, `shared/include/`, as in `#include "shared/str.h"`. A target therefore reaches the headers of the libraries that it links and of no other library. A header guard is `APPS_<LIB>_<MODULE>_H`, for example `APPS_CORE_PARSE_H`. An app's own `src/<layer>/` holds only modules that no library it links has, so an app's include never resolves to a different file than the library's.

### Build profiles and analysis

Each library target links the `apps_build_project` profile of the project that builds it, and the test-only targets link `apps_build_tests`. An app's sanitizer build therefore instruments the library code, and `shared`'s arena poisoning runs in the app's ASan build. `SYSTEM` on `add_subdirectory` keeps the app's clang-tidy to the app's own code. The app's cppcheck reads the headers of the libraries that the app lists in `APPS_LIBRARIES`, so that it can expand their macros, and it also checks the inline functions of these headers. Only the top-level `libs/` project runs clang-tidy and cppcheck on the library code, and only it has the `lint` and `format` targets for the library sources. Each library appends its sources to the lint list.

### Tests

Only the top-level `libs/` project builds the library tests, and only when `APPS_BUILD_TESTING` is on. An app that adds the project never registers them. `libs_add_unit_test(<library> <module>)` compiles `test_<module>.c` into the executable `<library>_unit_<module>` and registers it as the CTest case `<library>.<module>`. The test links the library, `apps_build_tests`, acutest and the libraries that the call names.

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
