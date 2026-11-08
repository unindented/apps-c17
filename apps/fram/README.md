<div align="center">
  <img src="media/logo.webp" height="300" alt="">
</div>

<h1 align="center"><code>fram</code> /fɹeɪ̯m/</h1>

`fram` is a static photo and video gallery generator, written in C17. It uses Mustache templates to convert a directory tree of JPEG and MP4 files into album pages, with resized WebP images of each photo and video.

## LLM disclosure

> [!warning]
> This is all vibe-coded slop, produced with Claude Opus 4.8, 5, and 5.5.

## Installation

Download a prebuilt archive from a `fram-v` release on the [Releases](https://github.com/unindented/apps-c17/releases) page, or [build it from source](#development). Each release has one archive for each target, `fram-<version>-<target>.tar.gz`, where the target is `x86_64-linux-musl`, `aarch64-linux-musl` or `aarch64-macos`, and a `SHA256SUMS` file with the SHA-256 digest of each archive. An archive holds `fram-<version>-<target>/bin/fram` and, below `share/doc/fram/`, this README, `LICENSE.txt` and `THIRD_PARTY_NOTICES.md`. The Linux binaries are static `musl` executables. The archives do not include `ffmpeg` and `ffprobe`, which a gallery with MP4 files needs on `PATH`.

mise's [`http:` backend](https://mise.jdx.dev/dev-tools/backends/http.html) can install these archives. A GitHub release publishes no list of versions, so this `mise.toml` names the version:

```toml
[tools."http:fram"]
version = "0.5.0"
checksum_url = "https://github.com/unindented/apps-c17/releases/download/fram-v{{ version }}/SHA256SUMS"
strip_components = 1
bin_path = "bin"

[tools."http:fram".platforms]
linux-x64 = { url = "https://github.com/unindented/apps-c17/releases/download/fram-v{{ version }}/fram-{{ version }}-x86_64-linux-musl.tar.gz" }
linux-arm64 = { url = "https://github.com/unindented/apps-c17/releases/download/fram-v{{ version }}/fram-{{ version }}-aarch64-linux-musl.tar.gz" }
macos-arm64 = { url = "https://github.com/unindented/apps-c17/releases/download/fram-v{{ version }}/fram-{{ version }}-aarch64-macos.tar.gz" }
```

`mise install` does not read `checksum_url`. Run `mise lock` to record the digest of each platform's archive in `mise.lock`. Later installs then refuse an archive that does not match.

## Usage

### Commands

`fram` has two commands:

- `fram build`: Generate the gallery in `output_dir`. This command has these flags:
  - `-c, --config PATH`: Read the configuration from `PATH`. The default is `fram.toml` in the working directory.
  - `-w, --workers N`: Use at most `N` worker threads. `N` must be a positive integer. The default is the number of CPUs that `fram` detects. A phase never runs more workers than it has jobs, which is one for each media file, or one for each album when it renders pages. A build never runs more than four workers for each online CPU.
  - `-n, --dry-run`: Report the changes that the build would make, and make none. See [Dry run](#dry-run).
  - `--force`: Make every output again, and keep nothing from earlier builds. See [Output cache](#output-cache).
  - `-v, --verbose`: Print build progress to `stderr`.
- `fram config`: Print the resolved configuration as TOML. It also takes `-c, --config PATH`.

It also has these options:

- `fram --version` (or `-V`): Print the version.
- `fram --help` (or `-h`): Print the list of commands.
- `fram <command> --help`: Print the help for one command.

A command exits with status 0 when it succeeds, 1 when it fails, and 2 when it rejects the command line.

### Gallery layout

Run `fram` from a directory that has this layout, or name its config with `-c`. The example uses the default directory names:

```text
media/
  Album_One/
    photo-1.jpg
    video-1.mp4
  photo-2.jpeg
static/
  robots.txt
  assets/
    gallery.css
templates/
  partials/
    header.html
    footer.html
  album.html
fram.toml
```

- `media/` holds the media files. A media file is a regular file whose name ends in `.jpg`, `.jpeg` or `.mp4`, in any ASCII letter case, in any directory below `media/`. `fram` ignores every other file in `media/`, so it does not copy a `.png` image or a text file. [Albums and media](#albums-and-media) describes how `fram` makes albums.
- `static/` holds the files that `fram` copies into `output_dir` without changes, at the same relative path. For example, `static/robots.txt` becomes `output_dir/robots.txt`. Put a file that the gallery must publish as it is here.
- `templates/` holds the Mustache templates and their partials, as [Templates](#templates) describes.
- `fram.toml` is the config file, as [Configuration](#configuration) describes.

The media directory and the templates directory must exist. The static directory is optional. The repository and the release archives contain no starter templates or static files, so each gallery supplies its own.

When `fram` searches `media/` and `static/`, it follows symlinks to directories, but it reads each directory only once:

- If more than one path leads to a directory, `fram` uses one of them. That is the path that goes through no symlink, or, if every path goes through a symlink, the first of those paths in byte order. For example, if `album` is a symlink to `trips`, the media files publish only under `trips`.
- `fram` skips a symlink that points back into one of its own parent directories.

### Configuration

`fram.toml` must set `title`, `description` and `author`:

```toml
title = "Family Photos"
description = "Photos and videos of the family"
author = "Daniel Perez Alvarez"
```

This table and the [Templates table](#templates-table) list every key that the config file can contain. `fram` rejects any other key, also inside a derivative table, the `sort` table or the `[templates]` table. For example, a misspelled `ouput_dir` makes the load fail. `fram` does not ignore the key and use the default `output_dir`.

| Key | Default | Bounds | Meaning |
| --- | --- | --- | --- |
| `title` | Required | A string | The gallery title, `site.title` in templates, and the root album's title. |
| `description` | Required | A string | The gallery description, `site.description` in templates. |
| `author` | Required | A string | The gallery author, `site.author` in templates. |
| `base_url` | `""` | Empty, or an `http://` or `https://` URL, in any letter case, with a host, and without `?`, `#`, a space or a control character | The absolute URL of the deployed gallery. `fram` removes any trailing `/`. Empty is the same as absent, and a template reads it as an empty section. |
| `media_dir` | `"media"` | A directory | The directory of the media files. |
| `output_dir` | `"public"` | Not empty | The directory that the build writes to and controls. See [Output directory](#output-directory). |
| `templates_dir` | `"templates"` | A directory | The directory of the templates and partials. |
| `static_dir` | `"static"` | A directory, if it exists | The files that the build copies into `output_dir`. A build skips it when it does not exist. |
| `derivatives` | `s`, `m` and `l` | One to six of `xs`, `s`, `m`, `l`, `xl` and `xxl` | The named derivatives. See [Derivatives](#derivatives). |
| `sort` | `{ albums = "asc", media = "asc" }` | `"asc"` or `"desc"` for each | The order of each album's child albums and of its direct media items, by source filename. |
| `video_frame_seconds` | `1` | 0 to 86400 | The position of the video poster frame, in seconds. A video that is not longer than this uses its midpoint. |
| `templates` | | A table | The `[templates]` table below. |

`fram` resolves each directory, the defaults included, against the directory of the config file, so `-c sub/fram.toml` without `output_dir` writes `sub/public`. It removes trailing `/` characters from each directory, and rejects a directory that is then empty, `/` included. A directory can be an absolute path, or a relative path that contains `..`. `fram config` prints each directory resolved, and leaves out an empty optional string.

The load stops at the first bad key and reports it. The build then stops before it reads anything else.

#### Templates table

The optional `[templates]` table names the template of each kind of page, relative to `templates_dir`. Each key that the table leaves out keeps its default. A diagnostic names a key with its table, such as `config key 'templates.album' must be a safe relative template name: '../x.html'`.

```toml
[templates]
album = "album.html"
aggregates = []
```

| Key | Default | Bounds | Meaning |
| --- | --- | --- | --- |
| `album` | `"album.html"` | A safe template name | The template of each album page. |
| `aggregates` | `[]` | An array of safe template names | The templates that the build renders once, with the root album's context. |

A template name must be a safe relative path. It can contain only letters, digits, `_`, `-`, `.` and `/`, and it cannot contain an empty segment or the segment `.` or `..`. The name of an aggregate template is both its source path below `templates_dir` and its output path below `output_dir`, so a name that contains `/` makes the same subdirectories in `output_dir`. Every template that the build renders must exist. A missing one fails the build with an error that names the template and the key, such as `(for 'templates.album')`.

### Output directory

> [!caution]
> The first build into a directory removes everything in that directory that the build did not write, including hidden files. Set `output_dir` to a directory that you use for nothing else.

A build writes an output over any file that is at its path. A written output gets mode `0666` reduced by the process umask, and a rewritten output does not keep its old mode. It keeps an output that is current, as the [output cache](#output-cache) describes, and leaves its file untouched. After a successful build, the build removes every file and directory below `output_dir` that it did not write. Examples are the pages of removed albums, the outputs of deleted media files and static files, the derivatives of an earlier configuration, and files that you added by hand. If a symlink does not lead to a current output, the build removes the symlink and not its target. A failed build removes nothing. If rendering or writing fails, some outputs can be new while others stay unchanged. A write is not atomic, so an interrupted build can leave one output cut short. The next build writes that output again.

Before a build searches the media directory, `fram` rejects `output_dir` in these cases:

- `output_dir` is at or below `media_dir`, `templates_dir` or `static_dir`. `fram` also makes this check when `output_dir` does not exist yet.
- `output_dir` exists and contains one of those directories or the config file, for example with `output_dir = "."`.

These checks do not find an input directory that is bind-mounted inside `output_dir`. The build removes the contents of such a mount.

Every search of an input directory skips `output_dir`, so a symlink into `output_dir` cannot make the build read generated files as inputs.

Before it renders a page or writes a file, `fram` rejects an output plan that has one of these conflicts:

- Two outputs have the same path. An example is a static file at the path of an album page, a derivative or an aggregate. Paths that differ only in ASCII letter case, such as `assets/Logo.png` and `assets/logo.png`, are the same path, because a case-insensitive filesystem, such as the macOS default, stores them as one file.
- One output path must be both a file and a directory. This check also ignores ASCII letter case.
- An output is at or below `media_dir`, `templates_dir` or `static_dir`, for example through a symlink inside `output_dir`. This check applies also when no file exists at that path yet.
- An output overwrites the config file, a media file, a static file, or any file below `templates_dir`, partials included.

These checks compare the directories and files themselves, not the text of their paths, so they also find a different spelling of the same path or a symlink to it. The build creates `output_dir` only after all of these checks pass, so a rejected build creates nothing.

### Dry run

`fram build --dry-run` runs every phase of a build, probes every media file, and renders every page. It changes nothing in `output_dir`, and it does not create `output_dir`. It makes no derivatives, and only checks which derivatives are current. It prints one line to `stdout` for each path that the build would change or keep. Each line has a mark, a tab and the path. This is a shortened listing:

```text
write	public/album/index.html
keep	public/_fram/v2-s-120x120c-q70-m-1200x800-q85-f1000/album/one-jpg-m.webp
keep	public/_fram/originals/album/one-jpg.jpg
write	public/_fram/output-cache.tsv
remove	public/_fram/v1-s-120x120c-q70-m-1200x800-q85-f1000
remove	public/_fram/v1-s-120x120c-q70-m-1200x800-q85-f1000/album
remove	public/_fram/v1-s-120x120c-q70-m-1200x800-q85-f1000/album/one-jpg-m.webp
```

| Mark | Meaning |
| --- | --- |
| `write` | The build would write this output. |
| `keep` | The build would keep this output, because it is current. |
| `remove` | The build would remove this path, because the current inputs do not produce it. The path can be a directory. |

The `write` and `keep` lines come first, in the order that the build plans them:

1. The outputs of each album, in album order.
2. Aggregates.
3. Static files.
4. The output cache.

The `remove` lines come after them, in sorted order. To get only the paths, for `sort`, `diff` or `comm`, use `cut -f2-`. A dry run reports a failure as a build does, on `stderr` with the same exit code, and prints no listing. With `-v`, the progress messages go to `stderr`, so `stdout` holds only the listing.

A dry run writes nothing, so a build can still fail after a dry run succeeds. A dry run cannot find these failures:

- The build cannot write to `output_dir`, or its disk is full.
- `fram` cannot read a static file or an original. A dry run does not open the files that it would copy.
- An old file is at a path where a new output needs a directory, or the reverse. For example, the old output is the file `public/blog`, and the new output is `public/blog/index.html`.
- `output_dir` exists but is not a usable directory.
- A source image is damaged after its header. A dry run does not decode images.

### Output cache

A build writes the output cache to `_fram/output-cache.tsv` below `output_dir`, also when the build fails after it creates `output_dir`. The next build reads it to decide what to keep:

- The build keeps a derivative while its media file has the path, size and modification time that the cache recorded, and the derivative has the size that the cache recorded. The derivative's path holds its settings, so a changed setting makes a new derivative. The cache also records the versions of libjpeg-turbo and libwebp, so a `fram` built with a new release of either makes every derivative again.
- The build keeps a copied original or static file in the same way, by its source.
- The build renders each page and aggregate on every build, and writes it only when its bytes change.
- The build reuses what a probe read from a media file while the file has the size and modification time that the cache recorded, and the same `fram` version wrote the cache. A probe reads the dimensions, the EXIF orientation and the duration of a video. So the build does not read an unchanged photo or run `ffprobe` for an unchanged video.

Some tools, such as `exiftool -P`, change a file but keep its size and modification time. After such a change, run `fram build --force` once. That build probes every media file and writes every output again.

When you publish `output_dir`, you also publish the output cache. It holds the path of each media file below `media_dir`, with the raw filename that page titles and URL slugs change. It also holds the size and modification time of each media file and static file. To keep this private, exclude `_fram/output-cache.tsv` when you deploy.

### Limits

The build rejects an input file that is too large before it reads the file. The error message gives the limit and the file size. The copy of an original has no size limit of its own, because the build streams the copy from its source.

| Input | Limit |
| --- | --- |
| JPEG source or video poster frame | 2,147,483,647 bytes, just under 2 GiB |
| Config file | 1 MiB |
| Template or partial | 4 MiB |

These limits also apply:

- A source image can have at most 100 million pixels.
- A gallery can have at most 64 partials in the `partials/` directory of `templates_dir`.
- A rendered page can be at most 256 MiB, and a render can use at most 300,000 partial expansions. A render that passes a limit fails.
- An output path below `output_dir` can be at most 1,024 bytes, and each of its segments at most 255 bytes.
- The build does not read an output cache over 256 MiB, and probes every media file again.

The build reserves its memory once, in fixed sizes that come from these limits. Under an address-space limit (`ulimit -v`), it runs fewer workers and then a smaller reservation, as [libs/ARCHITECTURE.md](../../libs/ARCHITECTURE.md#build-memory) describes:

- A build needs about 280 MB (`ulimit -v 270000`). With a lower limit, the build stops at startup with `fram: out of memory`.
- The stacks of the worker threads need room beside the reservation. If the limit leaves too little room, the build fails with `cannot start worker thread`.
- A build can need more memory than it reserved or than the limit leaves. Examples are a template whose nested sections expand without bound, and a decode that is too large for the remaining memory. The build then prints the errors that the command and the failing job logged, prints `fram: out of memory (in '<file>')`, and exits with status 1.
- Without an address-space limit, the build's log and each worker's errors have at least 256 MiB each. A worker that reports more errors than that also ends the build with the out-of-memory report.

## Albums and media

Each directory below `media_dir` becomes a nested album page if it contains media files, or if a directory below it is an album. `media_dir` itself becomes `output_dir/index.html`. For each JPEG, the build makes every configured derivative and copies the original. Each derivative is a lossy WebP image. For each MP4, the build makes the same derivatives from an auto-rotated poster frame, and the copied original is the video that the gallery plays. A gallery with MP4 files needs `ffmpeg` and `ffprobe` on `PATH`. A gallery with only images does not need them.

### Albums

`fram` sorts albums and media items by source filename, in the order that `sort.albums` and `sort.media` set. The default order of both is ascending. So album directories with dates in their names can list the newest album first, while the files in each album stay in the order in which you took them. The cover of an album is the first media item that the album lists. If the album has no media items of its own, its cover is the cover of the first child album that it lists.

The title of an album is its directory name, and the title of a media item is its filename, extension included. `fram` shows each underscore in a title as a space.

### Derivatives

The default derivatives are these:

| Name | Size                  | Crop | Quality |
| ---- | --------------------- | ---- | ------- |
| `s`  | 120 x 120             | Yes  | 70      |
| `m`  | 520 x 360             | Yes  | 80      |
| `l`  | Fits within 792 x 594 | No   | 85      |

This `derivatives` table sets the same defaults:

```toml
derivatives = { s = { width = 120, height = 120, quality = 70, crop = true }, m = { width = 520, height = 360, quality = 80, crop = true }, l = { width = 792, height = 594, quality = 85 } }
```

A `derivatives` table replaces the defaults and does not add to them. So a gallery that adds a derivative such as `xl` must also list each default derivative that it still uses.

A gallery can define one to six derivatives, named after their size: `xs`, `s`, `m`, `l`, `xl` and `xxl`. Each name becomes a field of each media item and an output directory, and a fixed name never hides another key of the template data. Each derivative must set these keys:

- `width` and `height`, from 1 to 16,383 each, which define a bounding box of at most 100 million pixels.
- `quality`, the WebP quality, from 1 to 100.

With `crop = true`, `fram` crops the image around its center to the aspect ratio of the bounding box before it resizes the image. The default value of `crop` is `false`. `fram` does not enlarge a media file that is smaller than the bounding box.

`fram` applies the EXIF orientation before it sizes the derivatives, and then applies the crop policy of each derivative. It decodes each media file at a smaller scale, in eighths. It uses the smallest scale that gives each derivative at least twice its own size to resize from. So it decodes only a fraction of the pixels of a large photo. Each worker decodes one media file at a time, so the memory for decodes grows with `--workers`.

### Output paths

The directory segments of generated albums and the filenames of generated media are lowercase slugs that are safe in a URL. `fram` never renames source files or directories.

Derivatives are at `_fram/<recipe>/<album-slug>/<media-slug>-<derivative>.webp`. The media slug includes the source extension, so `foo.jpg` becomes `foo-jpg`, and all derivatives of one media item are in one album directory. The recipe holds the name, dimensions, crop policy and WebP quality of each derivative, the poster-frame position, and the version of the derivative algorithm. So a build never reuses derivatives of an older recipe after you change a setting that affects the output. Originals are at `_fram/originals/<album-slug>/<media-slug>.<extension>`, where the extension is `.jpg`, `.jpeg` or `.mp4` in lowercase.

Every `url` that `fram` makes is relative and uses the slugs of the output names. So the same output works through `file://`, or on a server below any URL prefix. Each `absolute_url` is the same path below `base_url`, and is empty when `base_url` is not set.

## Templates

Templates use [Mustache](https://mustache.github.io/) syntax and are in `templates_dir`. There are two kinds of templates:

| Kind | Configured by | Rendered |
| --- | --- | --- |
| Album | `templates.album` | Once for each album, to the album's `index.html`, with the album's fields, direct media, child albums and breadcrumbs. |
| Aggregate | `templates.aggregates` | Once, with the root album's context, to the same relative path below `output_dir`. |

`{{value}}` escapes its output for HTML. `{{{value}}}` and `{{& value}}` write it without escaping, and work only for a key that ends in `_html`. `fram` makes no HTML of its own, so it has no such key, and an unescaped tag of any key that the data holds fails the render. So a value such as a title cannot add markup to a page. A name that the data does not hold writes nothing, as with `{{value}}`.

Every page and aggregate that `fram` renders is valid UTF-8 without the characters that XML 1.0 forbids, so a browser and a feed reader accept it. `fram` replaces each invalid sequence in a value or a template with U+FFFD. It drops U+FFFE, U+FFFF, and the C0 control characters other than tab, line feed and carriage return.

Each album, media item and derivative sets each of its fields. An absent value or a false flag is empty, so a section over it is skipped, and the field never resolves from an enclosing album. A name that the album, item or derivative does not have resolves in the enclosing scopes, as in Mustache. A flag such as `is_last` is `true` when it holds and empty otherwise, so it works both as a section and as text.

Every template gets these keys:

| Key | Value |
| --- | --- |
| `site.title`, `site.description`, `site.author`, `site.base_url` | The values from the configuration. |
| `album.title`, `album.url`, `album.absolute_url`, `album.media_count_total`, `album.is_media_count_total_one`, `album.cover`, `album.has_media`, `album.has_sub_albums`, `album.parent` | The fields of the current album, also at the top level. `media_count_total` counts the media items of the album and of every album below it. `is_media_count_total_one` is true when that count is 1, so a template can choose between "item" and "items". `cover` is a media value, with the same named derivatives as `media`, and is empty for an album with no media below it. `parent` is empty for the root album. |
| `sub_albums` | The child albums, each with the album fields. |
| `breadcrumbs` | The albums from the root album to the current album, each with the album fields and `is_last`. `is_last` is true only for the current album on its own page. That crumb's `url` is empty, so the page does not link to itself, and its `absolute_url` still names the page. An aggregate is not an album's page, so none of its crumbs is last. |
| `media` | The media items directly in the current album. Each item has `title`, `kind` (`image` or `video`), `is_image`, `is_video`, `url` (the copied original), `absolute_url`, and `duration`, the length of a video in milliseconds, empty for an image. Each configured derivative is a named child with `url`, `absolute_url`, `width` and `height`, so the defaults give `s.url`, `s.width`, `s.height`, `m.url`, and so on. |
| `generator` | The name and version of `fram`, for a tag such as `<meta name="generator" content="{{generator}}">`. |
| `path_to_root` | The `../` prefix from the page to `output_dir`, empty for a page at the top. A link to a static file is `{{path_to_root}}style.css`. It stays the page's prefix inside `sub_albums`, `breadcrumbs` and `parent`. |

A partial reference `{{> name}}` loads `templates_dir/partials/<name>.html`. A partial name can contain only letters, digits, `_` and `-`. A reference to another name fails the build. `fram` ignores the other files of `partials/` and its subdirectories. The build compiles every template that it renders and every partial once, before it writes anything, and reports in one run every template that it cannot read or compile. The aggregate templates use the same partials.

```html
{{> menu}}
<main>
  <h1>{{album.title}}</h1>
  <ul>
    {{#media}}
    <li><a href="{{url}}"><img src="{{s.url}}" width="{{s.width}}" height="{{s.height}}" alt="{{title}}"></a></li>
    {{/media}}
  </ul>
</main>
{{> footer}}
```

## Host notes

`fram` writes plain files that any static host serves.

GitHub Pages runs Jekyll when it publishes from a branch, and Jekyll drops every file or directory whose name starts with `_`, `.` or `#`. `fram` writes every derivative and every copied original below `_fram/`, so the album pages then show no images and play no videos. Publish the gallery with a GitHub Actions workflow that uploads `output_dir`, or put an empty `.nojekyll` file in `static_dir`. The build removes any file that it did not write, so a `.nojekyll` that you add to `output_dir` by hand does not survive the next build.

## Development

[BUILD.md](../../BUILD.md) describes the prerequisites, the CMake presets, the tests, CI and packaging, [ARCHITECTURE.md](ARCHITECTURE.md) describes the code, and [STYLE.md](../../STYLE.md) describes the code style.

## Dependencies

The root [vendor/](../../vendor/) directory holds these dependencies, except the libraries in `libs/`. [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists the licenses of the vendored dependencies.

- [libs/](../../libs/): The libraries that `fram` shares with the other apps: `cli`, `core`, `os`, `runtime`, `formats`, `output` and `templates`, and `testing` and `os_memory` for the unit tests. [libs/ARCHITECTURE.md](../../libs/ARCHITECTURE.md) describes them.
- [copt](https://github.com/fardaniqbal/copt): Command-line option parsing.
- [tomlc17](https://github.com/cktan/tomlc17): TOML configuration parsing.
- [libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo): JPEG decoding.
- [libwebp](https://chromium.googlesource.com/webm/libwebp): WebP encoding.
- [stb_image_resize](https://github.com/nothings/stb_image_resize): Image resizing.
- [mustache4c](https://github.com/mity/mustache4c): Mustache template rendering.
- [acutest](https://github.com/mity/acutest): Unit tests.

## License

`fram` is MIT-licensed, as [LICENSE.txt](https://github.com/unindented/apps-c17/blob/main/LICENSE.txt) states.
