<div align="center">
  <img src="media/logo.webp" height="300" alt="">
</div>

<h1 align="center"><code>sosig</code> /ˈsɔsɪdʒ/</h1>

`sosig` is a static site generator, written in C17. It uses Mustache templates to convert Markdown and Org files into HTML pages.

## LLM disclosure

> [!warning]
> This is all vibe-coded slop, produced with Claude Opus 4.8, 5, and 5.5.

## Installation

Download a prebuilt binary from a `sosig-v` release on the [Releases](https://github.com/unindented/apps-c17/releases) page, or [build it from source](#building).

## Usage

### Commands

`sosig` has two commands:

- `sosig build`: Generate the site in `output_dir`. This command has these flags:
  - `-c, --config PATH`: Read the configuration from `PATH`. The default is `sosig.toml` in the working directory.
  - `-w, --workers N`: Set the maximum number of worker threads for the build. `N` must be a positive integer. The default is the number of CPUs that `sosig` detects. The build never runs more workers than the number of content files, and never more than four workers for each online CPU.
  - `-n, --dry-run`: Report the changes that the build would make, but do not make them. See [Dry run](#dry-run).
  - `--force`: Ignore the [build cache](#build-cache) for this build.
  - `-v, --verbose`: Print build progress to `stderr`.
- `sosig config`: Print the resolved configuration as TOML. It also takes `-c, --config PATH`.

It also has these options:

- `sosig --version` (or `-V`): Print the version, and the grammars that the binary includes for [syntax highlighting](#syntax-highlighting).
- `sosig --help` (or `-h`): Print the list of commands.
- `sosig <command> --help`: Print the help for one command.

A command exits with status 0 when it succeeds, 1 when it fails, and 2 when it rejects the command line.

### Site layout

Run `sosig` from a directory that has this layout, or name its config with `-c`. The example uses the default directory names:

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

Each part of the project has one purpose:

- `content/` holds the content files and their assets. The content files are the regular files whose names end in `.md` or `.org`, in any letter case, in any directory below `content/`. For example, `post.md`, `post.MD` and `notes.Org` are content files. Each content file is one entry. Every other file below `content/` is an asset, and `sosig` copies it as it copies a static file. For example, `content/diagram.svg` becomes `output_dir/diagram.svg`, so `post-2.md` can show it with `![Diagram](diagram.svg)`. See [URLs and slugs](#urls-and-slugs) for when such a link works.
- `static/` holds the files that `sosig` copies into `output_dir` without changes, at the same relative path. For example, `static/robots.txt` becomes `output_dir/robots.txt`. Put a file that belongs to one entry, such as an image, next to that entry in `content/`. Put a file that the whole site uses, such as a stylesheet, in `static/`.
- `templates/` holds the Mustache templates and their partials, as [Templates](#templates) describes.
- `sosig.toml` is the config file, as [Configuration](#configuration) describes.

> [!caution]
> `sosig` publishes every file below `content/`, including hidden files such as `.DS_Store`, a `.git/` directory, editor backups such as `notes.org~`, and Org archives such as `notes.org_archive`. Keep a file that must not be public outside `content/`.

The content directory and the templates directory must exist, and the static directory is optional. The repository and the release archives contain no starter templates or static files, so each site supplies its own.

When `sosig` searches `content/` and `static/`, it follows symlinks to directories, but it reads each directory only once:

- If more than one path leads to a directory, `sosig` uses only one of them: the path that goes through no symlink, or, if every path goes through a symlink, the first of those paths in byte order. For example, if `blog` is a symlink to `posts`, the entries publish only under `posts`.
- `sosig` skips a symlink that points back into one of its own parent directories.

### Configuration

`sosig.toml` must set `title`, `description`, `author` and `base_url`:

```toml
title = "C17 Notes"
description = "Notes on C17, Markdown and Org"
author = "Author Name"
base_url = "https://www.example.com"
```

This table lists every key that the config file can contain. `sosig` rejects any other key. For example, if the file contains the misspelled key `ouput_dir`, `sosig` cannot load the file. It does not ignore the key and use the default value of `output_dir`.

| Key | Default | Bounds | Meaning |
| --- | --- | --- | --- |
| `title` | Required | A string | The title of the site, `site.title` in templates. |
| `description` | Required | A string | The description of the site, `site.description` in templates. |
| `author` | Required | A string | The name of the site's author, `site.author` in templates. |
| `base_url` | Required | An `http://` or `https://` URL with a host, and without `?`, `#`, a space or a control character. The scheme can be in any letter case. | The absolute URL of the site, for the links of feeds and the `absolute_url` of each entry. `sosig` removes any trailing `/`, so a template can join it with a path that starts with `/` and not produce `//`. |
| `permalink` | `"/{section}/{slug}.html"` | A pattern with `{slug}` that expands to a safe relative path | The pattern for the URL and output path of each entry. See [URLs and slugs](#urls-and-slugs). |
| `content_dir` | `"content"` | Must be a directory | The directory that contains the content files. |
| `output_dir` | `"public"` | Not empty | The directory that the build writes to. The build controls all of its contents. See [Output directory](#output-directory). |
| `templates_dir` | `"templates"` | Must be a directory | The directory that contains the templates and partials. |
| `static_dir` | `"static"` | A directory, if it exists | The directory of files that the build copies into `output_dir` without changes. A build skips it when it does not exist. |
| `content_template` | `"content.html"` | A safe relative template name | The template for each entry, unless the `template` field of the entry overrides it. |
| `aggregate_templates` | `["index.html"]` | An array of safe relative template names | The templates that the build renders once, with every entry that is not a draft. |
| `feed_templates` | `["atom.xml"]` | An array of safe relative template names | The templates that the build renders once, with the newest `feed_count` entries. |
| `feed_count` | `25` | 0 or more | The maximum number of entries in each feed. |
| `frontmatter_ignored_keys` | `[]` | An array of strings | The keys that Markdown frontmatter can contain in addition to the keys that [Markdown frontmatter](#markdown-frontmatter) lists. `sosig` accepts any value for these keys, and does not read it. For denote notes, set it to `["identifier", "signature"]`. |

`sosig` resolves each directory in the config, the defaults included, against the directory of the config file, so `-c sub/sosig.toml` without `output_dir` writes `sub/public`. It removes trailing `/` characters from each directory value, and rejects a value that is then empty, `/` included. A directory can be an absolute path, or a relative path that contains `..`. `sosig config` prints every directory resolved.

A template name in the configuration, or in the `template` field of an entry, must be a safe relative path. It can contain only letters, digits, `_`, `-`, `.`, and `/`. It cannot contain an empty segment, or the segment `.` or `..`. The name of an aggregate or feed template is both its source path below `templates_dir` and its output path below `output_dir`. So a name that contains `/` creates the same subdirectories in `output_dir`. If `aggregate_templates` or `feed_templates` is an empty array, the build renders no templates of that kind.

### Output directory

> [!caution]
> The first build into a directory removes everything in that directory that the build did not write, including hidden files. Set `output_dir` to a directory that you use for nothing else.

When a build writes an output, it replaces any file that is already at that path. It keeps an output that the [build cache](#build-cache) shows is current, such as the copy of a static file or an asset whose size and modification time did not change. After a successful build, the build removes every file and directory below `output_dir` that it did not write, so `output_dir` contains only the outputs of the current inputs. For example, the build removes the pages of deleted or renamed entries, the outputs of removed templates, removed static files and removed assets, and anything that you added by hand. If a symlink does not lead to a current output, the build removes the symlink itself. It does not change the file or directory that the symlink points to. A failed build removes nothing. If rendering or writing fails before the build completes, some outputs can be new while other outputs stay unchanged.

Before a build searches the content directory, `sosig` rejects `output_dir` in these cases:

- `output_dir` is at or below `content_dir`, `templates_dir`, or `static_dir`. `sosig` also makes this check when `output_dir` does not exist yet.
- `output_dir` exists and contains one of those directories or the config file, for example with `output_dir = "."`.

`sosig` makes this check even when one of those directories is missing or is not a directory, so one run reports both errors.

Every search of an input directory skips `output_dir`, so a symlink that leads into `output_dir` cannot make the build read generated files as inputs.

Before it renders page templates or writes output files, `sosig` rejects an output plan that has one of these conflicts:

- Two outputs have the same path, for example a static file or an asset at the same path as a content page, an aggregate, or a feed. An HTML file that Emacs exported next to its Org file is such an asset: `content/a.html` has the same path as the page of `content/a.org`. To fix this conflict, delete the exported file or move it out of `content/`. Paths that differ only in ASCII letter case, such as `About/index.html` and `about/index.html`, count as the same path, because a case-insensitive filesystem such as the macOS default stores them as one file.
- One output path must be both a file and a directory. This check also ignores ASCII letter case.
- An output is at or below `content_dir`, `templates_dir`, or `static_dir`, for example through a symlink inside `output_dir`. This check also applies when no file exists at that path yet.
- An output overwrites the config file, a content file, a static file, an asset, or any file below `templates_dir`, including partials.

These checks compare the directories themselves, not the text of the paths, so they also find a different spelling of the same directory, or a symlink to it. The build creates `output_dir` only after all of these checks pass, so a rejected build creates nothing.

### Dry run

`sosig build --dry-run` runs every phase of a build and renders every template. But it changes nothing in `output_dir`, and it does not create `output_dir`. For each path that the build would change or keep, it prints one line to `stdout`. Each line has a mark, a tab, and the path. This is a shortened listing:

```
write	public/new-post.html
keep	public/about.html
write	public/index.html
write	public/atom.xml
write	public/robots.txt
write	public/_sosig/v1-build-cache.tsv
remove	public/notes
remove	public/notes/old-post.html
```

| Mark | Meaning |
| --- | --- |
| `write` | The build would write this output. |
| `keep` | The build would not change this content page, aggregate, or feed, because the [build cache](#build-cache) shows that it is current, or this static file or asset, because its source is unchanged and the copy has the source's size. |
| `remove` | The build would remove this path, because the current inputs no longer produce it. The path can be a directory. |

The `write` and `keep` lines come first, in the order that the build plans them:

1. Content pages, newest first.
2. Aggregates.
3. Feeds.
4. Static files, then assets.
5. The build cache.

The `remove` lines come after them, in sorted order. To get only the paths for `sort`, `diff`, or `comm`, use `cut -f2-`. A dry run reports a failure in the same way as a build: on `stderr`, with the same exit code. A dry run that fails prints no listing. With `-v`, the progress messages still go to `stderr`, so `stdout` contains only the listing when you send it through a pipe.

A dry run writes nothing, so a build can still fail after a dry run succeeds. A dry run cannot find these failures:

- The build has no permission to write, or the disk is full.
- `sosig` cannot read a static file or an asset. A dry run does not open the files that it would copy.
- An old file is at a path where a new output needs a directory, or an old directory is at a path where a new output needs a file. For example, the old output is the file `public/blog`, and the new output is `public/blog/index.html`.
- `output_dir` exists but is not a usable directory.

### Build cache

A build writes the build cache to `_sosig/v1-build-cache.tsv` below `output_dir`. The build cache records the size and modification time of every input that the outputs depend on: each content file, each file below `templates_dir`, and each static file and asset. Its first line records the configuration values that rendering reads, the `sosig` version, and the grammars that the binary includes.

The next build keeps a content page, and does not render it again, if its content file, its templates, and the configuration did not change. The build keeps the aggregates and feeds if no content file changed, and keeps the copy of a static file or an asset whose source did not change. Some changes make the build render pages again:

- If any entry changes, the build renders again each page whose template reads `content_entries`, and each page whose body [links another content file](#links-between-content-files).
- If a page has a denote link to an asset in its body, the build renders that page in every build, because no row records which asset a page links. An aggregate or a feed that shows the body is made again only when an entry changes, so after you rename the asset, it keeps the old link until then.
- If any file below `templates_dir` changes, such as a partial, the build renders every page again.

Every build still converts the Markdown or Org of every content file.

Some tools change a file but keep its size and modification time. After such a change, the build can keep a page that no longer matches its content file. To correct this, run `sosig build --force` once. That build renders every page again, copies every static file and asset again, and writes a new build cache.

When you publish `output_dir`, you also publish the build cache. Its output paths are already public, but it also contains the names of the template files, and the size and modification time of each content file, template, static file and asset. If you do not want to publish this information, exclude `_sosig/v1-build-cache.tsv` when you deploy.

### Limits

The build rejects an input file that is too large before it reads the file. The error message gives the limit and the file size.

| Input | Limit |
| --- | --- |
| Content file | 256 MiB |
| Config file | 1 MiB |
| Template or partial | 4 MiB |

A site can have at most 64 partials in the `partials/` directory of `templates_dir`. A rendered page can be at most 256 MiB, a render can use at most 300,000 partial expansions, and a render that passes a limit fails. A build cache over 64 MiB is not read, and the build makes every output again.

The build reserves its memory once, when it starts, in fixed sizes that come from these limits. Reserved memory that the build does not use takes address space but no memory. An address-space limit (`ulimit -v`) can refuse the full reservation. When that happens, the build runs fewer workers, down to one. If the reservation still does not fit, the build makes it smaller until it fits, so the build has less room for its data. [ARCHITECTURE.md](ARCHITECTURE.md#memory-reserve-then-commit-on-demand) describes this process. Because the build runs fewer workers before it makes its reservation smaller, the address-space limit that a build needs does not increase with `-w`:

- A build needs about 280 MB (`ulimit -v 270000`). With a lower limit, the build stops at startup with `sosig: out of memory`.
- The stacks of the worker threads need room beside the reservation: about 24 MB for two workers. If the limit leaves less room than this, the build fails with `cannot start worker thread`.
- A build can need more memory than it reserved, for example for a template whose nested sections expand without bound. Then the build prints the errors that the command and the failing job logged up to that point, prints `sosig: out of memory (in '<file>')`, and exits with status 1.
- Each worker has at least 256 MiB to keep the errors that it reports. A worker that reports about a million one-line errors, or about 290 files that each contain about 16,000 unknown keys, can also make the build end with that out-of-memory report.

## Writing content

Each content file is one entry. A Markdown entry starts with TOML frontmatter, and an Org entry starts with keywords. Both parsers accept a UTF-8 BOM and CRLF line endings.

> [!warning]
> Markdown and Org can contain raw HTML, and `sosig` writes this HTML to the output without sanitizing it. Build only content files that you trust.

### Entry fields

| Field | Markdown frontmatter | Org keyword | Value |
| --- | --- | --- | --- |
| `title` | `title` | `#+title:` | Required. Plain text, so Org markup such as `=foo=` shows as it is written. |
| `date` | `date`, a TOML date or datetime | `#+date:`, an Org timestamp such as `[2024-07-12 Fri]`, `<2024-08-19 Mon 11:02>`, or `2024-07-12`, read as UTC | Required. Templates receive it in RFC 3339 format, and `sosig` sorts the entries by it. |
| `modified` | `modified`, a TOML date or datetime | `#+last_modified:`, an Org timestamp, as for `#+date:` | Optional. The date of the last change to the entry. Templates receive it in RFC 3339 format, as for `date`. The default is an empty string. |
| `description` | `description` | `#+description:` | Optional summary. The default is an empty string. |
| `tags` | `tags`, a list | `#+filetags:`, where `:Emacs:Org:` gives `Emacs` and `Org` | Optional. |
| `slug` | `slug` | `#+slug:` | Optional. The default is the name of the content file without its extension, normalized as [URLs and slugs](#urls-and-slugs) describes. |
| `draft` | `draft` | `#+draft:`, `true` or `false` | Optional. The default is `false`. `sosig` validates the fields of a draft, but skips its body and all of its output. |
| `template` | `template` | `#+template:` | Optional. A template name that overrides `content_template`. It must obey the [template name rules](#configuration). |

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

`sosig` rejects any other key, unless `frontmatter_ignored_keys` lists it. For example, the misspelled key `drfat` makes the build fail. The build does not ignore the key and use the default value of `draft`.

### Org keywords

An Org entry needs no fences:

```org
#+title: Content Entry Title
#+date: [2026-07-01 Wed]
#+last_modified: [2026-07-03 Fri 09:30]
#+filetags: :c:sosig:
```

`sosig` reads the keywords at the top of the file. It stops at the first line that is not a blank line, a comment, a property drawer, or a keyword. It does not read a keyword in the body, such as a keyword inside a code block. `sosig` trims the value of a keyword and matches its name in any letter case, so `#+TITLE:` and `#+title:` are the same. Emacs also reads `#+title:`, `#+date:`, `#+description:`, and `#+filetags:`, so `sosig` can publish a note that you write for Emacs export, and you do not have to change the note. `sosig` also reads `#+last_modified:`. Emacs does not read this keyword, but the ox-huguito exporter for Hugo does.

`sosig` ignores every other keyword, such as `#+options:`, `#+author:`, or `#+startup:`, because Org files contain keywords for other tools. So a misspelled optional keyword such as `#+drfat:` has no effect. To mark an entry as a draft, use `#+draft: true`.

`sosig` merges a repeated keyword in the same way as Emacs:

- It joins `#+title:` lines with a space.
- It joins `#+description:` lines with a newline.
- It adds the tags of each `#+filetags:` line.
- It uses the first `#+slug:`, `#+draft:`, or `#+template:` line, as Emacs does for most keywords that have one value.
- It also joins `#+date:` lines. The result contains more than one timestamp, so the build rejects it.

`sosig` joins `#+last_modified:` lines in the same way as `#+date:` lines, as ox-huguito does, so the build also rejects two `#+last_modified:` lines.

The filename of a denote or org-roam note starts with a timestamp. The filename of a denote note also contains the tags. If such a note has no `#+slug:`, `sosig` publishes it at a URL such as `/20240819t110231-a-test-note-typescript.html`. This URL changes when you change the tags of the note. To keep the URL fixed, set `#+slug:`.

### URLs and slugs

The `permalink` pattern sets the URL and output path of each entry. It supports the placeholders `{slug}` and `{section}`. It must contain `{slug}`, so that each entry gets a different path. `{section}` is the directory of the content file, relative to `content_dir`, with each directory segment normalized as a slug. If an expanded path ends in `/`, `sosig` adds `index.html` to it. For example, `/{slug}/` gives an entry a URL such as `/<slug>/index.html`.

A relative path from a page to an asset, such as `![Diagram](diagram.svg)`, works only if the page is in the output directory that matches the directory of its content file. The default permalink, `/{section}/{slug}.html`, gives this result if slug normalization does not change the directory names. A `/{slug}/` permalink puts each page one directory lower, and a directory named `Field Notes` becomes the section `field-notes`, so in these cases the link breaks. `sosig` does not change such a link. A link to another content file works for every permalink, because `sosig` points it to the page of that file, as [Links between content files](#links-between-content-files) describes. Both kinds of link break in an aggregate or a feed that includes the body of a page below a section, because a browser resolves the link against the path of the aggregate or the feed.

`sosig` normalizes a slug in these steps:

1. It changes ASCII letters to lowercase.
2. It replaces each run of ASCII punctuation or whitespace with `-`.
3. It removes leading and trailing dashes.
4. It replaces each non-ASCII input byte with lowercase hexadecimal digits.
5. It uses `untitled` if no characters remain.

### Links between content files

A link in a content file can point to another content file. `sosig` writes the link as the relative path from the directory of the linking page to the page of the other file. `sosig` knows the URL of every page, so the link works for every permalink and every directory name, and on a site below a subpath. A link names the other file by its path or by its denote identifier:

| Link                 | Org                                 | Markdown                          |
| -------------------- | ----------------------------------- | --------------------------------- |
| By path              | `[[file:other.org][Other]]`         | `[Other](other.md)`               |
| By denote identifier | `[[denote:20240819T110654][Other]]` | `[Other](denote:20240819T110654)` |

A path link names a file whose path ends in `.md` or `.org`, in any letter case, in either format. The path is relative to the directory of the linking file, and it can contain `.` and `..` segments. In Markdown, the link has no scheme such as `https:`. `sosig` compares the path as written, so write a space in a Markdown path as a space inside angle brackets, such as `[Other](<Field Notes/other.md>)`, and not as `%20`. `sosig` does not change a link to any other file, such as an image, or a path that starts with `/`.

A denote link names the file whose name contains the identifier, in one of the two forms that denote writes: the `YYYYMMDDTHHMMSS` timestamp at the start of the name, or the text after `@@` up to the next `==`, `--`, `__`, `@@`, or `.`. These forms cover every identifier format and every order of the parts of a file name. The file can be a content file or an asset, such as a PDF that denote renamed. A link to an asset points to the copy of the asset in `output_dir`. If two files have the same identifier, the link names a content file before an asset, and then the first path in sorted order. `sosig` does not read the `#+identifier:` keyword, the `identifier` key, or the exclusion settings of denote. In Markdown, `<denote:20240819T110654>` is also a denote link.

A link can also name a place in the other file:

- A Markdown `#fragment`, or an Org `::#custom-id` search option, becomes the fragment of the URL, such as `other.html#custom-id`. The link goes to that heading of the other page, because each heading has an `id`, as [Generated HTML](#generated-html) describes.
- `sosig` removes every other Org search option, such as `::*Heading`, because it does not read the headlines of the other file.

An Org `file` link without a description shows the URL that `sosig` writes, as Emacs shows the path that it exports. A denote link without a description shows `denote:` and the identifier, as it does in Emacs.

A link to a file that does not exist below `content_dir`, or to a draft, makes the build fail, because the link would be broken on the published page. A draft has no page. Each such link is one error, which names the target and the linking file:

```text
link 'other.md' names no content file (in 'content/a.md')
link 'b.org' names draft 'content/notes/b.org' (in 'content/notes/a.org')
denote link '19990101T000000' names no file (in 'content/a.org')
denote link '20240101T000000' names draft 'content/b.org' (in 'content/a.org')
```

The build checks every link before it writes to `output_dir`, and it reports the broken links together with the other errors of the build. A failed build changes nothing, and a dry run reports the same errors. The build does not check the links in a draft, because it does not convert the body of a draft. Emacs exports a link to a missing file, or writes `[[Denote query for ‘ID’]]`, and both give a broken link on the published page.

To publish a directory of denote notes without changes:

- Use the `org` file type or the `markdown-toml` file type of denote. `sosig` cannot read the YAML frontmatter of the `markdown-yaml` file type.
- For Markdown notes, set `frontmatter_ignored_keys = ["identifier", "signature"]`, so that `sosig` accepts the keys that denote writes.
- Give each Markdown link a description. Denote writes a Markdown link without a description in Org syntax, `[[denote:20240819T110654]]`, which is not a Markdown link, so `sosig` shows it as text. The default link commands of denote write a description.

### Markdown

`sosig` uses md4c to parse Markdown, and writes the HTML itself. It enables these extensions:

- Autolinks: a bare URL, a `www.` address and an e-mail address are links, as GitHub makes them
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

`sosig` uses org4c to parse Org, and writes the HTML itself. It renders `a_{b}` as a subscript and keeps `snake_case` as text, in the same way as `#+OPTIONS: ^:{}`. Its output differs from the HTML export of Emacs in these ways:

- An `#+OPTIONS:` line in a file overrides these settings, for example with `^:t`, `f:nil`, or `tex:nil`. This also happens when the line is inside a code block.
- `#+BEGIN_EXPORT html` blocks and `@@html:...@@` snippets pass raw HTML through to the output. `sosig` omits `#+HTML:` lines from the output.
- If Markdown has the same feature, `sosig` writes the same HTML as for Markdown. See [Generated HTML](#generated-html).
- `sosig` writes an inline `src_c{…}` as its code, unless its `:exports` is `none` or `results`. By default, Emacs exports the results of an inline `src_c{…}`, but `sosig` never evaluates code.
- A `[[id:...]]` link points to an anchor on the same page, so a link to another note does not work. A `[[file:other.org]]` link and a `[[denote:...]]` link point to the page of the other note, as [Links between content files](#links-between-content-files) describes.

As in Emacs, a property drawer at the top of the file renders nothing. A `Footnotes` headline and its subtree also render nothing, but their footnote definitions go into the footnote list at the end. The Org renderer of `sosig` is a port of the renderer in org4c, so the other differences that the org4c [README](https://github.com/unindented/org4c#differences-from-emacs-ox-html) lists also apply to `sosig`, except for the HTML that the next section describes.

### Generated HTML

For the features that Markdown and Org share, `sosig` writes the same HTML, so one stylesheet and one math script work for both formats:

- Bold text is `<strong>`, and italic text is `<em>`.
- Headings start at `<h2>`, one level below the `<h1>` of the page title in a content template. Markdown's `#` and Org's `*` are `<h2>`, and each lower level adds one, up to `<h6>`.
- Each heading has an `id`: an Org headline's `CUSTOM_ID`, or else a slug of its text, with ASCII letters in lowercase, digits and `_` kept, each run of spaces and `-` as one `-`, none at either end, and every other character dropped, a non-ASCII letter included. A heading with no slug is `section`, and a slug that an earlier heading of the page has gets `-1`, `-2` and so on. So `## Build & test` is `#build-test`. A slug matches GitHub's for ASCII text without punctuation between words, which GitHub keeps as `--`. Each heading sits in a `<div class="heading">` with a link to itself after it, `<a class="anchor" href="#<id>" aria-labelledby="<id>"></a>`, empty for a stylesheet to draw. The link takes its name from the heading, so it holds no English word of its own, and it sits outside the heading, so it is not part of the heading's name. Ids are unique within one body, so a template that writes the bodies of several entries on one page, inside `{{#content_entries}}`, can repeat a heading's or a footnote's id across them.
- A footnote reference is `<sup><a href="#fn-1" id="fnref-1-1" role="doc-noteref">1</a></sup>`. The footnotes come after the body, in a `<section class="footnotes" role="doc-endnotes">` list. Each footnote has `role="doc-backlink"` links back to its references. A footnote with one paragraph has no `<p>`. An Org footnote with more than one paragraph keeps each paragraph as a `<p>`.
- A task list item starts with a disabled `<input type="checkbox" class="task-list-item-checkbox">`. Org's partly done `[-]` item is unchecked.
- A note, tip, important note, warning, or caution is a `<div class="admonition-note">` (or `admonition-tip`, and so on) that starts with a `<p class="admonition-title">`. In Markdown, it is a `> [!NOTE]` block. In Org, it is a `#+BEGIN_NOTE` block. In both formats, the block name can be in any letter case.
- A Markdown spoiler, `||text||`, is `<label class="spoiler"><input type="checkbox" role="switch"><span>text</span></label>`: a switch that a reader turns on to see the text, with the keyboard or a click and no script. A stylesheet hides the text while the switch is off, with `visibility: hidden`, which hides it from a screen reader too and keeps its width in the sentence, and names the switch, for example with `.spoiler::before { content: "" / "Spoiler"; }`.
- Inline math is `<x-equation>`, and displayed math is `<x-equation type="display">`. The element contains the LaTeX, for a script such as MathJax or KaTeX. In Org, `$y$` and `\(y\)` are inline, and `$$y$$`, `\[y\]`, and a LaTeX environment are displayed.
- A code block is `<pre><code class="language-c">`, and Org's inline `src_c{…}` is `<code class="language-c">`. `sosig` highlights both of them. An Org example block and Org's fixed-width `: ` lines are `<pre><code>`, as a Markdown code block without a language is.
- An image alone in its paragraph is `<p><img></p>`.

Org has features that Markdown lacks. Their classes start with `org-`:

- Org's underline, `_text_`, is `<u>`, which needs no class.
- A caption makes an image or a code block a `<figure>` with a `<figcaption>`, and gives a table a `<caption>`. The caption starts with its number in a `<span class="org-figure-number">`, `org-listing-number` or `org-table-number`. The span holds only the number, so a stylesheet writes the word, for example with `.org-figure-number::before { content: "Figure "; }`.
- A code block with a `#+NAME:` has that `id` on its `<pre>`. A source or example block with line numbers or code references writes each line as a `<span class="code-line">`. A numbered line starts with a `<span class="code-line-numbers">` that holds its `<span class="code-line-number">`. The text of the line, with its line end, is a `<span class="code-line-text">`. A stylesheet can lay the lines out as a grid, with the numbers in the first column, and keep the numbers out of a copy of the code. These classes have no `org-` prefix because the markup of numbered code lines is shared, not Org's own. A line with a code reference also has the class `org-coderef` and the id `coderef-<name>`, and a link to it is an `<a class="org-coderef-link">`.
- A headline's TODO keyword is a `<span class="org-todo">`, or a `<span class="org-done">` for a done keyword. Its priority is a `<span class="org-priority">`. Its tags are a `<span class="org-tags">` with a `<span class="org-tag">` for each tag.
- A timestamp is a `<span class="org-timestamp">`, a citation is a `<span class="org-citation">`, a verse block is a `<p class="org-verse">`, and a center block is a `<div class="org-center">`.
- The `:class` of `#+ATTR_HTML:`, and the name of a special block that is not an admonition, stay as the author wrote them, without the prefix.

### Syntax highlighting

If the binary includes a grammar for the language of a code block, `sosig` writes each token of the block as a `<span>`. The class of the `<span>` tells the kind of token:

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

`sosig` does not supply a stylesheet, so each site must set the colors for these classes.

The release binaries highlight these languages:

| Language | Names for code blocks |
| --- | --- |
| C | `c` |
| Bourne shells | `sh`, `shell`, `bash`, and the other Bourne-family names that Emacs gives to `sh-mode`, such as `ksh`, `dash`, and `zsh` |
| HTML | `html` |
| CSS | `css` |
| JavaScript with JSX | `javascript`, `js`, `jsx` |
| TypeScript | `typescript`, `ts` |
| TSX | `tsx` |
| JSON | `json` |

A language name matches in any letter case, so `sosig` highlights a `Bash` code block, but Emacs does not. If you compile `sosig` yourself, you can choose its grammars with `APPS_GRAMMARS`, as [BUILD.md](../../BUILD.md#grammars) describes. `sosig --version` lists the grammars that a binary includes.

`sosig` highlights each code block in only one language, so in an HTML code block it only escapes the contents of the `<script>` and `<style>` elements. `sosig` does not highlight a code block, and only escapes its text, in these cases:

- The code block has no language.
- The binary includes no grammar for the language of the code block.
- The code block is too large or too complex to highlight: tree-sitter would need more than 128 MiB for it, or it contains tens of thousands of nested or unclosed brackets.

## Templates

Templates use [Mustache](https://mustache.github.io/) syntax and are in `templates_dir`. There are three kinds of templates:

| Kind | Configured by | Rendered |
| --- | --- | --- |
| Content | `content_template`, or the `template` field of an entry | Once for each entry, to the output path of the entry. The fields of the entry are available at the top level. |
| Aggregate | `aggregate_templates` | Once, to the same relative path below `output_dir`. The fields of an entry are not available at the top level. |
| Feed | `feed_templates` | In the same way as an aggregate template, but with only the newest `feed_count` entries. |

`{{value}}` escapes its output for HTML. `{{{value}}}` and `{{& value}}` write it without escaping, and work only for a key that ends in `_html`, which holds HTML that `sosig` made itself. Any other key that the data holds fails the render, so a value that it did not make, such as a title, cannot add markup to a page. A name that the data does not hold writes nothing, as with `{{value}}`. Use `{{{body_html}}}` to write the rendered body.

Every page and feed that `sosig` renders is valid UTF-8 without the characters that XML 1.0 forbids, so a browser and a feed reader accept it. `sosig` replaces each invalid sequence in a value or a template with U+FFFD, and drops the C0 control characters but tab, line feed and carriage return, and U+FFFE and U+FFFF.

Each entry sets every one of the fields below, with empty text where a value is absent, so the field never resolves from an enclosing scope. Empty text is false in a section, so `{{#description}}…{{/description}}` shows its contents only for a description that is not empty.

Every link in an entry's data is relative to the page and already holds its `path_to_root`, except in the feed templates, where every link is absolute, below `base_url`.

The templates get these keys:

| Key | Templates | Value |
| --- | --- | --- |
| `site.title`, `site.description`, `site.author`, `site.base_url` | All | The values from the configuration. |
| `feed` | Feed templates | `id` and `url` (the feed's own URL), `html_url` (the site's home), `modified` (the newest change among the feed's entries, or `1970-01-01T00:00:00Z` for a feed without entries). |
| `content_entries` | All | Every entry that is not a draft, newest first. A feed template receives only the newest `feed_count` entries. Use `{{#content_entries}}` to iterate through them. |
| `generator` | All | The name and version of `sosig`, for a tag such as `<meta name="generator" content="{{generator}}">`. |
| `path_to_root` | All | The `../` prefix from the page to `output_dir`, empty for a page at the top. A link to a static file is `{{path_to_root}}style.css`. The output path of a content page comes from `permalink`, and the output path of an aggregate or feed template is its name. |
| `title`, `date`, `modified`, `description`, `slug`, `url`, `absolute_url`, `body_html`, `tags` | Content templates, and any template inside `{{#content_entries}}` | The fields of the entry. `url` links to the entry's page, relative to the page being rendered, or absolute in a feed template. `absolute_url` is always absolute, below `base_url`, for a canonical link or a page that a host serves at any depth, such as a 404 page. Use `{{#tags}}{{.}}{{/tags}}` to iterate through the tags. |

A partial reference `{{> name}}` loads `templates_dir/partials/<name>.html`. A partial name can contain only letters, digits, `_` and `-`. A reference to another name fails the build, and `sosig` ignores the other files of `partials/` and its subdirectories. The build compiles every template that it renders and every partial once, before it writes anything, and reports every template that cannot be read or does not compile in one run. The feed templates also use the `.html` partials.

```html
{{> header}}
<article>
  <h1>{{title}}</h1>
  <p class="meta">{{date}} | {{#tags}}{{.}} {{/tags}}</p>
  {{{body_html}}}
</article>
{{> footer}}
```

If an entry does not set `modified`, its `modified` is empty, except in a feed template, where it is the entry's `date`. So `{{#modified}}` writes its contents only for an entry that sets it, and a content template can show the date of the last change only when the entry has one:

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

A link in `body_html` to another content file or an asset is relative to the entry's own page, so `body_html` works on that page and in a feed whose `<content>` sets `xml:base` to the entry's `url`, as above, which a feed reader then resolves the links against. On a page at another depth, such as an aggregate in a subdirectory, its links would not resolve.

## Development

[ARCHITECTURE.md](ARCHITECTURE.md) describes the code. [BUILD.md](../../BUILD.md) describes the CMake targets and the reasons for the configurations below. [STYLE.md](../../STYLE.md) describes the code style.

### Prerequisites

- A POSIX-like system with CMake 3.25 or later, a C17 compiler such as Clang or GCC, and pthreads.
- Optional build tools: `ninja` for the `multi` preset.
- Optional quality tools: `cwrap`, `clang-format`, `clang-tidy`, `cppcheck`.
- Zig 0.17 and `llvm-strip` for the Linux `musl` archives.

The repository includes the dependencies in [vendor/](../../vendor/), and the libraries in [libs/](../../libs/).

### Building

Run the commands in this section and in the sections below from `apps/sosig`, the directory of this README. Each preset builds in `build/<preset>`, outside the source tree, and treats compiler warnings as errors. If `CC` is set, the preset builds in `build/<preset>/<CC>` instead, such as `build/debug/gcc-14`. CMake reads `CC` only when it configures a directory for the first time, so each compiler needs its own CMake cache. The commands use eight parallel jobs. Change that number to suit your machine.

```sh
cmake --preset debug
cmake --build --preset debug -j 8
```

| Preset | Configuration | Binary |
| --- | --- | --- |
| `debug` | `Debug` with AddressSanitizer and UndefinedBehaviorSanitizer. It runs `clang-tidy` and `cppcheck` during compilation if they are available. | `build/debug/bin/sosig` |
| `tsan` | `Debug` with ThreadSanitizer. | `build/tsan/bin/sosig` |
| `release` | `RelWithDebInfo`, with no tests. The binary in the build tree keeps its debug information, and packaging strips the installed copy. | `build/release/bin/sosig` |
| `multi` | `Debug` and `RelWithDebInfo` in one build tree, with the `Ninja Multi-Config` generator. Build with `--preset multi-debug` or `--preset multi-relwithdebinfo`. | `build/multi/bin/Debug/sosig`, `build/multi/bin/RelWithDebInfo/sosig` |

`clang-tidy` runs only with Clang, because it rejects the warning options of GCC. To run linting, configure the `debug` preset, and then run this command:

```sh
cmake --build --preset lint
```

### Testing

CTest registers the unit tests and the end-to-end golden tests. Build a preset, and then run its tests:

```sh
ctest --preset debug -j 8
```

The test presets are `debug`, `tsan`, `multi-debug`, and `multi-relwithdebinfo`. The `multi-relwithdebinfo` tests check the same CMake configuration as the `release` preset, which builds no tests. To run only part of a suite, select the tests by label or by name pattern:

```sh
ctest --preset debug -L unit -j 8
ctest --preset debug -L golden -j 8
ctest --preset debug -R template -j 8
```

### Continuous integration

The workflow presets run the same configure, build, and test steps as CI:

| Workflow | Steps |
| --- | --- |
| `cmake --workflow --preset ci-debug` | Build in `Debug`, run linting, and run all ASan and UBSan tests. |
| `cmake --workflow --preset ci-tsan` | Build with TSan and run all TSan tests. |
| `cmake --workflow --preset ci-release` | Build in `RelWithDebInfo`. |
| `cmake --workflow --preset ci-grammars` | Build in `RelWithDebInfo` with only the TypeScript grammar. CI runs this workflow only on Linux. |
| `cmake --workflow --preset ci-multi` | Build and test in `Debug` and `RelWithDebInfo` with the `Ninja Multi-Config` generator. CI runs this workflow a second time with `CC=gcc-14` to check GCC. |

To run the Linux workflows on a machine with Podman, build the pinned Ubuntu image from the root [Containerfile](../../Containerfile), with the repository root as the build context. Then run each workflow in a new container, with the directory of `sosig` as the working directory:

```sh
podman build --tag apps-c17-linux-ci --file ../../Containerfile ../..
podman run --rm --workdir /src/apps/sosig apps-c17-linux-ci ci-debug
podman run --rm --workdir /src/apps/sosig apps-c17-linux-ci ci-tsan
podman run --rm --workdir /src/apps/sosig apps-c17-linux-ci ci-release
podman run --rm --workdir /src/apps/sosig apps-c17-linux-ci ci-grammars
podman run --rm --workdir /src/apps/sosig apps-c17-linux-ci ci-multi
podman run --rm --workdir /src/apps/sosig -e CC=gcc-14 apps-c17-linux-ci ci-multi
```

The image contains a snapshot of the repository, including the vendored dependencies, without `.git/` and the `build/` directories, which [.containerignore](../../.containerignore) excludes, so the Linux build products stay in the container and do not go into a `build/` directory of the host. The image supports x86-64 and AArch64 hosts, and pins LLVM 23 and GCC 14. It does not include Zig, because only the release archives need Zig.

### Packaging

Only the release configurations have package presets. Build a preset, and then package it:

```sh
cmake --preset release
cmake --build --preset release -j 8
cpack --preset release
```

| Preset | Archive |
| --- | --- |
| `release` | A stripped native archive in `build/release/`. |
| `release-linux-x86_64` | `build/release-linux-x86_64/sosig-<version>-x86_64-linux-musl.tar.gz`, cross-built with Zig. |
| `release-linux-aarch64` | `build/release-linux-aarch64/sosig-<version>-aarch64-linux-musl.tar.gz`, cross-built with Zig. |

A source archive needs a configured tree but no compiled binary. It holds the repository without the other apps, and it does not contain build products or private workspace metadata:

```sh
cmake --preset release
cpack --config build/release/CPackSourceConfig.cmake
```

The source archive is `build/release/sosig-<version>-source.tar.gz`.

The version of every archive comes from `project(VERSION)` in [CMakeLists.txt](CMakeLists.txt). Tag each release commit with the matching `sosig-vMAJOR.MINOR.PATCH`. The tag starts the root release workflow, [release.yml](../../.github/workflows/release.yml), which tests, packages and publishes `sosig` only. A manual run of this workflow with `dry_run` tests and packages the dispatched commit, and publishes nothing.

## Dependencies

The root [vendor/](../../vendor/) directory contains these dependencies, except the libraries in `libs/`. [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists the licenses of the vendored dependencies.

- [libs/](../../libs/): The libraries that `sosig` shares with the other apps: `core`, `os`, `runtime`, `formats`, `markup` and `output`, and for the unit tests `testing` and `os_memory`. [libs/src/core/](../../libs/src/core/) holds arena allocation, counted strings, output buffers, the diagnostic log and the small primitives, and [libs/ARCHITECTURE.md](../../libs/ARCHITECTURE.md) describes the others.
- [copt](https://github.com/fardaniqbal/copt): Command-line option parsing.
- [tomlc17](https://github.com/cktan/tomlc17): TOML configuration and frontmatter parsing.
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
