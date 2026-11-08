# Architecture

This document describes the code of `cwrap`: the directories, the wrap pipeline, its fixed-point invariant, and the design decisions. [README.md](README.md) covers usage and wrapping behavior, [BUILD.md](../../BUILD.md) covers the CMake build, and [STYLE.md](../../STYLE.md) covers the code style. [libs/ARCHITECTURE.md](../../libs/ARCHITECTURE.md) describes the shared libraries, which hold the arenas, the strings, the log, the host interface and the out-of-memory report.

## Layout

- [src/app/](src/app/): The process entry point, the command-line interface, the command boundary, and the rewrap of one input.
- [src/domain/](src/domain/): Comment lexing, grouping, filling, Doxygen alignment, source rewriting, and the version.
- [tests/](tests/): The golden and idempotence test suites, and the seeded random test. Each C file under [tests/fixtures/](tests/fixtures/) has a matching file of expected output under [tests/expected/](tests/expected/).
- [cmake/](cmake/): The golden and idempotence test drivers.
- [CMakeLists.txt](CMakeLists.txt) and [CMakePresets.json](CMakePresets.json): The project entry point and the supported CMake configurations.
- [vendor.cmake](vendor.cmake): The build of the bundled dependencies `copt` and `acutest`, whose sources are in the root [vendor/](../../vendor/) directory.

First-party dependencies point inward. `app` can depend on every other directory, and `domain` depends on no other directory in `src/`. Every directory can depend on the libraries in `libs/`, and no library depends on `cwrap`. A first-party include starts at the `src/` root or at the include root of a library, such as `#include "domain/fill.h"` or `#include "core/ascii.h"`.

## Wrap pipeline

`cmd_wrap_run` reserves the memory of the run, gives each input to `wrap_input`, and prints the run's log to `stderr` once. `wrap_input` reads one input, calls `rewrite_source`, and prints the result, writes the file, or reports a change. `rewrite_source` ([src/domain/rewrite.h](src/domain/rewrite.h)) runs these stages over one source buffer:

1. **Lex** (`lex_comment_spans`) finds every comment span outside strings and character literals, and marks a comment after code as trailing.
2. **Group** (`comment_group`) joins line comments into comment blocks, classifies slash-star blocks as starred or hanging, and leaves out the comments that it copies unchanged.
3. **Extract** (`comment_extract_body`) strips the decoration and classifies each body line as prose, tag, list item, sample, decoration, directive or blank.
4. **Align** (`doxygen_align_parameters`) aligns the descriptions of the `@param` tags in one column, and of the `@retval` tags in another.
5. **Fill** (`fill_body_lines`) fills each prose paragraph greedily with the atoms of `atom_split`.
6. **Emit** (`comment_emit`) restores the opener, the prefixes and the closer, with the line endings of the comment block.
7. **Splice** copies the bytes outside the comment blocks unchanged.

A slash-star block on one line that fits the column and has no tab skips the stages from Extract on, and `rewrite_source` copies it unchanged.

### Fixed-point invariant

A rewrap is idempotent. A second rewrap of the output of a first rewrap changes nothing, and `--check` agrees. `--check` works as a CI gate only because of this property. Every closer, prefix and indent that `comment_emit` writes must parse again to the same shape of comment block:

- A starred block whose refilled prose fits on the opener line puts its closer on that line. A closer alone on its line parses again as a line to join.
- A wrapped tag description parses again as a continuation of that tag at any indent, so it refills the same at any width.
- No refilled continuation line starts with a Doxygen command, a list marker or a tool directive. Such a line parses again as a new tag, list item or directive line.

## Design decisions

Each decision states what the code does, why, and what it costs. Read the decision before you change the design, and make sure that the change deals with each reason.

### Memory: fixed reservations for one input at a time

`cmd_wrap_run` reserves the log (64 MiB) on its own. It reserves one more block, and splits it into the input arena (8 GiB) and the scratch (8 GiB). The input arena holds one source and its rewritten output, and the scratch holds the temporaries of that input. Each input gets both arenas by value, so the next input uses the same memory again. The run thus needs memory for its largest input, not for the sum of its inputs. The fixed sizes need no pass over the inputs before the first read. The cost is that an input whose output grows faster than its source can outgrow the arenas and run out of memory. Under `ulimit -v`, `arena_reserve_halving` makes the block smaller, so a smaller input can run out of memory. [libs/ARCHITECTURE.md](../../libs/ARCHITECTURE.md#arenas) describes how an arena reserves, commits and halves its memory.

### One thread

`cwrap` runs on one thread and rewraps one input at a time, because a run has no need to rewrap inputs in parallel. `os_replace` reads the umask by setting it and restoring it, and a diagnostic calls `strerror`. Both are safe only with one thread. The cost is that a run over many large files does not use more than one CPU.

### Out of memory exits the process

When an arena cannot commit more pages, it calls `os_oom`, which `main.c` defines. `os_oom` calls `oom_report`, and then exits with status 1. `cmd_wrap_run` names its log with `oom_set_log`. Before `wrap_input` reads an input, it names the input with `oom_set_job`, so the report names the input that failed. No call site checks for allocation failure, and the domain cannot fail. A stopped run is safe because each in-place write is atomic, so each file is either as it was or completely rewritten. The cost is that a run stops at the first input that does not fit. [libs/ARCHITECTURE.md](../../libs/ARCHITECTURE.md#os) describes the report.

### Every error in one run

Each fallible function records its errors in the log that its caller gives, and `cmd_wrap_run` prints the log once. The log holds every error with no truncation. Check mode and in-place mode report every bad input in one run, and a rejected command line reports only its first error. The user thus sees every input error in one run. The cost is a log whose size depends on the number of inputs, not on their size. The run reserves the log apart from the block that `ulimit -v` makes smaller, so the log keeps its full size.

### In-place mode checks every input before the first write

In in-place mode, a first pass reads every path with `wrap_input_read_file` and writes nothing. If any path is bad, the run writes no file. A bad input among many thus leaves every file as it was, unlike `clang-format -i`, which rewrites the good inputs. The user fixes the bad input and runs the same command again. The cost is a second open and read of each file, which the `fsync` of each written file hides. A write that fails, or a file that changes during the run, still stops the run after it wrote the inputs before it. `os_replace` makes each write atomic and synced, and `cwrap` never writes with `os_write`.

### Seeded random test in the normal suite

`tests/test_random.c` generates source files with the seeded generator of the `testing` library and checks that a second rewrap at the same width changes nothing. The fixed seed, `TEST_RNG_SEED`, makes the test check the same cases on each run, and a failed check names the case. The test checks the [fixed-point invariant](#fixed-point-invariant) on files that nobody wrote by hand, in every test preset and under its sanitizers, with no fuzzing engine. The cost is that the test reaches only the input shapes that its generator lists.
