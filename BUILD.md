# Build system

This document describes the CMake builds of the projects in this repository: how to build, test and package each project, the targets and their links, and the reasons for their settings. Every build needs CMake 3.25 or later. [Development](#development) gives the commands. The sections [sosig](#sosig), [fram](#fram), [cwrap](#cwrap) and [surge](#surge) give only what differs for one app.

## Projects

The repository root is not a CMake project. Each of these directories is a CMake project of its own, and you configure a project from its directory:

| Directory | Project |
| --- | --- |
| [apps/sosig/](apps/sosig/) | The `sosig` static site generator |
| [apps/fram/](apps/fram/) | The `fram` gallery generator |
| [apps/cwrap/](apps/cwrap/) | The `cwrap` comment formatter |
| [apps/surge/](apps/surge/) | The `surge` static site generator for git repositories |
| [libs/](libs/) | The libraries that the apps share, with their tests, analysis and lint |

Only one project is configured in each build tree, so the cache variables and the targets that the presets name have the same name in every project.

## Development

### Prerequisites

- A POSIX-like system, CMake 3.25 or later, and a C17 compiler such as Clang or GCC. sosig, fram, surge and `libs/` also need pthreads.
- Optional: `ninja` for the `multi` preset, and `cwrap`, `clang-format`, `clang-tidy` and `cppcheck` for analysis and lint.
- Zig 0.17 and `llvm-strip`, for the Linux `musl` archives.

The repository holds the vendored dependencies in [vendor/](vendor/) and the libraries in [libs/](libs/). fram and surge need more tools, as their sections list.

### Building

Run the commands from the directory of the project, such as `apps/sosig`. Each preset builds in `build/<preset>` and treats compiler warnings as errors. If `CC` is set, the preset builds in `build/<preset>/<CC>`, such as `build/debug/gcc-14`. CMake reads `CC` only when it configures a directory for the first time, so each compiler needs its own build tree. Change `-j 8` to suit your machine.

```sh
cmake --preset debug
cmake --build --preset debug -j 8
```

| Preset | Configuration | App binary |
| --- | --- | --- |
| `debug` | `Debug` with AddressSanitizer and UndefinedBehaviorSanitizer. It runs `clang-tidy` and `cppcheck` during compilation if they are installed. | `build/debug/bin/<app>` |
| `tsan` | `Debug` with ThreadSanitizer. | `build/tsan/bin/<app>` |
| `release` | `RelWithDebInfo`, with no tests. The binary in the build tree keeps its debug information, and packaging strips the installed copy. | `build/release/bin/<app>` |
| `multi` | `Debug` and `RelWithDebInfo` in one build tree, with the `Ninja Multi-Config` generator. Build with `--preset multi-debug` or `--preset multi-relwithdebinfo`. | `build/multi/bin/<config>/<app>` |

In `libs/`, a preset builds each library as `src/<library>/lib<library>.a` in its build tree, and `multi` as `src/<library>/<config>/lib<library>.a`.

To run the lint, configure the `debug` preset, and then run `cmake --build --preset lint`. `clang-tidy` runs only with Clang, as [Linting and formatting](#linting-and-formatting) explains.

### Testing

Build a preset, and then run its tests:

```sh
ctest --preset debug -j 8
ctest --preset debug -L unit -j 8
ctest --preset debug -R template -j 8
```

The test presets are `debug`, `tsan`, `multi-debug` and `multi-relwithdebinfo`. The `multi-relwithdebinfo` tests check the configuration of the `release` preset, which builds no tests. `-L` selects tests by label, and `-R` by a pattern of their names. Each test has the label of its app, or of its library in `libs/`, and also `unit`, `golden` or `idempotent`. [Tests](#tests) describes the tests.

### Continuous integration

The workflow presets run the same configure, build and test steps as CI:

| Workflow | Steps |
| --- | --- |
| `cmake --workflow --preset ci-debug` | Build in `Debug`, run the lint, and run all tests with ASan and UBSan. |
| `cmake --workflow --preset ci-tsan` | Build with TSan and run all tests. |
| `cmake --workflow --preset ci-release` | Build in `RelWithDebInfo`. |
| `cmake --workflow --preset ci-multi` | Build and test in `Debug` and `RelWithDebInfo` with the `Ninja Multi-Config` generator. |

The CI workflow, [ci.yml](.github/workflows/ci.yml), checks each project that a change can affect, on Linux and on macOS, through [checks.yml](.github/workflows/checks.yml). It runs `ci-multi` again on Linux with `CC=gcc-14`. It also checks that the objects of `core` in the ASan build carry AddressSanitizer symbols. Its `Summary` job reports the result of every job. The workflows refer to actions by tag, and [actions.lock](.github/workflows/actions.lock) records the commit of each tag. After you change a `uses:` line, run `gh actions-lock --no-narrow`, from the [gh-actions-lock](https://github.com/github/gh-actions-lock) extension. The `Lockfile` job fails when the lockfile does not match the workflows.

To run a Linux workflow with Podman, build the image from the root [Containerfile](Containerfile) in the repository root. Then run each workflow in a new container, with the project's directory as the working directory:

```sh
podman build --tag apps-c17-linux-ci --file Containerfile .
podman run --rm --workdir /src/apps/sosig apps-c17-linux-ci ci-debug
podman run --rm --workdir /src/apps/sosig -e CC=gcc-14 apps-c17-linux-ci ci-multi
```

The image holds a copy of the repository without the directories that [.containerignore](.containerignore) excludes, such as `.git/` and `build/`, so the Linux build products stay in the container. It supports x86-64 and AArch64 hosts. It pins LLVM 23, GCC 14 and a `cwrap` release, and installs NASM, FFmpeg, `git` and mise for the tests of fram and surge. It does not include Zig, because only the release archives need Zig.

### Packaging

Only the release configurations have package presets. Build a preset, and then package it:

```sh
cmake --preset release
cmake --build --preset release -j 8
cpack --preset release
```

| Preset | Archive |
| --- | --- |
| `release` | A native archive in `build/release/`. |
| `release-linux-x86_64` | `build/release-linux-x86_64/<app>-<version>-x86_64-linux-musl.tar.gz`, cross-built with Zig. |
| `release-linux-aarch64` | `build/release-linux-aarch64/<app>-<version>-aarch64-linux-musl.tar.gz`, cross-built with Zig. |

A source archive needs a configured tree but no compiled binary:

```sh
cmake --preset release
cpack --config build/release/CPackSourceConfig.cmake
```

The source archive is `build/release/<app>-<version>-source.tar.gz`. [Releases](#releases) describes the archives and the release workflow.

## Shared files

| Path | Responsibility |
| --- | --- |
| [cmake/apps_build_profiles.cmake](cmake/apps_build_profiles.cmake) | Sets warnings, optimization, fortification and sanitizers (see [Build profiles](#build-profiles)) |
| [cmake/apps_quality.cmake](cmake/apps_quality.cmake) | Configures static analysis, and defines `apps_add_lint_targets`, which creates the `format` and `lint` targets of a project |
| [cmake/apps_testing.cmake](cmake/apps_testing.cmake) | Defines `apps_add_unit_test`, which registers a unit test of an app and selects the host that the test links |
| [cmake/apps_golden.cmake](cmake/apps_golden.cmake) | Defines the steps that the golden tests of sosig, fram and surge share |
| [cmake/apps_packaging.cmake](cmake/apps_packaging.cmake) | Configures the installation and the CPack archives of an app |
| [cmake/apps_restyle_graphviz_svg.cmake](cmake/apps_restyle_graphviz_svg.cmake) | Adds light and dark styles to a target graph |
| [cmake/toolchains/](cmake/toolchains/) | Configures Zig to cross-compile for the Linux `musl` release targets |
| [cmake/presets.json](cmake/presets.json) | Defines the configure, build, test and workflow presets that every project has |
| [cmake/release-presets.json](cmake/release-presets.json) | Includes `presets.json`, and adds the `release-linux-*` presets and the package presets of an app |
| [.clang-format](.clang-format) and [.clang-tidy](.clang-tidy) | Configure `clang-format` and `clang-tidy` for every project |
| [Containerfile](Containerfile) | Builds the Linux CI image for every project |
| [vendor/](vendor/) | Holds the sources of every vendored project, with the manifest and the script that check and update them (see [Vendored dependencies](#vendored-dependencies)) |

Each project adds the root `cmake/` directory to `CMAKE_MODULE_PATH`. Each module has `include_guard(GLOBAL)`, so a module runs one time in a build tree.

## App layout

The top-level `CMakeLists.txt` of an app sets the project policy, includes the CMake modules of the root `cmake/` directory and of its own `cmake/` directory, includes its `vendor.cmake`, adds the `libs/` project, and adds each source directory. sosig, fram and surge also find the thread library. These are the other CMake files of an app:

| Path | Responsibility |
| --- | --- |
| `src/CMakeLists.txt` | Defines the application library `<app>_app` and the `<app>` executable |
| `src/*/CMakeLists.txt` | Add the sources, the header file sets and the unit tests of their directory |
| `tests/CMakeLists.txt` | Adds the golden tests and the app's own test helpers |
| `vendor.cmake` | Defines a target for each vendored dependency that the app uses, from the sources in the root [vendor/](vendor/) directory |
| `cmake/<app>_run_golden.cmake` | Runs a golden test |
| `CMakeGraphVizOptions.cmake` | Sets the filters and the layout of the target graph |
| `CMakePresets.json` | Includes [cmake/release-presets.json](cmake/release-presets.json) |

## Targets of an app

Each app has one application library, two hosts, one target for each vendored dependency, the libraries of `libs/` that it links, and the interface targets of [Build profiles](#build-profiles). Each executable links the application library and one of the two hosts. Each app's section has the graph of its targets, which CMake generates from the `release` preset. A graph does not show test-only targets such as the in-memory host and the `testing` library.

### Application library

`<app>_app` is a private static library that holds every application source except `src/app/main.c`. The executable and every unit test link it. It is static because it supports one product and has no public ABI. The source directories add their files to `<app>_app` and define no libraries of their own, because more libraries would add link boundaries without independent APIs.

`<app>_app` links as `PUBLIC` dependencies the libraries whose types the application headers expose, and every other dependency as `PRIVATE`. In sosig, fram and surge, `Threads::Threads` is `PRIVATE`, but CMake still adds it to each executable that links the static library. `src/CMakeLists.txt` links `vendor_tomlc17`, the one vendored dependency that the code of these apps uses directly. The libraries link the other vendored dependencies that they use, such as copt in `cli` and mustache4c in `templates`.

The arena of `core` calls `os_oom` when an allocation does not fit, and each program defines `os_oom`, as [libs/ARCHITECTURE.md](libs/ARCHITECTURE.md#os_oom) describes. An app defines it in `src/app/main.c`, and the `testing` library defines it for the unit tests.

The version template is `src/domain/<app>_version.c.in`. The file that CMake generates from it defines `<app>_version_string()`, and in sosig, fram and surge also `<app>_generator_string()`.

### Hosts

Each executable selects its host when it links. Every app links the POSIX host `os_posix` and, for its unit tests, the in-memory host `os_memory` from the `os` library. The executable and the unit tests registered without `OS memory` link `os_posix`, and the tests registered with `OS memory` link `os_memory` (see [Unit tests](#unit-tests)). Both hosts are object libraries, because `<app>_app` refers to them, and a single-pass linker skips a static archive that comes before the code that uses it on the link line.

### Vendored dependencies

The sources of each vendored project are in the root [vendor/](vendor/) directory, and each app's `vendor.cmake` compiles the sources that the app uses. An app that links `markup` includes [libs/src/markup/vendor.cmake](libs/src/markup/vendor.cmake) for the vendored projects of that library. Each vendored project has its own target, `vendor_<lib>`. The header-only projects, acutest and copt, are interface libraries, and the others are static libraries. The targets have the same names in every app, because the libraries of `libs/` link the app's `vendor_acutest`, `vendor_mustache` and `vendor_tomlc17`.

[vendor/vendor.toml](vendor/vendor.toml) pins each vendored project: its upstream repository, its release when it has one, and its commit. It also lists the upstream files that the project's directory holds, the patches that this repository applies, and the files that this repository writes, such as the configuration headers of libjpeg-turbo. The `THIRD_PARTY_NOTICES.md` of each app and of `libs/` records the pin of each project that it lists. Renovate reads the releases in the manifest and opens a pull request for each new release. [vendor/vendor.py](vendor/vendor.py) needs Python 3.11 or later:

- `vendor/vendor.py check` fetches each pinned commit and makes the copy that the manifest describes. It fails when a directory of `vendor/` differs from that copy, when a release no longer names its commit, or when a notices file records another pin. The `Vendored sources` job of CI runs this check.
- `vendor/vendor.py sync <project>...` replaces the directory of each project with the copy of its release, or of its pinned commit when it has no release, and records the pin in the manifest and in every notices file. For a tree-sitter grammar, it prints the upstream changes to the query files whose patterns `markup` keeps in `libs/src/markup/queries/`.

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

[libs/CMakeLists.txt](libs/CMakeLists.txt) adds one directory of `libs/src/` for each library, as [libs/ARCHITECTURE.md](libs/ARCHITECTURE.md) describes. An app adds the project with `add_subdirectory(../../libs ${CMAKE_BINARY_DIR}/libs EXCLUDE_FROM_ALL SYSTEM)`:

- `EXCLUDE_FROM_ALL` builds a library only when a target of the app links it.
- Each library links `apps_build_project` of the app, so it compiles with the app's warnings and sanitizer options.
- `SYSTEM` makes the include directories of the libraries system directories for the app, so the analysis of the app checks only the app's own code.
- The libraries use the vendored targets of the app. The project adds `cli` only when `vendor_copt` exists, `formats` only when `vendor_tomlc17` exists, `markup` only when `formats` and the targets of `libs/src/markup/vendor.cmake` exist, and `templates` only when `formats` and `vendor_mustache` exist. cwrap therefore builds without `formats`, `markup` and `templates`, and fram without `markup`.
- An app never builds the tests of the libraries.

Configured on its own, from `libs/`, the project defines the vendored targets that the libraries need in [libs/vendor.cmake](libs/vendor.cmake), and builds every library, its analysis, its lint and its tests. `libs_add_unit_test(<lib> <module>)` registers the test `<lib>.<module>`, which links the library, `apps_build_tests`, acutest and the libraries in `LIBRARIES`. With `OS posix` or `OS memory`, the test also links `os_posix` or `os_memory`, and with `OS memory` it compiles with `APPS_TEST_OS_MEMORY` set to 1. Each library test has the labels `<lib>` and `unit`, and the same timeouts as the tests of an app.

### Grammars

`markup` links the grammars that [libs/src/markup/vendor.cmake](libs/src/markup/vendor.cmake) builds. That file also builds md4c, org4c and the tree-sitter runtime. Each app that links `markup` includes it from its own `vendor.cmake`, and the `libs/` project includes it from `libs/vendor.cmake`.

`APPS_GRAMMARS` selects the tree-sitter grammars that a binary links. The default is an empty value, which links every grammar, as in the release archives. A list links only the grammars that it names, and each name must be the name of one `markup_add_tree_sitter_grammar` call in `libs/src/markup/vendor.cmake`. These names are `c`, `bash`, `html`, `css`, `javascript`, `typescript`, `tsx` and `json`. If the binary does not link the grammar for the language of a code block, `markup` only escapes the block, as it does for a language that has no grammar.

```sh
cmake --preset release -B build/release-small -DAPPS_GRAMMARS="c;bash"
cmake --build build/release-small -j 8
```

TypeScript and TSX are separate grammars. You can select either of them without `javascript`. Their queries include the query files of the JavaScript grammar.

The configure step stops with an error when a name matches no grammar. It also stops with an error when `APPS_GRAMMARS` is not empty and `APPS_BUILD_TESTING` is on, because the tests need every grammar.

Each selected grammar is a static library, `vendor_tree_sitter_<name>`, which holds the highlight queries of the grammar as an embedded byte array. The parsers come from the root `vendor/` directory. The queries come from [libs/src/markup/queries/](libs/src/markup/queries/), where `markup` keeps its own copy of the upstream patterns. A grammar bump changes a query only when you port the changes of the upstream query files, as [libs/THIRD_PARTY_NOTICES.md](libs/THIRD_PARTY_NOTICES.md) describes. `vendor_tree_sitter_grammars` is an interface library. It links the selected grammars and carries the generated header `tree_sitter_grammars.h`, which declares the language, the queries and the names of each selected grammar. [libs/src/markup/highlight.c](libs/src/markup/highlight.c) defines its table of grammars from this header.

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

All three targets require C17 or later. A parent project can select a newer standard, but not an older one. Generator expressions select the options for each configuration, so the same rules work with single-config and multi-config generators. The presets set `CMAKE_COMPILE_WARNING_AS_ERROR`, so the compiler treats warnings as errors. The CMake build does not force this setting on a parent project.

### Sanitizers

`APPS_SANITIZER` accepts `none`, `address` or `thread`. `address` enables AddressSanitizer and UndefinedBehaviorSanitizer, and `thread` enables ThreadSanitizer. The option adds flags to the standard CMake configurations and creates no custom build types.

With `address`, the CMake build instruments first-party code, the libraries and vendored code. The libraries build with the first-party profile, so they keep every check, and the arena poisoning of `core` runs in an app's ASan build. For vendored code, the build turns off the `null` and `object-size` checks, because vendored code does not support them.

## Linting and formatting

When a project is the top-level CMake project, CMake runs `clang-tidy` and `cppcheck` while it compiles the first-party targets, through their `C_CLANG_TIDY` and `C_CPPCHECK` properties. Neither tool checks vendored code, and an app's analysis does not check the libraries, which the `libs/` project checks. The `HeaderFilterRegex` of the root `.clang-tidy` covers the headers under `src/` and `libs/`. In an app, the include path of `cppcheck` holds the include roots of the libraries that `APPS_LIBRARIES` in the app's `CMakeLists.txt` lists, because `cppcheck` needs these headers to expand macros such as `STR_FMT`. Thus `cppcheck` also analyzes the inline functions of these headers.

Test targets run `cppcheck` but not `clang-tidy`, because their intentional failure cases cause analyzer errors. When CMake cross-compiles, neither tool runs, because the analyzers on the host cannot find the headers that Zig supplies for the target platform. When the compiler is not Clang, the build turns off `clang-tidy`, because it rejects GCC-only warning options such as `-Wlogical-op`. `cppcheck` runs with any compiler.

The `format` and `lint` targets run `clang-format` and then `cwrap`, which refills comment prose to 100 columns, the `.clang-format` column limit. The targets skip a tool that is not installed. CI and the Linux image install a pinned `cwrap` release, not the one that `apps/cwrap` builds, so a change to how `cwrap` rewraps reaches the sources only through a new release and a new pin.

Both targets check the `.c` and `.h` files that the project's targets list in their `SOURCES`, `INTERFACE_SOURCES` and header sets. `apps_add_lint_targets` gathers them after the last target is defined, so a new file needs only its target. The test files are linted only when `APPS_BUILD_TESTING` is on. A file that no target compiles, such as a test helper header or `libs/src/core/test_format_compile.c`, is added to the sources of a target for this. A source that starts with a generator expression gets a CMake warning, because lint cannot see its path. In an app, both tools also check the version template, which `clang-format` reads as a `.c` file because of `--assume-filename`.

## Tests

`APPS_BUILD_TESTING` controls whether CMake defines the tests of an app. It is on by default when the app is the top-level CMake project, and off by default when a parent project adds the app as a subproject.

Each source directory adds its own unit tests, and `tests/` adds the golden tests. The tests of the libraries run in the `libs/` project, and no app registers them. Every CTest case of an app has the label of the app, and also the label `unit`, `golden` or `idempotent`.

### Unit tests

`apps_add_unit_test(<module>)` compiles `test_<module>.c` into the executable `<app>_unit_<module>`, and registers it as the CTest case `<app>.<module>`. The executable links `apps_build_tests`, acutest, the `testing` library, `<app>_test_support` when the app defines it, `<app>_app` and one host.

With the `OS memory` option, `apps_add_unit_test` links the in-memory host and compiles the test with `<APP>_TEST_OS_MEMORY` set to 1. New tests use this option. The tests registered without `OS memory` link the POSIX host, and each app's section lists them. The contract tests of `os/os.h`, `os.contract_posix` and `os.contract_memory`, run in the `libs/` project, and both must pass.

Each unit test has a CTest timeout of 60 seconds, so a test that hangs fails and does not block the run. A ThreadSanitizer build waits 300 seconds. No test target compiles an application source file a second time with different definitions.

### Golden tests

sosig, fram and surge share the steps of their golden tests through [cmake/apps_golden.cmake](cmake/apps_golden.cmake). Each golden script does these steps in a scratch tree of its own:

1. It copies the fixture and puts stale files in its output, which the first build must remove.
2. It builds the fixture three times: on one worker, on two workers with the output cache, and on eight workers with `--force`. The later builds must not change a generated file. The first build's `stderr` must be the warnings in `tests/expected/<fixture>.warnings.txt`, or empty without that file.
3. A dry run must keep every output but the output cache.
4. It compares the list of output files with `tests/expected/<fixture>`, and then the files.

The comparison leaves out the output cache, `_<app>/output-cache.tsv`, which records the modification times of the sources and has no expected copy. It looks each name up on the other side, because a sync tool can rewrite the Unicode normalization of a checked-out name. The scripts find the files with globs when the test runs, so a change to a fixture does not require CMake to configure the project again.

### Tests in a parent build

A parent project that turns on the tests of an app must call `enable_testing()` in its top-level `CMakeLists.txt`. CTest starts to find tests at the root of the build directory, and a call to `enable_testing()` in a subproject cannot create the test file at that root. When a subproject turns on its tests, CMake prints this requirement.

## Releases

### Version

`project(VERSION)` in an app's `CMakeLists.txt` holds the only copy of its numeric release version. CMake generates one source file from the app's version template. When the version changes, CMake compiles this file again and links the targets that use it again.

Release versions have the form `MAJOR.MINOR.PATCH`. `project(VERSION)` accepts only numeric components, so a prerelease suffix needs a second version value. Tag each release commit with `<app>-vMAJOR.MINOR.PATCH`. The tag starts the root [release.yml](.github/workflows/release.yml), which tests, packages and publishes that app only: the two Linux `musl` archives, an AArch64 macOS archive, and a `SHA256SUMS` file over them. A manual run of the workflow with `dry_run` tests and packages the dispatched commit, and publishes nothing.

### Install

The installation of an app holds the executable and its documentation files, `README.md`, `LICENSE.txt` and `THIRD_PARTY_NOTICES.md`, all in the `<app>_runtime` component. `GNUInstallDirs` selects the install paths. An app installs no library and no header, so it has no export set and no development component.

### Packages

CPack creates one stripped archive for each release target, `<app>-<version>-<target>.tar.gz`. It installs only the `<app>_runtime` component into the archive, because a vendored project that upstream's CMake builds, such as surge's libgit2, has `install()` rules without a component. The cache variable `APPS_RELEASE_TARGET` names the release target, for example `x86_64-linux-musl` in a Linux preset.

The macOS deployment target, `CMAKE_OSX_DEPLOYMENT_TARGET`, is `13.0` by default. You can set a higher value in the CMake cache.

The Linux `musl` presets cross-compile with the Zig toolchain files in the root `cmake/toolchains/` directory. These files select `llvm-strip`, because the strip tool of the host cannot read the cross-compiled ELF files.

The source archive holds the whole repository except the directories of the other apps, because an app builds with the root CMake modules, the libraries in `libs/` and the sources in `vendor/`. It also leaves out the version-control directories, temporary files, the `build/` directories and `CMakeUserPresets.json`.

## sosig

![CMake target dependency graph of sosig](apps/sosig/media/dependencies.svg)

### sosig files and targets

- `sosig` links the libraries `cli`, `core`, `os`, `runtime`, `formats`, `markup`, `output` and `templates`, and `sosig_app` links them all as `PUBLIC` dependencies.
- [apps/sosig/vendor.cmake](apps/sosig/vendor.cmake) includes [libs/src/markup/vendor.cmake](libs/src/markup/vendor.cmake), as [Grammars](#grammars) describes.

### sosig tests

The golden tests highlight code blocks, so they need every tree-sitter grammar. The tests registered without `OS memory` are `template_context_posix`, which renders the templates of the golden site `site_file_permalink` from `tests/fixtures/`, and the tests that use no host.

`sosig_fixture_sites` in [apps/sosig/tests/CMakeLists.txt](apps/sosig/tests/CMakeLists.txt) lists the fixture sites in `tests/fixtures/`. Each site is the golden test `sosig.golden.<site>`, which [apps/sosig/cmake/sosig_run_golden.cmake](apps/sosig/cmake/sosig_run_golden.cmake) runs as [Golden tests](#golden-tests) describes. It compares every file byte for byte.

## fram

![CMake target dependency graph of fram](apps/fram/media/dependencies.svg)

### fram files and targets

- fram needs NASM on x86-64, for the SIMD routines of libjpeg-turbo, and `ffmpeg` and `ffprobe`, for MP4 files and for the golden test.
- `fram` links the libraries `cli`, `core`, `os`, `runtime`, `formats`, `output` and `templates`, and `fram_app` links them all as `PUBLIC` dependencies.
- `src/media/` links libjpeg-turbo, libwebp and stb, because only its own code uses them.

### fram vendored dependencies

The pinned stb implementation has its own translation unit, [apps/fram/src/media/stb.c](apps/fram/src/media/stb.c), which the target `vendor_stb` compiles. This file compiles the resizer with its allocator hooks, and keeps the warnings of vendored code out of the compilation of first-party code.

The libjpeg-turbo target compiles the source files that the build system of libjpeg-turbo compiles. It uses the configuration headers in the root `vendor/libjpeg-turbo/config/` directory in place of the headers that this build system generates. On x86-64, the SIMD routines compile from NASM sources. On AArch64, they compile from Neon intrinsics, and other architectures use the C routines. The NASM sources are the object library `vendor_libjpeg_turbo_simd`, because the options of the vendored-code profile are for the C compiler, and NASM rejects them.

The libwebp target compiles the encoder, the decoder, the DSP and utility sources that they use, and libsharpyuv. It does not use the configuration header that the build system of libwebp generates. Only the tests decode WebP, so `fram` links no part of the decoder. The encoder selects its SIMD routines at run time. On x86-64, the CMake build compiles the SSE4.1 and AVX2 sources with their instruction sets enabled, and tells every other source that these routines exist. `WEBP_USE_THREAD` makes the one-time DSP setup of the encoder safe when several workers encode at the same time. libwebp also calls the math library, which glibc supplies apart from the C library, so the build links `libm` only when a call to a math function does not link without it.

### fram tests

[apps/fram/tests/CMakeLists.txt](apps/fram/tests/CMakeLists.txt) adds the golden test, the test helpers of `fram` and the seeded random tests in `tests/test_random.c`. Every unit test of `fram` links `fram_test_support`, which holds these helpers:

- a fixture JPEG encoder over libjpeg-turbo, in `tests/test_jpeg_fixture.h`, which writes baseline, progressive, CMYK and YCCK JPEGs, and a lossless JPEG that the probe rejects
- a WebP decoder for derivative outputs, in `tests/test_webp_output.h`

The tests registered without `OS memory` are the tests that run processes or decode video, `proc`, `video`, `album_scan_video`, `cmd_build_video` and `derivative_renderer_video`, and the tests that use no host. The derivative renderer exports `derivative_renderer_decode_scale`, so its test checks the scale that its jobs select directly, and not through the decoded output.

The golden test `fram.golden` builds the gallery in `tests/fixtures/gallery`, which mixes JPEG and MP4 sources, as [Golden tests](#golden-tests) describes. `tests/CMakeLists.txt` defines it only when the configure step finds both `ffmpeg` and `ffprobe`. [apps/fram/cmake/fram_run_golden.cmake](apps/fram/cmake/fram_run_golden.cmake) compares the pages and the copied originals byte for byte, after it replaces the `@PROJECT_VERSION@` placeholder in each expected page with the project version. It checks only that each derivative is not empty, because the WebP output can change with the libwebp version and the SIMD path. Unit tests check the dimensions and the pixels of derivatives.

## cwrap

![CMake target dependency graph of cwrap](apps/cwrap/media/dependencies.svg)

### cwrap files and targets

- `cwrap` links only `core` and `os`, and `cwrap_app` links both as `PUBLIC` dependencies. It links no `runtime`, so its binary has no worker pool and no thread library. `src/CMakeLists.txt` links the vendored dependencies.
- The two vendored projects of `cwrap`, acutest and copt, are header-only, so `cwrap` uses only `apps_build_project` and `apps_build_tests`. With `address`, the build instruments first-party code and `core`.
- The common presets give `cwrap` the `tsan` and `ci-tsan` presets too, but CI does not run them, because `cwrap` starts no threads.

### cwrap tests

The tests registered without `OS memory` are `cli` and `cli_dispatch`, which are not yet converted to the in-memory host and create their fixtures in a real temporary directory, and the tests that use no host. [apps/cwrap/tests/CMakeLists.txt](apps/cwrap/tests/CMakeLists.txt) adds the golden and idempotence tests and the seeded random tests. Select the idempotence test with `ctest --preset debug -L idempotent`.

The golden test `cwrap.golden` runs [apps/cwrap/cmake/cwrap_run_golden.cmake](apps/cwrap/cmake/cwrap_run_golden.cmake). The script rewraps every fixture in `tests/fixtures/` at width 40, in a scratch tree of its own. It then compares the list of fixture files with the list of files in `tests/expected/`, and compares each result with its expected file byte for byte.

The idempotence test `cwrap.idempotent` runs [apps/cwrap/cmake/cwrap_run_idempotent.cmake](apps/cwrap/cmake/cwrap_run_idempotent.cmake) in another scratch tree. The script rewraps the fixture files, the expected files, every first-party source file in `src/` and `tests/`, and the sources of the libraries that `APPS_LIBRARIES` in [apps/cwrap/CMakeLists.txt](apps/cwrap/CMakeLists.txt) lists, at widths 20, 40, 60 and 100. A second rewrap of each result must make no change, and `--check` must agree that the first result is stable.

Both scripts find their input files with globs when the test runs, so a change to a fixture or a source file does not require CMake to configure the project again.

## surge

![CMake target dependency graph of surge](apps/surge/media/dependencies.svg)

### surge files and targets

- surge needs `git` for the tests that read the fixture repositories, and `python3` and mise 2026.11.0 for the clone test and the mise test. Without one of them, the configure step leaves out the tests that need it, as [surge tests](#surge-tests) describes.
- `surge` links the libraries `cli`, `core`, `os`, `runtime`, `formats`, `markup`, `output` and `templates`, and `surge_app` links them all as `PUBLIC` dependencies.
- [apps/surge/vendor.cmake](apps/surge/vendor.cmake) includes [libs/src/markup/vendor.cmake](libs/src/markup/vendor.cmake), as [Grammars](#grammars) describes, and adds libgit2, as [surge vendored dependencies](#surge-vendored-dependencies) describes. `src/vcs/` links `vendor_libgit2`, because only its own code uses it.

### surge vendored dependencies

[vendor/vendor.toml](vendor/vendor.toml) pins libgit2 at release `1.9.7`. The vendored copy holds the library's sources and headers, its CMake build, and the bundled xdiff, zlib and llhttp, without the tests, the fuzzers, the examples, the CLI and the other bundled dependencies.

`apps/surge/vendor.cmake` builds libgit2 with upstream's CMake, through `add_subdirectory` with `EXCLUDE_FROM_ALL` and `SYSTEM`. Upstream's configure checks find the platform's `stat` fields, `futimens`, `qsort_r` variant, `iconv` and entropy source on macOS and under both Zig `musl` toolchains, so the same build serves the debug presets, CI and the release archives, with no `pkg-config` and no `find_package`. Before it adds the directory, `vendor.cmake` forces these cache options, so a cache from another configuration cannot change them:

- `BUILD_SHARED_LIBS`, `BUILD_TESTS`, `BUILD_CLI`, `BUILD_EXAMPLES` and `BUILD_FUZZERS` off
- `USE_HTTPS`, `USE_SSH` and `USE_GSSAPI` off, so libgit2 has no network transport
- `USE_BUNDLED_ZLIB` and `USE_THREADS` on
- `REGEX_BACKEND=regcomp`, the POSIX regex of the C library, which `musl` and macOS have, so no `pcre2` is vendored

`vendor.cmake` also sets these values for libgit2's directory only, and restores them after it:

- A cache entry `CMAKE_C_STANDARD=17`. libgit2 sets `CMAKE_C_STANDARD` as a cache variable (`90`), and its old policies let that value replace a normal variable of the same name. The cache entry that `vendor.cmake` sets first stays, so the default is C17 in libgit2's directory and in `surge` on every configure. `apps_build_vendor` requires C17 of every vendored target anyway.
- `CMAKE_POLICY_DEFAULT_CMP0077=NEW`, so that libgit2's `option()` calls, which predate the policy, do not clear the normal variables of `surge`, such as `CMAKE_C_EXTENSIONS`. CMake still warns that libgit2's `cmake_minimum_required(VERSION 3.5.1)` is deprecated.
- `CMAKE_LINK_LIBRARIES_ONLY_TARGETS` off, because libgit2 links the system libraries that it finds, such as `rt` on Linux, by name.
- `CMAKE_DISABLE_FIND_PACKAGE_OpenSSL` and `CMAKE_DISABLE_FIND_PACKAGE_GSSAPI`, because libgit2 looks for both whatever `USE_HTTPS` and `USE_GSSAPI` say, and a cross build would find the build host's copies.

Each object library of libgit2, `libgit2`, `util`, `xdiff`, `zlib` and `llhttp`, links `apps_build_vendor`, so the sanitizer builds instrument it, and compiles with `-w`. `vendor_libgit2` is an interface target over upstream's archive, `libgit2package`, with libgit2's `include/` directory as a system include directory, so `surge` links one `vendor_<lib>` target as for every other vendored project. libgit2's own `install()` rules have no component, so [cmake/apps_packaging.cmake](cmake/apps_packaging.cmake) installs only the `surge_runtime` component into an archive, which keeps `libgit2.a` and libgit2's headers out of it.

### surge tests

The tests registered without `OS memory` are the tests that read real repositories through libgit2, outside `os/os.h`, and the tests that use no host. The first group is `cmd_build` and the unit tests of `src/vcs/`, `changeset`, `filetree`, `pack_export`, `refset`, `repository` and `revision`, which also link `vendor_libgit2`. `gitmodules` of `src/vcs/` links it too, and parses text in memory, so it reads no repository.

The fixture repositories are `git fast-import` streams with fixed dates in [apps/surge/tests/repos/](apps/surge/tests/repos/), so every object id is the same on every run. Every test that reads one needs `git`. Without `git`, the configure step leaves out `cmd_build`, the tests of `src/vcs/` but `gitmodules`, the golden tests and `surge.fixture_repos`. These tests use the fixture repositories:

- `surge.fixture_repos_setup` runs [apps/surge/cmake/surge_make_fixture_repos.cmake](apps/surge/cmake/surge_make_fixture_repos.cmake), which makes the repositories in `build/<preset>/fixture-repos/<config>` with `GIT_CONFIG_NOSYSTEM=1` and a `HOME` of its own. It is the CTest fixture setup `surge_fixture_repos`, which `cmd_build` and the `src/vcs/` tests but `gitmodules` require, and they find the repositories through `SURGE_FIXTURE_REPOS_DIR`.
- `surge.fixture_repos` runs [apps/surge/cmake/surge_check_fixture_repos.cmake](apps/surge/cmake/surge_check_fixture_repos.cmake). It makes the repositories in a scratch tree of its own, and checks that each one holds exactly the refs and ids that its `tests/repos/<name>.refs` lists, so a change to a stream or to the script shows up as a change of the ids that the golden tests rely on.

`surge_fixture_sites` in [apps/surge/tests/CMakeLists.txt](apps/surge/tests/CMakeLists.txt) lists the fixture sites in `tests/fixtures/`. Each site is the golden test `surge.golden.<site>`, which [apps/surge/cmake/surge_run_golden.cmake](apps/surge/cmake/surge_run_golden.cmake) runs in `build/<preset>/golden/<config>/<site>`. It follows [Golden tests](#golden-tests), with these differences:

1. It makes the fixture repositories in the scratch tree, and sets `TMPDIR` to a scratch directory.
2. For a site whose expected directory holds only `stderr.txt`, such as a `refuse-*` site, it runs one build. The build must exit with status 1 with exactly that text, and create no `public/`. The script runs `build --verbose --workers 1`, so `stderr.txt` also shows the phases that ran before the build stopped.
3. After the dry run, `TMPDIR` must be empty.
4. It compares every file byte for byte, the packs included, after it replaces the `@PROJECT_VERSION@` placeholder in each expected text file. It checks each pack with `git verify-pack` and `git index-pack --strict`, and each `info/refs` against `git for-each-ref`.
5. With `python3`, it runs [apps/surge/tests/clone_test.py](apps/surge/tests/clone_test.py) with `python3 -I`. The test serves the output on localhost, clones every clone export over dumb HTTP, and compares the refs with the source. It then clones from a server that answers a missing path with status 200 and an HTML page, and checks for the corrupt-object errors. With mise as well, it runs [apps/surge/tests/mise_test.py](apps/surge/tests/mise_test.py), which installs a release project of the site with mise, and checks that a tampered checksum or archive fails.
6. It moves a branch and changes the release directories, and checks what each dry run writes, keeps and removes, and that the previous pack stays for one build, also after a failed build. Last, a `--force` dry run and one after a template change must make every page and feed again.

Without `python3`, the golden test runs without the clone test and the mise test, and without mise, it runs without the mise test. CI installs mise 2026.11.0 for the `surge` job on Linux and macOS through the `mise` input of [checks.yml](.github/workflows/checks.yml), and the GCC job runs without it. The output cache of surge holds the modification times of the static files and a page recipe that hashes those of the copied templates.
