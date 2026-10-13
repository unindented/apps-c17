# Build system

This document describes the CMake builds of the projects in this repository: their targets, how the targets link to each other, and the reasons for their settings. Every build needs CMake 3.25 or later. The Development section of each app's README lists the commands that compile, test and package that app: [sosig](apps/sosig/README.md#development), [fram](apps/fram/README.md#development) and [cwrap](apps/cwrap/README.md#development). [libs/README.md](libs/README.md#development) lists the commands of the `libs/` project.

The first sections describe what every project shares. The sections [sosig](#sosig), [fram](#fram) and [cwrap](#cwrap) describe what one app has of its own.

## Projects

The repository root is not a CMake project. Each of these directories is a CMake project of its own, and you configure a project from its directory:

| Directory | Project |
| --- | --- |
| [apps/sosig/](apps/sosig/) | The `sosig` static site generator |
| [apps/fram/](apps/fram/) | The `fram` gallery generator |
| [apps/cwrap/](apps/cwrap/) | The `cwrap` comment formatter |
| [libs/](libs/) | The libraries that the apps share, with their tests, analysis and lint |

Only one project is configured in each build tree, so the cache variables and the targets that the presets name have the same name in every project.

## Shared files

| Path | Responsibility |
| --- | --- |
| [cmake/apps_build_profiles.cmake](cmake/apps_build_profiles.cmake) | Sets warnings, optimization, fortification and sanitizers (see [Build profiles](#build-profiles)) |
| [cmake/apps_quality.cmake](cmake/apps_quality.cmake) | Configures static analysis, and defines `apps_add_lint_targets`, which creates the `format` and `lint` targets of a project |
| [cmake/apps_testing.cmake](cmake/apps_testing.cmake) | Defines `apps_add_unit_test`, which registers a unit test of an app and selects the host that the test links |
| [cmake/apps_packaging.cmake](cmake/apps_packaging.cmake) | Configures the installation and the CPack archives of an app |
| [cmake/apps_restyle_graphviz_svg.cmake](cmake/apps_restyle_graphviz_svg.cmake) | Adds light and dark styles to a target graph |
| [cmake/toolchains/](cmake/toolchains/) | Configures Zig to cross-compile for the Linux `musl` release targets |
| [cmake/presets.json](cmake/presets.json) | Defines the configure, build, test and workflow presets that every project has |
| [cmake/release-presets.json](cmake/release-presets.json) | Includes `presets.json`, and adds the `release-linux-*` presets and the package presets that every app has |
| [.clang-format](.clang-format) and [.clang-tidy](.clang-tidy) | Configure `clang-format` and `clang-tidy` for every project |
| [Containerfile](Containerfile) | Builds the Linux CI image for every project |
| [vendor/](vendor/) | Holds the sources of every vendored project, as upstream ships them, with the manifest and the script that check and update them (see [Vendored dependencies](#vendored-dependencies)) |

Each project adds the root `cmake/` directory to `CMAKE_MODULE_PATH`. Each module has `include_guard(GLOBAL)`, so a module that two projects of one build tree include runs one time.

## App layout

The top-level `CMakeLists.txt` of an app sets the project policy, includes the CMake modules of the root `cmake/` directory and of its own `cmake/` directory, includes its `vendor.cmake`, adds the `libs/` project, and adds each source directory. These are the other CMake files of an app:

| Path | Responsibility |
| --- | --- |
| `src/CMakeLists.txt` | Defines the application library `<app>_app` and the `<app>` executable |
| `src/*/CMakeLists.txt` | Add the sources, the header file sets and the unit tests of their directory |
| `tests/CMakeLists.txt` | Adds the golden tests and the app's own test helpers |
| `vendor.cmake` | Defines a target for each vendored dependency that the app uses, from the sources in the root [vendor/](vendor/) directory |
| `cmake/<app>_lint.cmake` | Lists the first-party sources that the `format` and `lint` targets check |
| `cmake/<app>_run_golden.cmake` | Runs a golden test |
| `CMakeGraphVizOptions.cmake` | Sets the filters and the layout of the target graph |
| `CMakePresets.json` | Includes [cmake/release-presets.json](cmake/release-presets.json), and adds the presets of the app's own names |

## Targets of an app

Each app has one application library, two hosts, one target for each vendored dependency, the libraries of `libs/` that it links, and the interface targets of [Build profiles](#build-profiles). Each executable links the application library and one of the two hosts. Each app's section has the graph of its targets and their links, which CMake generates from the `release` preset. A graph does not show the dependencies between source modules, and it does not show test-only targets such as the in-memory host and the `testing` library.

### Application library

`<app>_app` is a private static library that holds every application source except `src/app/main.c`. The executable and every unit test link it. It is static because it supports one product and has no public ABI.

Each source directory holds the code for one purpose. The source directories add their files to `<app>_app` and do not define libraries of their own, because more libraries would add link boundaries without independent APIs.

`<app>_app` links as `PUBLIC` dependencies the libraries whose types the application headers expose, and every other dependency as `PRIVATE`.

`core`, the library in [libs/src/core/](libs/src/core/), supplies the arena, the counted string, the output buffer and the diagnostic log. When an allocation does not fit, the arena calls `os_oom`, and each program defines its own `os_oom`. Each app's `src/app/main.c` defines the `os_oom` of that app, which the app's ARCHITECTURE.md describes. The `testing` library defines the `os_oom` of the unit tests.

### Hosts

Each executable selects its host when it links. The POSIX host is an object library outside `<app>_app`, and the executable links it. The unit tests that use the default, `OS posix`, also link it (see [Unit tests](#unit-tests)). The tests registered with `OS memory` link the in-memory host instead. sosig and fram link `os_posix` and `os_memory` from the `os` library. cwrap has hosts of its own, as [cwrap](#cwrap) describes.

Both hosts are object libraries, because `<app>_app` refers to them and a single-pass linker skips a static archive when the archive comes before the code that uses it on the link line.

### Vendored dependencies

The sources of each vendored project are in the root [vendor/](vendor/) directory, and each app's `vendor.cmake` compiles the sources that the app uses. An app that links `markup` includes [libs/src/markup/vendor.cmake](libs/src/markup/vendor.cmake) for the vendored projects of that library. Each vendored project has its own target, `vendor_<lib>`: the header-only projects, acutest and copt, are interface libraries, and the other vendored projects are static libraries. The targets have the same names in every app, because the libraries of `libs/` link the app's `vendor_acutest` and `vendor_tomlc17`.

[vendor/vendor.toml](vendor/vendor.toml) pins each vendored project: its upstream repository, its release when it has one, and its commit. It also lists the upstream files that the directory of the project holds, the patches that this repository applies to them, and the files that this repository writes, such as the configuration headers of libjpeg-turbo. The `THIRD_PARTY_NOTICES.md` of each app and of `libs/` records the pin of each project that it lists. Renovate reads the releases in the manifest and opens a pull request for each new release. [vendor/vendor.py](vendor/vendor.py), which needs Python 3.11 or later, uses the manifest:

- `vendor/vendor.py check` fetches each pinned commit and makes the copy that the manifest describes. It fails when a directory of `vendor/` differs from that copy, when a release no longer names its commit, or when a notices file records another pin. The Vendored sources job of CI runs this check.
- `vendor/vendor.py sync <project>...` replaces the directory of each project with the copy of the commit that its release names, or of its pinned commit when it has no release, and records the pin in the manifest and in every notices file. For a tree-sitter grammar, it prints the upstream changes to the query files whose patterns `markup` keeps in `libs/src/markup/queries/`.

To bump a project, change its `release`, or its `commit` when it has no release, and run `vendor/vendor.py sync` with its name.

### Regenerating a graph

> [!tip]
> Use these commands in the app's directory, such as `apps/sosig`, to update its graph:
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

## The libs project

[libs/CMakeLists.txt](libs/CMakeLists.txt) adds one directory of `libs/src/` for each library, and that directory defines the library's target, as [libs/ARCHITECTURE.md](libs/ARCHITECTURE.md) describes. An app adds the project with `add_subdirectory(../../libs ${CMAKE_BINARY_DIR}/libs EXCLUDE_FROM_ALL SYSTEM)`:

- `EXCLUDE_FROM_ALL` builds a library only when a target of the app links it.
- Each library links `apps_build_project` of the app, so it compiles with the app's warnings and sanitizer options.
- `SYSTEM` makes the include directories of the libraries system directories for the app, so the analysis of the app checks only the app's own code.
- The libraries use the vendored targets of the app. The project adds `formats` only when the target `vendor_tomlc17` exists, so cwrap, which vendors no tomlc17, builds without it. It adds `markup` only when `formats` and the targets of `libs/src/markup/vendor.cmake` exist, so fram and cwrap build without it.
- An app never builds the tests of the libraries.

Configured on its own, from `libs/`, the project defines the vendored targets that the libraries need in [libs/vendor.cmake](libs/vendor.cmake), and builds every library, its analysis, its lint and its tests. `libs_add_unit_test(<lib> <module>)` registers the test `<lib>.<module>`, which links the library, `apps_build_tests`, acutest and the libraries in `LIBRARIES`. With `OS posix` or `OS memory`, the test also links `os_posix` or `os_memory`, and with `OS memory` it compiles with `APPS_TEST_OS_MEMORY` set to 1. Each library test has the labels `<lib>` and `unit`, and the same timeouts as the tests of an app. Each library appends its sources to the global property `apps_libs_lint_sources`, and the project creates its `format` and `lint` targets from that list.

### Grammars

`markup` links the grammars that [libs/src/markup/vendor.cmake](libs/src/markup/vendor.cmake) builds. That file also builds md4c, org4c and the tree-sitter runtime. Each app that links `markup` includes it from its own `vendor.cmake`, and the `libs/` project includes it from `libs/vendor.cmake`.

`APPS_GRAMMARS` selects the tree-sitter grammars that a binary links. The default is an empty value, which links every grammar, as in the release archives. A list links only the grammars that it names, and each name in the list must be the name of one `markup_add_tree_sitter_grammar` call in `libs/src/markup/vendor.cmake`. These names are `c`, `bash`, `html`, `css`, `javascript`, `typescript`, `tsx` and `json`. If the binary does not link the grammar for the language of a code block, `markup` only escapes the block, as it does for a language that has no grammar.

```sh
cmake --preset release -B build/release-small -DAPPS_GRAMMARS="c;bash"
cmake --build build/release-small -j 8
```

TypeScript and TSX are separate grammars. You can select either of them without `javascript`. Their queries include the query files of the JavaScript grammar.

The configure step stops with an error when a name matches no grammar. It also stops with an error when `APPS_GRAMMARS` is not empty and `APPS_BUILD_TESTING` is on, because the tests need every grammar.

Each selected grammar is a static library, `vendor_tree_sitter_<name>`. This library holds the highlight queries of the grammar as an embedded byte array. The parsers come from the root `vendor/` directory. The queries come from [libs/src/markup/queries/](libs/src/markup/queries/), where `markup` keeps its own copy of the upstream patterns. A grammar bump changes a query only when you port the changes of the upstream query files, as [libs/THIRD_PARTY_NOTICES.md](libs/THIRD_PARTY_NOTICES.md) describes. `vendor_tree_sitter_grammars` is an interface library. It links the selected grammars and carries the generated header `tree_sitter_grammars.h`, which declares the language, the queries and the names of each selected grammar. [libs/src/markup/highlight.c](libs/src/markup/highlight.c) defines its table of grammars from this header.

With `address`, the CMake build also turns off the `function` check for the tree-sitter runtime, because the runtime calls the `create` function of each grammar scanner, and this function has no prototype.

### Format checks in core

`core` has its own compile-fail tests for `buf_printf` and `log_error`. These tests check that a call does not compile when its arguments do not match its format. The test cases are:

- a raw `struct Str` for `%.*s`
- a missing argument
- a `ptrdiff_t` for `%d`
- a format that is not a literal
- a value that is not a string, given to `STR_FMT`
- a mismatch through a forwarding wrapper

These tests run in the `libs/` project, as `core.format_compile.*`, and no app registers them.

## Build profiles

[cmake/apps_build_profiles.cmake](cmake/apps_build_profiles.cmake) defines three interface targets, which carry the compile and link options:

- `apps_build_project` sets the strict warnings for first-party code and the sanitizer options.
- `apps_build_tests` sets the common warning group and the sanitizer options.
- `apps_build_vendor` sets optimization and the sanitizer options that vendored code supports.

All three targets require C17 or later. A parent project can select a newer standard, but not an older one.

Generator expressions select the options separately for each configuration, so the same rules work with single-config generators and with multi-config generators.

The presets set `CMAKE_COMPILE_WARNING_AS_ERROR`, so the compiler treats warnings as errors. The CMake build does not force this setting on a parent project.

### Sanitizers

`APPS_SANITIZER` accepts `none`, `address` or `thread`. `address` enables AddressSanitizer and UndefinedBehaviorSanitizer, and `thread` enables ThreadSanitizer. The option adds flags to the standard CMake configurations and does not create custom build types.

With `address`, the CMake build instruments first-party code, the libraries and vendored code. The libraries build with the first-party profile, so they keep every check, and the arena poisoning of `core` runs in an app's ASan build. CI checks that the objects of `core` in each project's ASan build carry AddressSanitizer symbols. For vendored code, the build turns off the `null` and `object-size` checks, because vendored code does not support these checks. First-party code keeps every check.

## Linting and formatting

When a project is the top-level CMake project, the `C_CLANG_TIDY` and `C_CPPCHECK` properties of its first-party targets make CMake run `clang-tidy` and `cppcheck` while it compiles these targets. Neither tool checks vendored code, and an app's analysis does not check the libraries, which the `libs/` project checks. The `HeaderFilterRegex` of the root `.clang-tidy` covers the headers under `src/` and `libs/`. In an app, the include path of `cppcheck` contains the include roots of the libraries that `APPS_LIBRARIES` in the app's `CMakeLists.txt` lists, because `cppcheck` needs these headers to expand macros such as `STR_FMT` of `core`. Thus `cppcheck` also analyzes the inline functions that these headers define.

Test targets run `cppcheck` but not `clang-tidy`, because their intentional failure cases cause analyzer errors. When CMake cross-compiles, neither tool runs. Zig supplies the headers for the target platform, and the analyzers on the host cannot find these headers when they repeat the compile command. When the compiler is not Clang, the CMake build turns off `clang-tidy`, because `clang-tidy` reads the compile command and rejects GCC-only warning options such as `-Wlogical-op`. `cppcheck` runs with any compiler.

The `format` and `lint` targets run `clang-format` and then `cwrap`, which refills comment prose to 100 columns, the `.clang-format` column limit. Either tool is skipped when it is not installed. CI and the Linux image install a pinned `cwrap` release. It is the installed `cwrap`, not the one that `apps/cwrap` builds, so a change to how it rewraps reaches the sources only through a new release and the pin in CI and the Linux image.

Both targets run both tools on the first-party files in an explicit list: the `SOURCES` of `apps_add_lint_targets` in an app's `cmake/<app>_lint.cmake`, or the `apps_libs_lint_sources` that each library appends to. In an app, they also run both tools on the version template, which `clang-format` reads as the `.c` file that it configures because of `--assume-filename`. When you add a source file or a test file, add it to this list and to its target. A glob at configure time would not find a new file until CMake configures the project again.

## Tests

`APPS_BUILD_TESTING` controls whether CMake defines the tests of an app. It is on by default when the app is the top-level CMake project, and off by default when a parent project adds the app as a subproject.

Each source directory adds its own unit tests, and `tests/` adds the golden tests. The tests of the libraries run in the `libs/` project, and no app registers them. Every CTest case of an app has the label of the app, and also the label `unit`, `golden` or `idempotent`.

### Unit tests

`apps_add_unit_test(<module>)` compiles `test_<module>.c` into the executable `<app>_unit_<module>`, and registers this executable as the CTest case `<app>.<module>`. The executable links `apps_build_tests`, acutest, the `testing` library, `<app>_test_support` when the app defines it, `<app>_app` and one host. The host is the app's own `<app>_os_<OS>` when the app defines one, and the library's `os_<OS>` otherwise.

With the `OS memory` option, `apps_add_unit_test` links the in-memory host and compiles the test with `<APP>_TEST_OS_MEMORY` set to 1. New tests use this option. The tests registered without `OS memory` link the POSIX host. Each app's section lists these tests.

When one source file compiles into more than one executable, the `NAME` option sets the name of each executable and its test. The contract tests of `os/os.h`, `os.contract_posix` and `os.contract_memory`, run in the `libs/` project, and both must pass.

Each unit test has a CTest timeout of 60 seconds, so a test that hangs fails and does not block the test run. A ThreadSanitizer build waits 300 seconds.

No test target compiles an application source file a second time with different definitions.

### Test-support library

The `testing` library, [libs/src/testing/test_support.h](libs/src/testing/test_support.h), holds the helpers that several `test_*.c` files need: fixture directories, files and trees, the working directory, environment variables, the capture of the standard streams, an arena or a log over a fresh reservation, checks that compare the text of a log or a text exactly, the out-of-memory check `test_is_oom`, a seeded random generator, and a printable copy of any bytes for a failure message. It also defines the `os_oom` of the unit tests. [libs/ARCHITECTURE.md](libs/ARCHITECTURE.md#testing) describes it.

### Tests in a parent build

A parent project that turns on the tests of an app must call `enable_testing()` in its top-level `CMakeLists.txt`. CTest starts to find tests at the root of the build directory, and a call to `enable_testing()` in a subproject cannot create the test file at that root. When a subproject turns on its tests, CMake prints this requirement.

## Releases

### Version

`project(VERSION)` in an app's `CMakeLists.txt` holds the only copy of its numeric release version. CMake generates one source file from the app's version template, and this file defines `<app>_version_string()`. When the version changes, CMake compiles this file again and links the targets that use it again.

Release versions have the form `MAJOR.MINOR.PATCH`. `project(VERSION)` accepts only numeric components, so a prerelease suffix needs a second version value. A release tag is `<app>-vMAJOR.MINOR.PATCH`, and it starts the root [release.yml](.github/workflows/release.yml) for that app.

### Install

The installation of an app contains the executable and its documentation files, `README.md`, `LICENSE.txt` and `THIRD_PARTY_NOTICES.md`. All of these files are in the `<app>_runtime` component. `GNUInstallDirs` selects the install paths. An app installs no library and no header, so it has no export set and no development component.

### Packages

CPack creates one stripped archive for each release target, `<app>-<version>-<target>.tar.gz`. The cache variable `APPS_RELEASE_TARGET` names the release target, for example `x86_64-linux-musl` in a Linux preset.

The macOS deployment target, `CMAKE_OSX_DEPLOYMENT_TARGET`, is `13.0` by default. You can set a higher value in the CMake cache.

The Linux `musl` presets cross-compile with the Zig toolchain files in the root `cmake/toolchains/` directory. These files select `llvm-strip`, because the strip tool of the host cannot read the cross-compiled ELF files.

The source archive, `<app>-<version>-source.tar.gz`, holds the whole repository except the directories of the other apps, because an app builds with the root CMake modules, the libraries in `libs/` and the sources in `vendor/`. The archive keeps the default CPack exclusions for version-control directories and temporary files. It also leaves out the `build/` directories and `CMakeUserPresets.json`.

## sosig

![CMake target dependency graph of sosig](apps/sosig/media/dependencies.svg)

### sosig files and targets

- [apps/sosig/CMakeLists.txt](apps/sosig/CMakeLists.txt) also finds the thread library.
- [apps/sosig/vendor.cmake](apps/sosig/vendor.cmake) also includes [libs/src/markup/vendor.cmake](libs/src/markup/vendor.cmake), as [Grammars](#grammars) describes.
- [apps/sosig/CMakePresets.json](apps/sosig/CMakePresets.json) adds the `grammars` presets, which build `sosig` with only the TypeScript grammar.
- `sosig` links the libraries `core`, `os` with the POSIX host `os_posix`, `runtime`, `formats`, `markup` and `output`, and for the unit tests `testing` and the in-memory host `os_memory`.
- `sosig_app` links `vendor_tomlc17`, `core`, `formats`, `markup`, `os`, `output` and `runtime` as `PUBLIC` dependencies. `Threads::Threads` is one of its `PRIVATE` dependencies, but CMake still adds it to each executable that links the static library.
- `src/build/` links the vendored dependencies that only its own code uses, and `src/CMakeLists.txt` links the other vendored dependencies.
- The version template is [apps/sosig/src/domain/sosig_version.c.in](apps/sosig/src/domain/sosig_version.c.in). The file that CMake generates from it also defines `sosig_generator_string()`.

### sosig tests

The golden tests highlight code blocks, so they need every tree-sitter grammar, as [Grammars](#grammars) describes. The tests registered without `OS memory` link the POSIX host. These tests are:

- the test that needs a real filesystem: `output_pruner_case`, which tests the prune on a case-insensitive `/tmp` and skips on a case-sensitive `/tmp`
- the suites that are not yet converted to the in-memory host, which create their fixtures in a real temporary directory: `cli`, `cli_dispatch`, `cmd_config`, `entry_renderer`, `site_config` and `template`
- the tests that use no host

The production render limit is 256 MiB, and a failure test at that limit uses too much memory. Thus the template test passes a 64 KiB limit to `template_render_file_limited`, and does not compile the template code again with a smaller limit.

`sosig_fixture_sites` in [apps/sosig/tests/CMakeLists.txt](apps/sosig/tests/CMakeLists.txt) lists the fixture sites in `tests/fixtures/`. Each fixture site is the golden test `sosig.golden.<site>`. [apps/sosig/cmake/sosig_run_golden.cmake](apps/sosig/cmake/sosig_run_golden.cmake) does these steps:

1. It copies the site into a scratch tree of its own.
2. It puts stale files in the output. The first build must remove these files.
3. It builds the site three times. The second build reuses the build cache, and the third build passes `--force`. These two builds must not change a generated file.
4. It compares the list of output files with `tests/expected/<site>`, and it compares every file byte for byte.

The comparison leaves out the build cache, `_sosig/v1-build-cache.tsv`, which holds the modification times of the copied sources and has no expected copy.

The script finds the expected files and the actual files with globs after `sosig` runs. These globs run in the test and are not inputs of the CMake build, so a change to a fixture does not require CMake to configure the project again.

## fram

![CMake target dependency graph of fram](apps/fram/media/dependencies.svg)

### fram files and targets

- [apps/fram/CMakeLists.txt](apps/fram/CMakeLists.txt) also finds the thread library.
- `fram` links the libraries `core`, `os` with the POSIX host `os_posix`, `runtime`, `formats` and `output`, and for the unit tests `testing` and the in-memory host `os_memory`.
- `fram_app` links `vendor_tomlc17`, `core`, `formats`, `os`, `output` and `runtime` as `PUBLIC` dependencies. `Threads::Threads` is one of its `PRIVATE` dependencies, but CMake still adds it to each executable that links the static library.
- `src/build/` and `src/media/` link the vendored dependencies that only their own code uses, and `src/CMakeLists.txt` links the other vendored dependencies.
- The version template is [apps/fram/src/domain/fram_version.c.in](apps/fram/src/domain/fram_version.c.in). The file that CMake generates from it also defines `fram_generator_string()`.

### fram vendored dependencies

The pinned stb implementation has its own translation unit, [apps/fram/src/media/stb.c](apps/fram/src/media/stb.c), which the target `vendor_stb` compiles. This file compiles the resizer with its allocator hooks, and it keeps the warnings of vendored code out of the compilation of first-party code.

The libjpeg-turbo target compiles the source files that the build system of libjpeg-turbo compiles. It uses the configuration headers in the root `vendor/libjpeg-turbo/config/` directory in place of the headers that this build system generates. On x86-64, the SIMD routines of libjpeg-turbo compile from NASM sources, so an x86-64 build needs NASM. On AArch64, these routines compile from Neon intrinsics, and other architectures use the C routines. The NASM sources are the object library `vendor_libjpeg_turbo_simd`, because the options of the vendored-code profile are for the C compiler, and NASM rejects them.

The libwebp target compiles the encoder, the decoder, the DSP sources and utility sources that they use, and libsharpyuv. It does not use the configuration header that the build system of libwebp generates. Only the tests decode WebP, so `fram` links no part of the decoder. The encoder selects its SIMD routines at run time. On x86-64, the CMake build compiles the SSE4.1 and AVX2 sources with their instruction sets enabled, and tells every other source that these routines exist. `WEBP_USE_THREAD` makes the one-time DSP setup of the encoder safe when several workers encode at the same time. libwebp also calls the math library, which glibc supplies separately from the C library. Thus the CMake build links `libm` only when a call to a math function does not link without it.

### fram tests

[apps/fram/tests/CMakeLists.txt](apps/fram/tests/CMakeLists.txt) adds the golden test, the test helpers of `fram` and the seeded random tests in `tests/test_random.c`. `fram_test_support` holds the test helpers of `fram`, which every unit test of `fram` links:

- a fixture JPEG encoder over libjpeg-turbo, in `tests/test_jpeg_fixture.h`, which writes baseline, progressive, CMYK and YCCK JPEGs, and a lossless JPEG that the probe rejects
- a WebP decoder for derivative outputs, in `tests/test_webp_output.h`

The tests registered without `OS memory` link the POSIX host. These tests are:

- the tests that need the real host: `output_pruner_case`, which tests the prune on a case-insensitive `/tmp` and skips on a case-sensitive `/tmp`; and `proc`, `video`, `album_scanner_video`, `cmd_build_video` and `derivative_renderer_video`, which run processes or decode video
- the suites that are not yet converted to the in-memory host, which create their fixtures in a real temporary directory: `cli`, `cli_dispatch`, `cmd_config`, `gallery_config` and `template`
- the tests that use no host

The production render limit is 256 MiB, and a failure test at that limit uses too much memory. Thus the template test passes a 64 KiB limit to `template_render_file_limited`, and does not compile the template code again with a smaller limit. The derivative renderer exports `derivative_renderer_decode_scale`, which returns the scale that its jobs select, so its test checks this selection directly and not through the decoded output.

The golden test `fram.golden` builds the fixture gallery in `tests/fixtures/gallery`, which mixes JPEG and MP4 sources in the default `media/` directory. `tests/CMakeLists.txt` defines this test only when the configure step finds both `ffmpeg` and `ffprobe`. [apps/fram/cmake/fram_run_golden.cmake](apps/fram/cmake/fram_run_golden.cmake) does these steps:

1. It copies the gallery into a scratch tree of its own.
2. It puts stale files in the output. The first build must remove these files.
3. It builds the gallery three times. The second build reuses the build cache, and the third build passes `--force`. These two builds must not change a generated file.
4. It compares the list of output files with `tests/expected/gallery`.

The comparison leaves out the build cache, `_fram/v1-build-cache.tsv`, which holds the modification times of the copied sources and has no expected copy.

The script compares the non-media files and the copied originals byte for byte, after it replaces the `@PROJECT_VERSION@` placeholder in each expected page with the project version. For each generated derivative, it checks only that the file is not empty, because the WebP output can change with the libwebp version and the SIMD path. Unit tests check the dimensions and the pixels of derivatives.

The script finds the expected files and the actual files with globs after `fram` runs. These globs run in the test and are not inputs of the CMake build, so a change to a fixture does not require CMake to configure the project again.

## cwrap

![CMake target dependency graph of cwrap](apps/cwrap/media/dependencies.svg)

### cwrap files and targets

- [apps/cwrap/tests/CMakeLists.txt](apps/cwrap/tests/CMakeLists.txt) adds the golden and idempotence tests and the seeded random tests. [apps/cwrap/cmake/cwrap_run_idempotent.cmake](apps/cwrap/cmake/cwrap_run_idempotent.cmake) runs the idempotence test.
- `cwrap` links the libraries `core` and `os` with the POSIX host `os_posix`, and for the unit tests `testing` and the in-memory host `os_memory`. It links no `runtime`, so its binary carries no worker pool and no thread library.
- `cwrap_app` holds every application source except `src/app/main.c`, and links `core` and `os` as `PUBLIC` dependencies. `src/CMakeLists.txt` links the vendored dependencies.
- The two vendored projects of `cwrap`, acutest and copt, are header-only, so their targets are interface libraries. `cwrap` has no compiled vendored code, so it uses two of the three build profiles: `apps_build_project` and `apps_build_tests`. With `address`, the CMake build instruments first-party code and the `core` library.
- The common presets give `cwrap` the `tsan` and `ci-tsan` presets too. CI does not run a ThreadSanitizer build of `cwrap`, because `cwrap` starts no threads.
- The version template is [apps/cwrap/src/domain/cwrap_version.c.in](apps/cwrap/src/domain/cwrap_version.c.in).

### cwrap tests

The tests registered without `OS memory` link the POSIX host. These tests are:

- the suites that are not yet converted to the in-memory host, which create their fixtures in a real temporary directory: `cli` and `cli_dispatch`
- the tests that use no host

The contract tests of the hosts that `cwrap` links are `os.contract_posix` and `os.contract_memory`, in the `libs/` project.

The golden test `cwrap.golden` runs [apps/cwrap/cmake/cwrap_run_golden.cmake](apps/cwrap/cmake/cwrap_run_golden.cmake). The script rewraps every fixture in `tests/fixtures/` at width 40, in a scratch tree of its own. It then compares the list of fixture files with the list of files in `tests/expected/`, and it compares each result with its expected file byte for byte.

The idempotence test `cwrap.idempotent` runs `cmake/cwrap_run_idempotent.cmake` in another scratch tree. The script rewraps the fixture files, the expected files and every first-party source file in `src/` and `tests/`, and the sources of the libraries that `APPS_LIBRARIES` in [apps/cwrap/CMakeLists.txt](apps/cwrap/CMakeLists.txt) lists, `libs/src/core`, `libs/src/os` and `libs/src/testing`, at widths 20, 40, 60 and 100. A second rewrap of each result must make no change, and `--check` must agree that the first result is stable.

Both scripts find their input files with globs. These globs run in the test and are not inputs of the CMake build, so a change to a fixture or a source file does not require CMake to configure the project again.
