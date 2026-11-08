<div align="center">
  <img src="media/logo.webp" height="300" alt="">
</div>

<h1 align="center"><code>sosig</code> /ˈsɔsɪdʒ/</h1>

`sosig` is a static site generator, written in C17. It uses Mustache templates to convert Markdown and Org files into HTML pages.

## LLM disclosure

> [!warning]
> This is all vibe-coded slop, produced with Claude Opus 4.8, 5, and 5.5.

## Installation

Download a prebuilt archive from a `sosig-v` release on the [Releases](https://github.com/unindented/apps-c17/releases) page, or build it from source as [BUILD.md](../../BUILD.md) describes. Each release has one archive for each target, `sosig-<version>-<target>.tar.gz`. The target is `x86_64-linux-musl`, `aarch64-linux-musl` or `aarch64-macos`. The release also has a `SHA256SUMS` file with the SHA-256 digest of each archive. An archive holds `sosig-<version>-<target>/bin/sosig`, and below `share/doc/sosig/` it holds this README, `LICENSE.txt` and `THIRD_PARTY_NOTICES.md`. The Linux binaries are static `musl` executables.

The [`http:` backend](https://mise.jdx.dev/dev-tools/backends/http.html) of mise can install these archives. A GitHub release publishes no list of versions, so this `mise.toml` names the version:

```toml
[tools."http:sosig"]
version = "0.3.0"
checksum_url = "https://github.com/unindented/apps-c17/releases/download/sosig-v{{ version }}/SHA256SUMS"
strip_components = 1
bin_path = "bin"

[tools."http:sosig".platforms]
linux-x64 = { url = "https://github.com/unindented/apps-c17/releases/download/sosig-v{{ version }}/sosig-{{ version }}-x86_64-linux-musl.tar.gz" }
linux-arm64 = { url = "https://github.com/unindented/apps-c17/releases/download/sosig-v{{ version }}/sosig-{{ version }}-aarch64-linux-musl.tar.gz" }
macos-arm64 = { url = "https://github.com/unindented/apps-c17/releases/download/sosig-v{{ version }}/sosig-{{ version }}-aarch64-macos.tar.gz" }
```

`mise install` does not read `checksum_url`. Run `mise lock` to record the digest of each archive in `mise.lock`. After that, an install refuses an archive that does not match.

## Usage

### Commands

`sosig` has two commands:

- `sosig build` generates the site in `output_dir`. It has these flags:
  - `-c, --config PATH`: Read the configuration from `PATH`. The default is `sosig.toml` in the working directory.
  - `-w, --workers N`: Use at most `N` worker threads. `N` must be a positive integer. The default is the number of CPUs. The build never runs more workers than content files, or more than four workers for each online CPU.
  - `-n, --dry-run`: Report the changes, but do not make them. See [Dry run](#dry-run).
  - `--force`: Make every output again, and keep nothing from earlier builds. See [What a build keeps](#what-a-build-keeps).
  - `-v, --verbose`: Print build progress to `stderr`.
- `sosig config` prints the resolved configuration as TOML. It also takes `-c, --config PATH`.

These options print information and exit:

- `sosig --version` (or `-V`) prints the version, and the grammars that the binary has for [syntax highlighting](#syntax-highlighting).
- `sosig --help` (or `-h`) prints the list of commands.
- `sosig <command> --help` prints the help for one command.

The exit status is 0 on success, 1 on failure, and 2 when `sosig` rejects the command line.

### Site layout

Run `sosig` from a directory with this layout, or give the path of its config with `-c`. The example uses the default directory names:

```text
content/
  post-1.md
  post-2.md
  diagram.svg
static/
  assets/
    site.css
  robots.txt
templates/
  partials/
    header.html
    footer.html
  atom.xml
  content.html
  index.html
sosig.toml
```

- `content/` holds the content files and their assets. A content file is a regular file whose name ends in `.md` or `.org`, in any letter case, at any depth below `content/`. For example, `post.md`, `post.MD` and `notes.Org` are content files. Each content file is one entry. Every other file below `content/` is an asset, and `sosig` copies it as a static file. For example, `content/diagram.svg` becomes `output_dir/diagram.svg`, so `post-2.md` can show it with `![Diagram](diagram.svg)`. [URLs and slugs](#urls-and-slugs) tells when such a link works.
- `static/` holds files that `sosig` copies into `output_dir` without changes, at the same relative path. For example, `static/robots.txt` becomes `output_dir/robots.txt`. Put a file that one entry uses, such as an image, next to that entry in `content/`. Put a file that the whole site uses, such as a stylesheet, in `static/`.
- `templates/` holds the Mustache templates and their partials. See [Templates](#templates).
- `sosig.toml` is the config file. See [Configuration](#configuration).

> [!caution]
> `sosig` publishes every file below `content/`. This includes hidden files such as `.DS_Store`, a `.git/` directory, editor backups such as `notes.org~`, and Org archives such as `notes.org_archive`. Keep a private file outside `content/`.

The content directory and the templates directory must exist. The static directory is optional. The repository and the release archives have no starter templates or static files, so each site supplies its own.

When `sosig` searches `content/` and `static/`, it follows symlinks to directories, but it reads each directory only once:

- If more than one path leads to a directory, `sosig` uses the path that goes through no symlink. If every path goes through a symlink, it uses the first path in byte order. For example, if `blog` is a symlink to `posts`, the entries publish only under `posts`.
- `sosig` skips a symlink that points to one of its own parent directories.

### Configuration

`sosig.toml` must set `title`, `description`, `author` and `base_url`:

```toml
title = "C17 Notes"
description = "Notes on C17, Markdown and Org"
author = "Author Name"
base_url = "https://www.example.com"
```

This table and the [Templates table](#templates-table) list every key that the config file can contain. `sosig` rejects any other key. For example, the misspelled key `ouput_dir` stops the load. `sosig` does not ignore it and use the default `output_dir`.

| Key | Default | Bounds | Meaning |
| --- | --- | --- | --- |
| `title` | Required | A string | The site title, `site.title` in templates. |
| `description` | Required | A string | The site description, `site.description` in templates. |
| `author` | Required | A string | The site author, `site.author` in templates. |
| `base_url` | Required | An `http://` or `https://` URL with a host, without `?`, `#`, a space or a control character. The scheme can be in any letter case. | The absolute URL of the site, for feed links and `absolute_url`. `sosig` removes any trailing `/`, so a template can append a path that starts with `/`. |
| `permalink` | `"/{section}/{slug}.html"` | A pattern with `{slug}` that expands to a safe relative path | The URL and output path of each entry. See [URLs and slugs](#urls-and-slugs). |
| `content_dir` | `"content"` | An existing directory | The directory of the content files. |
| `output_dir` | `"public"` | Not empty | The directory that the build writes to and controls. See [Output directory](#output-directory). |
| `templates_dir` | `"templates"` | An existing directory | The directory of the templates and partials. |
| `static_dir` | `"static"` | A directory, if it exists | The directory of files that the build copies without changes. The build skips it if it does not exist. |
| `feed_count` | `25` | 0 or more | The maximum number of entries in each feed. |
| `frontmatter_ignored_keys` | `[]` | An array of strings | More keys that Markdown frontmatter can contain. `sosig` accepts any value for them and does not read it. For denote notes, set it to `["identifier", "signature"]`. |
| `templates` | | A table | The `[templates]` table below. |

`sosig` resolves each directory, the defaults included, against the directory of the config file. So `-c sub/sosig.toml` without `output_dir` writes `sub/public`. It removes trailing `/` characters from each directory, and rejects a value that is then empty, `/` included. A directory can be an absolute path, or a relative path that contains `..`. `sosig config` prints each directory after it resolves it.

The load stops at the first bad key. A config error stops the build before it searches `content_dir`.

#### Templates table

The optional `[templates]` table names the template of each kind of page, relative to `templates_dir`. Each key that the table leaves out keeps its default, and `sosig` rejects any other key in it. A diagnostic names a key with its table, such as `config key 'templates.content' must be a safe relative template name: '../x.html'`.

```toml
[templates]
content = "content.html"
aggregates = ["index.html"]
feeds = ["atom.xml"]
```

| Key | Default | Bounds | Meaning |
| --- | --- | --- | --- |
| `content` | `"content.html"` | A safe relative template name | The template for each entry. The `template` field of an entry overrides it. |
| `aggregates` | `["index.html"]` | An array of safe relative template names | Templates that the build renders once, with every entry that is not a draft. |
| `feeds` | `["atom.xml"]` | An array of safe relative template names | Templates that the build renders once, with the newest `feed_count` entries. |

A template name, in the table or in the `template` field of an entry, must be a safe relative path. It can contain only letters, digits, `_`, `-`, `.` and `/`. It cannot contain an empty segment, or the segment `.` or `..`. The name of an aggregate or feed template is its source path below `templates_dir` and also its output path below `output_dir`, so a name with `/` creates subdirectories in `output_dir`. If `aggregates` or `feeds` is an empty array, the build renders no template of that kind. Every template that the build renders must exist. A missing one fails the build with an error that names the template and the key, such as `(for 'templates.content')`.

### Output directory

> [!caution]
> The first build into a directory removes everything in it that the build did not write, hidden files included. Set `output_dir` to a directory that you use for nothing else.

A build replaces any file at the path of an output. A written output gets mode `0666` reduced by the process umask, and a rewritten output does not keep its old mode. It leaves an output that would not change as it is, as [What a build keeps](#what-a-build-keeps) describes. After a successful build, the build removes every file and directory below `output_dir` that it did not write. This removes, for example, the pages of deleted or renamed entries, the outputs of removed templates, removed static files and assets, and files that you added by hand. For a symlink that does not lead to a current output, the build removes the symlink and does not change its target. A failed build removes nothing. If a render or a write fails, some outputs can be new while others stay old. A write is not atomic, so an interrupted build can leave one output cut short. The next build writes that output again.

Before a build searches the content directory, `sosig` rejects `output_dir` in these cases:

- `output_dir` is at or below `content_dir`, `templates_dir`, or `static_dir`, even if `output_dir` does not exist yet.
- `output_dir` exists and contains one of those directories or the config file, for example with `output_dir = "."`.

Each search of an input directory skips `output_dir`, so a symlink into `output_dir` cannot make the build read generated files as inputs.

Before it renders a page or writes a file, `sosig` rejects these conflicts:

- Two outputs have the same path. For example, a static file or an asset has the same path as a content page, an aggregate, or a feed. An HTML file that Emacs exported next to its Org file is such an asset: `content/a.html` has the same path as the page of `content/a.org`. Delete the exported file or move it out of `content/`. Paths that differ only in ASCII letter case, such as `About/index.html` and `about/index.html`, are the same path, because a case-insensitive filesystem such as the macOS default stores them as one file.
- One output path must be both a file and a directory. This check also ignores ASCII letter case.
- An output is at or below `content_dir`, `templates_dir`, or `static_dir`, for example through a symlink inside `output_dir`. This check applies also when no file exists at that path yet.
- An output overwrites the config file, a content file, a static file, an asset, or any file below `templates_dir`, partials included.

These checks compare the directories and files themselves, not the text of the paths. So they also find a different spelling of the same directory, or a symlink to it. The build creates `output_dir` only after all checks pass, so a rejected build creates nothing.

### Dry run

`sosig build --dry-run` runs every phase of a build and renders every template. It changes nothing in `output_dir`, and it does not create `output_dir`. For each path that the build would change or keep, it prints one line to `stdout`: a mark, a tab, and the path. This is a short example:

```text
write	public/new-post.html
keep	public/about.html
write	public/index.html
write	public/atom.xml
write	public/robots.txt
write	public/_sosig/output-cache.tsv
remove	public/notes
remove	public/notes/old-post.html
```

| Mark | Meaning |
| --- | --- |
| `write` | The build would write this output. |
| `keep` | The build would leave this output as it is. See [What a build keeps](#what-a-build-keeps). |
| `remove` | The build would remove this path, because the current inputs do not make it. The path can be a directory. |

The `write` and `keep` lines come first, in this order:

1. Content pages, newest first.
2. Aggregates.
3. Feeds.
4. Static files, then assets.
5. The output cache.

The `remove` lines come last, in sorted order. To get only the paths, for `sort`, `diff` or `comm`, use `cut -f2-`. A dry run reports a failure as a build does, on `stderr` and with the same exit status, and then prints no listing. With `-v`, the progress lines go to `stderr`, so `stdout` holds only the listing.

A dry run writes nothing, so a build can fail after a dry run succeeds. A dry run cannot find these failures:

- The build has no permission to write to `output_dir`, or the disk is full.
- `sosig` cannot read a static file or an asset. A dry run does not open the files that it would copy.
- An old file is where a new output needs a directory, or an old directory is where a new output needs a file. For example, the old output is the file `public/blog`, and the new output is `public/blog/index.html`.
- `output_dir` exists but is not a usable directory.

### What a build keeps

A build renders every content page, aggregate and feed. It writes each one only when its bytes differ from the file in `output_dir`. An unchanged file keeps its modification time, so a deploy tool that compares times does not upload it again.

A build copies a static file or an asset only when its source changed. The output cache, `_sosig/output-cache.tsv` below `output_dir`, records the path, size and modification time of each source that the build copied. The next build keeps a copy if its source has the same path, size and modification time, and the copy has the size that the cache records. A tool that changes a file but keeps its size and modification time defeats this check. After such a change, run `sosig build --force` once.

When you publish `output_dir`, you also publish the output cache, with the modification time of each static file and asset. To leave it out, exclude `_sosig/` when you deploy. GitHub Pages runs Jekyll when it publishes from a branch, and Jekyll drops every file or directory whose name starts with `_`. So it drops `_sosig/`, and a site whose own outputs avoid such names still works.

### Limits

The build rejects an input file that is too large before it reads the file. The error gives the limit and the file size. Static files and assets have no size limit, because the build streams each copy from its source.

| Input | Limit |
| --- | --- |
| Content file | 256 MiB |
| Config file | 1 MiB |
| Template or partial | 4 MiB |

Other limits:

- A content file, the config file, or a template that contains a `NUL` byte fails the read.
- `templates_dir/partials/` can hold at most 64 partials.
- A rendered page can be at most 256 MiB, and a render can expand at most 300,000 partials. A render past a limit fails.
- The relative path of an output can be at most 1,024 bytes, and each segment of it at most 255 bytes.
- The build does not read an output cache larger than 256 MiB, and it copies every static file and asset again.

The build reserves its memory once, in fixed sizes that come from these limits. Under an address-space limit (`ulimit -v`), it runs fewer workers, and then makes a smaller reservation, as [libs/ARCHITECTURE.md](../../libs/ARCHITECTURE.md#build-memory) describes:

- A build needs about 280 MB of address space (`ulimit -v 270000`). With a lower limit, the build stops at startup with `sosig: out of memory`.
- The worker threads need room for their stacks beside the reservation. If the limit leaves too little room, the build fails with `cannot start worker thread`.
- A build can need more memory than it reserved, for example for a template whose nested sections expand without bound. Then the build prints the errors that it logged until then, prints `sosig: out of memory (in '<file>')`, and exits with status 1.
- Without an address-space limit, the build's log and each worker's errors have at least 256 MiB each. A worker that reports more, for example about a million one-line errors, also ends the build with this out-of-memory report.

## Writing content

Each content file is one entry. A Markdown entry starts with TOML frontmatter, and an Org entry starts with keywords. Both parsers accept a UTF-8 BOM and CRLF line endings.

> [!warning]
> Markdown and Org can contain raw HTML, and `sosig` writes this HTML to the output without sanitizing it. Build only content files that you trust.

### Entry fields

| Field | Markdown frontmatter | Org keyword | Value |
| --- | --- | --- | --- |
| `title` | `title` | `#+title:` | Required. Plain text, so Org markup such as `=foo=` shows as written. |
| `date` | `date`, a TOML date or datetime | `#+date:`, an Org timestamp such as `[2024-07-12 Fri]`, `<2024-08-19 Mon 11:02>` or `2024-07-12`, in UTC | Required. Templates get it in RFC 3339 format. `sosig` sorts the entries by it. |
| `modified` | `modified`, a TOML date or datetime | `#+last_modified:`, an Org timestamp | Optional. The date of the last change, in RFC 3339 format. The default is empty. |
| `description` | `description` | `#+description:` | Optional summary. The default is empty. |
| `tags` | `tags`, an array of strings | `#+filetags:`, where `:Emacs:Org:` gives `Emacs` and `Org` | Optional. |
| `slug` | `slug` | `#+slug:` | Optional. The default is the filename without its extension. `sosig` normalizes the slug as [URLs and slugs](#urls-and-slugs) describes. |
| `draft` | `draft`, a boolean | `#+draft:`, `true` or `false` | Optional. The default is `false`. `sosig` checks the fields of a draft, but skips its body and makes no output for it. |
| `template` | `template` | `#+template:` | Optional. A template name that overrides `templates.content`. It must obey the [template name rules](#templates-table). |

### Markdown frontmatter

The frontmatter is TOML between `+++` fences. This example uses every key that the frontmatter can contain:

```toml
+++
title = "Content Entry Title"
date = 2026-07-01T12:00:00Z
modified = 2026-07-03T09:30:00Z
description = "Short summary for listings."
slug = "content-entry-title"
tags = ["c", "sosig"]
draft = false
template = "feature.html"
+++
```

`sosig` rejects any other key, unless `frontmatter_ignored_keys` lists it. For example, the misspelled key `drfat` makes the build fail. The build does not ignore it and use the default `draft`.

`sosig` rejects a Markdown file without both `+++` fences, for example a file that starts with a YAML `---` block.

### Org keywords

An Org entry needs no fences:

```org
#+title: Content Entry Title
#+date: [2026-07-01 Wed]
#+last_modified: [2026-07-03 Fri 09:30]
#+filetags: :c:sosig:
```

`sosig` reads the keywords at the top of the file. It stops at the first line that is not a blank line, a comment, a property drawer or a keyword, so it never reads a keyword in the body. It trims each value, and matches each keyword name in any letter case, so `#+TITLE:` and `#+title:` are the same. Emacs export also reads `#+title:`, `#+date:`, `#+description:` and `#+filetags:`, so `sosig` can publish a note that you wrote for Emacs without changes. Emacs does not read `#+last_modified:`, but the ox-huguito exporter for Hugo does.

`sosig` ignores every other keyword, such as `#+options:`, `#+author:` or `#+startup:`, because Org files hold keywords for other tools. So a misspelled keyword such as `#+drfat:` has no effect. To mark an entry as a draft, use `#+draft: true`.

`sosig` merges a repeated keyword as Emacs does:

- It joins `#+title:` lines with a space.
- It joins `#+description:` lines with a newline.
- It adds the tags of each `#+filetags:` line.
- It uses the first `#+slug:`, `#+draft:` or `#+template:` line.
- It joins `#+date:` lines, and also `#+last_modified:` lines, as ox-huguito does. The result holds more than one timestamp, so the build rejects it.

The filename of a denote or org-roam note starts with a timestamp, and the filename of a denote note also holds its tags. If such a note has no `#+slug:`, its URL is like `/20240819t110231-a-test-note-typescript.html`, and the URL changes when you change the tags. Set `#+slug:` to keep the URL fixed.

### URLs and slugs

The `permalink` pattern sets the URL and the output path of each entry. It can contain the placeholders `{slug}` and `{section}`. It must contain `{slug}`, so that each entry gets a different path. `{section}` is the directory of the content file below `content_dir`, with each directory name normalized as a slug. If an expanded path ends in `/`, `sosig` adds `index.html`. For example, `/{slug}/` gives a URL such as `/<slug>/index.html`.

A relative link from a page to an asset, such as `![Diagram](diagram.svg)`, works only if the page is in the output directory that matches the directory of its content file. The default permalink, `/{section}/{slug}.html`, does this if normalization does not change the directory names. A `/{slug}/` permalink puts each page one directory lower, and a directory named `Field Notes` becomes the section `field-notes`. In these cases the link breaks, and `sosig` does not change it. A link to another content file works with every permalink, because `sosig` rewrites it, as [Links between content files](#links-between-content-files) describes. Both kinds of link break in an aggregate or a feed that includes the body of a page below a section, because the browser resolves the link against the path of the aggregate or feed.

`sosig` normalizes a slug in these steps:

1. It changes ASCII letters to lowercase.
2. It replaces each run of ASCII characters that are not letters or digits, such as punctuation and whitespace, with `-`.
3. It removes leading and trailing dashes.
4. It replaces each non-ASCII byte with its two lowercase hexadecimal digits.
5. It uses `untitled` if no characters remain.

### Links between content files

A link in a content file can point to another content file. `sosig` writes the link as the relative path from the linking page to the page of the other file. This works for every permalink, every directory name, and a site below a subpath. A link names the other file by its path or by its denote identifier:

| Link                 | Org                                 | Markdown                          |
| -------------------- | ----------------------------------- | --------------------------------- |
| By path              | `[[file:other.org][Other]]`         | `[Other](other.md)`               |
| By denote identifier | `[[denote:20240819T110654][Other]]` | `[Other](denote:20240819T110654)` |

A path link names a file whose path ends in `.md` or `.org`, in any letter case, in either format. The path is relative to the directory of the linking file, and can contain `.` and `..` segments. In Markdown, the link has no scheme such as `https:`. `sosig` compares the path as written, so write a space in a Markdown path as a space inside angle brackets, such as `[Other](<Field Notes/other.md>)`, not as `%20`. `sosig` does not change a link to any other file, such as an image, or a path that starts with `/`.

A denote link names the file whose name holds the identifier, in one of the two forms that denote writes. The first form is the `YYYYMMDDTHHMMSS` timestamp at the start of the name. The second form is the text after `@@`, up to the next `==`, `--`, `__`, `@@` or `.`. These forms cover every identifier format and every order of the parts of a filename. The file can be a content file or an asset, such as a PDF that denote renamed. A link to an asset points to the copy of the asset in `output_dir`. If two files have the same identifier, a content file comes before an asset, and then the first path in sorted order. `sosig` does not read the `#+identifier:` keyword, the `identifier` key, or the exclusion settings of denote. In Markdown, `<denote:20240819T110654>` is also a denote link.

A link can also name a place in the other file:

- A Markdown `#fragment`, or an Org `::#custom-id` search option, becomes the fragment of the URL, such as `other.html#custom-id`. The link goes to that heading, because each heading has an `id`, as [Generated HTML](#generated-html) describes.
- `sosig` removes every other Org search option, such as `::*Heading`, because it does not read the headlines of the other file.

An Org `file` link without a description shows the URL that `sosig` writes, as Emacs shows the exported path. A denote link without a description shows `denote:` and the identifier, as in Emacs.

A link to a file that does not exist below `content_dir`, or to a draft, makes the build fail, because a draft has no page. Each such link is one error, which names the target and the linking file:

```text
link 'other.md' names no content file (in 'content/a.md')
link 'b.org' names draft 'content/notes/b.org' (in 'content/notes/a.org')
denote link '19990101T000000' names no file (in 'content/a.org')
denote link '20240101T000000' names draft 'content/b.org' (in 'content/a.org')
```

The build checks every link before it writes to `output_dir`, and reports the broken links with its other errors. A failed build changes nothing, and a dry run reports the same errors. The build does not check the links in a draft, because it does not convert the body of a draft. For comparison, Emacs exports a link to a missing file, or writes `[[Denote query for ‘ID’]]`, and both give a broken link on the published page.

To publish a directory of denote notes without changes:

- Use the `org` or the `markdown-toml` file type of denote. `sosig` cannot read the YAML frontmatter of the `markdown-yaml` file type.
- For Markdown notes, set `frontmatter_ignored_keys = ["identifier", "signature"]`, so that `sosig` accepts the keys that denote writes.
- Give each Markdown link a description. Without a description, denote writes a Markdown link in Org syntax, `[[denote:20240819T110654]]`. This is not a Markdown link, so `sosig` shows it as text. The default link commands of denote write a description.

### Markdown

`sosig` uses md4c to parse Markdown, and writes the HTML itself. It enables these extensions:

- Autolinks: a bare URL, a `www.` address and an e-mail address are links, as on GitHub
- Tables
- Strikethrough
- Task lists
- LaTeX math spans
- Spoilers
- Superscript and subscript
- Admonitions
- Footnotes
- Highlights

### Org

`sosig` uses org4c to parse Org, and writes the HTML itself. It renders `a_{b}` as a subscript and keeps `snake_case` as text, as `#+OPTIONS: ^:{}` does. Its output differs from the HTML export of Emacs in these ways:

- An `#+OPTIONS:` line in a file overrides these settings, for example with `^:t`, `f:nil` or `tex:nil`. This also happens when the line is inside a code block.
- `#+BEGIN_EXPORT html` blocks and `@@html:...@@` snippets pass raw HTML to the output. `sosig` omits `#+HTML:` lines.
- For a feature that Markdown also has, `sosig` writes the same HTML as for Markdown. See [Generated HTML](#generated-html).
- `sosig` writes an inline `src_c{…}` as its code, unless its `:exports` is `none` or `results`. By default Emacs exports the results of an inline `src_c{…}`, but `sosig` never runs code.
- A `[[id:...]]` link points to an anchor on the same page, so it does not work as a link to another note. Use a `[[file:other.org]]` or `[[denote:...]]` link, as [Links between content files](#links-between-content-files) describes.

As in Emacs, a property drawer at the top of the file renders nothing. A `Footnotes` headline and its subtree also render nothing, but their footnote definitions go into the footnote list at the end. The Org renderer of `sosig` is a port of the renderer in org4c. So the other differences that the org4c [README](https://github.com/unindented/org4c#differences-from-emacs-ox-html) lists also apply, except for the HTML that the next section describes.

### Generated HTML

For the features that Markdown and Org share, `sosig` writes the same HTML, so one stylesheet and one math script work for both formats:

- Bold text is `<strong>`, and italic text is `<em>`.
- Headings start at `<h2>`, one level below the `<h1>` of the page title. Markdown's `#` and Org's `*` are `<h2>`, and each lower level adds one, up to `<h6>`.
- Each heading has an `id`. The `id` is the `CUSTOM_ID` of an Org headline, or else a slug of the heading text. The slug keeps ASCII letters in lowercase, digits and `_`. It writes each run of spaces and `-` as one `-`, with none at either end, and drops every other character, non-ASCII letters included. So `## Build & test` is `#build-test`. A heading with an empty slug gets `section`. A slug that an earlier heading of the page has gets `-1`, `-2`, and so on. The slug matches the GitHub slug for ASCII text without punctuation between words, where GitHub keeps `--`.
- Each heading is in a `<div class="heading">`, followed by a link to itself, `<a class="anchor" href="#<id>" aria-labelledby="<id>"></a>`. The link is empty, for a stylesheet to draw. It takes its name from the heading, so it holds no English word, and it is outside the heading, so it is not part of the heading's name.
- Ids are unique in one body only. A template that writes several bodies on one page, inside `{{#content_entries}}`, can repeat a heading id or a footnote id.
- A footnote reference is `<sup><a href="#fn-1" id="fnref-1-1" role="doc-noteref">1</a></sup>`. The footnotes come after the body, in a `<section class="footnotes" role="doc-endnotes">` list. Each footnote has `role="doc-backlink"` links back to its references. A footnote with one paragraph has no `<p>`. An Org footnote with more than one paragraph keeps each paragraph as a `<p>`.
- A task list item starts with a disabled `<input type="checkbox" class="task-list-item-checkbox">`. An Org item that is partly done, `[-]`, is unchecked.
- A note, tip, important note, warning or caution is a `<div class="admonition-note">` (or `admonition-tip`, and so on) that starts with a `<p class="admonition-title">`. In Markdown, it is a `> [!NOTE]` block. In Org, it is a `#+BEGIN_NOTE` block. The block name can be in any letter case.
- A Markdown spoiler, `||text||`, is `<label class="spoiler"><input type="checkbox" role="switch"><span>text</span></label>`. A reader turns the switch on to see the text, with the keyboard or a click, and no script. The stylesheet hides the text while the switch is off, with `visibility: hidden`, which also hides it from a screen reader and keeps its width in the sentence. The stylesheet also names the switch, for example with `.spoiler::before { content: "" / "Spoiler"; }`.
- Inline math is `<x-equation>`, and displayed math is `<x-equation type="display">`. The element holds the LaTeX, for a script such as MathJax or KaTeX. In Org, `$y$` and `\(y\)` are inline, and `$$y$$`, `\[y\]` and a LaTeX environment are displayed.
- A code block is `<pre><code class="language-c">`, and an Org inline `src_c{…}` is `<code class="language-c">`. `sosig` highlights both. An Org example block and Org fixed-width `: ` lines are `<pre><code>`, as a Markdown code block without a language is.
- An image alone in its paragraph is `<p><img></p>`.

The classes of the Org features that Markdown does not have start with `org-`:

- Org underline, `_text_`, is `<u>`, with no class.
- A caption makes an image or a code block a `<figure>` with a `<figcaption>`, and gives a table a `<caption>`. The caption starts with its number in a `<span class="org-figure-number">`, `org-listing-number` or `org-table-number`. The span holds only the number, so the stylesheet writes the word, for example with `.org-figure-number::before { content: "Figure "; }`.
- A code block with a `#+NAME:` has that `id` on its `<pre>`.
- A source or example block with line numbers or code references writes each line as a `<span class="code-line">`. A numbered line starts with a `<span class="code-line-numbers">` that holds its `<span class="code-line-number">`. The text of the line, with its line end, is a `<span class="code-line-text">`. A stylesheet can lay out the lines as a grid, with the numbers in the first column, and keep the numbers out of a copy of the code. These classes have no `org-` prefix, because this markup is not specific to Org.
- A line with a code reference also has the class `org-coderef` and the id `coderef-<name>`. A link to it is an `<a class="org-coderef-link">`.
- A TODO keyword is a `<span class="org-todo">`, or a `<span class="org-done">` for a done keyword. A priority is a `<span class="org-priority">`. Headline tags are a `<span class="org-tags">` with a `<span class="org-tag">` for each tag.
- A timestamp is a `<span class="org-timestamp">`, a citation is a `<span class="org-citation">`, a verse block is a `<p class="org-verse">`, and a center block is a `<div class="org-center">`.
- The `:class` of `#+ATTR_HTML:`, and the name of a special block that is not an admonition, stay as written, without the prefix.

### Syntax highlighting

If the binary has a grammar for the language of a code block, `sosig` writes each token of the block as a `<span>`. Its class tells the kind of token:

- `tok-attribute`
- `tok-comment`
- `tok-constant`, `tok-constant-builtin`
- `tok-constructor`
- `tok-delimiter`
- `tok-embedded`
- `tok-escape`
- `tok-function`, `tok-function-builtin`, `tok-function-method`, `tok-function-special`
- `tok-keyword`
- `tok-label`
- `tok-number`
- `tok-operator`
- `tok-property`
- `tok-punctuation-bracket`, `tok-punctuation-delimiter`, `tok-punctuation-special`
- `tok-string`, `tok-string-special`
- `tok-tag`, `tok-tag-error`
- `tok-type`, `tok-type-builtin`
- `tok-variable`, `tok-variable-builtin`, `tok-variable-parameter`

`sosig` supplies no stylesheet, so each site sets the colors for these classes.

The release binaries highlight these languages:

| Language | Names for code blocks |
| --- | --- |
| C | `c` |
| Bourne shells | `sh`, `shell`, `bash`, and the other Bourne-family names that Emacs gives to `sh-mode`, such as `ksh`, `dash` and `zsh` |
| HTML | `html` |
| CSS | `css` |
| JavaScript with JSX | `javascript`, `js`, `jsx` |
| TypeScript | `typescript`, `ts` |
| TSX | `tsx` |
| JSON | `json` |

A language name matches in any letter case, so `sosig` highlights a `Bash` code block, which Emacs does not. A build from source can choose its grammars with `APPS_GRAMMARS`, as [BUILD.md](../../BUILD.md#grammars) describes. `sosig --version` lists the grammars of a binary.

`sosig` highlights each code block in one language only, so in an HTML code block it only escapes the contents of `<script>` and `<style>` elements. `sosig` only escapes the text of a code block, with no highlighting, in these cases:

- The code block has no language.
- The binary has no grammar for the language.
- The code block is too large or too complex: tree-sitter would need more than 128 MiB for it, or it holds tens of thousands of nested or unclosed brackets.

## Templates

Templates use [Mustache](https://mustache.github.io/) syntax and are in `templates_dir`. There are three kinds:

| Kind | Configured by | Rendered |
| --- | --- | --- |
| Content | `templates.content`, or the `template` field of an entry | Once for each entry, to the output path of the entry. The entry fields are at the top level. |
| Aggregate | `templates.aggregates` | Once, to the same relative path below `output_dir`. No entry fields are at the top level. |
| Feed | `templates.feeds` | As an aggregate, but with only the newest `feed_count` entries. |

`{{value}}` escapes its output for HTML. `{{{value}}}` and `{{& value}}` write it without escaping, and work only for a key that ends in `_html`, which holds HTML that `sosig` made. Any other key that the data holds fails the render, so a value that `sosig` did not make, such as a title, cannot add markup to a page. A name that the data does not hold writes nothing. Use `{{{body_html}}}` to write the rendered body.

Every page and feed is valid UTF-8 without the characters that XML 1.0 forbids, so browsers and feed readers accept it. `sosig` replaces each invalid sequence in a value or a template with U+FFFD. It drops the C0 control characters except tab, line feed and carriage return, and it drops U+FFFE and U+FFFF.

Each entry sets every field in the table, with empty text where a value is absent, so a field never resolves from an enclosing scope. Empty text is false in a section, so `{{#description}}…{{/description}}` shows its contents only for a description that is not empty.

Every link in the data of an entry is relative to the page and already holds its `path_to_root`. The exception is a feed template, where every link is absolute, below `base_url`.

| Key | Templates | Value |
| --- | --- | --- |
| `site.title`, `site.description`, `site.author`, `site.base_url` | All | The values from the config. |
| `feed` | Feed | `id` and `url`, the URL of the feed. `html_url`, the home of the site. `modified`, the newest change among the feed's entries, or `1970-01-01T00:00:00Z` for a feed without entries. |
| `content_entries` | All | Every entry that is not a draft, newest first. Entries with the same date sort by output path. A feed gets only the newest `feed_count` entries. Iterate with `{{#content_entries}}`. |
| `generator` | All | The name and version of `sosig`, for a tag such as `<meta name="generator" content="{{generator}}">`. |
| `path_to_root` | All | The `../` prefix from the page to `output_dir`, empty for a page at the top. A link to a static file is `{{path_to_root}}style.css`. |
| `title`, `date`, `modified`, `description`, `slug`, `url`, `absolute_url`, `body_html`, `tags` | Content, and any template inside `{{#content_entries}}` | The entry fields. `url` links to the entry's page, relative to the current page, or absolute in a feed. `absolute_url` is always absolute, below `base_url`, for a canonical link or a page that a host serves at any depth, such as a 404 page. Iterate tags with `{{#tags}}{{.}}{{/tags}}`. |

The output path of a content page comes from `permalink`. The output path of an aggregate or feed template is its name.

A partial reference `{{> name}}` loads `templates_dir/partials/<name>.html`. A partial name can contain only letters, digits, `_` and `-`. A reference to another name fails the build. `sosig` ignores the other files of `partials/` and its subdirectories. The feed templates also use the `.html` partials. Before it writes anything, the build compiles each template that it renders and each partial once. It reports every template that it cannot read or compile in one run.

```html
{{> menu}}
<article>
  <h1>{{title}}</h1>
  <p class="meta">{{date}} | {{#tags}}{{.}} {{/tags}}</p>
  {{{body_html}}}
</article>
{{> footer}}
```

If an entry does not set `modified`, its `modified` is empty, except in a feed template, where it is the entry's `date`. So a content template can show the date of the last change only for an entry that has one:

```html
<p class="meta">{{date}}{{#modified}} | Updated {{modified}}{{/modified}}</p>
```

An Atom feed template can use `feed.modified` for the `<updated>` element of the feed, and `modified` for the `<updated>` element of each entry:

```xml
<id>{{feed.id}}</id>
<link rel="self" type="application/atom+xml" href="{{feed.url}}"/>
<link rel="alternate" type="text/html" href="{{feed.html_url}}"/>
<updated>{{feed.modified}}</updated>
{{#content_entries}}
<entry>
  <id>{{url}}</id>
  <title>{{title}}</title>
  <updated>{{modified}}</updated>
  <content type="html" xml:base="{{url}}">{{body_html}}</content>
</entry>
{{/content_entries}}
```

A link in `body_html` to another content file or an asset is relative to the entry's own page. So `body_html` works on that page, and in a feed whose `<content>` sets `xml:base` to the entry's `url`, as above, because a feed reader resolves the links against it. On a page at another depth, such as an aggregate in a subdirectory, these links do not resolve.

## Development

[BUILD.md](../../BUILD.md) describes how to build, test and package `sosig`, and [ARCHITECTURE.md](ARCHITECTURE.md) describes its code.

## Dependencies

The root [vendor/](../../vendor/) directory holds these dependencies, except the libraries in `libs/`. [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists the licenses of the vendored dependencies.

- [libs/](../../libs/): The libraries that `sosig` shares with the other apps: `cli`, `core`, `os`, `runtime`, `formats`, `markup`, `output` and `templates`, and for the unit tests `testing` and `os_memory`. [libs/ARCHITECTURE.md](../../libs/ARCHITECTURE.md) describes them.
- [copt](https://github.com/fardaniqbal/copt): Command-line option parsing.
- [tomlc17](https://github.com/cktan/tomlc17): TOML parsing of the config and the frontmatter.
- [md4c](https://github.com/mity/md4c): Markdown parsing.
- [org4c](https://github.com/unindented/org4c): Org parsing.
- [mustache4c](https://github.com/mity/mustache4c): Mustache template rendering.
- [tree-sitter](https://github.com/tree-sitter/tree-sitter): Syntax highlighting, with these grammars:
  - [tree-sitter-c](https://github.com/tree-sitter/tree-sitter-c)
  - [tree-sitter-bash](https://github.com/tree-sitter/tree-sitter-bash)
  - [tree-sitter-html](https://github.com/tree-sitter/tree-sitter-html)
  - [tree-sitter-css](https://github.com/tree-sitter/tree-sitter-css)
  - [tree-sitter-javascript](https://github.com/tree-sitter/tree-sitter-javascript)
  - [tree-sitter-typescript](https://github.com/tree-sitter/tree-sitter-typescript)
  - [tree-sitter-json](https://github.com/tree-sitter/tree-sitter-json)
- [acutest](https://github.com/mity/acutest): Unit tests.

## License

`sosig` is MIT-licensed, as [LICENSE.txt](https://github.com/unindented/apps-c17/blob/main/LICENSE.txt) states.
