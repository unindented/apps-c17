# Build system

This document describes the targets of the `sosig` CMake build, how the targets link to each other, and the reasons for its settings. The CMake build needs CMake 3.25 or later. The [Development](README.md#development) section of the README lists the commands that compile, test and package `sosig`.

## Layout

The top-level [CMakeLists.txt](CMakeLists.txt) sets the project policy, finds the thread library, includes the CMake modules of the root [cmake/](../../cmake/) directory and of its own [cmake/](cmake/) directory, includes [vendor.cmake](vendor.cmake), adds the libraries project [libs/](../../libs/), and adds each source directory. These are the other CMake files:

| Path | Responsibility |
| --- | --- |
| [src/CMakeLists.txt](src/CMakeLists.txt) | Defines the application library `sosig_app` and the `sosig` executable |
| `src/*/CMakeLists.txt` | Add the sources, the header file sets and the unit tests of their directory. [src/runtime/CMakeLists.txt](src/runtime/CMakeLists.txt) also defines the POSIX host `sosig_os_posix` |
| [tests/CMakeLists.txt](tests/CMakeLists.txt) | Adds the golden tests, the unit-test support library and the in-memory host |
| [vendor.cmake](vendor.cmake) | Defines a target for each vendored dependency that `sosig` uses, from the sources in the root [vendor/](../../vendor/) directory, and a target for each tree-sitter grammar that `SOSIG_GRAMMARS` selects, and generates the header that declares these grammars |
| [../../libs/CMakeLists.txt](../../libs/CMakeLists.txt) | Defines the `shared` library, which `sosig_app` links |
| [../../cmake/apps_build_profiles.cmake](../../cmake/apps_build_profiles.cmake) | Sets warnings, optimization, fortification and sanitizers |
| [../../cmake/apps_quality.cmake](../../cmake/apps_quality.cmake) | Configures formatting and static analysis |
| [cmake/sosig_lint.cmake](cmake/sosig_lint.cmake) | Lists the first-party sources that the `format` and `lint` targets check |
| [../../cmake/apps_testing.cmake](../../cmake/apps_testing.cmake) | Defines `apps_add_unit_test`, which registers a unit test and selects the host that the test links |
| [cmake/sosig_run_golden.cmake](cmake/sosig_run_golden.cmake) | Runs one golden test |
| [../../cmake/apps_packaging.cmake](../../cmake/apps_packaging.cmake) | Configures installation and CPack archives |
| [../../cmake/apps_restyle_graphviz_svg.cmake](../../cmake/apps_restyle_graphviz_svg.cmake) | Adds light and dark styles to the target graph |
| [../../cmake/toolchains/](../../cmake/toolchains/) | Configures Zig to cross-compile for the Linux `musl` release targets |
| [CMakeGraphVizOptions.cmake](CMakeGraphVizOptions.cmake) | Sets the filters and the layout of the target graph |
| [CMakePresets.json](CMakePresets.json) | Includes the configure, build, test, package and workflow presets of [../../cmake/release-presets.json](../../cmake/release-presets.json), which includes [../../cmake/presets.json](../../cmake/presets.json), and adds the `grammars` presets |

## Targets

The CMake build has one application library, two hosts, one target for each vendored dependency, the `shared` library, and the interface targets of [Build profiles](#build-profiles). Each executable links the application library and one of the two hosts. CMake generates this graph of the targets and their links from the `release` preset. The graph does not show the dependencies between source modules, and it does not show test-only targets such as the in-memory host `sosig_os_memory`.

![CMake target dependency graph](media/dependencies.svg)

### Application library

`sosig_app` is a private static library that holds every application source except [src/app/main.c](src/app/main.c) and `src/runtime/fs.c`. The `sosig` executable and every unit test link it. It is static because it supports one product and has no public ABI.

Each source directory holds the code for one purpose. Except for the host in `src/runtime/`, the source directories add their files to `sosig_app` and do not define libraries of their own, because more libraries would add link boundaries without independent APIs. `src/build/` and `src/formats/` link the vendored dependencies that only their own code uses, and `src/CMakeLists.txt` links the other vendored dependencies.

`sosig_app` links `vendor_tomlc17` and `shared` as `PUBLIC` dependencies, because the application headers expose the types of these libraries. The other dependencies are `PRIVATE`. `Threads::Threads` is one of the `PRIVATE` dependencies, but CMake still adds it to each executable that links the static library.

`shared`, the library in [libs/shared/](../../libs/shared/), supplies the arena, the counted string, the output buffer and the diagnostic log. When an allocation does not fit, the arena calls `os_oom`, and each program defines its own `os_oom`. `src/app/main.c` defines the `os_oom` of `sosig`, which [ARCHITECTURE.md](ARCHITECTURE.md#out-of-memory-exits-the-process) describes. `sosig_test_support` defines the `os_oom` of the unit tests.

### Hosts

`src/runtime/fs.c` is the POSIX implementation of [src/runtime/os.h](src/runtime/os.h). CMake compiles this file into the object library `sosig_os_posix`, outside `sosig_app`, so each executable selects its host when it links. `sosig` links `sosig_os_posix`. The unit tests that use the default, `OS posix`, also link it (see [Unit tests](#unit-tests)). The tests registered with `OS memory` link `sosig_os_memory`, the in-memory host in [tests/os_memory.c](tests/os_memory.c), instead.

Both hosts are object libraries, because `sosig_app` refers to them and a single-pass linker skips a static archive when the archive comes before the code that uses it on the link line.

### Vendored dependencies

The sources of each vendored project are in the root [vendor/](../../vendor/) directory, and [vendor.cmake](vendor.cmake) compiles the sources that `sosig` uses. Each vendored project has its own target: the header-only projects, acutest and copt, are interface libraries, and the other vendored projects are static libraries. `vendor_tree_sitter_grammars` is also an interface library. It links the grammars that `SOSIG_GRAMMARS` selects and carries the header that declares these grammars, so `sosig_app` links this one target and not each grammar.

### Regenerating the graph

> [!tip]
> Use these commands in `apps/sosig` to update the graph:
>
> ```sh
> cmake --preset release --graphviz=build/release/target-dependencies.dot
> dot -Tsvg build/release/target-dependencies.dot \
>   -o build/release/target-dependencies.raw.svg
> cmake \
>   -DAPPS_GRAPHVIZ_SVG_INPUT=build/release/target-dependencies.raw.svg \
>   -DAPPS_GRAPHVIZ_SVG_OUTPUT=media/dependencies.svg \
>   -P ../../cmake/apps_restyle_graphviz_svg.cmake
> ```

> [!important]
> CMake reads `CMakeGraphVizOptions.cmake` automatically when you use `--graphviz`, but only from the directory of the top-level project, so each app has its own copy. Do not include this file from `CMakeLists.txt`.

## Build profiles

Three interface targets carry the compile and link options:

- `apps_build_project` sets the strict warnings for first-party code and the sanitizer options.
- `apps_build_tests` sets the common warning group and the sanitizer options.
- `apps_build_vendor` sets optimization and the sanitizer options that vendored code supports.

All three targets require C17 or later. A parent project can select a newer standard, but not an older one.

Generator expressions select the options separately for each configuration, so the same rules work with single-config generators and with multi-config generators.

The presets set `CMAKE_COMPILE_WARNING_AS_ERROR`, so the compiler treats warnings as errors. The CMake build does not force this setting on a parent project.

### Sanitizers

`APPS_SANITIZER` accepts `none`, `address` or `thread`. `address` enables AddressSanitizer and UndefinedBehaviorSanitizer, and `thread` enables ThreadSanitizer. The option adds flags to the standard CMake configurations and does not create custom build types.

With `address`, the CMake build instruments first-party code, the `shared` library and vendored code. `shared` builds with the first-party profile, so it keeps every check. For vendored code, it turns off the `null` and `object-size` checks, because vendored code does not support these checks. For the tree-sitter runtime, it also turns off the `function` check, because the runtime calls the `create` function of each grammar scanner, and this function has no prototype. First-party code keeps every check.

## Grammars

`SOSIG_GRAMMARS` selects the tree-sitter grammars that the `sosig` binary links. The default is an empty value, which links every grammar, as in the release archives. A list links only the grammars that it names, and each name in the list must be the name of one `sosig_add_tree_sitter_grammar` call in [vendor.cmake](vendor.cmake). These names are `c`, `bash`, `html`, `css`, `javascript`, `typescript`, `tsx` and `json`. If the binary does not link the grammar for the language of a code block, `sosig` only escapes the block, as it does for a language that has no grammar.

```sh
cmake --preset release -B build/release-small -DSOSIG_GRAMMARS="c;bash"
cmake --build build/release-small -j 8
```

TypeScript and TSX are separate grammars. You can select either of them without `javascript`. Their queries include query files of the JavaScript grammar, and the CMake build reads these files from the root `vendor/` directory for every value of `SOSIG_GRAMMARS`.

The configure step stops with an error when a name matches no grammar. It also stops with an error when `SOSIG_GRAMMARS` is not empty and `APPS_BUILD_TESTING` is on, because the tests need every grammar.

Each selected grammar is a static library, `vendor_tree_sitter_<name>`. This library holds the highlight queries of the grammar as an embedded byte array. `vendor.cmake` generates `tree_sitter_grammars.h`, which declares the language, the queries and the names of each selected grammar. [src/formats/highlight.c](src/formats/highlight.c) defines its table of grammars from this header.

`sosig --version` lists the grammars that the binary links. The build cache also records these grammars. When a binary with a different set of grammars builds a site, the first build renders every page again.

## Linting and formatting

When `sosig` is the top-level CMake project, the `C_CLANG_TIDY` and `C_CPPCHECK` properties of the first-party targets make CMake run `clang-tidy` and `cppcheck` while it compiles these targets. Neither tool checks vendored code or the `shared` library, which the `libs/` project checks. The include path of `cppcheck` contains the headers of `shared` in `libs/shared/include`, because `cppcheck` needs these headers to expand `STR_FMT`. Thus `cppcheck` also analyzes the inline functions that these headers define.

Test targets run `cppcheck` but not `clang-tidy`, because their intentional failure cases cause analyzer errors. When CMake cross-compiles, neither tool runs. Zig supplies the headers for the target platform, and the analyzers on the host cannot find these headers when they repeat the compile command. When the compiler is not Clang, the CMake build turns off `clang-tidy`, because `clang-tidy` reads the compile command and rejects GCC-only warning options such as `-Wlogical-op`. `cppcheck` runs with any compiler.

The `format` and `lint` targets run `clang-format` and then `cwrap`, which refills comment prose to 100 columns, the `.clang-format` column limit. Either tool is skipped when it is not installed. CI and the Linux image install a pinned `cwrap` release.

Both targets run both tools on the first-party files in an explicit list, the `SOURCES` of `apps_add_lint_targets` in [cmake/sosig_lint.cmake](cmake/sosig_lint.cmake). They also run both tools on the template [src/core/sosig_version.c.in](src/core/sosig_version.c.in), which `clang-format` reads as `sosig_version.c` because of `--assume-filename`. When you add a source file or a test file, add it to this list and to its target. A glob at configure time would not find a new file until CMake configures the project again.

## Tests

`APPS_BUILD_TESTING` controls whether CMake defines the tests. It is on by default when `sosig` is the top-level CMake project, and off by default when a parent project adds `sosig` as a subproject. The tests need every tree-sitter grammar, as [Grammars](#grammars) describes.

Each source directory adds its own unit tests. `tests/` adds the golden tests, `sosig_test_support` and `sosig_os_memory`. Every CTest case has the label `sosig`, and also the label `unit` or `golden`.

### Unit tests

`apps_add_unit_test(<module>)` compiles `test_<module>.c` into the executable `sosig_unit_<module>`, and registers this executable as the CTest case `sosig.<module>`. The executable links `apps_build_tests`, acutest, `sosig_test_support`, `sosig_app` and one host.

With the `OS memory` option, `apps_add_unit_test` links the in-memory host and compiles the test with `SOSIG_TEST_OS_MEMORY` set to 1. New tests use this option. The tests registered without `OS memory` link the POSIX host. These tests are:

- the tests that need a real filesystem: `fs` and `os_posix`, which test the POSIX layer; `test_support`, which tests the test-support library; and `output_pruner_case`, which tests the prune on a case-insensitive `/tmp` and skips on a case-sensitive `/tmp`
- the suites that are not yet converted to the in-memory host, which create their fixtures in a real temporary directory: `cli`, `cli_dispatch`, `cmd_config`, `entry_renderer`, `site_config` and `template`
- the tests that use no host

When one source file compiles into more than one executable, the `NAME` option sets the name of each executable and its test. [src/runtime/test_os.c](src/runtime/test_os.c) uses both options. It compiles into two tests: `sosig.os_posix`, which works in a real temporary directory, and `sosig.os_memory`. These tests are the contract tests of `src/runtime/os.h`, and both tests must pass.

Each unit test has a CTest timeout of 60 seconds, so a test that hangs fails and does not block the test run.

No test target compiles an application source file a second time with different definitions. The production render limit is 256 MiB, and a failure test at that limit uses too much memory. Thus the template test passes a 64 KiB limit to `template_render_file_limited`, and does not compile the template code again with a smaller limit.

### Test-support library

`sosig_test_support` holds the helpers that several `test_*.c` files need:

- functions that create a fixture root, write a fixture file and remove a fixture tree recursively
- functions that enter a working directory and go back to the previous one
- functions that capture and restore the standard streams
- functions that make an arena or a log over a fresh reservation, and checks that compare the text of a log exactly
- an out-of-memory check, `test_is_oom`

The library also defines the `os_oom` of the unit tests. This `os_oom` exits with its own exit status, so `test_is_oom` can run code in a child process and tell an out-of-memory exit apart from a crash. The library is an object library, so that this definition links into every test executable in any link order. The unit test of the library is [tests/test_test_support.c](tests/test_test_support.c).

[tests/test_support.c](tests/test_support.c) defines `TEST_NO_MAIN`, so the `main` function and the run state of acutest stay in the translation unit of each test executable. The library uses acutest declarations such as `acutest_check_`, `acutest_message_` and `acutest_abort_`, and the linker resolves these declarations against that translation unit. When a check in the library fails, acutest reports the failure against `test_support.c`, and the test that called the library fails.

### Golden tests

`sosig_fixture_sites` in `tests/CMakeLists.txt` lists the fixture sites in `tests/fixtures/`. Each fixture site is the golden test `sosig.golden.<site>`. `cmake/sosig_run_golden.cmake` does these steps:

1. It copies the site into a scratch tree of its own.
2. It puts stale files in the output. The first build must remove these files.
3. It builds the site three times. The second build reuses the build cache, and the third build passes `--force`. These two builds must not change a generated file.
4. It compares the list of output files with `tests/expected/<site>`, and it compares every file byte for byte.

The comparison leaves out the build cache, `_sosig/v1-build-cache.tsv`, which holds the modification times of the copied sources and has no expected copy.

The script finds the expected files and the actual files with globs after `sosig` runs. These globs run in the test and are not inputs of the CMake build, so a change to a fixture does not require CMake to configure the project again.

### Format checks in shared

`shared` has its own compile-fail tests for `buf_printf` and `log_error`. These tests check that a call does not compile when its arguments do not match its format. The test cases are:

- a raw `struct Str` for `%.*s`
- a missing argument
- a `ptrdiff_t` for `%d`
- a format that is not a literal
- a value that is not a string, given to `STR_FMT`
- a mismatch through a forwarding wrapper

These tests run in the `libs/` project, as `shared.format_compile.*`. The `sosig` build does not register them.

### Tests in a parent build

A parent project that turns on the `sosig` tests must call `enable_testing()` in its top-level `CMakeLists.txt`. CTest starts to find tests at the root of the build directory, and a call to `enable_testing()` in a subproject cannot create the test file at that root. When a subproject turns on its tests, CMake prints this requirement.

## Releases

### Version

`project(VERSION)` in `CMakeLists.txt` holds the only copy of the numeric release version. CMake generates one source file from `src/core/sosig_version.c.in`, and this file defines `sosig_version_string()` and `sosig_generator_string()`. When the version changes, CMake compiles this file again and links the targets that use it again.

Release versions have the form `MAJOR.MINOR.PATCH`. `project(VERSION)` accepts only numeric components, so a prerelease suffix needs a second version value.

### Install

The installation contains the executable and its documentation files, `README.md`, `LICENSE.txt` and `THIRD_PARTY_NOTICES.md`. All of these files are in the `sosig_runtime` component. `GNUInstallDirs` selects the install paths. The project installs no library and no header, so it has no export set and no development component.

### Packages

CPack creates one stripped archive for each release target, `sosig-<version>-<target>.tar.gz`. The cache variable `APPS_RELEASE_TARGET` names the release target, for example `x86_64-linux-musl` in a Linux preset.

The macOS deployment target, `CMAKE_OSX_DEPLOYMENT_TARGET`, is `13.0` by default. You can set a higher value in the CMake cache.

The Linux `musl` presets cross-compile with the Zig toolchain files in the root `cmake/toolchains/` directory. These files select `llvm-strip`, because the strip tool of the host cannot read the cross-compiled ELF files.

The source archive, `sosig-<version>-source.tar.gz`, holds the whole repository except the directories of the other apps, because `sosig` builds with the root CMake modules, the libraries in `libs/` and the sources in `vendor/`. The archive keeps the default CPack exclusions for version-control directories and temporary files. It also leaves out the `build/` directories and `CMakeUserPresets.json`.
