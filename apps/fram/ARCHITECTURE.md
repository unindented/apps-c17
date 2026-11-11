# Architecture

`fram` is a static photo and video gallery generator in C17. It reads a TOML config, a directory tree of JPEG and MP4 files, and Mustache templates, and it writes the gallery to `output_dir`. This document describes the directories, the build pipeline, the modules and the design decisions. [README.md](README.md) describes usage and template behaviour, [libs/ARCHITECTURE.md](../../libs/ARCHITECTURE.md) the shared libraries, [BUILD.md](../../BUILD.md) the CMake build, and [STYLE.md](../../STYLE.md) the code style.

## Layout

- [src/app/](src/app/): The entry point `main`, the `cli_spec` that describes `fram` to the `cli` library, and the `build` and `config` commands.
- [src/build/](src/build/): The build phases, the template context and the output manifest.
- [src/domain/](src/domain/): The configuration, albums, media items, and the tool name and version.
- [src/media/](src/media/): The JPEG, WebP and EXIF code, and the process and video code that runs `ffprobe` and `ffmpeg`.
- [tests/](tests/): The seeded random tests, the fixture JPEG encoder and the WebP output decoder. The golden test builds [tests/fixtures/gallery](tests/fixtures/gallery/), which has JPEG and MP4 sources, and compares the result with [tests/expected/gallery](tests/expected/gallery/).
- [cmake/](cmake/): The golden test driver. The root [cmake/](../../cmake/) directory holds the CMake files that every app uses, as [BUILD.md](../../BUILD.md) describes.
- [CMakeLists.txt](CMakeLists.txt) and [CMakePresets.json](CMakePresets.json): The project and its CMake configurations.
- [vendor.cmake](vendor.cmake): The build of the dependencies in the root [vendor/](../../vendor/) directory.
- [libs/](../../libs/): The libraries that `fram` links: `cli`, `core`, `os`, `runtime`, `formats`, `output` and `templates`, and `testing` and `os_memory` for the unit tests.

### Dependency rules

First-party dependencies point inward:

- `app` can depend on every other directory.
- `build` can depend on `domain` and `media`.
- `domain` can depend on `media`.
- `media` depends on no other first-party directory.
- Every directory can depend on the libraries in `libs/`, and no library depends on `fram`.

A module can skip layers, and modules in the same directory can depend on each other. A first-party include starts at the `src/` root or at the include root of a library, for example `#include "build/template_context.h"` or `#include "core/ascii.h"`. So each include names the directory or library that it depends on.

## Build pipeline

`cmd_build_run` gives the arena sizes to `build_run` from the `runtime` library. `build_run` reserves the memory, calls `cmd_build_execute`, prints the log, and releases the memory. `cmd_build_execute` runs the first phase, and `run_build_phases` runs the other phases, in this order:

1. **Load inputs** (`load_build_inputs`): Load the config, check the input directories and `output_dir`, walk the media, template and static trees, and check for `ffmpeg` and `ffprobe` when the gallery has an MP4.
2. **Load templates** (`load_templates`): Compile the album template, each aggregate template, the not-found template and each partial once.
3. **Build album skeleton** (`build_album_skeleton`): On the main thread, make the albums, the media records, their output paths, covers and counts, and one frame slot for each video.
4. **Load the output cache** (`load_output_cache`): Read `_fram/output-cache.tsv`, unless `--force` is set. A missing or unusable cache is empty, not an error.
5. **Probe media** (`probe_media`): In parallel, take each source's dimensions, orientation and duration from its probe fact, or probe the source and extract a video's poster frame.
6. **Manifest** (`populate_output_manifest`): Record every output path, and reject the conflicts that the [README](README.md#output-directory) lists.
7. **Prepare output** (`prepare_output_dir`): Create `output_dir`. This is the first write.
8. **Copy static files** (`copy_static_files`): Copy each static file that is not current.
9. **Generate derivatives** (`generate_derivatives`): In parallel, make each derivative that is not current, and copy each original that is not current.
10. **Render pages** (`render_album_pages`): In parallel, render each album page into an arena of the worker that renders it.
11. **Write** (`write_generated_site`): Write the album pages, then render and write the aggregates and the not-found page. A file whose bytes did not change stays.
12. **Write the output cache** (`write_output_cache`): Write a row for each output that the build wrote or kept, and a probe fact for each media item. This phase runs after a failed phase too.
13. **Remove stale outputs** (`prune_stale_outputs`): Remove each entry below `output_dir` that the build did not write. The prune compares device and inode values, so on a case-insensitive filesystem it does not remove a current output whose name changed only in case.
14. **Report the dry run** (`report_dry_run`): Print the `write`, `keep` and `remove` listing. Only a dry run has this phase.

The order follows the data. A page can show media dimensions, album covers and counts, and these are complete only after the skeleton and the probe. So the main thread makes every string of the skeleton first, and the parallel phases only fill in fields and slots.

When the build ends, it removes each temporary video frame that is still there. An out-of-memory exit, Ctrl-C or `kill` leaves the frames in `TMPDIR`, because `src/` installs no signal handlers. A running `ffmpeg` child also finishes its frame after such an exit.

### Errors and status output

Until the first write, the build goes on after an error while the next phase has something to check. So one run reports each template that does not compile, each album and source past a path limit, each source that it cannot probe, and each output conflict. Then the build stops before it creates `output_dir`. An invalid config, an input directory that the build cannot read or walk, a missing video tool, or a skeleton error stops the build at once, because the next phase has nothing to check. The config load stops at its first error.

After the first write, a failed phase is the last phase that runs. The derivative and page phases finish their other jobs and report each job that fails. `copy_static_files` and `site_writer` stop at the first file that fails. The prune runs only when every write succeeded. It reports each entry that it cannot list or remove, and goes on.

A parallel phase gives each job its own log. After the workers finish, `job_run` appends the failed jobs' logs in job order, so the order does not change with the worker count. It prints the distinct logs up to a cap, and a count of the others, as [Logs](../../libs/ARCHITECTURE.md#logs) describes. A render failure ends with `(while rendering '<output>')`, and the comparison of two logs stops at that text. So when every page has the same template failure, the build prints it once.

No phase prints a diagnostic, except the out-of-memory report. `build_run` prints the build's errors and then its warnings. With `--verbose`, the main thread prints a status line between phases, and each worker prints its progress line, such as `probing media 3/12`.

## Modules

Each header documents its module's interface.

- [cli_spec](src/app/cli_spec.h): Tells the `cli` library the description, default config file, version text and commands of `fram`.
- [cmd_build](src/app/cmd_build.h): Runs `fram build`, as [Build pipeline](#build-pipeline) describes.
- [cmd_config](src/app/cmd_config.h): Loads the config and prints it resolved.
- [album_scan](src/build/album_scan.h): Builds the album skeleton, probes the media, formats the probe facts, and removes the video frames.
- [derivative_renderer](src/build/derivative_renderer.h): Makes the derivatives and copies the originals through the output writer.
- [output_manifest](src/build/output_manifest.h): Checks `output_dir` against the input directories, and records every output path with its attributes.
- [page_renderer](src/build/page_renderer.h): Renders each album page in a worker.
- [site_writer](src/build/site_writer.h): Writes the album pages, and renders and writes the aggregates and the not-found page.
- [template_context](src/build/template_context.h): Resolves each template key when a render asks for it, so a new derivative name needs no change to the C model.
- [album](src/domain/album.h): `Album`, one generated page and its contents.
- [site_config](src/domain/site_config.h): `SiteConfig`, its load, its print, and the derivative recipe.
- [media_item](src/domain/media_item.h): `MediaItem`, one JPEG or MP4 source, and `MediaDerivative`, one of its outputs.
- [fram_version](src/domain/fram_version.h): The tool name and version, which CMake sets.
- [exif](src/media/exif.h): Reads the EXIF orientation of a JPEG, and uses top-left when the data is missing or malformed.
- [image](src/media/image.h): Probes and decodes JPEGs, orients, crops and resizes images, and encodes WebP, with no filesystem access.
- [proc](src/media/proc.h): Runs a child process without a shell, and captures its output up to a limit. When the child fails, the message ends with the newest bytes of its standard error, on one line, with each run of line breaks shown as `; `.
- [video](src/media/video.h): Probes a video's duration with `ffprobe`, and extracts an auto-rotated JPEG frame with `ffmpeg`.

## Shared libraries

[libs/ARCHITECTURE.md](../../libs/ARCHITECTURE.md) describes the code that `fram` shares with the other apps:

- The arenas, strings, buffers and logs: [core](../../libs/ARCHITECTURE.md#core).
- The out-of-memory report: [`os_oom`](../../libs/ARCHITECTURE.md#os_oom).
- The host interface, its POSIX and in-memory hosts, and the contract tests: [os](../../libs/ARCHITECTURE.md#os).
- The memory reservation of a build: [Build memory](../../libs/ARCHITECTURE.md#build-memory).
- The worker pool, the parallel jobs and the directory walk: [runtime](../../libs/ARCHITECTURE.md#runtime).
- The output manifest, the output writer, the output cache and the prune: [output](../../libs/ARCHITECTURE.md#output).
- The template set and its renders: [templates](../../libs/ARCHITECTURE.md#templates).

`fram` adds these rules:

- `os_read` reads the media with `OS_READ_BYTES`, because a JPEG contains `NUL` bytes. It reads the config, the templates, the partials and the output cache with `OS_READ_TEXT`, which rejects a `NUL`.
- Each walk of an input tree excludes `output_dir`, and skips a second path to a directory, so an aliased directory publishes one copy of its media.
- A static file is an output at its path below `static_dir`, and `copy_static_files` copies it to the same path below `output_dir`.

## Conventions

### Threads and ownership

Each worker has a perm, an arena for data that outlives a job, and a scratch, an arena for temporary data. Two jobs that run at the same time never share an arena or a log. Each job uses the arenas of the worker that runs it. A page job renders into its worker's perm, and every other job allocates only in a copy of its worker's scratch. The main thread makes every shared allocation before the parallel phases, the frame slots included. A worker reads shared data that does not change, marks only the outputs that it writes or keeps, and owns one index in each result array.

### Paths

A template name must be a safe relative path, and a partial name is a stricter identifier. A media source path keeps its filesystem bytes, but the album and media output names are lowercase ASCII slugs. Every byte of a slug is safe in a URL, so a URL below the output root is the output path itself. The template context puts the page's `path_to_root` before it. `output_path_check_limits` checks each output path against `OUTPUT_PATH_RELATIVE_LEN_MAX` and each segment against `FILENAME_LEN_MAX`.

### Host access

No code in `src/` makes a system call on a path or prints through `stdio`, except `proc` and `video`. All other host access goes through `os.h`. A new unit test links the in-memory host, unless it needs a fact that only a real filesystem has or it starts a process. [STYLE.md](../../STYLE.md#errors-and-diagnostics) gives the rules for diagnostics.

## Design decisions

Each decision says what the code does, why, and what it costs. Read the decision before you change the design, and make sure that the change deals with each reason.

### Memory: arenas belong to workers

**Decision.** Each worker has its own perm and scratch. `BUILD_PERM_LEN` (4 GiB) and `BUILD_SCRATCH_LEN` (3 GiB) come from the input, encode and render limits, so one scratch holds the largest source beside its encoded WebP. A job gives back the pages of a source of `JOB_SOURCE_DISCARD_LEN_MIN` (16 MiB) or more with `arena_discard`, after it probes the source and again after it decodes it. **Why.** A bump arena is not thread-safe, and separate arenas need no lock. If a source grows past the limit after the walk, only its job fails. A gallery has thousands of media items, and an arena for each item would need too much address space. **Cost.** A worker's arenas live longer than its jobs, so a source under 16 MiB stays committed after its job. The worker's next job reads into the same pages, which costs less than a new fault of each page.

### Decoded pixels stay on `malloc`

**Decision.** A `struct Image` owns its pixels on `malloc`, and `image_free` releases them. The working memory of libjpeg-turbo and libwebp is also on `malloc`, and the stb resizer allocates through `image_alloc`. A failed allocation in any of them calls `os_oom`. [STYLE.md](../../STYLE.md#fram) allows `malloc` and `free` only in `media/image.c`, `media/stb.c` and the tomlc17 allocator wrapper. **Why.** An arena gives memory back only when its region is released, so each worker would keep its largest decode committed for the rest of the build. A progressive JPEG holds its coefficients at full resolution at every decode scale, so the scaled decode does not bound its memory. A fixed share of the reservation for pixels would refuse galleries of small photos. **Cost.** The pixels get what the arenas leave, and decode memory grows with the worker count. Under a tight `ulimit -v`, a large decode can end the build with the out-of-memory report, and the report names the decode whose allocation failed, which is not always the large one.

### Out of memory exits the process

**Decision.** `os_oom` in [src/app/main.c](src/app/main.c) calls `cli_oom_exit` with the name `fram`, which prints the report and exits with status 1, as [libs/ARCHITECTURE.md](../../libs/ARCHITECTURE.md#os_oom) describes. The report names the file that the thread works on. A probe or derivative job names its source, and a page job names its album's source directory. The main thread names each static file, album page, aggregate, the not-found page and the output cache when it writes them. **Why.** `fram` is a CLI tool, and a user can make the output again. No call site checks for allocation failure, and a failed pixel allocation ends the build like any other allocation. **Cost.** The exit skips the removal of temporary video frames and does not stop a running `ffmpeg` child.

### Video tools run on the real host

**Decision.** `proc` and `video` run `ffprobe` and `ffmpeg`, and create, read and remove the temporary frame, on the real host, whichever host an executable links. **Why.** A fake host would have to emulate the file output of `ffmpeg`. **Cost.** The video tests cannot use the in-memory host, and write their fake tools to the real filesystem.

### Every error after the config in one run

**Decision.** Each fallible function records every error in its caller's log and returns `-1`, and the build goes on until the first write while a phase has something to check. **Why.** A user sees every error after the config in one run, and the order of errors does not depend on the worker count. **Cost.** A phase that runs after a failure must not need what the failure left out. For example, an output path does not depend on probed dimensions, so the manifest can check every output after a probe failed.

### Seeded random tests in the normal suite

**Decision.** [tests/test_random.c](tests/test_random.c) checks the EXIF reader and the JPEG probe on generated inputs, from a fixed seed (`TEST_RNG_SEED`). A well-formed EXIF segment reads back its orientation, and a mutated segment or header reads without a crash or a read past the input. **Why.** The tests run in every test preset, under its sanitizers, with no fuzzing engine, and each failure reproduces. **Cost.** The tests reach only the input shapes that their generators make.
