# Architecture

`fram` is a small C17 static photo and video gallery generator. It reads a TOML config, a directory tree of JPEG and MP4 media, and Mustache templates, and it writes the generated files to a directory that the gallery owns. This document describes the code: the directories and their dependency rules, the build pipeline, the job of each module, the conventions that every module follows, and the design decisions behind them. [README.md](README.md) covers usage and template behavior, [BUILD.md](../../BUILD.md) covers the CMake build, and [STYLE.md](../../STYLE.md) covers the code style.

## Layout

- [src/app/](src/app/): The process entry point `main`, the `cli_spec` that describes the app to the `cli` library, and the commands.
- [src/build/](src/build/): Build orchestration, the parallel phases, templates, and manifest population.
- [src/domain/](src/domain/): Gallery configuration, albums, media items, and the tool name and version.
- [src/media/](src/media/): The boundaries for JPEG, WebP, and EXIF data, and the process and video services on the real host.
- [tests/](tests/): The golden test suite, the seeded random tests, and the test helpers of `fram`: the fixture JPEG encoder and the WebP output decoder. The fixture gallery [tests/fixtures/gallery](tests/fixtures/gallery/), which mixes JPEG and MP4 sources, has its expected output in [tests/expected/gallery](tests/expected/gallery/).
- [CMakeLists.txt](CMakeLists.txt) and [CMakePresets.json](CMakePresets.json): The project entry point and the supported CMake configurations.
- [cmake/](cmake/): The golden test driver. The root [cmake/](../../cmake/) directory holds the CMake profiles, quality tools, packaging, and reusable cross toolchains that every app uses. [BUILD.md](../../BUILD.md) documents their boundaries and policy.
- [libs/](../../libs/): The libraries that `fram` links: `cli`, `core`, `os`, `runtime`, `formats`, `output` and `templates`, and for the unit tests `testing` and `os_memory`. The `libs/` project builds and tests them, and [libs/ARCHITECTURE.md](../../libs/ARCHITECTURE.md) describes their modules.
- [vendor.cmake](vendor.cmake): The build of the bundled dependencies, whose sources are in the root [vendor/](../../vendor/) directory: `copt`, `tomlc17`, `libjpeg-turbo`, `libwebp`, `stb_image_resize`, `mustache4c`, and `acutest`.

### Dependency rules

First-party dependencies point inward:

- `app` can depend on every other directory.
- `build` can depend on `domain` and `media`.
- `domain` can depend on `media`.
- `media` depends on no other first-party directory.
- Every directory can depend on the libraries in `libs/`, and no library depends on `fram`.

A module can skip layers, and modules in the same directory can depend on each other. Vendored code is outside this hierarchy. A first-party include starts at the `src/` root or at the include root of a library that the app links, for example `#include "build/template_context.h"` or `#include "core/ascii.h"`, so every include names the directory or the library that it depends on.

## Terms

These words have one meaning everywhere in this document:

- **Arena.** A `struct Arena` from `core`. It has two pointers, `beg` and `end`, into one region. When the region is reserved and the arena commits pages on demand, it has a third pointer, `limit`. An arena never frees an individual allocation.
- **Perm.** A thread's arena for data that lives longer than the function that makes it. A function takes it by pointer, as `struct Arena* perm`.
- **Scratch.** A thread's arena for temporary data. A function takes it by value, as `struct Arena scratch`, so its allocations are free again when the function returns.
- **Log.** A `struct Log`: a growable buffer with one line for each error, and an error count. A command prints its log once. A log grows in an arena of its own, or in an arena that all users of the log get by pointer, so no callee gets the log's arena by value.
- **Block.** The one reservation that a command makes when it starts. The command splits the block into its arenas, and releases the block when it ends.
- **Region.** A range of address space that one arena uses. Two arenas never use the same region at the same time.
- **Reserve.** To get address space from the system without memory behind it.
- **Commit.** To make reserved pages usable. The system charges committed pages against its memory limit.
- **Build.** One run of `fram build`. [BUILD.md](../../BUILD.md) documents the CMake build of `fram` itself.
- **Keep.** To leave an existing output in place, because it is current. `output_writer_produce` decides, and marks a kept output in the manifest, as it marks a written output. A derivative, a copied original and a static file declare their source's path below its root, size and modification time as attributes, so the writer keeps one that the output cache vouches for without making it. A page and an aggregate declare none, so every build makes them, and the writer keeps one whose bytes did not change.

## Entry point and commands

[main.c](src/app/main.c) defines `main`. `main` only calls `cli_dispatch` with the POSIX host, so the unit tests can link the code that maps an action to an exit code. These are the other modules in `src/app`:

- [cli_spec](src/app/cli_spec.h): `cli_spec_init` tells the [cli](../../libs/src/cli/cli.h) library what fram is: its description and noun, its default config file, its version text, and its `build` and `config` commands. `main` hands it to `cli_dispatch`, which parses the command line, runs the command or prints the version, the help or the diagnostic, and reserves the memory of the text it prints.
- [cmd_build](src/app/cmd_build.h): `cmd_build_run` generates the gallery, as [Build pipeline](#build-pipeline) describes.
- [cmd_config](src/app/cmd_config.h): `cmd_config_print` loads the config that `-c` names, `fram.toml` by default, and prints the resolved config, in the memory that `cli_dispatch` reserves for the `config` command.
- [exit_code](../../libs/src/core/exit_code.h), in the `core` library: Every command returns an `enum ExitCode`: `EXIT_CODE_OK` (0), `EXIT_CODE_FAILURE` (1), or `EXIT_CODE_USAGE` (2).

## Build pipeline

A build runs in two layers:

- `cmd_build_run` is the outer boundary. It gives `build_run` from the `runtime` library its arena sizes. `build_run` resolves the worker count, and reserves the build's memory once, as one block of address space. It splits the block into arenas: the build's log, and a perm and a scratch for the main thread and for each worker. `build_run` calls `cmd_build_execute`, prints the build's errors and then its warnings to `stderr` in one `os_print` call each, and releases the block.
- `cmd_build_execute` owns the `BuildState`. It runs the fourteen phases below in sequence, with the arenas that it gets, and it prints the verbose status lines. It runs the first phase itself, and `run_build_phases` runs the other thirteen. Whatever the outcome, it then releases what mustache4c allocated for the templates.

The phases call these modules in `src/build`:

- `album_scan` builds the album tree and probes the media.
- `output_manifest` plans the output paths.
- `derivative_renderer` generates the media outputs.
- `page_renderer` renders the album pages.
- `site_writer` writes the generated gallery.

The prune is `output_pruner` from the `output` library, which removes what the build did not write. Every change under `output_dir` goes through `output_writer`, which does not make the changes in a dry run.

### Phases

The phase order follows the data dependencies. A template can read media dimensions, album covers, and descendant counts, and none of these values is complete until the build creates the album tree and probes every media item. Thus the build makes every arena-owned string serially, and then runs separate parallel phases to probe media, generate derivatives, and render pages.

1. **Load inputs** (`load_build_inputs`): `site_config_load` reads and validates the config that `-c` names, `fram.toml` by default, and resolves each directory against the config's directory. `read_root_require` requires each configured read root to be a usable directory, and reports the cause when one is not. It skips the optional static root when a gallery does not have one. Then, before the build walks an input tree, `output_manifest_check_output_dir` rejects an `output_dir` that is at or below `media_dir`, `templates_dir`, or `static_dir`, and an `output_dir` that contains one of them or the config file. A root that fails its check still lets the overlap check run, so one run reports both, and the build stops before the walks. Then `walk_files_with_suffixes(paths, media_dir, output_dir, suffixes, ...)` finds and sorts the JPEG and MP4 sources in one walk, every file below `templates_dir`, partials included, in a second walk, and the static files in a third. Each walk of an input tree excludes `output_dir` by its identity, so a symlink into the output tree cannot send the outputs of an earlier build back in as inputs. When an MP4 exists, the phase checks once for `ffmpeg` and `ffprobe`. A failure in this phase stops the build, with one exception: a rejected config key gets its default value, and the build continues. This exception does not apply to a directory key, `media_dir`, `output_dir`, `templates_dir` or `static_dir`, because its default names a directory that the user did not choose.
2. **Load templates** (`load_templates`): `template_set_load` from the `templates` library reads and compiles the album template, every aggregate template and every partial, once, and reports every template that cannot be read or does not compile in one run. The set is read-only from then on, so every worker renders from it.
3. **Build album skeleton** (`build_album_skeleton` -> `album_scan_build_skeleton`): The build walks the sorted media paths serially. It creates the root album and the nested albums, and allocates every string and media record in the main thread's perm. It converts the generated album and media output names to slugs, plans the page and media paths, and builds the flat media array. It then fills the parent, child, cover, and count relationships. Before it selects covers, it reverses the child array and the media array of each album when the configured sort order is descending. The flat media array and the album array stay in source order. A raw directory basename becomes an album title, with each `_` shown as a space. The phase also allocates one video-frame slot for each media item, with its temporary path for a video, and one rendered-page slot for each album.
4. **Load the output cache** (`load_output_cache`): The build reads `_fram/output-cache.tsv` below `output_dir` with `output_cache_load` into the main thread's perm, unless `--force` tells it to ignore the file. It holds a row for each output that an earlier build wrote or kept, and a fact for each source that it probed. If the file is missing, cannot be read, or is in another format, the build uses an empty cache and gives a verbose note. None of these cases is an error.
5. **Probe media** (`probe_media` -> `album_scan_probe_media`): `job_run` runs one job for each media item. Each job stats its source into its media record, and looks up the fact of the source, keyed by its path below `media_dir`. If the fact names the size, the nanosecond modification time and the `fram` version, it fills the probe fields, so the job reads no source and runs no tool. Every other job probes its source:
   - An image job reads the source, probes its JPEG dimensions, reads its EXIF orientation, and calculates its presented and derivative dimensions.
   - A video job probes the duration, and selects the configured frame time, or the midpoint when that time is at or after the end. It extracts one auto-rotated temporary JPEG into its frame slot, and probes the dimensions of that frame.

   Each job reads into a copy of its worker's scratch arena, up to `IMAGE_INPUT_LEN_MAX`, so if a source or a frame is over the limit, only that job fails. After the job probes a source of 16 MiB or more, it gives back the pages of the source with `arena_discard`. Each job writes only to the scalar fields of its media record and to its own frame slot, and it writes its errors to its own log, in the log share of its worker. `pool_run` joins every worker before `job_run` merges the job logs into the build's log. When the build ends, on success and on failure, it removes every frame that a slot still marks as created. An out-of-memory exit skips that cleanup and leaves the current frames in `TMPDIR`, and a running `ffmpeg` child finishes its frame after the exit. Ctrl-C and `kill` leave the frames in the same way, because `src/` installs no signal handlers.

6. **Manifest** (`populate_output_manifest` -> `output_manifest_populate`): Before the build writes a generated file, it records every output path for album pages, derivatives, originals, aggregates and static files, with the attributes that the Keep entry of the glossary describes, and it records the output cache's path last. It rejects four kinds of conflict:
   1. Two producers claim the same path, and `manifest_add` rejects the second claim. Two paths that differ only in ASCII case are the same path, because a case-insensitive filesystem, such as the macOS default, stores them as one file.
   2. One output uses `a/b` and another output uses `a/b/c`. The first path must be a file, but the second path needs a directory at `a/b`. `manifest_find_prefix_collision` finds this conflict after the build records every path. It folds ASCII case in the same way, so `A/B` and `a/b/c` also conflict.
   3. An output names an existing build input. `output_manifest_populate` compares the device and inode of each output with the device and inode of these inputs: the config, every discovered media file and static file, and every file below `templates_dir`. The template tree includes partials and templates that no configuration names. The load inputs phase lists that tree, and the manifest uses the list.
   4. An output is at or below `media_dir`, `templates_dir`, or `static_dir`, even when no file exists there yet. Such an output would come back as an input: the next build would find it as media below `media_dir` or copy it as a static file below `static_dir`, and the same build could read it as a partial below `templates_dir`. In the load inputs phase, `output_manifest_check_output_dir` already compared the device and inode of each root with the nearest existing ancestor of `output_dir`, and with each ancestor of that directory. This rejects an `output_dir` inside a root before any walk, whether `output_dir` exists yet or not. `output_manifest_populate` then compares each root with each existing directory between `output_dir` and the output. This catches a symlink inside `output_dir` that points into a root. These comparisons detect alternate spellings and symlinked roots. A sibling directory such as `photosx` does not match, and the comparisons skip a missing root. This check runs before the input-file check, so for an output below a root, the build reports this conflict.
7. **Prepare output** (`prepare_output_dir`): The build creates `output_dir` through `output_writer_mkdirs`. This is the build's first write, so when an earlier phase stops a build, the build creates nothing, not even an empty `output_dir`. Later writes create their own parent directories. From this point, every change under `output_dir` goes through the build's `OutputWriter`. In a dry run, the writer makes no change: it creates no `output_dir`, writes no file, and records what the prune would remove instead of removing it.
8. **Copy static files** (`copy_static_files`): Each static file goes through `output_writer_produce` with no make function, which keeps the copy that the cache vouches for, and otherwise streams it from its source.
9. **Generate derivatives** (`generate_derivatives` -> `derivative_renderer_generate`): A second `job_run` phase runs one job for each media item. The job passes each derivative and then the original to `output_writer_produce`. The writer keeps an output that the cache vouches for, calls the job's make function for any other derivative, and streams any other original. A dry run makes no derivative whose attributes changed, so it decodes nothing. The first make function that runs for an item decodes its source, and the other derivatives of the item share that decode:
   - For an image, the job reads the source into a copy of its worker's scratch arena, decodes it, and applies the EXIF orientation, in place for an upright source.
   - For a video, the job decodes the retained auto-rotated frame. If a probe fact answered for the video, the job first extracts the frame, so a cached video whose derivatives are current never runs `ffmpeg`.

   libjpeg-turbo scales the decode down in the DCT to the smallest eighth that leaves the source rectangle of every derivative, the whole source or its center crop, at least twice the size of the derivative in each dimension, so a large photo decodes only a fraction of its pixels. Before it decodes, each job reserves from one process-wide decode budget the maximum number of bytes that it holds at one time. This maximum is the memory either to decode and orient the source, including the scaled pixels and the full-resolution coefficients of a progressive JPEG, or to write its largest derivative. The scale counts every derivative, whichever the writer makes, so the bytes of a derivative do not depend on which others were kept. The job holds the reservation until it releases the decoded image, so the budget, not the worker count, bounds the decode memory. After the job decodes a source or a frame of 16 MiB or more, it gives back the pages of the source or frame with `arena_discard`, before it orients the image. The job loops over the ordered derivative definitions, and applies the center-crop policy and the bounding box of each one. It encodes each result as a lossy WebP into a copy of its worker's perm, which the page phase fills only later, and the writer writes it in one write, or keeps the file when its bytes are equal, so an encode failure writes nothing. Each video job removes its temporary frame when it finishes, as a best effort. If the unlink of a frame fails, the frame stays marked for the final cleanup of the build.

10. **Render pages** (`render_album_pages` -> `page_renderer_render_pages`): A third `job_run` phase renders each album through the configured album template into its result slot, in the perm of the worker that renders it. Each render reads the complete immutable album set, and uses the `path_to_root` of the current album to change URLs relative to the output root into URLs relative to the page.
11. **Write** (`write_generated_site` -> `site_writer`): The build produces the album pages first. Then it renders and produces each aggregate template. Each goes through `output_writer_produce`, which keeps a file whose bytes are equal. Each write removes its destination and writes a new file, neither atomically nor synced, so this phase is not transactional at any level. A late template failure can leave new, old, and stale files together, and an interrupted write can leave one output truncated. The next build repairs both: it writes every page whose bytes differ, makes again every output whose size differs from its row, and removes the stale files.
12. **Write the output cache** (`write_output_cache`): `output_cache_write` writes a row for each output with attributes that this build wrote or kept, and a fact for each media item, which `album_scan_probe_fact` formats. This phase runs after a failed phase too, because an output whose write failed is not marked, so it gets no row, and every fact stays correct. If the probe failed, the build stopped before it prepared `output_dir`, so the previous cache does not change.
13. **Remove stale outputs** (`prune_stale_outputs` -> `output_pruner_prune`): After the build writes every output, it removes every directory entry below `output_dir` that is not an output and is not a directory that an output is below, so the tree holds only what the current inputs produce. The prune compares device and inode values, not names, so on a case-insensitive filesystem it does not remove the old spelling of a current output. It sorts the identities once, so each lookup is a binary search. It removes a symlink as a link and never goes into one, so it touches nothing outside `output_dir`. `manifest_builder_check_output_dir` already rejected an input root inside `output_dir`, except one bind-mounted there, which the prune empties. It removes a stale directory after it removes everything in it. When it cannot list or remove a directory entry, it reports the failure and continues. It does not report again a directory that such a failure left non-empty. A failed build never gets to this phase, so it removes nothing. In a dry run, the prune finds no `output_dir` when no earlier build made one, and then it has nothing to remove.
14. **Report the dry run** (`report_dry_run` -> `output_writer_report`): This phase runs only in a dry run. The build prints to `stdout`, in one `os_print` call, a `write` or `keep` line for each output path in manifest order. Then it prints a `remove` line for each path that the prune would remove, sorted bytewise. If an output path is still unmarked, a call site wrote without the output writer. In that case the report fails, so that it does not print a list that leaves out the write.

### Errors and status output

No phase writes a diagnostic to `stderr` itself. The one exception is the out-of-memory report, which `os_oom` prints through `oom_report` before the process exits. Every fallible function records its errors in the log that its caller gives, and `build_run` prints the build's errors and then its warnings in one `os_print` call each.

Until the first write, the build continues after an error wherever the next phase still has something to check. One run reports every bad config key, and every album and source in the album skeleton that is past a path limit. The exception is the sub-albums and sources of an album that the build already reported, because they fail with that album. When the skeleton is complete, the run also reports every source that the build cannot probe, and every output collision. If the build recorded an error, it then stops before it creates `output_dir`. A config that the build cannot read, a rejected directory key, a root that the build cannot read or walk, a missing video tool, or a skeleton error stops the build before the next phase, because it leaves the build nothing to check.

After the first write, a failed phase is the last phase that runs. The derivative phase still generates the outputs of every other media item, and reports each item that fails. The page phase still renders every other album, and reports each album that fails. `copy_static_files` stops at the first static file that fails, and `site_writer` at the first album page or aggregate template that fails. The prune runs only when every write succeeded, and it reports every directory entry that it cannot list or remove.

A parallel phase gives each job a log of its own. After `pool_run` joins the workers, `job_run` appends the logs of the failed jobs to the build's log in job order: at most `JOB_ERROR_REPORT_COUNT_MAX` (20) distinct logs, then a count of the others. A render ends its failure message with `(while rendering '<output>')`, and the comparison of two logs stops at that text, so when every page repeats the same template failure, the build prints that failure once.

Verbose status is separate from diagnostics, and both go through `os_print`. The main thread writes a status line between phases. A worker prints nothing to `stderr` except its progress line and an out-of-memory report. The worker formats the whole progress line (`\r<label> n/total`, such as `probing media 3/12`) before it prints it in one `os_print` call, under a lock that `job.c` owns, so the lines of two workers never interleave.

## Modules

Each item below links to the module's header, which documents its interface. The `src/app` modules are in [Entry point and commands](#entry-point-and-commands).

### Build (`src/build`)

- [album_scan](src/build/album_scan.h): The serial skeleton pass and the first parallel phase. `album_scan_build_skeleton` creates albums and media from the sorted source paths in the caller's arena, plans their output, and completes their relationships. `album_scan_probe_media` fills only the scalar fields below the probe boundary. It fills them from the probe fact of a source when the fact still matches the source, and `album_scan_probe_fact` formats the fact that the next build reads. Otherwise it probes the source, and for a video it creates one temporary JPEG in the frame slot that the build allocated for the video. `album_scan_remove_video_frames` removes the frames that a slot still marks as created.
- [derivative_renderer](src/build/derivative_renderer.h): The second parallel phase. `derivative_renderer_generate` produces each named derivative and the original through `output_writer_produce`, decodes and orients a source image or a retained video frame at most once, when the writer first makes a derivative, and applies the crop and fit rules of each derivative definition. A derivative path holds the recipe of the settings that affect the output, and appends the derivative name to a slug of the full filename, which includes the source extension. Originals use a stable namespace.
- [output_manifest](src/build/output_manifest.h): Builds the [manifest](../../libs/src/output/manifest.h) before the build writes a generated file, through the `output` library's [manifest_builder](../../libs/src/output/manifest_builder.h), with `media_dir`, `templates_dir` and `static_dir` as its roots, each labelled by its config key. `output_manifest_check_output_dir` checks `output_dir` against the input roots before the build walks an input tree. `output_manifest_populate` records every album page, derivative, original, aggregate, and static file output, and rejects the conflicts that the manifest phase lists. The root and input checks compare device and inode values, so they detect alternate spellings, hard links, symlinks, and case-insensitive paths to the same file or directory.
- [page_renderer](src/build/page_renderer.h): The third parallel phase. `page_renderer_render_pages` renders each album into the result slot for its index, in the perm of the worker that renders it. Each render reads the complete album set through a template context that is `const` at every level.
- [site_writer](src/build/site_writer.h): Produces the generated gallery from the rendered results through `output_writer_produce`, which writes a page only when its bytes changed. It produces each album page first. Then it renders and produces the aggregate templates. It stops at the first output that fails to render or write. `copy_static_files` in `cmd_build` copies the static files through the output writer.
- [template_context](src/build/template_context.h): `template_context_render` renders a page template of the build's `TemplateSet` with a `TemplateContext`, through the `templates` library, and resolves each name lazily as the render asks for it: `site.*`, `album`, `sub_albums`, `media`, `breadcrumbs`, `generator`, `path_to_root`, and the fields of the current album and of each listed album, media item and derivative. Unescaped Mustache interpolation works only for a key that ends in `_html`, and fram has none, so an unescaped tag of any value fails the render. A media node looks up the configured derivative names when it renders, so a new `xl` definition gives `xl.url`, `xl.width`, and `xl.height` without a change to the C model.

The build also uses [job](../../libs/src/runtime/job.h) from the `runtime` library, which runs the three parallel phases on the worker pool, and the `output` library: [output_cache](../../libs/src/output/output_cache.h) reads and writes the output cache, [output_path](../../libs/src/output/output_path.h) reports an output path that exceeds a limit and builds the `../` prefix back to the output root for `album_scan`, `output_manifest`, and `site_writer`, [output_writer](../../libs/src/output/output_writer.h) makes every change under `output_dir` for the derivative renderer, the site writer, and the prune, and [output_pruner](../../libs/src/output/output_pruner.h) removes every directory entry below `output_dir` that the manifest does not name and that no output is below, as the prune phase describes, and never enters `media_dir`, `templates_dir` or `static_dir`.

### Domain (`src/domain`)

- [album](src/domain/album.h): Defines `Album`, one generated page and its immediate contents. The album, its strings, its media records, and its pointer arrays live in the arena of the skeleton pass. The album records its raw source directory, generated slug path, parent, immediate children and media, recursive cover and media count, output path, URL, depth, and relative path to the output root.
- [site_config](src/domain/site_config.h): Defines `SiteConfig`, `GalleryDerivative`, and the config load and print operations. The load validates and normalizes each value, and copies it into the arena that the caller gives. It rejects a key that is not in the schema, at the top level or in a derivative or `sort` table, and applies the defaults of optional keys. Later phases use the resolved config and do not change it. `base_url` needs an `http://` or `https://` scheme, which the load compares without regard to case, and a host that is not empty. The load removes trailing `/` characters from `base_url` and from each configured directory. It also validates template names, the ordered table of named derivatives, and the `sort` table, and builds the derivative recipe.
- [media_item](src/domain/media_item.h): Defines `MediaItem`, one JPEG or MP4 source, and `MediaDerivative`, one configured output for that item. The serial skeleton pass fills the source, derivative-array, original, and URL fields. The probe phase fills only the scalar source stat, duration, orientation, and dimension fields. The media derivative array stays aligned with `SiteConfig.derivatives`. `media_item_is_video_path`, which uses `text_has_suffix_folded`, is the one extension test that the skeleton pass and the video-tool check share to classify a source. The test folds ASCII case.
- [fram_version](src/domain/fram_version.h): The tool name and version, which CMake sets when it configures `fram`. The probe facts and the templates record them, so they are in the lowest layer.

### Media (`src/media`)

- [exif](src/media/exif.h): Reads the IFD0 orientation from JPEG EXIF data. When the metadata is missing or malformed, the reader uses the top-left orientation.
- [image](src/media/image.h): Buffer-to-buffer JPEG probing, RGB decoding scaled in the DCT, EXIF-orientation transforms, linear-light resizing, crop views that copy no pixels, WebP encoding, and aspect-ratio calculations. The module wraps libjpeg-turbo for probing and decoding, stb for resizing, and libwebp for encoding, and does no filesystem I/O.
- [proc](src/media/proc.h): Runs one argument vector without a shell, and captures the process output up to a limit. It reads standard output and standard error together, so the child never waits on a full pipe. When the child exits with a failure, the reason ends with the newest standard-error bytes that fit, because the last line of the child usually names the cause. Each run of line breaks shows as `; `, so the error stays on one line. The copy of the argument vector that the spawn takes lives in the caller's scratch arena.
- [video](src/media/video.h): Wraps `ffprobe` to probe the duration, and `ffmpeg` to extract an auto-rotated JPEG frame. `video_frame_position_ms` places the frame for both the probe and a deferred extraction. Each video has a frame slot: a temporary path that the build allocates before the probe, which `video_extract_frame` fills with `mkostemp`, and a flag that marks a frame as created until `video_frame_remove` removes it.

### Formats and templates libraries

`fram` also uses the `formats` library: [toml](../../libs/src/formats/toml.h) validates TOML text at the boundary where parsed bytes become owned text, and checks for unknown keys. The `templates` library compiles the templates and renders every album page and aggregate.

### Host, runtime and core libraries

`fram` takes its host interface, its POSIX host and the out-of-memory report from the `os` library, the worker pool, the directory walk and the check of the input directories from the `runtime` library, and its small primitives from the `core` library. [libs/ARCHITECTURE.md](../../libs/ARCHITECTURE.md) describes their modules. These rules are the ones that `fram` adds:

- `os_read` reads each text file, the config, the templates, the partials and the output cache, with `OS_READ_TEXT`, which rejects a `NUL`, and the media with `OS_READ_BYTES`, because a JPEG contains `NUL` bytes, as [Strings and text input](#strings-and-text-input) describes.
- `os_print` carries every byte that `fram` prints: the log at the end of a command, a rejected command line, `--help` and `--version`, the resolved config, the verbose status lines, and the workers' progress lines.
- The build runs its jobs on the worker pool, because decoding a JPEG, resizing and encoding a WebP, and running `ffmpeg` take time for each file.
- The walk excludes `output_dir`, and skips a second alias of a directory, so an aliased directory publishes one copy of its media.
- `output_manifest` records each static file at its path below `static_dir` with `manifest_builder_add_copies`, and `copy_static_files` copies it to the same path below `output_dir`.

## Cross-cutting conventions

Every module follows these rules. When a design decision is behind a rule, the link goes to its reasons and costs.

### Memory

- Every first-party allocation comes from an arena, except decoded pixels, and each thread has a perm and a scratch. The build's log has an arena of its own. `template_context_render`, `image_encode_webp`, and `os_read` return their result in the caller's perm. See [Memory: arenas only](#memory-arenas-only) and [Memory: a perm and a scratch for each worker](#memory-a-perm-and-a-scratch-for-each-worker).
- A command reserves its memory once, when it starts. It splits the block into arenas of fixed sizes, and the input and render limits bound those sizes. An arena commits pages only when its allocations get to them, and then commits one mebibyte more than the allocation needs. Only committed pages count against the system's memory. See [Memory: reserve, then commit on demand](#memory-reserve-then-commit-on-demand).
- A `struct Image` owns its pixels on `malloc`, and `image_free` is the only place where a caller frees memory. The decode budget bounds the pixels. See [Decoded pixels stay on `malloc`](#decoded-pixels-stay-on-malloc).
- An allocation that does not fit calls `os_oom`, which reports and exits, so no call site checks for allocation failure. The vendored libraries that still use `malloc`, and the pixel allocations, also end in `os_oom`. See [Out of memory exits the process](#out-of-memory-exits-the-process).
- Under the address sanitizer, two arena allocations are never adjacent, so code that extends the last allocation in place uses `arena_extend`. See [Address sanitizer coverage in arenas](#address-sanitizer-coverage-in-arenas).
- A log's arena is never an arena that a callee gets by value, so the log never grows into bytes that a callee uses. See [A log must not grow in a borrowed arena](#a-log-must-not-grow-in-a-borrowed-arena).

### Threads and ownership

Two jobs that run at the same time never share an arena or a log. Each job uses the arenas of the worker that runs it, and each worker has its own arenas. A page job renders into its worker's perm, and every other job allocates only in a copy of its worker's scratch. Before the phases that use them, the serial skeleton pass and the main thread make every allocation in the main thread's perm, including the frame slots. A worker gets immutable shared context, except for the manifest marks of the outputs that it writes or keeps, and it owns one index in each mutable result array. `pool_run` joins every worker before the main thread reads the result slots, and the rendered pages stay in the worker arenas until the write phase. See [Memory: a perm and a scratch for each worker](#memory-a-perm-and-a-scratch-for-each-worker).

### Strings and text input

Every first-party string is a counted `struct Str` with a signed `ptrdiff_t` length, and JPEG bytes travel as one too. Every length, count, and pixel dimension is a `ptrdiff_t`. See [Counted strings with signed lengths](#counted-strings-with-signed-lengths).

`os_read` reads the media with `OS_READ_BYTES` and accepts any bytes, because JPEGs contain `NUL` bytes. No text format gives a meaning to a `NUL` byte, so the text consumers stop a `NUL` before it gets into the generated output. The config, template, partial and output cache reads use `OS_READ_TEXT`, which rejects an embedded `NUL`. `toml_datum_is_text` rejects a TOML string that contains a decoded `NUL`. `toml_require_known_keys` rejects a TOML key that contains a `NUL`, before it names the key. A path that contains a `NUL` fails at the `os` boundary with `EINVAL`, because a system call would read the path only up to that byte.

### Errors and diagnostics

- A fallible action whose caller needs a message beyond success or failure takes a final `struct Log* log`. It records every error that it finds in that log, and never truncates one. A message is one `printf` format that the compiler checks. See [A growable log, and every error in one run](#a-growable-log-and-every-error-in-one-run) and [Log messages use checked format strings](#log-messages-use-checked-format-strings).
- A filesystem, image, process, or video action also takes the `operation` of the caller, such as `failed to read config`, and logs the whole message, because only the action knows which path failed. An image or process action also takes the file that the bytes or the child belong to, and names that file at the end. A failure inside a file names the file as `(in '<file>')` through `log_error_in`.
- An action, and a producer that can fail, return `0` or `-1`, and the producer writes its result through an `_out` parameter. A producer that cannot fail returns its result directly, for example a `struct Str` by value. A predicate returns `bool`. Running out of memory is never a return value. [STYLE.md](../../STYLE.md#function-signatures) gives the full rules.
- A first-party diagnostic is lowercase prose. It names the failed operation first, puts literal names and values in single quotes, and puts a value after the cause. Messages from the system or from a vendored library use their original capitalization and punctuation. [STYLE.md](../../STYLE.md#errors-and-diagnostics) gives the full rules.

### Paths and limits

- **Path safety.** Template names must be safe relative paths, and partial names use a more restrictive identifier grammar. Media source paths retain their filesystem bytes, but generated album segments and media filenames are lowercase ASCII slugs. A URL relative to the output root is the output path without change, because every byte of a slug is unreserved in a URL, and the template context puts the `path_to_root` of the current page before it. The build checks configured output names before it joins them to `output_dir`. These text checks prevent `.` and `..` traversal. Three checks cover symlink aliases that already exist, each at its own boundary: the identity checks of the input roots, the exclusion of `output_dir` from the walks, and the identity checks of the inputs.
- **Input size limits.** Each caller of `os_read` gives a limit for the kind of file that it reads. The read checks the limit before it allocates, so it never loads a file that is too large. `album_scan` and `derivative_renderer` read media with `IMAGE_INPUT_LEN_MAX` from `image`. This limit is `INT_MAX`, far more than a JPEG within the pixel limit needs, and a read of that size fits in the worker's scratch. `TOML_CONFIG_LEN_MAX` in the `formats` library is 1 MiB for the config. `TEMPLATE_FILE_LEN_MAX` in the `templates` library is 4 MiB for each template and each partial. `os_write` streams an original from the file itself, so an original has no size limit.
- **Path limits.** The build checks each generated output path against two limits. `OUTPUT_PATH_RELATIVE_LEN_MAX` limits the whole relative path, and `FILENAME_LEN_MAX` limits each path segment. `output_path_check_limits` reports a failure of either limit in the same way for every producer.

### Host access

No code in `src/` makes a system call on a path or prints through `stdio`, except the video code: only the POSIX host, `os/os_posix.c` in `libs/`, does. `proc` and `video` start `ffprobe` and `ffmpeg`, and `video` creates, inspects, and removes the temporary frame that `ffmpeg` writes. They do this on the real host, whichever host an executable links. All other host access goes through `os.h`, and the link step selects the host for each executable. A new unit test links the in-memory host, unless it needs a fact that only a real filesystem has, or it starts a process. See [The `os` interface](#the-os-interface).

## Design decisions

Each decision below states what the code does and why, and some decisions also state what they cost. Read the decision before you change one of these designs, and make sure that the change deals with each of its reasons. The decisions come in groups, in this order: memory, strings, logs and their messages, host access, tests, and where the shared code lives.

### Memory: arenas only

**Decision.** All first-party memory comes from arenas, and the code never frees an individual allocation. Decoded pixels are the one exception, with a decision of their own below. A scratch goes by value, so its memory is free again when the function returns. A perm goes by pointer.

**Reasons.**

- No comment needs to say which code frees a buffer. Only a `struct Image` documents its release.
- The bounds check is one comparison, and the comparison cannot overflow.
- A `Buf` can grow in place when it is the last allocation.
- An arena is one region, not a list of chunks, so a callee that allocates in a by-value copy leaks nothing.

### Memory: a perm and a scratch for each worker

**Decision.** Each pool worker has its own perm and its own scratch, and a job uses the arenas of the worker that runs it. A page job renders its page into a copy of the perm, and stores the copy back as the perm only when the render succeeds. Every other job allocates only in a copy of its worker's scratch. The serial skeleton pass allocates the albums, the media records, and the frame slots in the main thread's perm before the parallel phases start. The build's log has an arena of its own, and `job_run` splits the main thread's scratch into one share of job logs for each worker.

**Reasons.**

- A bump arena is not thread-safe, and separate arenas need no lock.
- A failed render leaves nothing in the arena.
- A job reads its source or frame with `IMAGE_INPUT_LEN_MAX` as the size limit, and a read of that size fits in the worker's scratch, so if a source grew past the limit after the walk, only that job fails.
- Arenas belong to threads, not to albums or media items. A gallery has thousands of media items, and a cap large enough for the largest item, once for each item, would need too much address space.

### Memory: reserve, then commit on demand

**Decision.** `build_run` reserves the build's memory once and halves it under an address-space limit, as [libs/ARCHITECTURE.md](../../libs/ARCHITECTURE.md#build-memory) describes. Every thread's perm is `BUILD_PERM_LEN` (4 GiB) and its scratch `BUILD_SCRATCH_LEN` (3 GiB), and the log is `BUILD_LOG_LEN` (256 MiB), sizes that come from `IMAGE_INPUT_LEN_MAX`, `IMAGE_ENCODED_LEN_MAX`, the template and render limits and the expansion limit. Decoded pixels are not part of the reservation, as [Decoded pixels stay on `malloc`](#decoded-pixels-stay-on-malloc) describes. A worker's arenas live longer than its jobs, so `arena_discard` gives back the pages of a source of `JOB_SOURCE_DISCARD_LEN_MIN` (16 MiB) or more after it is probed, and again after it is decoded, where a `malloc` design would free it.

**Reasons.** A job does not give back the pages of a source under `JOB_SOURCE_DISCARD_LEN_MIN`, because the worker's next job reads into the same pages, and faulting them in again would cost more than the read. The derivative phase gives back a large source after it decodes the source and reads its orientation. The pixels hold all that the derivatives need, and the worker's scratch lives longer than the job. Without the discard, the pages of the source would stay resident next to the decodes of other workers, and next to the second buffer of decoded size for a rotated photo.

### Out of memory exits the process

**Decision.** When an arena cannot commit more pages, when the system refuses even the smallest reservation, or when the allocation of a vendored library or of decoded pixels fails, the program calls `os_oom`. Each program defines `os_oom`. The `fram` version, in [src/app/main.c](src/app/main.c), calls `cli_oom_exit` from [cli/cli_dispatch.h](../../libs/src/cli/cli_dispatch.h) with the name `fram`, and `cli_oom_exit` does these steps:

1. It takes a lock, so the first of two workers that run out at once prints its report and exits while the second waits.
2. It prints the report of `oom_report` from [os/oom.h](../../libs/src/os/oom.h): the errors in the command's log and in the thread's job log, then `fram: out of memory` and the name of the file that the thread was processing, as `(in '<file>')`.
3. It exits with `EXIT_CODE_FAILURE` through `_Exit`, which runs no `atexit` handler that another thread could be using.

`oom_set_log` and `oom_set_job` name what the report prints. `build_run` from [runtime/build_run.h](../../libs/src/runtime/build_run.h) names the build's log with `oom_set_log`, as `cli_dispatch` names the log of the `config` command. A job names its file and its log with `oom_set_job` when it starts: a probe or derivative job names its source, and a page job the source directory of its album. A main-thread phase names its file, with no job log, as it goes: each static file that it copies, the output cache, each album page that it writes, and each aggregate template.

**Reasons.**

- `fram` is a CLI tool, and it is easy to make its output directory again.
- No call site checks for allocation failure.
- The report names the file that the thread was working on. A user needs that name to find the input that failed.
- A failed pixel allocation ends the build like any other allocation failure, and does not end only one job, so the build has one failure policy and no `NULL` check after a pixel allocation.

**Vendored code.** mustache4c, tomlc17, libjpeg-turbo, and libwebp still use `malloc`. `fram` replaces tomlc17's allocator with one that calls `os_oom` when `realloc` fails. When mustache4c fails and reports no reason, the wrapper treats the failure as out of memory and calls `os_oom`. Both image codecs report a failed allocation as an error, and `image_decode` and `image_encode_webp` change this error into a call to `os_oom`. The hooks of the stb resizer allocate through `image_alloc`, which calls `os_oom` when `malloc` fails.

**Limit.** The exit skips the build's cleanup of temporary video frames, and it does not stop a running `ffmpeg` child. Ctrl-C and `kill` leave them in the same way, because `src/` installs no signal handlers. With a temporary directory for each build, the cleanup would have only one directory to remove.

### Address sanitizer coverage in arenas

**Decision.** When `fram` is compiled with the address sanitizer, an arena over a reserved region poisons the pages that it commits, and `arena_alloc` unpoisons only the bytes that it returns. Such an arena leaves a poisoned gap before each allocation, so two of its allocations are never adjacent, and code that extends the last allocation in place uses `arena_extend`. An arena over memory that the caller owns has no gap.

**Reasons.** ASan cannot see an overflow that stays inside one large mapping. Without the poisoning, the debug preset loses most of its overflow checks.

**Limits.**

- The arena does not poison scratch memory again when a function returns, so ASan does not find a use of scratch memory after the function that owned it returned.
- When a job's copy of its worker's arenas commits, it poisons the free space again, so a job that allocates past the usable part does not read what an earlier job allocated there.
- `arena_release` leaves the shadow memory to the sanitizer's `munmap` interceptor, which clears it, so a later mapping at the same addresses gets no old poison. Unpoisoning the whole region instead would write an eighth of each reservation as resident shadow memory.

### Decoded pixels stay on `malloc`

**Decision.** A `struct Image` owns its pixels on `malloc`: the output of `image_decode`, `image_orient`, and `image_resize_region`. The working memory of libjpeg-turbo, such as its row buffers and the coefficients of a progressive JPEG, is also on `malloc`, and so are the encoder buffers of libwebp. The resizer buffers of stb use `image_alloc` through the hooks in [src/media/stb.c](src/media/stb.c). `image_free` is the only way for a caller to release a `struct Image`, and `struct Image` is the one type whose memory a caller frees. `image_alloc` calls `os_oom` instead of returning `NULL`. Everything else on the image path is in arenas: the source bytes and the encoded WebP, in the worker's scratch arena, and every path and message.

**Reasons.**

- An arena gets memory back only when its region is released. If the buffers of the decode were in a worker's scratch, the largest decode of each worker would stay committed for the rest of the build. At the measured peaks for a 100-megapixel 4:2:2 photo, that is up to 302 MiB for a baseline JPEG decoded at full size, and 683 MiB for a progressive one, so about 2.9 GiB or 6.7 GiB with 10 workers. A progressive CMYK one peaks at about 1.05 GiB.
- The decode budget would stop working. It counts a reservation as returned when the job calls `image_free`, but the pages would stay committed in the worker's region, so the worker count would bound memory again.
- Pixels are large and short-lived, the code allocates them once for each file, and the decode budget bounds them. `malloc` handles that case well, and a bump arena handles it badly.
- The exception has one type, `struct Image`, and two files, `media/image.c` and `media/stb.c`. [STYLE.md](../../STYLE.md#fram) allows `malloc` and `free` only in those two files, and in the allocator wrapper that `formats/toml.c` gives to tomlc17.
- Pixels do not go in a worker's region with a primitive that decommits it. Under strict overcommit, `madvise(MADV_DONTNEED)` leaves the pages charged, so the code would need `mmap(MAP_FIXED, PROT_NONE)` to return them. A stale by-value copy whose `end` is above the mark would then write into `PROT_NONE` pages and crash, but `arena.h` documents that a copy which makes pages usable again changes nothing. libwebp has no allocator hooks, so its memory could get into an arena only if the code patched its `malloc` calls, and the budget would have to wait for the decommit. All of this would remove only about eight `malloc` and `free` calls from one module. `arena_discard` avoids the hazard: it maps the pages again as readable and writable, so they stay usable, and it applies to a buffer that the caller names, not to the free space of an arena.
- Pixels get no fixed share of the reservation. The largest decode, of a 100-megapixel progressive CMYK JPEG, peaks at about 1.05 GiB, and the budget leaves room for two such decodes, so a fixed share would refuse any build under a `ulimit -v` below about 3 GB, including galleries of 24-megapixel photos.
- The decode budget does not scale with what the arenas leave, because that would change how many decodes run at the same time on a limited system, and it would need a second planner, based on decode memory.

**Cost.** The pixels get what the arenas leave. Under a tight `RLIMIT_AS`, a large decode that would fit without the arenas can end the build with the out-of-memory report, and the report names the decode whose allocation failed, which is not necessarily the large one.

**Revisited** with the scaled decode of libjpeg-turbo, which bounds the pixels of a baseline JPEG by its largest derivative and not by `IMAGE_PIXEL_COUNT_MAX`. A 100-megapixel 4:2:2 baseline JPEG decoded at an eighth peaks at 20 MiB. Pixel scratch for each worker, sized from the configuration, is still not reasonable:

- A progressive JPEG decode holds the coefficients at full resolution at every scale. The same photo as a progressive JPEG peaks at 401 MiB at an eighth, and a progressive CMYK one peaks at about 800 MiB.
- A gallery can configure a derivative up to `IMAGE_ENCODED_DIMENSION_MAX`, so some galleries get no reduction at all.
- The scale changes in steps of an eighth, so a decode can be larger than twice its largest derivative by an eighth of the source in each dimension.

Instead, the scaled decode makes the budget count bytes, so a decode reserves what it holds, not the pixel count of its source.

### Counted strings with signed lengths

**Decision.** Every first-party string is a `struct Str` from [str.h](../../libs/src/core/str.h): a pointer and a `ptrdiff_t` length, for both owned and borrowed text. JPEG bytes also travel as a `struct Str`. Every length, count, and pixel dimension is a `ptrdiff_t`. A value changes to `size_t` only at a libc, POSIX, or vendored call, or in a C buffer that goes to such a call. A pixel dimension changes to `int` only at a call into stb or libwebp, and to the unsigned `JDIMENSION` of libjpeg-turbo only at a call into libjpeg-turbo. Each such change asserts that the value is not negative, unless this is true by construction. A C string exists only where libc, POSIX, or a vendored API needs one. The POSIX host copies each path into a `PATH_MAX` buffer at the system call, with `copy_cstr`, `error_system_message` fills a caller's buffer through `strerror_r`, and `video` builds each `ffprobe` and `ffmpeg` argument with `str_cstr`.

**Reasons.**

- Subtraction cannot wrap to a very large value.
- A `NUL` byte is an ordinary byte, so no code calls `strlen` on owned text.

**Deliberate.** The `NUL` checks in [Strings and text input](#strings-and-text-input) are a product decision, and the string type does not force them. If the product decision changes, you can remove those checks and the tests that pin them.

### A growable log, and every error in one run

**Decision.**

- Each fallible function whose caller needs a message takes `struct Log* log` as its last parameter, and a callback type puts its `void* userdata` after the log. The log holds every error, with no truncation.
- After an error, the build continues while the next phase still has something to check. It stops before its first write if the log holds an error.
- The log records the errors of the phases in phase order, and the errors of one phase in walk order or job order, so the order does not change with the number of workers.
- A parallel phase prints at most `JOB_ERROR_REPORT_COUNT_MAX` (20) distinct job errors, and a count of the others.
- A rejected config value gets its default, so one bad value causes one error. The build accepts a `derivatives` table only when every entry is valid, and a rejected `base_url` becomes empty.

**Reasons.**

- A user sees every error in one run.
- No message buffer has a fixed size, so no message is truncated, and no message must be early enough in the order to survive truncation.

### A log must not grow in a borrowed arena

**Decision.** A `struct Log` and its `Buf` live in an arena that no callee gets by value. The build gives each log an arena of its own, or an arena that all users of the log get by pointer.

**Reason.** A `Buf` grows in place. If a callee gets a copy of the same arena by value, the callee allocates the same bytes that the log grows into. The two then write over each other, with no crash and no ASan report.

[STYLE.md](../../STYLE.md#memory-and-resources) states the rule. Treat a break of the rule as a bug, not as a style problem.

### Log messages use checked format strings

**Decision.** A log call takes one `printf` format and its arguments. `log_error`, `log_error_in`, `buf_printf`, and `buf_vprintf_measured` have `__attribute__((format(printf, ...)))`. A `struct Str` prints with `%.*s` and `STR_FMT`, and an integer prints with the conversion for its type. For example:

```c
log_error(log, "failed to read template: %s ('%.*s')", reason, STR_FMT(template_path));
```

**Reasons.**

- Each message is one sentence in the source, so a reader reads its wording on one line, and `grep` finds it.
- The compiler checks the number and the types of the arguments against the format. `-Wformat=2` is on for first-party code and for the libraries.
- `%td` prints a `ptrdiff_t` with no cast. Only a string needs a helper, `STR_FMT`.
- Each first-party function that formats a message has the attribute, so its callers get the same check.

**Costs and limits.**

- `STR_FMT` evaluates its argument two times. Give it a variable or another lvalue, and put a computed string in a variable first.
- A string longer than `INT_MAX` bytes causes an assertion failure at run time, because the `%.*s` precision is an `int`.
- The attribute is the only guard on the code that reads the arguments. Each formatting function measures the text with `vsnprintf`, allocates the text, and writes it with a second `vsnprintf`. These functions are `buf_printf` in `core`, which grows the buffer in place through `buf_vprintf_measured`, and the first-party functions that format their own message. Only those functions change the measured `int` length to a `ptrdiff_t`.
- A first-party variadic function does not pass its `va_list` on to a formatter that copies the list with `va_copy`. If it did, the Clang analyzer, version 22, would report a false `va_end` on an uninitialized list when it targets AArch64 Linux, where a `va_list` is a structure. Instead, the function starts its list once for each `vsnprintf` pass, as `buf_printf` does, which hands its second pass to `buf_vprintf_measured`. `record_error` in the `cli` library, `verbose_printf` in the `runtime` library, and `test_log_is` in `testing/test_support.c` follow this rule, as `render_fail` in the `templates` library does.
- `%.*s` stops at a `NUL` byte. The text readers reject a `NUL` in config and template content, `toml_datum_is_text` and `toml_require_known_keys` reject a `NUL` in TOML strings and keys, and the `os` boundary rejects a `NUL` in paths, so a message gets no `NUL` from text input. No message prints JPEG bytes. The `os` boundary's `EINVAL` message quotes a path that contains a `NUL` only up to that byte, and the contract test for this case shows the result.
- An `off_t` has no conversion of its own. It prints with `%jd` and a cast to `intmax_t`.
- `clang-format` breaks a string literal that is longer than the column limit, so some long messages are in two literals.

### The `os` interface

**Decision.** All host access goes through [os/os.h](../../libs/src/os/os.h) in the `os` library, except in `proc` and `video`. `os/os_posix.c` is the POSIX implementation. [os/os_memory.c](../../libs/src/os/os_memory.c) is an in-memory implementation for unit tests: an in-memory tree with symlinks, hard links, a modification-time counter, injected failures, and captured output. The link step selects one implementation for each executable. A function that accesses the host takes `struct Os*` just before its arenas, and an `os_*` function takes it first. `proc` and `video` run `ffprobe` and `ffmpeg`, and handle the frame that `ffmpeg` writes, on the real host, whichever host an executable links, because a fake would have to emulate the file output of `ffmpeg`. The video tests write their fake tools to the POSIX host.

**Reasons.**

- Tests of the walk, the manifest, the derivatives, the pages, the writes, and the prune need no temporary directories.
- A link-time choice needs no table of function pointers, because no executable needs two hosts.

**Linking.** `fram` and every unit test registered without `OS memory` link `os_posix`, the object library of `os/os_posix.c`. The tests registered with `OS memory` link `os_memory`, as [BUILD.md](../../BUILD.md#fram-tests) lists.

**Condition.** Each property that a test checks against the in-memory host must have a contract test. [os/test_os.c](../../libs/src/os/test_os.c), in the `libs/` project, compiles once against each host and runs the same cases, and both executables must pass, so every property that a test relies on in the in-memory host is also true on POSIX. Without the contract tests, the in-memory host can accept what POSIX rejects. For example, an in-memory host could accept `file/` as a file where POSIX returns `ENOTDIR`, and a contract test checks this case.

**Writes.** `os_write` writes either bytes or a file that it streams in 64 KiB pieces. `os_write`, `os_mkdirs`, `os_remove` and `os_remove_dir` are the only operations that change the host. A copy is an `os_write` from a file, and has no operation of its own. `os_write` removes a file, symlink or hard link at the destination without following it and writes a new file, so a link there never carries an output onto another file, such as a photo. The write is neither atomic nor synced, so an interrupted build can leave a truncated file. An interrupted build leaves the previous output cache, which does not keep such a file, because its size differs from its row, and whose probe facts still hold. The next build repairs such a file, which is cheaper than a temporary-file protocol. `os_stat` reports the size and modification time that the output attributes name.

### Seeded random tests in the normal suite

**Decision.** `tests/test_random.c` checks the parsers of untrusted file headers, the EXIF reader and the JPEG probe, on generated inputs. A well-formed EXIF segment reads back the orientation that the test built it with, and a mutated EXIF segment or JPEG header reads without a crash or a read past the input. The test draws its inputs from the generator in the `testing` library, a 64-bit linear congruential step, with a fixed seed (`TEST_RNG_SEED`), so the test checks the same cases every time. A failing check names the case.

**Reasons.**

- The pass runs in every test preset, under the sanitizers of that preset, with no fuzzing engine.
- A fixed seed makes the suite deterministic, so every failure reproduces.

**Limit.** The pass reaches only the input shapes that its generators list. An EXIF fuzzing harness would search past these shapes, but the tree does not have one yet.

### Where the shared code lives

**Decision.** The libraries in [libs/](../../libs/) hold the code that `fram` shares with the other apps. `core` holds the generic parts: the arena with reserve, commit, and poisoning, `Buf`, `Str`, `Log`, and the small primitives. `os`, `runtime`, `cli`, `formats`, `output` and `templates` hold the modules that more than one app uses in the same form. Each program defines `os_oom`, and has its own arena layout. The decoded-pixel exception stays out of the libraries.

**Reason.** These parts do not depend on what a program does.
