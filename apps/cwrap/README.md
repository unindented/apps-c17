<div align="center">
  <img src="media/logo.webp" height="300" alt="">
</div>

<h1 align="center"><code>cwrap</code> /kræp/</h1>

`cwrap` is a small C comment formatter written in C17. It refills the prose in C comments to a column width.

For example, running `cwrap -w 72` converts this:

```c
/**
 * @brief Copies `n` bytes from `src` to `dest`. The two regions must not overlap.
 * @param dest Destination buffer. It must be large enough for `n` bytes.
 * @param src Source buffer. It is read for `n` bytes.
 * @param n Number of bytes to copy.
 * @return `dest`.
 */
void* memcpy(void* dest, const void* src, size_t n) {
  unsigned char* d = dest;
  const unsigned char* s = src;

  // Walk both pointers forward until every requested byte has been copied into `dest`.
  while (n--)
    *d++ = *s++;
  return dest;
}
```

into this:

```c
/**
 * @brief Copies `n` bytes from `src` to `dest`. The two regions must
 *        not overlap.
 * @param dest Destination buffer. It must be large enough for `n`
 *             bytes.
 * @param src  Source buffer. It is read for `n` bytes.
 * @param n    Number of bytes to copy.
 * @return `dest`.
 */
void* memcpy(void* dest, const void* src, size_t n) {
  unsigned char* d = dest;
  const unsigned char* s = src;

  // Walk both pointers forward until every requested byte has been
  // copied into `dest`.
  while (n--)
    *d++ = *s++;
  return dest;
}
```

## LLM disclosure

> [!warning]
> This is all vibe-coded slop, produced with Claude Opus 4.8, 5, and 5.5.

## Installation

Download a prebuilt archive from a `cwrap-v` release on the [Releases](https://github.com/unindented/apps-c17/releases) page, or build it from source as [BUILD.md](../../BUILD.md) describes. Each release has one archive for each target, `cwrap-<version>-<target>.tar.gz`. The target is `x86_64-linux-musl`, `aarch64-linux-musl` or `aarch64-macos`. A `SHA256SUMS` file gives the SHA-256 digest of each archive. An archive holds `cwrap-<version>-<target>/bin/cwrap`, and below `share/doc/cwrap/` it holds this README, `LICENSE.txt` and `THIRD_PARTY_NOTICES.md`. The Linux binaries are static `musl` executables.

mise's [`http:` backend](https://mise.jdx.dev/dev-tools/backends/http.html) can install these archives. A GitHub release publishes no list of versions, so this `mise.toml` names the version:

```toml
[tools."http:cwrap"]
version = "0.3.1"
checksum_url = "https://github.com/unindented/apps-c17/releases/download/cwrap-v{{ version }}/SHA256SUMS"
strip_components = 1
bin_path = "bin"

[tools."http:cwrap".platforms]
linux-x64 = { url = "https://github.com/unindented/apps-c17/releases/download/cwrap-v{{ version }}/cwrap-{{ version }}-x86_64-linux-musl.tar.gz" }
linux-arm64 = { url = "https://github.com/unindented/apps-c17/releases/download/cwrap-v{{ version }}/cwrap-{{ version }}-aarch64-linux-musl.tar.gz" }
macos-arm64 = { url = "https://github.com/unindented/apps-c17/releases/download/cwrap-v{{ version }}/cwrap-{{ version }}-aarch64-macos.tar.gz" }
```

`mise install` does not read `checksum_url`. Run `mise lock` to record the digest of each platform's archive in `mise.lock`. Later installs then refuse an archive that does not match.

## Usage

### Inputs and options

`cwrap` reads files or standard input:

- `cwrap [options] <file>...`: Rewrap the comments in each input file.
- `cat foo.c | cwrap [options]`: Read standard input when the command line names no file.
- `cwrap [options] - < foo.c`: Read standard input. The `-` argument selects standard input explicitly.

These options control the wrapping and the output mode:

- `-w, --width N`: Wrap at column `N`. `N` must be from 1 through 10000. The default is 100.
- `-i, --in-place`: Write the result over each input file that the rewrap changes.
- `-c, --check`: Do not write any file. Exit with status 1 if the rewrap would change any input.

It also has these options:

- `cwrap --version` (or `-V`): Print the version.
- `cwrap --help` (or `-h`): Print the usage and the list of options.

The tool rejects these combinations of inputs and options:

- Standard input with file inputs.
- Standard input with `--in-place`.
- `--check` with `--in-place`.

A rejected command line reports only its first error, because the later errors usually come from the first one. `cwrap` exits with one of these statuses:

- 0 when it succeeds.
- 1 when it fails, or when `--check` finds an input that would change.
- 2 when it rejects the command line.

### Failures

An input file is bad when it:

- does not exist
- is not a regular file
- cannot be read
- contains a `NUL` byte

In the default mode, the tool prints the result of each file in turn. At the first bad input file, the tool stops and exits with status 1. It prints the results of the files before the bad file.

With `--in-place`, the tool first reads every input file and reports each bad file. If any input file is bad, the tool writes no file. A write can still fail after this first pass. Then the tool stops at that file, and the files before it keep their new contents. Each write replaces the file atomically, so the tool never leaves a file half-written. A write follows a symlink, creates no parent directory, keeps the permission bits of the file, and breaks a hard link to the file. When the command line names a file two times, the tool writes the file at its first position. At its second position, the file is already wrapped.

`--check` never writes a file. It reports every input that would change and every bad input, in path order.

### Limits

The tool rejects an input file larger than 64 MiB before it reads the file. The error message gives the limit and the file size. The tool rejects standard input larger than 64 MiB when it reads one byte past the limit. This error message gives the limit.

The tool reserves its memory once, when it starts, in fixed sizes. Reserved memory that the tool does not use takes address space but no memory. Under an address-space limit (`ulimit -v`), the tool makes the reservation smaller until it fits, as [ARCHITECTURE.md](ARCHITECTURE.md#memory-fixed-reservations-for-one-input-at-a-time) describes. It needs about 360 MB (`ulimit -v 350000`), and with less it stops at startup with `cwrap: out of memory`. An input that needs more memory than the tool reserved, such as a deeply indented comment at a narrow width, ends the run. The tool then prints the output and the errors up to that point, prints `cwrap: out of memory (in '<path>')`, and exits with status 1. For standard input, the message names `'stdin'`.

## Wrapping behavior

- The tool refills the prose in `//`, `///`, `//!`, `/* */`, `/** */`, and `/*! */` comments to the column. Each comment keeps its opener. Consecutive line comments merge only when they have the same opener and the same indent, at the left margin or indented. A change of indent, a code line or a blank line separates two runs of line comments.
- In a slash-star comment, a `*` under the `*` of the opener marks a starred comment. A `*` under the prose marks a list item. When the refilled prose of a starred comment fits on the opener line, the closer follows the prose on that line.
- The tool does not change a slash-star comment on one line when the comment fits the column and contains no tab. Thus a label such as `/* op    */`, with padding that aligns it with its neighbors, stays aligned.
- The tool does not change trailing comments, list items, indented samples, Markdown fences, or lines of punctuation. It also does not change a comment with no letter, digit or non-ASCII character, such as a banner.
- The tool does not change a `//` comment that a backslash at the end of the line continues. The continued lines have no `//` for a refill to restore.
- A comment line that starts with a tool directive stays unchanged on its own line, even when the line is wider than the column. The prose before the directive and the prose after it are separate paragraphs. Each directive matches only as a whole word, so `NOLINTED` is prose. The directives are:
  - `cppcheck-suppress`, with any suffix, such as `cppcheck-suppress-file`.
  - `NOLINT`, `NOLINTNEXTLINE`, `NOLINTBEGIN` and `NOLINTEND`, with or without a `(check)` suffix.
  - `clang-format off` and `clang-format on`.
  - `IWYU pragma:`.
- A Doxygen comment starts with `///`, `//!`, `/**`, or `/*!`. In a Doxygen comment, a line that starts with a command such as `@brief` or `\param` starts a new paragraph. The description of the command wraps under the first word after the command. The descriptions of all `@param` commands in a comment align in one column, and the descriptions of all `@retval` commands align in another column. The tool does not change `@code` and `@verbatim` regions, in the `@` or the `\` spelling.
- A wrap never starts a line with a Doxygen command, a list marker or a tool directive from inside a paragraph. Such a word stays on the line of the word before it.
- A wrap does not break these atoms: a backtick span that starts a word, a run of uppercase hex bytes such as `E2 80 94`, and a number with a size unit such as `264 MB`. The tool counts the width of a character by Unicode 16.0, so a wide character such as `日` takes two columns.
- Refilled lines keep the LF or CRLF line endings of their comment. The tool does not change the bytes outside the comments that it refills.

## Development

[BUILD.md](../../BUILD.md) describes how to build, test and package `cwrap`, and [ARCHITECTURE.md](ARCHITECTURE.md) describes its code.

## Dependencies

The root [vendor/](../../vendor/) directory contains these dependencies, except the libraries in `libs/`. [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists the licenses of the vendored dependencies.

- [libs/](../../libs/): The libraries that `cwrap` shares with the other apps: `core` and `os`, and for the unit tests `testing`. [libs/ARCHITECTURE.md](../../libs/ARCHITECTURE.md) describes them.
- [copt](https://github.com/fardaniqbal/copt): Command-line option parsing.
- [acutest](https://github.com/mity/acutest): Unit tests.

## License

`cwrap` is MIT-licensed, as [LICENSE.txt](https://github.com/unindented/apps-c17/blob/main/LICENSE.txt) states.
