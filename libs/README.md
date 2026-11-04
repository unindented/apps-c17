<div align="center">
  <img src="media/logo.webp" height="300" alt="">
</div>

<h1 align="center"><code>libs</code></h1>

`libs/` holds the libraries that the C17 apps in this repository share: `core`, `testing`, `os`, `runtime`, `cli`, `formats`, `markup`, `output` and `templates`. [ARCHITECTURE.md](ARCHITECTURE.md) describes each library and its design.

## LLM disclosure

> [!warning]
> This is all vibe-coded slop, produced with Claude Opus 4.8, 5, and 5.5.

## Usage

An app adds the `libs/` project to its build, and links the libraries that it uses:

```cmake
add_subdirectory(../../libs ${CMAKE_BINARY_DIR}/libs EXCLUDE_FROM_ALL SYSTEM)
target_link_libraries(myapp_app PUBLIC core os)
```

A library builds only when a target of the app links it, with the app's build profile. Each program that links `core` defines `os_oom`, as [ARCHITECTURE.md](ARCHITECTURE.md#os_oom) describes. [BUILD.md](../BUILD.md#the-libs-project) describes the build of the project.

## Development

[BUILD.md](../BUILD.md#development) gives the commands that build, test and lint the libraries. Run them from `libs/`. The tests include the compile tests of `core`, which check that the compiler rejects a call to `buf_printf` or `log_error` whose arguments do not match its format. [STYLE.md](../STYLE.md) describes the code style.

## Dependencies

The root [vendor/](../vendor/) directory contains these dependencies. [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists their licenses.

- [tomlc17](https://github.com/cktan/tomlc17): TOML parsing, for `formats`.
- [mustache4c](https://github.com/mity/mustache4c): Mustache templates, for `templates`.
- [md4c](https://github.com/mity/md4c) and [org4c](https://github.com/unindented/org4c): Markdown and Org parsing, for `markup`.
- [tree-sitter](https://github.com/tree-sitter/tree-sitter): Syntax highlighting, for `markup`, with the grammars that [BUILD.md](../BUILD.md#grammars) lists.
- [acutest](https://github.com/mity/acutest): Unit tests.

## License

The libraries are MIT-licensed, as [LICENSE.txt](../LICENSE.txt) states.
