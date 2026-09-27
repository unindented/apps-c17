# Build system

This document describes the targets of the `fram` CMake build, how the targets link to each other, and the reasons for its settings. The CMake build needs CMake 3.25 or later. The [Development](README.md#development) section of the README lists the commands that compile, test and package `fram`.

## Layout

The top-level [CMakeLists.txt](CMakeLists.txt) sets the project policy, finds the thread library, includes the CMake modules of the root [cmake/](../../cmake/) directory and of its own [cmake/](cmake/) directory, includes [vendor.cmake](vendor.cmake), adds the libraries project [libs/](../../libs/), and adds each source directory. These are the other CMake files:

| Path | Responsibility |
| --- | --- |
| [src/CMakeLists.txt](src/CMakeLists.txt) | Defines the application library `fram_app` and the `fram` executable |
| `src/*/CMakeLists.txt` | Add the sources, the header file sets and the unit tests of their directory. |
| [tests/CMakeLists.txt](tests/CMakeLists.txt) | Adds the golden test, the test helpers of `fram` and the seeded random tests |
| [vendor.cmake](vendor.cmake) | Defines a target for each vendored dependency that `fram` uses, from the sources in the root [vendor/](../../vendor/) directory |
| [../../libs/CMakeLists.txt](../../libs/CMakeLists.txt) | Defines the libraries that `fram` links: `shared`, `core`, `runtime` with the POSIX host `os_posix`, and `formats`, and for the unit tests `testing` and the in-memory host `os_memory` |
| [../../cmake/apps_build_profiles.cmake](../../cmake/apps_build_profiles.cmake) | Sets warnings, optimization, fortification and sanitizers |
| [../../cmake/apps_quality.cmake](../../cmake/apps_quality.cmake) | Configures formatting and static analysis |
| [cmake/fram_lint.cmake](cmake/fram_lint.cmake) | Lists the first-party sources that the `format` and `lint` targets check |
| [../../cmake/apps_testing.cmake](../../cmake/apps_testing.cmake) | Defines `apps_add_unit_test`, which registers a unit test and selects the host that the test links |
| [cmake/fram_run_golden.cmake](cmake/fram_run_golden.cmake) | Runs the golden test |
| [../../cmake/apps_packaging.cmake](../../cmake/apps_packaging.cmake) | Configures installation and CPack archives |
| [../../cmake/apps_restyle_graphviz_svg.cmake](../../cmake/apps_restyle_graphviz_svg.cmake) | Adds light and dark styles to the target graph |
| [../../cmake/toolchains/](../../cmake/toolchains/) | Configures Zig to cross-compile for the Linux `musl` release targets |
| [CMakeGraphVizOptions.cmake](CMakeGraphVizOptions.cmake) | Sets the filters and the layout of the target graph |
| [CMakePresets.json](CMakePresets.json) | Includes the configure, build, test, package and workflow presets of [../../cmake/release-presets.json](../../cmake/release-presets.json), which includes [../../cmake/presets.json](../../cmake/presets.json) |

## Targets

The CMake build has one application library, two hosts, one target for each vendored dependency, the libraries of [libs/](../../libs/), and the interface targets of [Build profiles](#build-profiles). Each executable links the application library and one of the two hosts. CMake generates this graph of the targets and their links from the `release` preset. The graph does not show the dependencies between source modules, and it does not show test-only targets such as the in-memory host `os_memory` and the `testing` library.

![CMake target dependency graph](media/dependencies.svg)

### Application library

`fram_app` is a private static library that holds every application source except [src/app/main.c](src/app/main.c). The `fram` executable and every unit test link it. It is static because it supports one product and has no public ABI.

Each source directory holds the code for one purpose. The source directories add their files to `fram_app` and do not define libraries of their own, because more libraries would add link boundaries without independent APIs. `src/build/` and `src/formats/` link the vendored dependencies that only their own code uses, and `src/CMakeLists.txt` links the other vendored dependencies.

`fram_app` links `vendor_tomlc17`, `core`, `formats`, `runtime` and `shared` as `PUBLIC` dependencies, because the application headers expose the types of these libraries. The other dependencies are `PRIVATE`. `Threads::Threads` is one of the `PRIVATE` dependencies, but CMake still adds it to each executable that links the static library.

`shared`, the library in [libs/shared/](../../libs/shared/), supplies the arena, the counted string, the output buffer and the diagnostic log. When an allocation does not fit, the arena calls `os_oom`, and each program defines its own `os_oom`. `src/app/main.c` defines the `os_oom` of `fram`, which [ARCHITECTURE.md](ARCHITECTURE.md#out-of-memory-exits-the-process) describes. The `testing` library defines the `os_oom` of the unit tests.

The other libraries of [libs/](../../libs/) that `fram_app` links hold the modules that `fram` shares with the other apps, as [libs/ARCHITECTURE.md](../../libs/ARCHITECTURE.md) describes. Each library compiles with the `apps_build_project` profile of `fram`, and builds only because `fram` links it.

### Hosts

`runtime/fs.c` in the `runtime` library is the POSIX implementation of [runtime/os.h](../../libs/runtime/os.h). CMake compiles this file into the object library `os_posix`, outside `fram_app`, so each executable selects its host when it links. `fram` links `os_posix`. The unit tests that use the default, `OS posix`, also link it (see [Unit tests](#unit-tests)). The tests registered with `OS memory` link `os_memory`, the in-memory host in [os_memory/os_memory.c](../../libs/os_memory/os_memory.c), instead.

Both hosts are object libraries, because `fram_app` refers to them and a single-pass linker skips a static archive when the archive comes before the code that uses it on the link line.

### Vendored dependencies

The sources of each vendored project are in the root [vendor/](../../vendor/) directory, and [vendor.cmake](vendor.cmake) compiles the sources that `fram` uses. Each vendored project has its own target: the header-only projects, acutest and copt, are interface libraries, and the other vendored projects are static libraries.

The pinned stb implementation has its own translation unit, [src/formats/stb.c](src/formats/stb.c), which the target `vendor_stb` compiles. This file compiles the resizer with its allocator hooks, and it keeps the warnings of vendored code out of the compilation of first-party code.

The libjpeg-turbo target compiles the source files that the build system of libjpeg-turbo compiles. It uses the configuration headers in the root `vendor/libjpeg-turbo/config/` directory in place of the headers that this build system generates. On x86-64, the SIMD routines of libjpeg-turbo compile from NASM sources, so an x86-64 build needs NASM. On AArch64, these routines compile from Neon intrinsics, and other architectures use the C routines. The NASM sources are the object library `vendor_libjpeg_turbo_simd`, because the options of the vendored-code profile are for the C compiler, and NASM rejects them.

The libwebp target compiles the encoder, the decoder, the DSP sources and utility sources that they use, and libsharpyuv. It does not use the configuration header that the build system of libwebp generates. Only the tests decode WebP, so `fram` links no part of the decoder. The encoder selects its SIMD routines at run time. On x86-64, the CMake build compiles the SSE4.1 and AVX2 sources with their instruction sets enabled, and tells every other source that these routines exist. `WEBP_USE_THREAD` makes the one-time DSP setup of the encoder safe when several workers encode at the same time. libwebp also calls the math library, which glibc supplies separately from the C library. Thus the CMake build links `libm` only when a call to a math function does not link without it.

### Regenerating the graph

> [!tip]
> Use these commands in `apps/fram` to update the graph:
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

With `address`, the CMake build instruments first-party code, the `shared` library and vendored code. `shared` builds with the first-party profile, so it keeps every check. For vendored code, it turns off the `null` and `object-size` checks, because vendored code does not support these checks. First-party code keeps every check.

## Linting and formatting

When `fram` is the top-level CMake project, the `C_CLANG_TIDY` and `C_CPPCHECK` properties of the first-party targets make CMake run `clang-tidy` and `cppcheck` while it compiles these targets. Neither tool checks vendored code or the libraries, which the `libs/` project checks. The include path of `cppcheck` contains the headers of `shared` in `libs/shared/include` and the include roots of the other libraries that `APPS_LIBRARIES` in [CMakeLists.txt](CMakeLists.txt) lists, because `cppcheck` needs these headers to expand macros such as `STR_FMT`. Thus `cppcheck` also analyzes the inline functions that these headers define.

Test targets run `cppcheck` but not `clang-tidy`, because their intentional failure cases cause analyzer errors. When CMake cross-compiles, neither tool runs. Zig supplies the headers for the target platform, and the analyzers on the host cannot find these headers when they repeat the compile command. When the compiler is not Clang, the CMake build turns off `clang-tidy`, because `clang-tidy` reads the compile command and rejects GCC-only warning options such as `-Wlogical-op`. `cppcheck` runs with any compiler.

The `format` and `lint` targets run `clang-format` and then `cwrap`, which refills comment prose to 100 columns, the `.clang-format` column limit. Either tool is skipped when it is not installed. CI and the Linux image install a pinned `cwrap` release.

Both targets run both tools on the first-party files in an explicit list, the `SOURCES` of `apps_add_lint_targets` in [cmake/fram_lint.cmake](cmake/fram_lint.cmake). They also run both tools on the template [src/core/fram_version.c.in](src/core/fram_version.c.in), which `clang-format` reads as `fram_version.c` because of `--assume-filename`. When you add a source file or a test file, add it to this list and to its target. A glob at configure time would not find a new file until CMake configures the project again.

## Tests

`APPS_BUILD_TESTING` controls whether CMake defines the tests. It is on by default when `fram` is the top-level CMake project, and off by default when a parent project adds `fram` as a subproject.

Each source directory adds its own unit tests. `tests/` adds the golden test, `fram_test_support` and the seeded random tests in `tests/test_random.c`. The tests of the libraries run in the `libs/` project, and `fram` does not register them. Every CTest case has the label `fram`, and also the label `unit` or `golden`.

### Unit tests

`apps_add_unit_test(<module>)` compiles `test_<module>.c` into the executable `fram_unit_<module>`, and registers this executable as the CTest case `fram.<module>`. The executable links `apps_build_tests`, acutest, the `testing` library, `fram_test_support`, `fram_app` and one host.

With the `OS memory` option, `apps_add_unit_test` links the in-memory host `os_memory` and compiles the test with `FRAM_TEST_OS_MEMORY` set to 1. New tests use this option. The tests registered without `OS memory` link the POSIX host. These tests are:

- the tests that need the real host: `output_pruner_case`, which tests the prune on a case-insensitive `/tmp` and skips on a case-sensitive `/tmp`; and `proc`, `video`, `album_scanner_video`, `cmd_build_video` and `derivative_renderer_video`, which run processes or decode video
- the suites that are not yet converted to the in-memory host, which create their fixtures in a real temporary directory: `cli`, `cli_dispatch`, `cmd_config`, `gallery_config` and `template`
- the tests that use no host

When one source file compiles into more than one executable, the `NAME` option sets the name of each executable and its test. The contract tests of `runtime/os.h`, `runtime.os_posix` and `runtime.os_memory`, run in the `libs/` project.

Each unit test has a CTest timeout of 60 seconds, so a test that hangs fails and does not block the test run.

No test target compiles an application source file a second time with different definitions. The production render limit is 256 MiB, and a failure test at that limit uses too much memory. Thus the template test passes a 64 KiB limit to `template_render_file_limited`, and does not compile the template code again with a smaller limit. The derivative renderer exports `derivative_renderer_decode_scale`, which returns the scale that its jobs select, so its test checks this selection directly and not through the decoded output.

### Test-support library

The `testing` library, [testing/test_support.h](../../libs/testing/test_support.h), holds the helpers that several `test_*.c` files need: fixture files and trees, the working directory, environment variables, the capture of the standard streams, an arena or a log over a fresh reservation, checks that compare the text of a log exactly, the out-of-memory check `test_is_oom`, and the seeded random generator for `tests/test_random.c`. It also defines the `os_oom` of the unit tests. [libs/ARCHITECTURE.md](../../libs/ARCHITECTURE.md#testing) describes it.

`fram_test_support` holds the test helpers of `fram`, which every unit test of `fram` links:

- a fixture JPEG encoder over libjpeg-turbo, in `tests/test_jpeg_fixture.h`, which writes baseline, progressive, CMYK and YCCK JPEGs, and a lossless JPEG that the probe rejects
- a WebP decoder for derivative outputs, in `tests/test_webp_output.h`

### Golden test

The golden test `fram.golden` builds the fixture gallery in `tests/fixtures/gallery`, which mixes JPEG and MP4 sources in the default `media/` directory. `tests/CMakeLists.txt` defines this test only when the configure step finds both `ffmpeg` and `ffprobe`. `cmake/fram_run_golden.cmake` does these steps:

1. It copies the gallery into a scratch tree of its own.
2. It puts stale files in the output. The first build must remove these files.
3. It builds the gallery three times. The second build reuses the build cache, and the third build passes `--force`. These two builds must not change a generated file.
4. It compares the list of output files with `tests/expected/gallery`.

The comparison leaves out the build cache, `_fram/v1-build-cache.tsv`, which holds the modification times of the copied sources and has no expected copy.

The script compares the non-media files and the copied originals byte for byte, after it replaces the `@PROJECT_VERSION@` placeholder in each expected page with the project version. For each generated derivative, it checks only that the file is not empty, because the WebP output can change with the libwebp version and the SIMD path. Unit tests check the dimensions and the pixels of derivatives.

The script finds the expected files and the actual files with globs after `fram` runs. These globs run in the test and are not inputs of the CMake build, so a change to a fixture does not require CMake to configure the project again.

### Format checks in shared

`shared` has its own compile-fail tests for `buf_printf` and `log_error`. These tests check that a call does not compile when its arguments do not match its format. The test cases are:

- a raw `struct Str` for `%.*s`
- a missing argument
- a `ptrdiff_t` for `%d`
- a format that is not a literal
- a value that is not a string, given to `STR_FMT`
- a mismatch through a forwarding wrapper

These tests run in the `libs/` project, as `shared.format_compile.*`. The `fram` build does not register them.

### Tests in a parent build

A parent project that turns on the `fram` tests must call `enable_testing()` in its top-level `CMakeLists.txt`. CTest starts to find tests at the root of the build directory, and a call to `enable_testing()` in a subproject cannot create the test file at that root. When a subproject turns on its tests, CMake prints this requirement.

## Releases

### Version

`project(VERSION)` in `CMakeLists.txt` holds the only copy of the numeric release version. CMake generates one source file from `src/core/fram_version.c.in`, and this file defines `fram_version_string()` and `fram_generator_string()`. When the version changes, CMake compiles this file again and links the targets that use it again.

Release versions have the form `MAJOR.MINOR.PATCH`. `project(VERSION)` accepts only numeric components, so a prerelease suffix needs a second version value.

### Install

The installation contains the executable and its documentation files, `README.md`, `LICENSE.txt` and `THIRD_PARTY_NOTICES.md`. All of these files are in the `fram_runtime` component. `GNUInstallDirs` selects the install paths. The project installs no library and no header, so it has no export set and no development component.

### Packages

CPack creates one stripped archive for each release target, `fram-<version>-<target>.tar.gz`. The cache variable `APPS_RELEASE_TARGET` names the release target, for example `x86_64-linux-musl` in a Linux preset.

The macOS deployment target, `CMAKE_OSX_DEPLOYMENT_TARGET`, is `13.0` by default. You can set a higher value in the CMake cache.

The Linux `musl` presets cross-compile with the Zig toolchain files in the root `cmake/toolchains/` directory. These files select `llvm-strip`, because the strip tool of the host cannot read the cross-compiled ELF files.

The source archive, `fram-<version>-source.tar.gz`, holds the whole repository except the directories of the other apps, because `fram` builds with the root CMake modules, the libraries in `libs/` and the sources in `vendor/`. The archive keeps the default CPack exclusions for version-control directories and temporary files. It also leaves out the `build/` directories and `CMakeUserPresets.json`.
