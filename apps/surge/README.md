<div align="center">
  <img src="media/logo.webp" height="300" alt="">
</div>

<h1 align="center"><code>surge</code></h1>

`surge` is a static site generator for git repositories, written in C17. It renders every branch of each repository that its config names as GitHub-style pages, with a clone URL that a static host serves.

## LLM disclosure

> [!warning]
> This is all vibe-coded slop, produced with Claude Opus 5.5.

## Installation

Download a prebuilt archive from a `surge-v` release on the [Releases](https://github.com/unindented/apps-c17/releases) page, or [build it from source](#building). Each release has one archive for each target, `surge-<version>-<target>.tar.gz`, where the target is `x86_64-linux-musl`, `aarch64-linux-musl` or `aarch64-macos`, and a `SHA256SUMS` file with the SHA-256 digest of each archive. An archive holds `surge-<version>-<target>/bin/surge` and, below `share/doc/surge/`, this README, `LICENSE.txt` and `THIRD_PARTY_NOTICES.md`. The Linux binaries are static `musl` executables.

mise's [`http:` backend](https://mise.jdx.dev/dev-tools/backends/http.html) can install these archives. A GitHub release publishes no list of versions, so this `mise.toml` names the version:

```toml
[tools."http:surge"]
version = "0.1.0"
checksum_url = "https://github.com/unindented/apps-c17/releases/download/surge-v{{ version }}/SHA256SUMS"
strip_components = 1
bin_path = "bin"

[tools."http:surge".platforms]
linux-x64 = { url = "https://github.com/unindented/apps-c17/releases/download/surge-v{{ version }}/surge-{{ version }}-x86_64-linux-musl.tar.gz" }
linux-arm64 = { url = "https://github.com/unindented/apps-c17/releases/download/surge-v{{ version }}/surge-{{ version }}-aarch64-linux-musl.tar.gz" }
macos-arm64 = { url = "https://github.com/unindented/apps-c17/releases/download/surge-v{{ version }}/surge-{{ version }}-aarch64-macos.tar.gz" }
```

`mise install` does not read `checksum_url`. Run `mise lock` to record the digest of each platform's archive in `mise.lock`, and later installs then refuse an archive that does not match. A site that `surge` builds can also publish its own releases for mise, with a list of versions, as [Releases](#releases) describes.

## Usage

### Commands

`surge` has two commands:

- `surge build`: Generate the site in `output_dir`. This command has these flags:
  - `-c, --config PATH`: Read the configuration from `PATH`. The default is `surge.toml` in the working directory.
  - `-w, --workers N`: Set the maximum number of worker threads for the build. `N` must be a positive integer. The default is the number of CPUs that `surge` detects. The build never runs more than four workers for each online CPU, and a parallel phase never starts more workers than it has jobs.
  - `-n, --dry-run`: Report the changes that the build would make, but do not make them. See [Dry run](#dry-run).
  - `--force`: Make every output again, keeping nothing from earlier builds. See [Output cache](#output-cache).
  - `-v, --verbose`: Print build progress to `stderr`.
- `surge config`: Print the resolved configuration as TOML. It also takes `-c, --config PATH`.

It also has these options:

- `surge --version` (or `-V`): Print the version, and the grammars that the binary includes for [syntax highlighting](#syntax-highlighting).
- `surge --help` (or `-h`): Print the list of commands.
- `surge <command> --help`: Print the help for one command.

A command exits with status 0 when it succeeds, 1 when it fails, and 2 when it rejects the command line. A build that succeeds can still print warnings to `stderr`, after any errors, and exits with status 0.

### Site layout

Run `surge` from a directory that has this layout, or name its config with `-c`. The example uses the default directory names:

```text
releases/
  apps-c17/
    sosig-v0.1.0/
      sosig-0.1.0-x86_64-linux-musl.tar.gz
      SHA256SUMS
static/
  style.css
templates/
  partials/
    head.html
  404.html
  blob.html
  branches.html
  commit.html
  commits.atom
  commits.html
  index.html
  release-project.html
  release.html
  releases.atom
  releases.html
  tags.atom
  tags.html
  tree.html
surge.toml
```

Each part of the site has one purpose:

- `surge.toml` is the config file, as [Configuration](#configuration) describes. It names the repositories.
- `templates/` holds the Mustache templates and their partials, as [Templates](#templates) describes. The repository and the release archives contain no default templates, so each site supplies its own.
- `static/` holds the files that `surge` copies into `output_dir` without changes, at the same relative path. For example, `static/style.css` becomes `output_dir/style.css`. It is optional.
- `releases/` holds the release assets that CI puts there for each repository, as [Releases](#releases) describes. Each repository names its own directory with `releases_dir`, which is optional.

`surge` reads the repositories themselves wherever their `path` points. It never changes a repository and never uses the network.

### Configuration

`surge.toml` must set `title`, `description`, `author` and `base_url`, and name at least one repository:

```toml
title = "Code"
description = "My repositories"
author = "Daniel Perez Alvarez"
base_url = "https://git.example.org"

[[repo]]
name = "apps-c17"
path = "../apps-c17"
releases_dir = "releases/apps-c17"

[[repo.project]]
name = "sosig"
path = "apps/sosig"
description = "Static site generator"
tag_prefix = "sosig-v"
```

The tables below list every key that the config file can contain, at each level. `surge` rejects any other key. For example, if the file contains the misspelled key `ouput_dir`, `surge` cannot load the file. It does not ignore the key and use the default value of `output_dir`. A diagnostic names a key with its table, and counts the tables from 0, such as `missing required config key 'repo[0].path'`.

Top-level keys:

| Key | Default | Bounds | Meaning |
| --- | --- | --- | --- |
| `title` | Required | A string | The title of the site, `site.title` in templates. |
| `description` | Required | A string | The description of the site, `site.description` in templates. |
| `author` | Required | A string | The name of the site's author, `site.author` in templates. |
| `base_url` | Required | An `http://` or `https://` URL with a host, and without `?`, `#`, a space or a control character. The scheme can be in any letter case. | The absolute URL of the site. `surge` removes any trailing `/`. The clone URLs, the feeds, the mise examples and the links of `404.html` use it. Every other link is relative. |
| `output_dir` | `"public"` | Not empty | The directory that the build writes to. The build controls all of its contents. See [Output directory](#output-directory). |
| `templates_dir` | `"templates"` | Must be a directory | The directory that contains the templates and partials. |
| `static_dir` | `"static"` | A directory, if it exists | The directory of files that the build copies into `output_dir`. A build skips it when it does not exist. |
| `log_page_size` | `100` | 1 to 10000 | The number of commits on each log page. |
| `feed_count` | `25` | 0 or more | The maximum number of entries in each commit, tag and release feed. |
| `headers_file` | `false` | A boolean | Whether the build writes `_headers` with the cache headers of the clone exports. See [Host notes](#host-notes). |
| `limits` | | A table | The `[limits]` table below. |
| `repo` | Required | At least one table | The `[[repo]]` tables below, one for each repository, in the order of the site index. |

The `[limits]` table:

| Key | Default | Bounds | Meaning |
| --- | --- | --- | --- |
| `blob_len_max` | `1048576` (1 MiB) | 0 to 4194304 (4 MiB) | The largest blob, in bytes, that the build reads for a page. A larger blob gets a blob page without contents, is never a document or a README, and shows no lines in a diff. |
| `raw_len_max` | `104857600` (100 MiB) | 0 to 1073741824 (1 GiB) | The largest blob, in bytes, that gets a raw copy. A larger blob gets none, and a verbose build counts these blobs. |
| `diff_file_count_max` | `300` | 0 or more | The most changed files whose diffs a commit page shows. |
| `diff_line_count_max` | `20000` | 0 or more | The most diff lines that a commit page shows. |
| `diff_len_max` | `1048576` (1 MiB) | 0 to 4194304 (4 MiB) | The most bytes of diff lines that a commit page shows. |
| `image_len_max` | `2097152` (2 MiB) | 0 to 4194304 (4 MiB) | The most bytes of images that a commit page embeds, over every side of every changed image it shows. A side that would pass it is left out, and a later, smaller one may still fit. |
| `pack_len_max` | `26214400` (25 MiB) | 0 or more | The largest clone pack, in bytes, that builds without a warning. A larger pack still builds. |

Each `[[repo]]` table:

| Key | Default | Bounds | Meaning |
| --- | --- | --- | --- |
| `name` | Required | See below | The directory of the repository in the output, `<name>/`, and of its clone export, `<name>.git/`. |
| `path` | Required | A repository | The path of the repository: a work tree or a bare git directory. `surge` opens the repository at that path, and does not search the parent directories or read `GIT_DIR`. |
| `description` | The repository's `description` file | A string | The description of the repository. An empty string shows no description. Without the key, `surge` reads the `description` file of the git directory, the main repository's for a linked worktree, and ignores git's default text. A `description` file over 64 KiB fails the build, as [Limits](#limits) lists. |
| `default_branch` | The branch that `HEAD` names | Not empty, and must name a branch | The branch of the repository home and the clone's `HEAD`. A repository whose `HEAD` is detached, or names a branch that does not exist, needs this key. |
| `releases_dir` | None | A directory, if it exists | The directory of the repository's release assets. See [Releases](#releases). |
| `mise` | `false` | A boolean. Needs `releases_dir`, and no project with a `tag_prefix` | Whether the pages of the repository's own release project show how to install it with mise. See [Version lists and mise](#version-lists-and-mise). |
| `project` | None | Tables | The `[[repo.project]]` tables below. |

Each `[[repo.project]]` table:

| Key | Default | Bounds | Meaning |
| --- | --- | --- | --- |
| `name` | Required | See below | The name of the project, and of its directory below `<repo>/releases/`. |
| `path` | Required | A relative path, without an empty, `.` or `..` segment | The directory of the project in the repository. `surge` removes any trailing `/`. The site index links the tree page of this directory on the default branch, and the build fails when the default branch has no such directory with a tree page. An empty repository has no default branch yet, so its projects are not checked and link nowhere. |
| `description` | Empty | A string | The description of the project. It has no fallback. |
| `tag_prefix` | None | Not empty, and no overlap with another project's prefix | The prefix of the tags that are the project's releases, such as `sosig-v`. A project without one has no releases. |
| `mise` | `false` | A boolean. Needs `tag_prefix` | Whether the project's release pages show how to install it with mise. See [Version lists and mise](#version-lists-and-mise). |

A repository or project name can contain only ASCII letters, digits, `.`, `_` and `-`, and is at most 251 bytes long, so that `<name>.git` fits in one path segment. It cannot start with `.` or end in `.git` in any letter case. Two repositories, or two projects of one repository, cannot have names that differ only in ASCII letter case. A repository cannot be named `index.html`, `404.html`, `_surge` or `_headers`, and a project cannot be named `tag`, `download` or `index.html`, in any letter case, because the output already has files or directories of those names. A top-level entry of `static_dir` named like a repository, or like its `<name>.git`, after ASCII case folding, makes the build fail.

`surge` resolves each directory in the config, the defaults included, against the directory of the config file, so `-c sub/surge.toml` without `output_dir` writes `sub/public`. It removes trailing `/` characters from each directory value, and rejects a value that is then empty, `/` included. A directory can be an absolute path, or a relative path that contains `..`. `surge config` prints every directory resolved, and leaves out an optional string that is empty, except a repository's `description` set to `""`, which shows no description.

The load reports every bad key in one run. A rejected optional key keeps its default, so a later check does not report it again, but any config error makes the build fail before it scans a repository, after it checks `output_dir` against the inputs, so one run reports both. A rejected `output_dir`, `templates_dir` or `static_dir` stops the build before it opens a repository, because its default names a directory that you did not choose.

### Output layout

The build writes this tree to `output_dir`. `<r>` is a repository name, `<ref>` a branch name, `<p>` a path in the branch's tree, `<oid>` a commit id, `<tag>` a tag name, and `<project>` a release project:

```text
index.html                                     site index
404.html                                       not-found page
_headers                                       with headers_file = true
_surge/output-cache.tsv                        output cache
<static files>
<r>/index.html                                 repo home
<r>/branches/index.html                        branches
<r>/tags/index.html                            tags
<r>/tags.atom                                  tag feed
<r>/releases.atom                              release feed, with a release project
<r>/tree/<ref>/index.html                      root directory of a branch
<r>/tree/<ref>/<p>/index.html                  directory
<r>/blob/<ref>/<p>/index.html                  file
<r>/raw/<ref>/<p>                              raw copy, up to raw_len_max
<r>/commits/<ref>/index.html                   first log page, newest commits
<r>/commits/<ref>/page-<n>.html                log page n, from 2
<r>/commits/<ref>.atom                         commit feed
<r>/commit/<oid>/index.html                    commit with its diff
<r>/releases/index.html                        releases
<r>/releases/<project>/index.html              release project
<r>/releases/<project>/versions.txt            versions for mise
<r>/releases/<project>/versions.json           version dates for mise
<r>/releases/tag/<tag>/index.html              release
<r>/releases/download/<tag>/<asset>            release asset
<r>.git/HEAD                                   clone export
<r>.git/info/refs
<r>.git/objects/info/packs
<r>.git/objects/pack/pack-<id>.pack, .idx
```

The paths follow GitHub's URL scheme, with each page as the `index.html` of its directory. A branch name or a tree path keeps its `/` characters as nested directories, so the branch `feature/x` has its tree pages below `tree/feature/x/`. Only branches get `tree/`, `blob/`, `raw/` and `commits/` pages. A tag has a row on the tags page, a link to its commit page, and a release page when it has a release.

Every link on a page is relative, so the site works below any path prefix. A link names the `index.html` of a page, so the pages link each other on a host that does not redirect `/x` to `/x/`. A link percent-encodes each byte of a branch name or a tree name outside RFC 3986's unreserved set, `+` included. The feeds and the mise examples use absolute links below `base_url`.

### Output directory

> [!caution]
> The first build into a directory removes everything in that directory that the build did not write, including hidden files. Set `output_dir` to a directory that you use for nothing else.

When a build writes an output, it replaces any file that is already at that path. After a successful build, the build removes every file and directory below `output_dir` that it did not write, so `output_dir` contains only the outputs of the current inputs. If a symlink does not lead to a current output, the build removes the symlink itself. It does not change the file or directory that the symlink points to. A failed build removes nothing. If a write fails after the build starts writing, some outputs can be new while other outputs stay unchanged. To recover, delete `output_dir` and build again.

Before a build scans a repository, `surge` rejects an `output_dir` that is at or below one of the build's inputs, or that holds one of them or the config file: `templates_dir`, `static_dir`, and the git directory, the work tree and `releases_dir` of each repository. The git directory of a linked worktree is its main repository's. For example, it rejects `output_dir = "/srv/git"` when a repository is `/srv/git/foo.git`. These checks compare the directories themselves, not the text of their paths, so they also find a symlink or a different spelling of the same directory. `surge` makes this check even when `templates_dir`, `static_dir` or a `releases_dir` is missing or is not a directory, so one run reports both errors.

Every search of `templates_dir` and `static_dir` skips `output_dir`, so a symlink that leads into `output_dir` cannot make the build read generated files as inputs.

Before it writes, `surge` records every output path, and rejects an output plan with one of these conflicts:

- Two outputs have the same path. Paths that differ only in ASCII letter case count as the same path, because a case-insensitive filesystem such as the macOS default stores them as one file. For example, the branches `x` and `x.atom` of one repository both claim `commits/x.atom`.
- One output path must be both a file and a directory. This check also ignores ASCII letter case.
- An output is at or below an input directory, or overwrites the config file, a template, a static file or a release asset, for example through a symlink inside `output_dir`.
- An output path is longer than 1024 bytes, or has a segment longer than 255 bytes, outside a tree. A tree path over a limit gets a warning instead, as [Names without pages](#names-without-pages) describes.

The build creates `output_dir` only after all of these checks pass, so a rejected build creates nothing.

### Dry run

`surge build --dry-run` runs every phase of a build, but it changes nothing in `output_dir`, and it does not create `output_dir`. For each path that the build would change or keep, it prints one line to `stdout`. Each line has a mark, a tab, and the path. This is a shortened listing:

```text
keep	public/apps-c17.git/HEAD
write	public/apps-c17.git/info/refs
write	public/apps-c17/index.html
keep	public/apps-c17/blob/main/README.md/index.html
write	public/_surge/output-cache.tsv
remove	public/apps-c17/tree/old-branch
remove	public/apps-c17/tree/old-branch/index.html
```

| Mark | Meaning |
| --- | --- |
| `write` | The build would write this output. |
| `keep` | The build would not change this output, because the [output cache](#output-cache) shows that it is current, or because the output already holds the bytes that the build would write. |
| `remove` | The build would remove this path, because the current inputs no longer produce it. The path can be a directory. |

The `write` and `keep` lines come first, in the order that the build plans them:

1. The clone export of each repository, in config order.
2. The pages of each repository: the home, the branches and tags pages, the tag feed and the release feed, then for each branch its tree, blob and raw outputs, its log pages and its commit feed, and then the commit pages.
3. `_headers`.
4. The release outputs of each repository.
5. `index.html` and `404.html`.
6. The static files.
7. The output cache.

The `remove` lines come after them, in sorted order. To get only the paths, for `sort`, `diff`, or `comm`, use `cut -f2-`. A dry run reports a failure in the same way as a build: on `stderr`, with the same exit code. A dry run that fails prints no listing. With `-v`, the progress messages still go to `stderr`, so `stdout` contains only the listing when you send it through a pipe.

A dry run lists an output whose inputs changed since the [output cache](#output-cache) recorded them as `write` without making it, so it renders no such page, and makes no pack for a clone export whose refs changed. It lists that pack as `pack-<id>.pack`, with the id of the new `info/refs`, because the real name is the hash of the pack's bytes. It makes only the outputs that have no recorded inputs, such as `HEAD` and `info/refs`, to compare them with the output. A dry run writes nothing, so a build can still fail after a dry run succeeds. A dry run cannot find these failures:

- The build has no permission to write to `output_dir`, or its disk is full.
- `surge` cannot read a static file or a release asset. A dry run does not open the files that it would copy.
- An old file is at a path where a new output needs a directory, or an old directory is at a path where a new output needs a file. For example, the old output is the file `public/apps-c17/raw/main/docs`, and the new output is `public/apps-c17/raw/main/docs/guide.md`.
- `output_dir` exists but is not a usable directory.

### Output cache

A build writes the output cache to `_surge/output-cache.tsv` below `output_dir`. It has one row for each output whose inputs the build can name: the output's path, its inputs, and its size. The next build keeps an output, and does not make it again, when its inputs are the same and the file still has that size. Every other output is made on every build and written only when its bytes change.

- Every page depends on the page recipe: the `surge` version, the grammars that the binary includes, `title`, `description`, `author`, `base_url`, `log_page_size`, `feed_count`, every limit but `pack_len_max`, and the path, size and modification time of every file below `templates_dir`. A change to any of them, a partial included, makes every page again.
- Every page of a repository also depends on what its `repo` map shows: the default branch, whether the branch names a commit, the description, and whether the repository has a release project.
- A commit page depends on its commit. A blob page depends on its blob and mode, a tree page on its tree, and both on the branch's root tree when they show Markdown or Org, because their links resolve against every path of the branch. The page of a branch's root also depends on the branch's newest commit. A log page depends on its commits and the refs that name them, and a feed on its entries.
- The home, the branches and tags pages and the site index depend on every ref and on the repository's place in the config, description, newest commit date and projects. The tags page and the release outputs also depend on the releases.
- A raw copy depends only on its blob. A static file and a release asset depend on their path, size and modification time. A clone pack depends on `info/refs`.

A new commit on a branch makes the branch's log pages and feed, the new commit's page, the tree and blob pages and raw copies of the paths that it changes, the pages that show Markdown or Org, the page of the branch's root, the repository's home and refs pages, the site index, and the changed files and new pack of the clone export again. `--force` ignores the output cache and writes every output.

Some tools change a file but keep its size and modification time, and libgit2 reads `$GIT_DIR/info/attributes` and a work tree's `.gitattributes` for a diff, which the output cache does not record. After such a change, run `surge build --force` once.

A build writes the output cache after its last output, also when an output failed, with a row only for what it wrote or kept. An interrupted build leaves the cache of the build before it, which keeps no output that the build cut short, because the file no longer has the recorded size, and none that it rewrote, because its inputs changed.

When you publish `output_dir`, you also publish the output cache. Its rows name the output paths and the ids of the commits, trees and blobs they show. If you do not want to publish this information, exclude `_surge/output-cache.tsv` when you deploy.

### Limits

The build rejects an input file that is too large before it reads the file. The error message gives the limit and the file size.

| Input | Limit |
| --- | --- |
| Config file | 1 MiB |
| Template or partial | 4 MiB |
| A repository's `description` file | 64 KiB |

A site can have at most 64 partials in the `partials/` directory of `templates_dir`. A rendered page can be at most 256 MiB, a render can use at most 300,000 partial expansions, and a render that passes a limit fails. An output cache over 256 MiB is not read, and the build makes every output again. A blob over `blob_len_max` or `raw_len_max` gets the treatment that [Configuration](#configuration) describes. A static file and a release asset have no size limit, because the build copies each one from the file itself.

The build reserves its memory once, when it starts, in fixed sizes that come from these limits: 4 GiB of address space for the persistent data of each thread, 3 GiB for its temporary data, and 256 MiB for the log and for each worker's job logs. Under an address-space limit (`ulimit -v`) the build runs fewer workers and then a smaller reservation, as [libs/ARCHITECTURE.md](../../libs/ARCHITECTURE.md#build-memory) describes. A build that needs more memory than it reserved prints the errors that it logged up to that point, then `surge: out of memory`, with the name of the repository or the file that the thread was working on when there was one, and exits with status 1.

## Repos and projects

Each `[[repo]]` table names one git repository: a work tree or a bare git directory. `surge` reads it through libgit2, with the user's and the system's git config ignored, so a setting in `~/.gitconfig` never changes a page. It reads a repository that another user owns, as the [trust model](#trust-model) describes.

`surge` publishes every branch below `refs/heads/` and every tag below `refs/tags/`. It has no option to leave out a branch. To keep a branch off the site, delete it from the repository, or publish a mirror without it. A symbolic ref below `refs/heads/` or `refs/tags/` is neither published nor exported.

The build fails for a repository that has one of these refs, and names each ref involved:

- Two branches whose names, or the directories above them, differ only in ASCII letter case, such as `Main` and `main`, `a` and `A/b`, or `X/x` and `x/y`, because their pages would share a directory on a case-insensitive filesystem.
- A branch or tag whose name is not valid UTF-8, which git allows, because a filesystem that takes only UTF-8 names, such as macOS's, refuses its pages' directories.
- A ref whose name is a `/`-prefix of another, such as `feature` and `feature/x`. git refuses to make them, but a hand-written `packed-refs` can hold them.
- A ref whose name git calls invalid.

A branch that names a tree or a blob, and not a commit, gets a warning and no pages, and its row on the branches page has no links.

An empty repository, with no commit, has a home that says so, branches and tags pages, a tag feed, and a clone export whose `HEAD` names the branch that the repository's `HEAD` names.

The site index lists each repository in config order, and nests its projects under it. A project is a directory of a monorepo, such as `apps/sosig`, with a name and a description. The index links each project to the tree page of its directory on the default branch, and a project with a `tag_prefix` to its release page. [Releases](#releases) describes release projects.

### Documents

The home of a repository shows the root of its default branch, the newest commit of that branch, the clone URL, and its documents, which a template can show as tabs. Each tree page shows the documents of its own directory in the same way. `surge` looks for these documents, in this order:

| Kind | Flag | Base names |
| --- | --- | --- |
| `readme` | `is_readme` | `README` |
| `license` | `is_license` | `LICENSE`, `LICENCE`, `COPYING` |
| `code-of-conduct` | `is_code_of_conduct` | `CODE_OF_CONDUCT` |
| `contributing` | `is_contributing` | `CONTRIBUTING` |
| `security` | `is_security` | `SECURITY` |

`surge` looks for each kind in the directory of the page, and for the root, then in `docs/`. A document's file name is one of the base names of its kind in any ASCII letter case, with no extension or with `.md`, `.markdown`, `.org` or `.txt` in any letter case. The first match in byte order wins. A symlink and a file over `blob_len_max` are skipped. `surge` renders a `.md` or `.markdown` file as Markdown with md4c, a `.org` file as Org with org4c, and any other file as escaped text in a `<pre>`. The front matter of a Markdown file, TOML between `+++` lines or YAML between `---` lines at its start, shows as a `toml` or `yaml` code block, as written, because `surge` does not read it. A file that starts with a `---` thematic break and has another later therefore shows the text between them as YAML, which GitHub also reads as front matter.

A heading's level is its level in the source, as on GitHub: Markdown's `#` and Org's `*` are `<h1>`, and each lower level adds one, up to `<h6>`. Each heading of a document has an `id`, so a link to `#building` in a README finds `## Building`: an Org headline's `CUSTOM_ID`, or else its text with ASCII letters in lowercase, digits and `_` kept, each run of spaces and `-` as one `-`, none at either end, and every other character dropped, a non-ASCII letter included. A heading with no such text is `section`, and a repeated one gets `-1`, `-2` and so on. The id matches GitHub's for ASCII text without punctuation between words, which GitHub keeps as `--`, so `## Build & test` is `#build-test` here and `#build--test` on GitHub. Each heading sits in a `<div class="heading">` with a link to itself after it, `<a class="anchor" href="#<id>" aria-labelledby="<id>"></a>`, empty for a stylesheet to draw. The link takes its name from the heading, so it holds no English word of its own, and it sits outside the heading, so it is not part of the heading's name. A template's own ids should hold a character that these ids never do, such as `.`, so that a heading cannot take one.

### Links in documents

A Markdown or Org link in a README, a document, or a Markdown or Org blob resolves against the branch that the page belongs to. A relative target resolves against the directory of the file, and a target that starts with `/` against the root of the repository. `surge` decodes `%XX` escapes in the target before it looks the target up, and then writes the link to one of these:

- A file: its blob page.
- A directory: its tree page.
- A target that names neither, but names a file when `.md` is added: that file's blob page.
- An image target that names a file: its raw copy, or its blob page when the file is over `raw_len_max`.

The link keeps its query and fragment. An Org `file:` link's `::#<id>` search option becomes the fragment `#<id>`, and its other search options are dropped. `surge` writes any other target as it is written, such as an absolute URL, a target with another scheme, or a relative target that names no path. A verbose build counts the relative links that name no path, for each repository, on the pages that it renders: a page that the build keeps adds none, and a document that two pages show, such as a README on the repository home, on its tree page and on its blob page, counts on each. On GitHub such a link gives a 404, and with every branch published, a warning for each one would mostly report old branches.

Raw HTML resolves the same way, as GitHub resolves it, in Markdown and in Org's `#+BEGIN_EXPORT html` blocks and `@@html:...@@` snippets. So a README that centers its logo with `<div align="center"><img src="media/logo.svg"></div>`, or picks one for the color scheme with a `<picture>`, shows it on every page that shows the README:

- A file that an element embeds resolves as an image does: the `src` of an `<img>`, a `<source>`, a `<video>`, an `<audio>`, a `<track>`, an `<iframe>` or an `<embed>`, the URL of each candidate of the `srcset` of an `<img>` or a `<source>`, a `<video>`'s `poster`, and an `<object>`'s `data`. A file over `raw_len_max` has no raw copy, so it links its blob page, which a video or an iframe cannot show.
- The `href` of an `<a>` or an `<area>` resolves as a link does.

`surge` writes an attribute with a resolved target double-quoted, keeps the descriptors of a `srcset`, such as `2x`, and leaves every other attribute, every other element, comments, and the contents of `<script>` and `<style>` as they are written.

### Files

A tree page lists the directories first and then the files, each group in byte order, with breadcrumbs from the repository to the directory. A symlink is a row with its target text, of which a row shows at most 4096 bytes, with a warning for a longer target, and a submodule is a row with its commit id, and neither has a link.

A blob page shows the mode and size of the file, links its raw copy, and shows its lines with `#L<n>` anchors, highlighted as [Syntax highlighting](#syntax-highlighting) describes. libgit2's binary check decides whether the file is binary. A binary file shows no lines. A template can show an image file, `.png`, `.jpg`, `.jpeg`, `.gif`, `.webp`, `.svg`, `.avif`, `.bmp` or `.ico` in any letter case, with `<img>` from its raw copy. A Markdown or Org file has both views in one page: the rendered view and the highlighted source. The fixture site has a small web component, [surge-view.js](tests/fixtures/site/static/surge-view.js), which shows the rendered view, or the source when the fragment names a line, such as `#L12`. Without JavaScript, a page that uses it shows both views.

The raw copy of a file is its bytes at `<r>/raw/<ref>/<p>`, for every file up to `raw_len_max`, SVG included.

### History

A branch's log lists its commits newest first, in the order of `git log --date-order`: by committer time, with no commit before one of its children. Each page holds `log_page_size` commits, with links to the first, previous, next and last pages. Each commit shows the branches and tags that name it. Every new commit moves the page boundaries, so a branch that moved makes all of its log pages again.

Every commit that a branch or a tag reaches has a commit page. A commit page shows the message, the author's and committer's names and dates, every parent as a link, a diffstat, a tree of the changed files, and the diffs in the order of that tree. It shows no e-mail address, and no branch or tag that names the commit, so it depends only on the commit.

A changed image shows its old and new sides, embedded in the commit page as `data:` URIs, because no other output holds a past version of a file. An image is a file with an [image extension](#files) whose diff the page shows and whose type did not change. Its sides go in, in the files' order, the old side and then the new, while their bytes stay within `image_len_max` in all, so a commit that changes many large images stays a page of bounded size. An SVG also shows its diff, since it is text.

The diff of a commit is against its first parent, or against nothing for a root commit, with rename detection as git's: a deleted file and an added one whose contents are at least 50% alike are a rename, as git finds them with its default `diff.renameLimit` of 1000. A commit whose deleted files times its added files pass 1000 × 1000 finds only exact renames, as git does then, every exact rename of up to 10000 changed files. The repository's own `diff.renameLimit` is not read. A file that changed from one type to another, such as a file that became a symlink, is one changed file. A side over `blob_len_max` shows no lines. Each file has the anchor `#diff-<n>`, numbered from 0 in the order of the changed-files tree, and each line has `#diff-<n>-hunk-<h>-L<old>` for its old line number and `-R<new>` for its new one, with hunks numbered from 0. The caps apply in that order:

- A file past `diff_file_count_max` is listed without its diff.
- The file whose lines would pass `diff_line_count_max` or `diff_len_max` is cut short there, and every file after it is listed without its diff.

`diff_len_max` counts the bytes of the lines that the page shows, and the diffstat counts those lines.

### Names without pages

A tree can hold names that a page cannot use. Each of these entries keeps its row in the listing, without a link, and gets no blob page, raw copy or tree page. `surge` does not enter such a directory. Each case is one warning that names every path involved:

- `.`, `..`, `.git` in any letter case, and the spellings that git's `verify_path` treats as `.git`: trailing dots or spaces, HFS+'s ignorable code points, an NTFS stream after a `:`, and NTFS's short name `git~1`.
- A name that holds `/`, `\`, a control byte, or bytes that are not UTF-8.
- A directory named `index.html`, whose tree page would be inside its parent's.
- Names in one directory that differ only in ASCII letter case, such as `xt_CONNMARK.h` and `xt_connmark.h`. `surge` picks no winner. Names that differ only in Unicode normalization or in non-ASCII case are not checked.
- A name whose pages would pass the output path limits: 1024 bytes for the path below `output_dir`, or 255 bytes for one segment.

`surge` shows every name, ref and message as valid UTF-8, as [Templates](#templates) describes. A warning shows a control byte or an invalid byte as `\xNN`.

## Releases

### Release directories

CI fills the `releases_dir` of a repository with one directory for each release, named after its tag, and `surge` publishes what it finds there. A release is a tag that has a directory `<releases_dir>/<tag>/`. A tag without such a directory, or with a file of that name, is only a tag.

- The notes of a release are the message of its annotated tag, rendered as Markdown. Their links pass through as they are written. A signed tag's signature is not part of the notes: as git does, `surge` cuts the message at the last line that opens a PGP, X.509 or SSH signature, such as `-----BEGIN PGP SIGNATURE-----`, and the release map holds the signature apart. `surge` does not check it, nor a commit's signature, which a commit summary holds as its `gpgsig` header gives it.
- The assets of a release are the regular files in its directory, in byte order of name. A subdirectory is not an asset. The build copies each asset to `<r>/releases/download/<tag>/<asset>` from the file itself, with no size limit.
- The date of a release is the date of its annotated tag, or the committer date of the commit of a lightweight tag. A tag of a tag uses the outer tag.

`releases_dir` does not have to exist, because CI may create it with the first release. If it exists, it must be a directory.

A release project groups releases by tag prefix. The release projects of a repository are its projects that have a `tag_prefix`, in config order. A repository with a `releases_dir` and no project with a `tag_prefix` has one release project of its own, named after the repository, whose tags may start with `v`. The version of a release is its tag without the project's prefix, or for the repository's own project without a leading `v`. For example, the tag `sosig-v0.1.0` of the project with `tag_prefix = "sosig-v"` is the version `0.1.0`.

A release whose tag fits no project's prefix keeps its release page and its row on the releases page, belongs to no project, and gets a warning. A project with a `tag_prefix` in a repository without `releases_dir` has a project page with no releases.

### Version lists and mise

Each release project has two files that mise's `http:` backend reads:

- `<r>/releases/<project>/versions.txt` lists the project's versions, one for each line, sorted by Semantic Versioning precedence, with the newest last. A version must be `MAJOR.MINOR.PATCH`, with an optional `-PRE`, as Semantic Versioning 2.0.0 writes them, and without build metadata. A release whose version is not one keeps its page, is left out of the list, and gets a warning. When two tags of one project give the same version, such as `v1.0.0` and `1.0.0`, the list has the first tag in byte order, and the other tag keeps its page and gets a warning.
- `<r>/releases/<project>/versions.json` lists the same versions in the same order, with the date of each release in RFC 3339, on one line: `{"versions":[{"version":"0.1.0","date":"2026-01-01T13:00:00+01:00"},…]}`. mise reads the dates for its `minimum_release_age` check.

A release project shows how to install it with mise only when the config asks for it, with `mise = true` on its `[[repo.project]]` table, or on the `[[repo]]` table for the repository's own project, because a project can publish releases that mise cannot install, such as a library or a set of documents. Every release project has its version lists, which only list versions and suit other tools as well.

The data of each releases page, project page and release page holds a `mise.toml` example for each project with `mise`, `mise_toml`, built from `base_url`, and an empty one for any other project. For the project `alpha` of the repository `monorepo` in the fixture site, it is this:

```toml
[tools."http:alpha"]
version = "latest"
version_list_url = "https://git.example.org/monorepo/releases/alpha/versions.txt"
version_date_url = "https://git.example.org/monorepo/releases/alpha/versions.json"
version_date_expr = 'map(fromJSON(body).versions, { [#.version, #.date] })'
checksum_url = "https://git.example.org/monorepo/releases/download/alpha-v{{ version }}/SHA256SUMS"
strip_components = 1
bin_path = "bin"

[tools."http:alpha".platforms]
linux-x64 = { url = "https://git.example.org/monorepo/releases/download/alpha-v{{ version }}/alpha-{{ version }}-x86_64-linux-musl.tar.gz" }
linux-arm64 = { url = "https://git.example.org/monorepo/releases/download/alpha-v{{ version }}/alpha-{{ version }}-aarch64-linux-musl.tar.gz" }
macos-arm64 = { url = "https://git.example.org/monorepo/releases/download/alpha-v{{ version }}/alpha-{{ version }}-aarch64-macos.tar.gz" }
```

The example expects archives that follow the naming of this repository's [release.yml](../../.github/workflows/release.yml): `<project>-<version>-<target>.tar.gz`, with the binary at `<root>/bin/<project>`. It takes the archives and the part of the tag before the version from the newest listed release that is not a pre-release, or from the newest listed release when every one is a pre-release, because mise's `latest` passes over a version with a `-PRE`. It has a platform line, `linux-x64`, `linux-arm64` or `macos-arm64`, for each of the targets `x86_64-linux-musl`, `aarch64-linux-musl` and `aarch64-macos` whose archive that release has, and `checksum_url` only when that release has `SHA256SUMS`. If the project lists no version, or that release has none of these archives, the example is empty.

### Checksums

When a release directory has a `SHA256SUMS` file, the build copies it with the other assets, and the mise example sets `checksum_url` to it. `surge` computes no checksums: CI writes this file, as `release.yml` does. A release without `SHA256SUMS` still publishes, with a warning when its project has `mise`, and when it is the release that the example takes its archives from, the example has no `checksum_url`.

mise checks the checksums only through its lock file. A plain `mise install` never reads `checksum_url`. `mise lock` records the digest of each platform's archive in `mise.lock`, and later installs refuse a download that does not match. The fixture templates say this next to the example.

These are the warnings of the release scan:

```text
warning: release tag fits no project's 'tag_prefix', so no 'versions.txt' lists it: '<tag>' (in repo '<repo>')
warning: release version is not MAJOR.MINOR.PATCH[-PRE], so 'versions.txt' leaves it out: '<version>' (in repo '<repo>', tag '<tag>')
warning: release tags name the same version, so 'versions.txt' lists only the first: '<tag>', '<tag>' (in repo '<repo>')
warning: release has no 'SHA256SUMS', so 'mise lock' records no checksums: '<tag>' (in repo '<repo>')
```

## Feeds

Each repository has Atom feeds:

| Feed | Entries |
| --- | --- |
| `<r>/commits/<ref>.atom` | The newest `feed_count` commits of the branch. Each entry is a commit page, with the subject as its title, the committer date, the author, and the rest of the message as text. A branch that names no commit has no feed. |
| `<r>/tags.atom` | The newest `feed_count` tags by date, a tie in byte order of name. Each entry links the tag's release page, else its commit page, else the tags page. Its id is the tags page with the tag's name as the fragment, so two tags of one commit are two entries. Its text is the tag's message, without its signature. |
| `<r>/releases.atom` | Only with a release project. The newest `feed_count` releases by date, a tie in byte order of tag. Each entry links its release page and has the release notes as HTML. |

The author of a tag or a release entry is the tagger, or else the author of its commit. A feed's `modified` date is the date of its newest entry, and a feed or an entry without a date has the Unix epoch, because Atom requires a date. Every link in a feed is absolute, below `base_url`, and every text holds only characters that XML 1.0 allows, as [Templates](#templates) describes.

## Clone export

Each repository gets a clone export, `<r>.git/`, that git clones over dumb HTTP from any static host:

```sh
git clone https://git.example.org/apps-c17.git
```

The export holds `HEAD`, `info/refs`, `objects/info/packs`, and one pack with its idx. It publishes only `refs/heads/*` and `refs/tags/*`, and never the repository's `config` or hooks:

- The pack holds every object that a branch or a tag reaches, annotated and nested tags included, and tags of trees or blobs.
- `info/refs` lists each ref sorted by name, with a `^{}` line after each annotated tag, as `git update-server-info` writes it.
- `HEAD` names the default branch, so a detached `HEAD` is never published.
- An empty repository has no pack, an empty `info/refs` and an empty `objects/info/packs`.

The name of the pack is the hash of its content, and the same refs give the same pack. A build keeps the pack while the refs do not change. When the refs change, the build writes a new pack, and keeps the previous pack for one more build, without listing it, so a client that read an older `info/refs` from a cache can still finish its clone. The build after that removes it, also when a build between them failed before it wrote the new pack. A `--force` build does not read the output cache, so it does not know the previous pack and removes it at once. A dry run does not make a changed pack, as [Dry run](#dry-run) describes.

## Templates

Templates use [Mustache](https://mustache.github.io/) syntax and are in `templates_dir`. `surge` has no default templates, so every site supplies the templates below. [tests/fixtures/site/templates/](tests/fixtures/site/templates/) holds a complete set, which the golden test renders.

| Template | Required | Renders |
| --- | --- | --- |
| `index.html` | Always | The site index, `index.html`. |
| `404.html` | Always | The not-found page, `404.html`. A host serves it at any depth, so it links with `{{site.base_url}}`, not with `{{path_to_root}}`. |
| `tree.html` | Always | Each repository home, `<r>/index.html`, and each tree page. The home is the tree page of the default branch's root, at the repository's own URL. |
| `blob.html` | Always | Each blob page. |
| `branches.html` | Always | Each branches page. |
| `tags.html` | Always | Each tags page. |
| `commits.html` | Always | Each log page. |
| `commit.html` | Always | Each commit page. |
| `commits.atom` | Always | Each commit feed. |
| `tags.atom` | Always | Each tag feed. |
| `releases.html` | With a release project | Each releases page, `<r>/releases/index.html`. |
| `release-project.html` | With a release project | Each project page, `<r>/releases/<project>/index.html`. |
| `release.html` | With a release project | Each release page, `<r>/releases/tag/<tag>/index.html`. |
| `releases.atom` | With a release project | Each release feed. |

A site has a release project when a repository has a `releases_dir`, or a project with a `tag_prefix`. A site without one needs none of the four release templates. Every required template must exist.

`{{value}}` escapes its output for HTML. `{{{value}}}` and `{{& value}}` write it without escaping, and work only for a key that ends in `_html`, which holds HTML that `surge` made itself: `rendered_html`, `source_html`, `diff_html`, `notes_html` and `content_html`. Any other key that the data holds fails the render, so a value that it did not make, such as a title, cannot add markup to a page. A name that the data does not hold writes nothing, as with `{{value}}`. In an Atom template, `{{content_html}}` inside `<content type="html">` escapes the HTML, as Atom expects.

Every page and feed that `surge` renders is valid UTF-8 without the characters that XML 1.0 forbids, so a browser and a feed reader accept it. `surge` replaces each invalid sequence in a value or a template with U+FFFD, and drops the C0 control characters but tab, line feed and carriage return, and U+FFFE and U+FFFF.

A partial reference `{{> name}}` loads `templates_dir/partials/<name>.html`. A partial name can contain only letters, digits, `_` and `-`. A reference to another name fails the build, and `surge` ignores the other files of `partials/` and its subdirectories. The build compiles every template that it renders and every partial once, before it writes anything, and reports every template that cannot be read or does not compile in one run. The feeds also use the `.html` partials. A partial can include itself, as the fixture's changed-files tree does, and a partial that never stops including itself fails at the expansion limit in [Limits](#limits).

### Data trees

`surge` builds the data of each page in C, and every map holds every key that the tables below list, with empty text or `false` where a value is absent. Mustache looks up a name that a map does not hold in the enclosing maps, so a missing key would silently show a value from an outer scope. Empty text is false in a section, so `{{#description}}…{{/description}}` shows its contents only for a description that is not empty. A flag such as `is_last` is `true` when it holds, and empty otherwise, so it works both as a section and as text. A date is RFC 3339 in the offset that git recorded, such as `2026-01-01T13:00:00+01:00`. A zero offset is written `Z`. An offset over 23:59 either way is written as `Z`, and a date outside the years 0000 to 9999 as `1970-01-01T00:00:00Z`, because RFC 3339 cannot write them. A size is a number of bytes.

Every link in a page's data is relative to the page and already holds its `path_to_root`, except in the feeds, where every link is absolute.

Every template gets these keys:

| Key | Value |
| --- | --- |
| `site.title`, `site.description`, `site.author`, `site.base_url` | The values from the configuration. `site.base_url` has no trailing `/`. |
| `generator` | The name and version of `surge`, for a tag such as `<meta name="generator" content="{{generator}}">`. |
| `path_to_root` | The `../` prefix from the page to `output_dir`, empty for a page at the top. A link to a static file is `{{path_to_root}}style.css`. |

The pages of a repository share these maps:

| Map | Keys |
| --- | --- |
| `repo` | `name`, `description`, `default_branch`, `url` (the repository home), `clone_url` (`<base_url>/<r>.git`), `branches_url`, `tags_url`, `tags_feed_url`, `commits_url` (the first log page of the default branch, empty when it names no commit), `has_releases` (whether the repository has a [release project](#release-directories)), `releases_url` (its releases page, empty without one), and `releases_feed_url` (its release feed, empty without one). A template can build a menu of the repository from it on every page. |
| `ref` | The branch of a page: `name`, `tree_url` (its root's tree page), `commits_url` (its first log page), `commits_feed_url` (its commit feed). |
| A commit summary | `oid`, `short_oid` (7 hex digits), `subject`, `author` (a name), `date` (the committer date), `signature` (the signature of a signed commit, or empty), `signature_kind` (`PGP`, `SSH` or `X.509`, from the line that opens the signature, or empty), `url` (its commit page). It is `false` for a ref that names no commit, so `{{#commit}}…{{/commit}}` shows its contents only for a commit. |
| An entry | One row of a directory: `name`, `kind` (`dir`, `file`, `executable`, `symlink` or `gitlink`), `is_dir`, `is_file`, `is_executable`, `is_symlink`, `is_gitlink`, `url` (empty for an entry without a page), `is_linked`, `link_target` (a symlink's target), `oid`, `size` (empty for a directory or a submodule). |
| A breadcrumb | `name`, `url`, `is_last`. The last breadcrumb is the page's own directory or file, with an empty `url`. |

#### `index.html`

| Key | Value |
| --- | --- |
| `repos` | One map for each repository, in config order: `name`, `description`, `url` (its home), `clone_url`, `default_branch`, `modified` (the committer date of the default branch's newest commit, empty for an empty repository), `projects`. |
| `repos[].projects` | One map for each project, in config order: `name`, `description`, `url` (its directory's tree page on the default branch), `has_releases` (whether it has a `tag_prefix`), `releases_url` (its project page, empty without one). |

#### `404.html`

`site`, `generator` and `path_to_root` only.

#### `tree.html`

The home and every tree page have the same keys, so one template renders them alike.

| Key | Value |
| --- | --- |
| `repo`, `ref` | The shared maps. For an empty repository, `ref` is the default branch with empty links. |
| `is_home` | Whether the page is the repository home, `<r>/index.html`, rather than a tree page. |
| `is_empty` | Whether the default branch has no commit, as in an empty repository, which has a home and no tree page. |
| `commit` | The commit summary of the branch's newest commit on the page of the root, and `false` on the page of any other directory. |
| `path` | The path of the directory, empty for the root. |
| `is_root` | Whether the directory is the root. |
| `breadcrumbs` | From the repository to the directory. |
| `tree_entries` | The entries of the directory: directories first, then files, each group in byte order. |
| `documents` | One map for each document of the directory, in the order of [Documents](#documents), the README included: `kind`, one flag for each kind, as [Documents](#documents) lists them, true for this one's, so a template can name each kind as it likes, `is_main` (whether it is the document that the page shows first: the README, or else the first document, so a page without a README can show its license in place of one), `path`, `url` (its blob page), `rendered_html` (rendered). |

#### `blob.html`

| Key | Value |
| --- | --- |
| `repo`, `ref` | The shared maps. |
| `path`, `name` | The path and the file name. |
| `breadcrumbs` | From the repository to the file. |
| `mode` | `100644` or `100755`. |
| `is_executable` | Whether the mode is `100755`. |
| `size` | The size of the blob. |
| `raw_url`, `has_raw` | The raw copy, empty and `false` for a blob over `raw_len_max`. |
| `is_too_large` | Whether the blob is over `blob_len_max`, so the page has no contents. |
| `is_binary` | Whether libgit2 judges the blob binary. Always false for a blob that is too large. |
| `is_image` | Whether the file name ends in an [image extension](#files). |
| `is_markup` | Whether the file is Markdown or Org. |
| `rendered_html` | The rendered view of a Markdown or Org file, or empty. |
| `source_html` | The lines, or empty for a binary blob or one that is too large: a `<pre class="surge-code"><code>` with a code line for each line, whose number is an `<a id="L<n>" href="#L<n>">`. |
| `line_count` | The number of lines. |

#### `branches.html`

| Key | Value |
| --- | --- |
| `repo` | The shared map. |
| `branches` | One map for each branch, in byte order of name: `name`, `is_default`, `author` and `date` (those of its commit, as the tags page has them for a tag), `tree_url`, `commits_url`, and `commit`, the commit summary of the branch. A branch that names a tree or a blob has an empty `author`, `date` and links, and a `commit` of `false`. |

#### `tags.html`

| Key | Value |
| --- | --- |
| `repo` | The shared map. |
| `tags` | One map for each tag, newest first by date, the tagger's, or else the committer's of the commit that the tag names, a tie in byte order of name, and a tag with neither, a lightweight tag of a tree or a blob, last: `name`, `is_annotated`, `author` (the tagger, else the author of the commit), `date` (that date in RFC 3339, or empty for none), `signature` (the signature of a signed tag, or empty), `signature_kind` (as a commit summary has it), `release_url` (its release page, or empty), and `commit`, the commit summary of the commit that the tag peels to, `false` for a tag of a tree or a blob. |

#### `commits.html`

| Key | Value |
| --- | --- |
| `repo`, `ref` | The shared maps. |
| `page` | `number` (from 1), `count`, `is_first`, `is_last`, and `first_url`, `previous_url`, `next_url` and `last_url`, each empty where there is no such page or it is this page. |
| `commits` | The commits of the page, newest first: the commit summary keys, and `refs`, the branches and tags that name the commit, each with `name`, `kind` (`branch` or `tag`), `is_branch`, `is_tag` and `url` (a branch's tree page, or the tags page). |

#### `commit.html`

| Key | Value |
| --- | --- |
| `repo` | The shared map. |
| `commit` | The commit summary keys, and `body` (the message after the subject), `author_date`, `committer` (a name), `is_root`, `is_merge`, and `parents`, each with `oid`, `short_oid` and `url`, and an empty `subject`, `author` and `date`. |
| `stats` | `file_count`, `additions`, `deletions`, counting the lines that the page shows. |
| `is_truncated` | Whether a cap cut the diff. |
| `files` | The changed files, in the order of the changed-files tree: `index` (from 0), `anchor` (`diff-<index>`), `path`, `old_path` (empty unless renamed), `similarity` (how alike a renamed file's sides are, in percent, `100` for an exact rename, empty unless renamed), `status` (`added`, `deleted`, `modified`, `renamed` or `type-changed`), `is_added`, `is_deleted`, `is_modified`, `is_renamed`, `is_type_changed`, `is_too_large`, `is_binary`, `is_shown` (whether the page shows its diff), `is_truncated` (whether a cap cut its diff), `additions`, `deletions`, `diff_html`, `is_image` (whether the page shows the file as an image: one whose diff it shows, with an [image extension](#files), and whose type did not change), `old_image_url` and `new_image_url` (each side as a `data:` URI, or empty for a side that does not exist or that `image_len_max` leaves out), and `is_image_omitted` (whether a side with bytes was left out). |
| `tree` | The root node of the changed-files tree. A node has `name`, `is_dir`, `children` (a directory's nodes, empty for a file) and `file` (a file's map from `files`, or `false` for a directory), so a partial that includes itself can render it. |

`diff_html` is a `<pre class="surge-diff"><code>` with a code line for each hunk's header, with the class `surge-hunk`, and a code line for each line, with the class `surge-add`, `surge-del`, `surge-context` or `surge-no-newline`. Each code line has two numbers, the old line's and the new line's, either of which is empty for a line that one side lacks. A number is an `<a>` with the id `<anchor>-hunk-<h>-L<old>` or `<anchor>-hunk-<h>-R<new>`, and a link to it. `diff_html` is empty for a file whose diff the page does not show.

A code line, in `source_html` and `diff_html`, is the same markup as a line of an Org code block with line numbers, in a document or a rendered blob. It is a `<span class="code-line">` that holds a `<span class="code-line-numbers">`, with a `code-line-number` for each number, and then the line's text, with its line end, in a `<span class="code-line-text">`. A stylesheet can lay out the lines of a block as the rows of a grid, with a column for each number and one for the text, keep the numbers in place while the code scrolls sideways, and keep them out of a copy of the code.

#### `releases.html`, `release-project.html` and `release.html`

| Template | Keys besides `repo` |
| --- | --- |
| `releases.html` | `projects`, one project map for each release project in config order, and `releases`, every release: each project's releases in project order, then the releases of no project in byte order of tag. |
| `release-project.html` | `project`, the project map. |
| `release.html` | `release`, the release map, and `project`, the project map of its project, `false` for a release of no project. |

A project map has `name`, `url` (its page), `versions_url`, `versions_json_url`, `mise_toml` (the [mise example](#version-lists-and-mise) as text, empty without `mise`), and `releases`, its listed releases newest first, then its other releases in byte order of tag.

A release map has `tag`, `version`, `date` (the date of its tag, as the tags page has it), `url` (its page), `is_listed` (whether `versions.txt` lists it), `project_name` and `project_url` (empty for no project), `commit` (the commit summary of the tag, `false` for a tag of a tree or a blob), `notes_html` (the tag's message rendered as Markdown, without its signature, whose relative links work on the release page and in a feed with `xml:base`), `signature` (the signature of a signed tag as the tag holds it, or empty), `signature_kind` (as a commit summary has it), `has_sums` (whether it has `SHA256SUMS`), `is_mise` (whether its project has `mise`), and `assets`, each with `name`, `url` and `size`.

#### `commits.atom`, `tags.atom` and `releases.atom`

| Key | Value |
| --- | --- |
| `repo` | The shared map, with absolute links. |
| `ref` | Only in `commits.atom`: the branch, with absolute links. |
| `feed` | `id` and `url` (the feed's own URL), `html_url` (the first log page, the tags page or the releases page), `modified`. |
| `feed_entries` | Newest first, each with `id`, `title`, `url`, `modified`, `author`, `content` (text, empty for a release) and `content_html` (HTML, empty for a commit or a tag). A relative link in `content_html` is relative to the entry's page, so the fixture's feeds set `xml:base="{{url}}"` on its `<content>`, which a feed reader resolves the link against. |

The fixture templates also give each feed an `<author>`, the site's `author`, because Atom needs one for an entry that has none.

### Syntax highlighting

A blob page and a diff highlight a file when the binary includes a grammar for the language that its path names. `surge` writes each token as a `<span>` whose class tells the kind of token:

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

`surge` does not supply a stylesheet, so each site must set the colors for these classes. A span never crosses a line, so every line of `source_html` and `diff_html` is whole HTML.

The path of a file names its language by the exact file name, or else by its extension in any letter case. `surge` does not read the contents of a file to choose:

| Language | File names and extensions |
| --- | --- |
| C | `.c`, `.h` |
| Bourne shells | `.sh`, `.bash`, `.zsh`, `.ksh` |
| HTML | `.html`, `.htm` |
| CSS | `.css` |
| JavaScript with JSX | `.js`, `.mjs`, `.cjs`, `.jsx` |
| TypeScript | `.ts`, `.mts`, `.cts` |
| TSX | `.tsx` |
| JSON | `.json` |
| Make, CMake, Dockerfile | `Makefile`, `GNUmakefile`, `makefile`, `.mk`, `CMakeLists.txt`, `.cmake`, `Dockerfile` |

The release binaries include no grammar for Make, CMake or Dockerfile, so those files are escaped text. If you compile `surge` yourself, you can choose its grammars with `APPS_GRAMMARS`, as [BUILD.md](../../BUILD.md#grammars) describes. `surge --version` lists the grammars that a binary includes. `surge` highlights each file in one language, so in an HTML file it only escapes the contents of the `<script>` and `<style>` elements. A code block in a README, a document or release notes is highlighted by the language that the block names, such as `c` or `bash`, in any letter case.

A diff highlights the whole old blob and the whole new blob of a file once each, and gives each line the HTML of its side: the old side for a deleted line, and the new side for an added line or a context line. So a line inside a comment or a string that starts above the hunk keeps its color. A diff is highlighted only when the whole diff of the commit is under its caps. `surge` writes a file as escaped text when its path names no grammar, when it is over `blob_len_max`, or when tree-sitter would need more than 128 MiB for it.

## Host notes

`surge` writes plain files that any static host serves. A few host settings decide whether every page and every clone works.

### 404 handling

A dumb-HTTP clone asks for paths that do not exist: git first asks for each object as a loose object, and only then reads the pack. A host must answer a missing path with a 404. `surge` always writes `404.html`, from the required template, and a static host serves it for a missing path with status 404. Cloudflare Pages assumes that a project without a top-level `404.html` is a single-page application, and answers every missing path with 200 and the index page. Against such a host, git 2.56 reports the page as a corrupt object (`inflate: data stream error`) for each loose object it asks for, then finds the objects in the pack and finishes the clone.

### Single-page-app fallback

If a host has a fallback that answers every missing path with one page, turn it off for the site. On Cloudflare Pages, the top-level `404.html` turns it off.

### File-size limits

Hosts limit the size of a file and the number of files. Cloudflare Pages allows 25 MiB for each file, and 20,000 files on the free plan or 100,000 on a paid plan. GitHub blocks files over 100 MiB in a repository, and limits a published Pages site to 1 GB. pgs.sh allows 100 MB for each file on pico+. The defaults follow these limits: `raw_len_max` is GitHub's 100 MiB, and `pack_len_max` is Cloudflare Pages' 25 MiB. A pack over `pack_len_max` still builds, with a warning that names the pack:

```text
warning: clone export pack exceeds max pack length (26214400 bytes) at 31457280 bytes, so a host with that file limit cannot serve it: '<r>.git/objects/pack/pack-<id>.pack' (in repo '<r>')
```

A release asset has no size limit in `surge`, so check large archives against the host's limit. Every branch gets every blob page and raw copy, and every commit a page, so a repository with many long-lived branches multiplies the number of files.

### Dotfiles and underscores

Nothing that a clone needs starts with a `.` or a `_`. The blob pages and raw copies of a tree's dotfiles do, such as `blob/main/.gitignore/index.html`, and so do `_headers` and `_surge/`. pgs.sh rejects dotfiles unless an `_pgs_ignore` allows them.

GitHub Pages runs Jekyll when it publishes from a branch, and Jekyll drops every file or directory whose name starts with `_`, `.` or `#`. The pages of tree entries named `#…`, `_…` or `.…` then vanish. Publish the site with a GitHub Actions workflow that uploads `output_dir`, or put an empty `.nojekyll` file in `static_dir`. The prune removes any file that the build did not write, so a `.nojekyll` that you add to `output_dir` by hand does not survive the next build.

### Trailing slashes

Every link that `surge` writes names an `index.html`, so the site works on any host. A GitHub URL with the host and owner replaced, such as `https://git.example.org/apps-c17/tree/main/docs`, works only on a host that redirects `/x` to `/x/`:

- Documented: Netlify (Pretty URLs, on by default), S3 website endpoints (302), pgs.sh (301), and Cloudflare's Workers static assets (307 under `auto-trailing-slash`). The Cloudflare Pages documentation shows only the reverse cases.
- Observed but not documented: GitHub Pages (301) and `python3 -m http.server` (301).

### Cache headers

A clone reads `HEAD`, `info/refs` and `objects/info/packs` first, and these change whenever the refs move. A pack never changes, because its name is the hash of its content. With `headers_file = true`, the build writes `_headers`, which Cloudflare Pages and Netlify read: one rule for each of the small files of every clone export, and one for the packs of each repository. For a site whose `base_url` is `https://example.org/git` and one repository, `apps-c17`, it is this:

```text
/git/*.git/HEAD
  Cache-Control: no-cache
/git/*.git/info/refs
  Cache-Control: no-cache
/git/*.git/objects/info/packs
  Cache-Control: no-cache
/git/apps-c17.git/objects/pack/*
  Cache-Control: public, max-age=31536000, immutable
```

Each path starts with the path of `base_url`, which is empty for a site at the host's root. Cloudflare Pages reads at most 100 rules, with one splat in each, so a site of up to 97 repositories fits. GitHub Pages' documentation does not mention custom headers. A `static/_headers` file collides with the generated one and makes the build fail; with `headers_file = false`, a `static/_headers` of your own is copied like any static file.

## Trust model

`surge` publishes repositories that you own and trust, and does not sanitize their contents:

> [!warning]
> READMEs, documents, release notes, and Markdown and Org files render with raw HTML enabled, and `surge` writes that HTML to the output without sanitizing it. Link targets pass through as they are written, `javascript:` included. `raw/` holds every blob up to `raw_len_max`, SVG files included, on the site's origin. `surge` writes no Content Security Policy, because the templates are yours. Build only repositories whose contents you trust.

libgit2's owner check is off, so `surge` reads a repository that another user owns, such as `/srv/git` owned by `git` while the build runs as you. The checks that protect the output tree stay on, because a trusted repository can still hold long or case-colliding paths, and a crafted object can hold any name: [Output directory](#output-directory) and [Names without pages](#names-without-pages) describe them.

## Development

[ARCHITECTURE.md](ARCHITECTURE.md) describes the code. [BUILD.md](../../BUILD.md) describes the CMake targets and the reasons for the configurations below. [STYLE.md](../../STYLE.md) describes the code style.

### Prerequisites

- A POSIX-like system with CMake 3.25 or later, a C17 compiler such as Clang or GCC, and pthreads.
- `git`, for the tests that read the fixture repositories. Without it, the configure step leaves those tests out.
- `python3`, for the clone test and the mise test in the golden test. Without it, the golden test runs without them.
- mise 2026.11.0, for the mise test in the golden test. Without it, the golden test runs without that test.
- Optional build tools: `ninja` for the `multi` preset.
- Optional quality tools: `cwrap`, `clang-format`, `clang-tidy`, `cppcheck`.
- Zig 0.17 and `llvm-strip` for the Linux `musl` archives.

The repository includes the dependencies in [vendor/](../../vendor/), libgit2 included, and the libraries in [libs/](../../libs/).

### Building

Run the commands in this section and in the sections below from `apps/surge`, the directory of this README. Each preset builds in `build/<preset>`, outside the source tree, and treats compiler warnings as errors. If `CC` is set, the preset builds in `build/<preset>/<CC>` instead, such as `build/debug/gcc-14`. CMake reads `CC` only when it configures a directory for the first time, so each compiler needs its own CMake cache. The commands use eight parallel jobs. Change that number to suit your machine.

```sh
cmake --preset debug
cmake --build --preset debug -j 8
```

| Preset | Configuration | Binary |
| --- | --- | --- |
| `debug` | `Debug` with AddressSanitizer and UndefinedBehaviorSanitizer. It runs `clang-tidy` and `cppcheck` during compilation if they are available. | `build/debug/bin/surge` |
| `tsan` | `Debug` with ThreadSanitizer. | `build/tsan/bin/surge` |
| `release` | `RelWithDebInfo`, with no tests. The binary in the build tree keeps its debug information, and packaging strips the installed copy. | `build/release/bin/surge` |
| `multi` | `Debug` and `RelWithDebInfo` in one build tree, with the `Ninja Multi-Config` generator. Build with `--preset multi-debug` or `--preset multi-relwithdebinfo`. | `build/multi/bin/Debug/surge`, `build/multi/bin/RelWithDebInfo/surge` |

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

The fixture repositories are `git fast-import` streams in [tests/repos/](tests/repos/), with fixed dates, so every object id is the same on every run. A test that reads a repository makes them first with `git`. `surge.fixture_repos` checks that each one holds exactly the refs and ids of its `tests/repos/<name>.refs` file.

The golden test `surge.golden.site` builds the fixture site in [tests/fixtures/site/](tests/fixtures/site/), over four repositories, three times: with one worker, with two workers and the output cache, and with eight workers and `--force`. The later builds must not change a generated file. It then compares the output with [tests/expected/site/](tests/expected/site/) byte for byte, the packs included, and the first build's warnings with [tests/expected/site.warnings.txt](tests/expected/site.warnings.txt). It checks each pack with `git verify-pack` and a strict `git index-pack`, and each `info/refs` against the refs of its source. It then moves a branch, changes the release directories, the default branch, a template, the limits and a blob, and checks what each dry run writes, keeps and removes. Each `refuse-*` fixture site is a golden test of a site that the build must refuse. A verbose build on one worker must print the exact `stderr` in its `tests/expected/<site>/stderr.txt`, so the file also shows which phases ran before the build stopped.

With `python3`, the golden test also runs [tests/clone_test.py](tests/clone_test.py). It serves the output on localhost, clones every `<r>.git` over dumb HTTP, and compares the refs of each clone with its source. It then clones from a server that answers a missing path with 200 and an HTML page, and checks for the corrupt-object errors that [404 handling](#404-handling) describes.

With `python3` and mise, the golden test also runs [tests/mise_test.py](tests/mise_test.py). It serves a copy of the output, reads the mise example of the fixture's `alpha` project from its page, and runs `mise ls-remote`, `mise lock`, `mise install` and `mise exec` against it, with every mise directory and config file in the test's scratch tree. A run with a tampered `SHA256SUMS` before a fresh lock, and a run with a tampered archive after a good lock, must each fail with a checksum mismatch.

### Continuous integration

The workflow presets run the same configure, build, and test steps as CI:

| Workflow | Steps |
| --- | --- |
| `cmake --workflow --preset ci-debug` | Build in `Debug`, run linting, and run all ASan and UBSan tests. |
| `cmake --workflow --preset ci-tsan` | Build with TSan and run all TSan tests. |
| `cmake --workflow --preset ci-release` | Build in `RelWithDebInfo`. |
| `cmake --workflow --preset ci-grammars` | Build in `RelWithDebInfo` with only the TypeScript grammar. CI runs this workflow only on Linux. |
| `cmake --workflow --preset ci-multi` | Build and test in `Debug` and `RelWithDebInfo` with the `Ninja Multi-Config` generator. CI runs this workflow a second time with `CC=gcc-14` to check GCC. |

CI installs mise 2026.11.0 for the `surge` job on Linux and macOS, so the golden test runs the mise test there. The GCC job runs the golden test without it.

To run the Linux workflows on a machine with Podman, build the pinned Ubuntu image from the root [Containerfile](../../Containerfile), with the repository root as the build context. Then run each workflow in a new container, with the directory of `surge` as the working directory:

```sh
podman build --tag apps-c17-linux-ci --file ../../Containerfile ../..
podman run --rm --workdir /src/apps/surge apps-c17-linux-ci ci-debug
podman run --rm --workdir /src/apps/surge apps-c17-linux-ci ci-tsan
podman run --rm --workdir /src/apps/surge apps-c17-linux-ci ci-release
podman run --rm --workdir /src/apps/surge apps-c17-linux-ci ci-grammars
podman run --rm --workdir /src/apps/surge apps-c17-linux-ci ci-multi
podman run --rm --workdir /src/apps/surge -e CC=gcc-14 apps-c17-linux-ci ci-multi
```

The image contains a snapshot of the repository, including the vendored dependencies, without `.git/` and the `build/` directories, which [.containerignore](../../.containerignore) excludes, so the Linux build products stay in the container and do not go into a `build/` directory of the host. The image supports x86-64 and AArch64 hosts, and pins LLVM 23 and GCC 14. It also installs `git` and mise 2026.11.0 for the tests of `surge`. It does not include Zig, because only the release archives need Zig.

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
| `release-linux-x86_64` | `build/release-linux-x86_64/surge-<version>-x86_64-linux-musl.tar.gz`, cross-built with Zig. |
| `release-linux-aarch64` | `build/release-linux-aarch64/surge-<version>-aarch64-linux-musl.tar.gz`, cross-built with Zig. |

The archive holds only the `surge_runtime` component, so libgit2's own install rules do not add `libgit2.a` or its headers.

A source archive needs a configured tree but no compiled binary. It holds the repository without the other apps, and it does not contain build products or private workspace metadata:

```sh
cmake --preset release
cpack --config build/release/CPackSourceConfig.cmake
```

The source archive is `build/release/surge-<version>-source.tar.gz`.

The version of every archive comes from `project(VERSION)` in [CMakeLists.txt](CMakeLists.txt). Tag each release commit with the matching `surge-vMAJOR.MINOR.PATCH`. The tag starts the root release workflow, [release.yml](../../.github/workflows/release.yml), which tests, packages and publishes `surge` only, with a `SHA256SUMS` over the archives. A manual run of this workflow with `dry_run` tests and packages the dispatched commit, and publishes nothing.

## Dependencies

The root [vendor/](../../vendor/) directory contains these dependencies, except the libraries in `libs/`. [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists the licenses of the vendored dependencies.

- [libs/](../../libs/): The libraries that `surge` shares with the other apps: `cli`, `core`, `os`, `runtime`, `formats`, `markup`, `output` and `templates`, and for the unit tests `testing` and `os_memory`. [libs/src/core/](../../libs/src/core/) holds arena allocation, counted strings, output buffers, the diagnostic log and the small primitives, and [libs/ARCHITECTURE.md](../../libs/ARCHITECTURE.md) describes the others.
- [libgit2](https://github.com/libgit2/libgit2): Reading repositories, diffs and clone packs, with its bundled xdiff, zlib and llhttp.
- [copt](https://github.com/fardaniqbal/copt): Command-line option parsing.
- [tomlc17](https://github.com/cktan/tomlc17): TOML configuration parsing.
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

`surge` is MIT-licensed, as [LICENSE.txt](https://github.com/unindented/apps-c17/blob/main/LICENSE.txt) states. The binary links libgit2, which is under the GPL version 2 with a linking exception, as [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) shows.
