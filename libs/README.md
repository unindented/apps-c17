<div align="center">
  <img src="media/logo.webp" height="300" alt="">
</div>

<h1 align="center"><code>libs</code></h1>

`libs/` holds the libraries that the C17 apps in this repository share: `core`, `testing`, `os`, `runtime`, `cli`, `formats`, `markup`, `output` and `templates`. [ARCHITECTURE.md](ARCHITECTURE.md) describes each library, its modules and its dependencies.

## LLM disclosure

> [!warning]
> This is all vibe-coded slop, produced with Claude Opus 4.8, 5, and 5.5.

## Usage

An app adds the `libs/` project to its own build, and links the libraries that it uses:

```cmake
add_subdirectory(../../libs ${CMAKE_BINARY_DIR}/libs EXCLUDE_FROM_ALL SYSTEM)
target_link_libraries(myapp_app PUBLIC core os)
```

A library builds only when a target of the app links it, and it compiles with the build profile of the app. Each program that links `core` defines `os_oom`, as [ARCHITECTURE.md](ARCHITECTURE.md#os_oom) describes. [BUILD.md](../BUILD.md#the-libs-project) describes the build of the project.

## Development

[ARCHITECTURE.md](ARCHITECTURE.md) describes the code. [BUILD.md](../BUILD.md) describes the CMake targets and the reasons for the configurations below. [STYLE.md](../STYLE.md) describes the code style.

### Prerequisites

- A POSIX-like system with CMake 3.25 or later, a C17 compiler such as Clang or GCC, and pthreads.
- Optional build tools: `ninja` for the `multi` preset.
- Optional quality tools: `cwrap`, `clang-format`, `clang-tidy`, `cppcheck`.

The repository includes the dependencies in [vendor/](../vendor/).

### Building

Run the commands in this section and in the sections below from `libs/`, the directory of this README. Each preset builds in `build/<preset>`, outside the source tree, and treats compiler warnings as errors. If `CC` is set, the preset builds in `build/<preset>/<CC>` instead, such as `build/debug/gcc-14`. CMake reads `CC` only when it configures a directory for the first time, so each compiler needs its own CMake cache. The commands use eight parallel jobs. Change that number to suit your machine.

```sh
cmake --preset debug
cmake --build --preset debug -j 8
```

| Preset | Configuration | Library |
| --- | --- | --- |
| `debug` | `Debug` with AddressSanitizer and UndefinedBehaviorSanitizer. It runs `clang-tidy` and `cppcheck` during compilation if they are available. | `build/debug/src/<library>/lib<library>.a` |
| `tsan` | `Debug` with ThreadSanitizer. | `build/tsan/src/<library>/lib<library>.a` |
| `release` | `RelWithDebInfo`, with no tests. | `build/release/src/<library>/lib<library>.a` |
| `multi` | `Debug` and `RelWithDebInfo` in one build tree, with the `Ninja Multi-Config` generator. Build with `--preset multi-debug` or `--preset multi-relwithdebinfo`. | `build/multi/src/<library>/Debug/lib<library>.a`, `build/multi/src/<library>/RelWithDebInfo/lib<library>.a` |

`clang-tidy` runs only with Clang, because it rejects the warning options of GCC. To run linting, configure the `debug` preset, and then run this command:

```sh
cmake --build --preset lint
```

### Testing

CTest registers the unit tests of every library, and the compile tests of `core`, which check that the compiler rejects a bad call to `buf_printf`, `log_error`, or a wrapper with the same `format` attribute. Build a preset, and then run its tests:

```sh
ctest --preset debug -j 8
```

The test presets are `debug`, `tsan`, `multi-debug`, and `multi-relwithdebinfo`. The `multi-relwithdebinfo` tests check the same CMake configuration as the `release` preset, which builds no tests. To run only part of a suite, select the tests by label or by name pattern:

```sh
ctest --preset debug -L unit -j 8
ctest --preset debug -L markup -j 8
ctest --preset debug -R arena -j 8
```

### Continuous integration

The workflow presets run the same configure, build, and test steps as CI:

| Workflow | Steps |
| --- | --- |
| `cmake --workflow --preset ci-debug` | Build in `Debug`, run linting, and run all ASan and UBSan tests. |
| `cmake --workflow --preset ci-tsan` | Build with TSan and run all TSan tests. |
| `cmake --workflow --preset ci-release` | Build in `RelWithDebInfo`. |
| `cmake --workflow --preset ci-multi` | Build and test in `Debug` and `RelWithDebInfo` with the `Ninja Multi-Config` generator. CI runs this workflow a second time with `CC=gcc-14` to check GCC. |

To run the Linux workflows on a machine with Podman, build the pinned Ubuntu image from the root [Containerfile](../Containerfile), with the repository root as the build context. Then run each workflow in a new container, with `libs/` as the working directory:

```sh
podman build --tag apps-c17-linux-ci --file ../Containerfile ..
podman run --rm --workdir /src/libs apps-c17-linux-ci ci-debug
podman run --rm --workdir /src/libs apps-c17-linux-ci ci-tsan
podman run --rm --workdir /src/libs apps-c17-linux-ci ci-release
podman run --rm --workdir /src/libs apps-c17-linux-ci ci-multi
podman run --rm --workdir /src/libs -e CC=gcc-14 apps-c17-linux-ci ci-multi
```

The image contains a snapshot of the repository, including the vendored dependencies, without the directories that [.containerignore](../.containerignore) excludes, so the Linux build products stay in the container and do not go into a `build/` directory of the host. The image supports x86-64 and AArch64 hosts, and pins LLVM 23 and GCC 14.

## Dependencies

The root [vendor/](../vendor/) directory contains these dependencies. [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists their licenses.

- [tomlc17](https://github.com/cktan/tomlc17): TOML parsing, for `formats`.
- [mustache4c](https://github.com/mity/mustache4c): Mustache templates, for `templates`.
- [md4c](https://github.com/mity/md4c) and [org4c](https://github.com/unindented/org4c): Markdown and Org parsing, for `markup`.
- [tree-sitter](https://github.com/tree-sitter/tree-sitter): Syntax highlighting, for `markup`, with the grammars that [BUILD.md](../BUILD.md#grammars) lists.
- [acutest](https://github.com/mity/acutest): Unit tests.

## License

The libraries are MIT-licensed, as [LICENSE.txt](../LICENSE.txt) states.
