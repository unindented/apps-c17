<div align="center">
  <img src="media/logo.webp" height="300" alt="">
</div>

<h1 align="center"><code>sharedstuff</code></h1>

`sharedstuff` is some stuff I use in different C17 projects. It is a static library with an arena allocator, counted strings, growable buffers, and an error log.

## LLM disclosure

> [!warning]
> I used LLMs extensively to create this project, mostly Claude Opus 4.8 and 5.

## Installation

Build the `release` preset, and then install the static library, its headers, and its CMake package files under a prefix:

```sh
cmake --preset release
cmake --build --preset release -j 8
cmake --install build/release --prefix /desired/prefix
```

Instead of installing the library, a project can build it from source, as [Linking with CMake](#linking-with-cmake) describes.

## Usage

### Linking with CMake

The CMake target of the library is `SharedStuff::SharedStuff`, a static library. A project can add the source tree of the library to its own build:

```cmake
add_subdirectory(path/to/sharedstuff sharedstuff)
target_link_libraries(MyTarget PRIVATE SharedStuff::SharedStuff)
```

Or the project can find the installed package:

```cmake
find_package(SharedStuff 0.1 CONFIG REQUIRED)
target_link_libraries(MyTarget PRIVATE SharedStuff::SharedStuff)
```

These variables configure the build. Set them with `-D`, or set them before the call to `add_subdirectory`:

| Variable | Default | Meaning |
| --- | --- | --- |
| `SHAREDSTUFF_BUILD_TESTING` | On for a top-level build, off otherwise | Builds the tests. If a parent project turns it on, the parent project must call `enable_testing()` at its top level. |
| `SHAREDSTUFF_INSTALL` | On for a top-level build, off otherwise | Adds the install rules. |
| `SHAREDSTUFF_SANITIZER` | `none` | Instruments the build with `address` (AddressSanitizer and UndefinedBehaviorSanitizer) or `thread` (ThreadSanitizer). The library target passes the link flags of the sanitizer to each target that links the library. |

### Defining `os_oom`

When an allocation does not fit in an arena, the arena calls `os_oom`. This function must not return. The library does not define `os_oom`, because only the application knows which message to print and which exit status to use when it runs out of memory. Each program that links the library must define `os_oom` once:

```c
_Noreturn void os_oom(void) {
  fputs("myprogram: out of memory\n", stderr);
  _Exit(EXIT_FAILURE);
}
```

## Modules

The library has four modules. Each module builds on the modules before it:

1. An arena allocates memory.
2. A string is a view of that memory.
3. A buffer grows a string in an arena.
4. A log is a buffer of error messages.

### Arenas

`<shared/arena.h>` defines `struct Arena`, which allocates memory in sequence from one region. If a program frees each allocation separately, it must decide for each pointer who owns it and when to free it, and each allocation can fail. An arena does not free allocations one at a time, and its allocation functions never return `NULL`. If an allocation does not fit, the arena calls [`os_oom`](#defining-os_oom). Data lives as long as its region, unless the program allocated the data through a copy of the arena and then dropped that copy.

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

`<shared/str.h>` defines `struct Str`, which holds a pointer and a signed `ptrdiff_t` length. A C string marks its end with a `NUL`, so a substring needs a copy. The lengths of C strings are unsigned, so `a - b` can wrap to a very large value. A substring of a `struct Str` is a view of the original bytes, a `NUL` is an ordinary byte, and arithmetic on lengths cannot wrap. The zero value, `(struct Str){0}`, is the empty string.

| Call | Effect |
| --- | --- |
| `S("literal")` | Makes a string from a literal. |
| `STR_INIT("literal")` | Initializes a string in a static table. |
| `str_from_cstr(cstr)` | Wraps a C string that comes from outside the program. |
| `str_head(s, n)`, `str_tail(s, n)` | Return a view of the first `n` bytes, or of the bytes after them. |
| `str_cut(s, c)` | Splits `s` at the first `c` into a `head` view and a `tail` view. Sets `is_found` if `s` contains `c`. |
| `str_eq`, `str_compare`, `str_startswith`, `str_endswith` | Compare two strings. |
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

`<shared/buf.h>` defines `struct Buf`, which grows a string in an arena. A program often builds output, such as a rendered page or a message, from many small appends, and does not know the total size in advance. While the buffer is the last allocation in its arena, the buffer grows in place, so a large output usually needs no copies. Start a buffer with the arena that it grows in, append to it, and then read it back as a string:

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

`<shared/log.h>` defines `struct Log`, which holds a buffer of error lines and a count of the errors. A fixed-size error buffer truncates long messages and holds only the first error. With a log, a program can do these things:

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
| `log_append(log, source)` | Copies the lines of `source` into `log`, and adds the error count of `source` to the count of `log`. For example, after the workers finish, copy the log of each worker into the main log. |

A wrapper that adds context formats its own message in scratch memory. Then it records the message with `log_error(log, "%.*s", ...)`.

To use a log correctly:

- Make the log the last parameter of a function that can fail. When the function records an error, return `-1`.
- Put the log in an arena that you never pass by value. The simplest choice is a region that only the log uses.
- Give each thread its own log. After the threads finish, merge the logs with `log_append` in a fixed order, so that the output does not depend on the order in which the threads run.
- `log_error` and `log_error_in` have the `format` attribute. Give the same attribute to each wrapper that formats its own arguments.

## Development

### Prerequisites

- A POSIX-like system with CMake 3.25 or later and a C17 compiler such as Clang or GCC.
- Optional build tools: `ninja` for the `multi` preset.
- Optional quality tools: `cwrap`, `clang-format`, `clang-tidy`, `cppcheck`.

The source tree includes the dependency in [vendor/](vendor/).

### Building

Each preset builds in `build/<preset>`, outside the source tree, and treats compiler warnings as errors. If `CC` is set, the preset builds in `build/<preset>/<CC>` instead, such as `build/debug/gcc-14`. CMake reads `CC` only when it configures a directory for the first time, so each compiler needs its own CMake cache. The commands use eight parallel jobs. Change that number to suit your machine.

```sh
cmake --preset debug
cmake --build --preset debug -j 8
```

| Preset | Configuration | Library |
| --- | --- | --- |
| `debug` | `Debug` with AddressSanitizer and UndefinedBehaviorSanitizer. It runs `clang-tidy` and `cppcheck` during compilation if they are available. | `build/debug/libSharedStuff.a` |
| `tsan` | `Debug` with ThreadSanitizer. | `build/tsan/libSharedStuff.a` |
| `release` | `RelWithDebInfo`, with no tests. | `build/release/libSharedStuff.a` |
| `multi` | `Debug` and `RelWithDebInfo` in one build tree, with the `Ninja Multi-Config` generator. Build with `--preset multi-debug` or `--preset multi-relwithdebinfo`. | `build/multi/Debug/libSharedStuff.a`, `build/multi/RelWithDebInfo/libSharedStuff.a` |

`clang-tidy` runs only with Clang, because it rejects the warning options of GCC. To run linting, configure the `debug` preset, and then run this command:

```sh
cmake --build --preset lint
```

### Testing

CTest registers the unit tests and the compile tests. The compile tests check that the compiler rejects a bad call to `buf_printf`, `log_error`, or a wrapper with the same `format` attribute. Build a preset, and then run its tests:

```sh
ctest --preset debug -j 8
```

The test presets are `debug`, `tsan`, `multi-debug`, and `multi-relwithdebinfo`. The `multi-relwithdebinfo` tests check the same CMake configuration as the `release` preset, which builds no tests. To run only part of a suite, select the tests by label or by name pattern:

```sh
ctest --preset debug -L unit -j 8
ctest --preset debug -R arena -j 8
ctest --preset debug -R buf -j 8
```

### Continuous integration

The workflow presets run the same configure, build, and test steps as CI:

| Workflow | Steps |
| --- | --- |
| `cmake --workflow --preset ci-debug` | Build in `Debug`, run linting, and run all ASan and UBSan tests. |
| `cmake --workflow --preset ci-tsan` | Build with TSan and run all TSan tests. |
| `cmake --workflow --preset ci-release` | Build in `RelWithDebInfo`. |
| `cmake --workflow --preset ci-multi` | Build and test in `Debug` and `RelWithDebInfo` with the `Ninja Multi-Config` generator. CI runs this workflow a second time with `CC=gcc-14` to check GCC. |

To run the Linux workflows on a machine with Podman, build the pinned Ubuntu image from [Containerfile](Containerfile). Then run each workflow in a new container:

```sh
podman build --tag sharedstuff-linux-ci --file Containerfile .
podman run --rm sharedstuff-linux-ci ci-debug
podman run --rm sharedstuff-linux-ci ci-tsan
podman run --rm sharedstuff-linux-ci ci-release
podman run --rm sharedstuff-linux-ci ci-multi
podman run --rm -e CC=gcc-14 sharedstuff-linux-ci ci-multi
```

The image contains a snapshot of the source tree, including the vendored dependency, without `.git/` and `build/`, which [.containerignore](.containerignore) excludes, so the Linux build products stay in the container and do not go into the `build/` directory of the host. The image supports x86-64 and AArch64 hosts, and pins LLVM 23 and GCC 14.

## Dependencies

[vendor/](vendor/) contains this dependency. [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists its license.

- [acutest](https://github.com/mity/acutest): Unit tests.

## License

`sharedstuff` is MIT-licensed, as [LICENSE.txt](LICENSE.txt) states.
