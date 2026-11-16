<div align="center">
  <img src="media/logo.webp" height="300" alt="">
</div>

<h1 align="center"><code>surge</code></h1>

`surge` is a static site generator for git repositories, written in C17. It renders every branch of each repository in its config as GitHub-style pages, and gives each repository a clone URL that a static host can serve.

## LLM disclosure

> [!warning]
> This is all vibe-coded slop, produced with Claude Opus 5.5.

## Installation

Download a prebuilt archive from a `surge-v` release on the [Releases](https://github.com/unindented/apps-c17/releases) page, or build it from source as [BUILD.md](../../BUILD.md) describes. Each release has these files:

- One archive for each target, `surge-<version>-<target>.tar.gz`. The targets are `x86_64-linux-musl`, `aarch64-linux-musl` and `aarch64-macos`. The Linux binaries are static `musl` executables.
- `SHA256SUMS`, with the SHA-256 digest of each archive.

An archive holds `surge-<version>-<target>/bin/surge`, and this README, `LICENSE.txt` and `THIRD_PARTY_NOTICES.md` below `share/doc/surge/`.

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

`mise install` does not read `checksum_url`. Run `mise lock` to record the digest of each platform's archive in `mise.lock`. Later installs then refuse an archive that does not match. A site that `surge` builds can publish its own releases for mise, with a list of versions, as [Releases](#releases) describes.

## Usage

### Commands

`surge` has two commands:

- `surge build`: Generate the site in `output_dir`. It has these flags:
  - `-c, --config PATH`: Read the configuration from `PATH`. The default is `surge.toml` in the working directory.
  - `-w, --workers N`: Set the maximum number of worker threads. `N` must be a positive integer. The default is the number of CPUs. The build never runs more than four workers for each online CPU, and a parallel phase never starts more workers than it has jobs.
  - `-n, --dry-run`: Report the changes that the build would make, and make none. See [Dry run](#dry-run).
  - `--force`: Make every output again, and keep nothing from earlier builds. See [Output cache](#output-cache).
  - `-v, --verbose`: Print build progress to `stderr`.
- `surge config`: Print the resolved configuration as TOML. It also takes `-c, --config PATH`.

It also has these options:

- `surge --version` (or `-V`): Print the version, and the grammars that the binary includes for [syntax highlighting](#syntax-highlighting).
- `surge --help` (or `-h`): Print the list of commands.
- `surge <command> --help`: Print the help for one command.

A command exits with status 0 when it succeeds, 1 when it fails, and 2 when it rejects the command line. A build that succeeds can still print warnings to `stderr`, after any errors. A build stops at the first phase that fails. The load of the inputs stops at its first failing step. Each later phase before the first write reports all of its errors.

### Site layout

Run `surge` from a directory with this layout, or name its config with `-c`. The example uses the default directory names:

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
  branch-list.html
  commit-feed.atom
  commit-list.html
  commit.html
  index.html
  release-feed.atom
  release-list.html
  release-project.html
  release.html
  tag-feed.atom
  tag-list.html
  tree.html
surge.toml
```

- `surge.toml` is the config file. See [Configuration](#configuration).
- `templates/` holds the Mustache templates and their partials. See [Templates](#templates). `surge` has no default templates, so each site supplies its own.
- `static/` is optional. It holds files that `surge` copies into `output_dir` without changes, at the same relative path. For example, `static/style.css` becomes `output_dir/style.css`.
- `releases/` holds the release assets that you or your pipeline put there for each repository. See [Releases](#releases). Each repository names its directory with the optional `releases_dir`.

`surge` reads each repository at its `path`. It never changes a repository and never uses the network.

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

The tables below list every key that the config file can contain. `surge` rejects any other key. For example, a misspelled `ouput_dir` makes the load fail. A diagnostic names a key with its table, and counts the tables from 0, such as `missing required config key 'repo[0].path'`.

Top-level keys:

| Key | Default | Bounds | Meaning |
| --- | --- | --- | --- |
| `title` | Required | A string | The site title, `site.title` in templates. |
| `description` | Required | A string | The site description, `site.description`. |
| `author` | Required | A string | The site author, `site.author`. |
| `base_url` | Required | An `http://` or `https://` URL with a host, in any letter case, without `?`, `#`, a space or a control character | The absolute URL of the site. `surge` removes any trailing `/`. The clone URLs, the feeds and the mise examples use it, and its path is the not-found page's `path_to_root`. Every other link is relative. |
| `output_dir` | `"public"` | Not empty | The directory that the build writes to and controls. See [Output directory](#output-directory). |
| `templates_dir` | `"templates"` | A directory | The templates and partials. |
| `static_dir` | `"static"` | A directory, if it exists | The files that the build copies into `output_dir`. |
| `not_found_path` | `"404.html"` | A safe relative path that ends in `.html` | The output path of the not-found page below `output_dir`. A host serves its not-found page from a path of its own, as [Host notes](../../README.md#host-notes) lists. For bunny.net, set `"bunnycdn_errors/404.html"`. |
| `log_page_size` | `100` | 1 to 10000 | The commits on each commit list page. |
| `feed_count` | `25` | 0 or more | The most entries in each commit, tag and release feed. |
| `headers_file` | `false` | A boolean | Whether the build writes `_headers`. See [Cache headers](#cache-headers). |
| `llm_trailer_keys` | `["Co-authored-by", "Assisted-by", "Generated-by"]` | Non-empty strings | The keys of the trailers that can mark a commit LLM-assisted, in any ASCII letter case. See [LLM-assisted commits](#llm-assisted-commits). |
| `llm_trailer_values` | `[]` | Non-empty strings | The texts that mark a commit LLM-assisted when one of those trailers holds one in its value, address included, in any ASCII letter case. With none, no commit is marked. |
| `limits` | | A table | The `[limits]` table below. |
| `templates` | | A table | The `[templates]` table below. |
| `repo` | Required | At least one table | The `[[repo]]` tables below, in the order of the site index. |

The `[limits]` table:

| Key | Default | Bounds | Meaning |
| --- | --- | --- | --- |
| `blob_len_max` | `1048576` (1 MiB) | 0 to 4194304 (4 MiB) | The largest blob, in bytes, that the build reads for a page. A larger blob gets a blob page without contents, is never a document or a README, and shows no lines in a diff. |
| `raw_len_max` | `104857600` (100 MiB) | 0 to 1073741824 (1 GiB) | The largest blob, in bytes, that gets a raw copy. A verbose build counts the larger blobs. |
| `diff_file_count_max` | `300` | 0 or more | The most changed files whose diffs a commit page shows. |
| `diff_line_count_max` | `20000` | 0 or more | The most diff lines that a commit page shows. |
| `diff_len_max` | `1048576` (1 MiB) | 0 to 4194304 (4 MiB) | The most bytes of diff lines that a commit page shows. |
| `image_len_max` | `2097152` (2 MiB) | 0 to 4194304 (4 MiB) | The most bytes of images that a commit page embeds, over all sides of all changed images. A side that does not fit is left out, and a later, smaller side can still fit. |
| `pack_len_max` | `26214400` (25 MiB) | 0 or more | The largest clone pack, in bytes, that builds without a warning. A larger pack still builds. |

The `[templates]` table names the template of each kind of page, relative to `templates_dir`. Each key that the table leaves out keeps its default. A default is the key with `_` as `-` and the page's extension, `.html` for a page and `.atom` for a feed, except `not_found`. Each value is a safe relative template name: only letters, digits, `_`, `-`, `.` and `/`, and no empty, `.` or `..` segment. Two keys can name one template, which then renders each kind of page with that page's data. [Templates](#templates) says which templates a site needs.

| Key | Default | Bounds | Meaning |
| --- | --- | --- | --- |
| `index` | `"index.html"` | A template name | The site index. |
| `tree` | `"tree.html"` | A template name | Each repository home and each tree page. |
| `blob` | `"blob.html"` | A template name | Each blob page. |
| `branch_list` | `"branch-list.html"` | A template name | Each branch list. |
| `tag_list` | `"tag-list.html"` | A template name | Each tag list. |
| `tag_feed` | `"tag-feed.atom"` | A template name | Each tag feed. |
| `commit` | `"commit.html"` | A template name | Each commit page. |
| `commit_list` | `"commit-list.html"` | A template name | Each commit list page. |
| `commit_feed` | `"commit-feed.atom"` | A template name | Each commit feed. |
| `release` | `"release.html"` | A template name | Each release page. |
| `release_list` | `"release-list.html"` | A template name | Each release list. |
| `release_project` | `"release-project.html"` | A template name | Each release project page. |
| `release_feed` | `"release-feed.atom"` | A template name | Each release feed. |
| `not_found` | `"404.html"` | A template name | The not-found page. |

Each `[[repo]]` table:

| Key | Default | Bounds | Meaning |
| --- | --- | --- | --- |
| `name` | Required | See below | The repository's directory in the output, `<name>/`, and its clone export, `<name>.git/`. |
| `path` | Required | A repository | A work tree or a bare git directory. `surge` opens the repository at that path. It does not search the parent directories or read `GIT_DIR`. |
| `description` | The `description` file | A string | The repository description. An empty string shows none. Without the key, `surge` reads the `description` file of the git directory (the main repository's, for a linked worktree), removes the white space at its end, and ignores git's default text. |
| `default_branch` | The branch that `HEAD` names | Not empty, a branch | The branch of the repository home and of the clone's `HEAD`. A repository whose `HEAD` is detached or names a missing branch needs this key. |
| `releases_dir` | None | A directory, if it exists | The release assets. See [Releases](#releases). |
| `mise` | `false` | A boolean. Needs `releases_dir`, and no project with a `tag_prefix` | Whether the pages of the repository's own release project show a mise install. See [Version lists and mise](#version-lists-and-mise). |
| `project` | None | Tables | The `[[repo.project]]` tables below. |

Each `[[repo.project]]` table:

| Key | Default | Bounds | Meaning |
| --- | --- | --- | --- |
| `name` | Required | See below | The project name, and its directory below `<repo>/releases/`. |
| `path` | Required | A relative path without an empty, `.` or `..` segment | The project's directory in the repository. `surge` removes any trailing `/`. The default branch must have a tree page at this path, or the build fails. The projects of an empty repository are not checked and link nowhere. |
| `description` | Empty | A string | The project description. It has no fallback. |
| `tag_prefix` | None | Not empty, and no overlap with another project's prefix | The prefix of the project's release tags, such as `sosig-v`. A project without one has no releases. |
| `mise` | `false` | A boolean. Needs `tag_prefix` | Whether the project's release pages show a mise install. See [Version lists and mise](#version-lists-and-mise). |

These rules apply to repository and project names:

- A name contains only ASCII letters, digits, `.`, `_` and `-`, and is at most 251 bytes long, so that `<name>.git` fits in one path segment.
- A name cannot start with `.` or end in `.git`, in any letter case.
- Two repositories, or two projects of one repository, cannot have names that differ only in ASCII letter case.
- A repository cannot be named `index.html`, `_surge` or `_headers`, or the first segment of `not_found_path`, which is `404.html` by default, and a project cannot be named `tag`, `download` or `index.html`, in any letter case.
- The build fails when a top-level entry of `static_dir` has the name of a repository or of its `<name>.git`, after ASCII case folding.

`surge` resolves each directory in the config, the defaults included, against the directory of the config file. So `-c sub/surge.toml` without `output_dir` writes `sub/public`. It removes trailing `/` characters from each directory value, and rejects a value that is then empty, such as `/`. A directory can be absolute, or relative with `..`. `surge config` prints every directory resolved. It leaves out an empty optional string, but it prints a repository `description` set to `""`.

The load stops at its first error. A config error stops the build before it opens a repository.

### Output layout

The build writes this tree to `output_dir`. `<r>` is a repository name, `<ref>` a branch name, `<p>` a path in the branch's tree, `<oid>` a commit id, `<tag>` a tag name, and `<project>` a release project:

```text
index.html                                     site index
<not_found_path>                               not-found page, 404.html by default
_headers                                       with headers_file = true
_surge/output-cache.tsv                        output cache
<static files>
<r>/index.html                                 repo home
<r>/branches/index.html                        branch list
<r>/tags/index.html                            tag list
<r>/tags.atom                                  tag feed
<r>/releases.atom                              release feed, with a release project
<r>/tree/<ref>/index.html                      root directory of a branch
<r>/tree/<ref>/<p>/index.html                  directory
<r>/blob/<ref>/<p>/index.html                  file
<r>/raw/<ref>/<p>                              raw copy, up to raw_len_max
<r>/commits/<ref>/index.html                   first commit list page, newest commits
<r>/commits/<ref>/page-<n>.html                commit list page n, from 2
<r>/commits/<ref>.atom                         commit feed
<r>/commit/<oid>/index.html                    commit with its diff
<r>/releases/index.html                        release list
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

The paths follow GitHub's URL scheme, with each page as the `index.html` of its directory. A branch name or a tree path keeps its `/` characters as nested directories, so the branch `feature/x` has its tree pages below `tree/feature/x/`. Only branches get `tree/`, `blob/`, `raw/` and `commits/` pages. A tag has a row on the tag list, a link to its commit page, and a release page when it has a release.

Every link on a page is relative, so the site works below any path prefix. The not-found page is the exception: its `path_to_root` is the path of `base_url`, because a host serves it at any depth. Each link names the `index.html` of a page, so the links work on a host that does not redirect `/x` to `/x/`. A link percent-encodes each byte of a branch or tree name outside RFC 3986's unreserved set, `+` included. The feeds and the mise examples use absolute links below `base_url`.

### Output directory

> [!caution]
> The first build into a directory removes everything in that directory that the build did not write, hidden files included. Set `output_dir` to a directory that you use for nothing else.

A build replaces any file that is already at the path of an output. A written output gets mode `0666` reduced by the process umask, and a rewritten output does not keep its old mode. After a successful build, the build removes every file and directory below `output_dir` that it did not write. For a symlink that does not lead to a current output, it removes the symlink and not its target. A failed build removes nothing. If a write fails, some outputs can be new while others stay old. To recover, delete `output_dir` and build again. A write is not atomic, so an interrupted build can leave one output cut short. The next build writes that output again.

Before it scans a repository, `surge` rejects an `output_dir` that is at or below one of these inputs, or that holds one of them or the config file:

- `templates_dir` and `static_dir`.
- The git directory, the work tree and the `releases_dir` of each repository. The git directory of a linked worktree is its main repository's.

For example, it rejects `output_dir = "/srv/git"` when a repository is `/srv/git/foo.git`. These checks compare the directories, not the text of their paths, so they also find a symlink or another spelling of the same directory. The searches of `templates_dir` and `static_dir` skip `output_dir`.

Before it writes, `surge` records every output path, and rejects a build with one of these conflicts:

- Two outputs have the same path. Paths that differ only in ASCII letter case count as one path, because a case-insensitive filesystem such as the macOS default stores them as one file. For example, the branches `x` and `x.atom` of one repository both claim `commits/x.atom`.
- One output path must be both a file and a directory, also after ASCII case folding.
- An output is at or below an input directory, or overwrites the config file, a template, a static file or a release asset, for example through a symlink inside `output_dir`.
- An output path outside a tree is longer than 1024 bytes, or has a segment longer than 255 bytes. A tree path over a limit gets a warning instead. See [Names without pages](#names-without-pages).

The build creates `output_dir` only after all of these checks pass, so a rejected build creates nothing.

### Dry run

`surge build --dry-run` runs every phase of a build. It changes nothing in `output_dir` and does not create it. For each path that the build would change or keep, it prints one line to `stdout`: a mark, a tab, and the path. This is a shortened listing:

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
| `keep` | The build would not change this output. The [output cache](#output-cache) shows that it is current, or the file already holds the same bytes. |
| `remove` | The build would remove this path, which can be a directory, because the current inputs do not make it. |

The `write` and `keep` lines come first, in this order:

1. The clone export of each repository, in config order.
2. The pages of each repository: the home, the branch and tag lists, the tag feed and the release feed, then for each branch its tree, blob and raw outputs, its commit list pages and its commit feed, and then the commit pages.
3. `_headers`.
4. The release outputs of each repository.
5. `index.html` and the not-found page.
6. The static files.
7. The output cache.

The `remove` lines come last, sorted. To get only the paths, for `sort`, `diff` or `comm`, use `cut -f2-`. A dry run reports a failure as a build does, on `stderr` with the same exit code, and then prints no listing. With `-v`, the progress messages go to `stderr`, so `stdout` holds only the listing.

A dry run lists an output whose inputs changed as `write` without making it. So it renders no such page, and makes no pack for a clone export whose refs changed. It lists that pack as `pack-<id>.pack` with the id of the new `info/refs`, because the real name is the hash of the pack's bytes. It makes only the outputs that have no recorded inputs, such as `HEAD` and `info/refs`, to compare them with the files. A build can fail after a dry run succeeds, because a dry run cannot find these failures:

- The build has no permission to write to `output_dir`, or the disk is full.
- `surge` cannot read a static file or a release asset. A dry run does not open the files that it would copy.
- An old file is at a path where a new output needs a directory, or the reverse. For example, the old output is the file `public/apps-c17/raw/main/docs`, and the new output is `public/apps-c17/raw/main/docs/guide.md`.
- `output_dir` exists but is not a usable directory.

### Output cache

A build writes the output cache to `_surge/output-cache.tsv` below `output_dir`. It has one row for each output whose inputs the build can name: the path, the inputs and the size. The next build keeps an output when its inputs are the same and the file still has that size. The build makes every other output on every build, and writes it only when its bytes change. A missing or unusable output cache is not an error, and the build then makes every output.

The inputs of each output are these:

- Every page depends on the page recipe: the `surge` version, the grammars of the binary, `title`, `description`, `author`, `base_url`, `log_page_size`, `feed_count`, `llm_trailer_keys`, `llm_trailer_values`, every limit but `pack_len_max`, every name of the `[templates]` table, and the path, size and modification time of every file below `templates_dir`. A change to any of them, a partial included, makes every page again.
- Every page of a repository depends on what its `repo` map shows: the default branch, whether the branch names a commit, the description, and whether the repository has a release project.
- A commit page depends on its commit. A blob page depends on its blob and mode, and a tree page on its tree. Both also depend on the branch's root tree when they show Markdown or Org, and a tree page also when it lists a symlink, because their links resolve against every path of the branch. A tree page or a home that lists a submodule with a URL also depends on the names of the site's repositories, which its link can name. The page of a branch's root also depends on the branch's newest commit. A commit list page depends on its commits and the refs that name them, and a feed on its entries.
- The home, the branch and tag lists and the site index depend on every ref, and on the repository's place in the config, description, newest commit date and projects. The tag list and the release outputs also depend on the releases. The site index's contributions calendar depends on the history of each default branch, which the refs and the default branch decide.
- A raw copy depends only on its blob. A static file and a release asset depend on their path, size and modification time. A clone pack depends on `info/refs`.

So a new commit on a branch makes these outputs again: the branch's commit list pages and feed, the new commit's page, the tree and blob pages and raw copies of the paths that it changes, the pages that show Markdown or Org, the tree pages that list a symlink, the page of the branch's root, the repository's home and its branch and tag lists, the site index, and the changed files and new pack of the clone export. `--force` ignores the output cache and writes every output.

The output cache does not see two kinds of change. Run `surge build --force` once after either:

- A tool changes a file but keeps its size and modification time.
- `$GIT_DIR/info/attributes` or a work tree's `.gitattributes` changes. libgit2 reads them for a diff.

A build writes the output cache after its last output, also when an output failed, with a row only for what it wrote or kept. An interrupted build leaves the previous cache. That cache keeps no output that the build cut short, because the file no longer has the recorded size, and none that it rewrote, because its inputs changed.

When you publish `output_dir`, you also publish the output cache. Its rows name the output paths and the ids of the commits, trees and blobs that they show. To keep this private, exclude `_surge/output-cache.tsv` when you deploy.

### Limits

The build rejects an input file that is too large before it reads the file. The error message gives the limit and the file size.

| Input | Limit |
| --- | --- |
| Config file | 1 MiB |
| Template or partial | 4 MiB |
| A repository's `description` file | 64 KiB |

These limits also apply:

- A site can have at most 64 partials in `partials/`.
- A rendered page can be at most 256 MiB, and a render can use at most 300,000 partial expansions. A render that passes a limit fails.
- The build does not read an output cache over 256 MiB, and makes every output again.
- A blob over `blob_len_max` or `raw_len_max` gets the treatment that [Configuration](#configuration) gives.
- A static file and a release asset have no size limit. The build copies each one from the file.
- The build rejects a config file, a template or a partial that contains a `NUL` byte, and a TOML string with an escape that decodes to `NUL`.

The build reserves its address space once, when it starts: 4 GiB for the persistent data of each thread, 3 GiB for its temporary data, and 256 MiB for the log and for each worker's job logs. Under an address-space limit (`ulimit -v`), the build runs fewer workers, and then makes a smaller reservation. See [libs/ARCHITECTURE.md](../../libs/ARCHITECTURE.md#build-memory). A build that runs out of memory prints the errors that it logged, then `surge: out of memory` with the repository or file that the thread was working on, if any, and exits with status 1.

## Repos and projects

Each `[[repo]]` table names one git repository, a work tree or a bare git directory. `surge` reads it through libgit2, and ignores the user's and the system's git config, so a setting in `~/.gitconfig` never changes a page. It reads a repository that another user owns. See [Trust model](#trust-model).

`surge` publishes every branch below `refs/heads/` and every tag below `refs/tags/`. It has no option to leave out a branch. To keep a branch off the site, delete it from the repository, or publish a mirror without it. `surge` does not publish or export a symbolic ref.

The build fails for a repository with one of these refs, and names each ref:

- Two branches whose names, or the directories above them, differ only in ASCII letter case, such as `Main` and `main`, `a` and `A/b`, or `X/x` and `x/y`. Their pages would share a directory on a case-insensitive filesystem.
- A branch or tag whose name is not valid UTF-8. A filesystem that takes only UTF-8 names, such as macOS's, refuses its pages' directories.
- A ref whose name is a `/`-prefix of another, such as `feature` and `feature/x`. git does not make them, but a hand-written `packed-refs` can hold them.
- A ref whose name git calls invalid.

A branch that names a tree or a blob, and not a commit, gets a warning and no pages. Its row on the branch list has no links.

An empty repository, with no commit, has a home that says so, a branch list and a tag list, a tag feed, and a clone export. The export's `HEAD` names the branch that the repository's `HEAD` names.

The site index lists each repository in config order, with its projects under it. A project is a directory of a monorepo, such as `apps/sosig`, with a name and a description. The index links each project to the tree page of its directory on the default branch, and a project with a `tag_prefix` to its release page. See [Releases](#releases).

### Contributions

The site index also gets a contributions calendar, as a GitHub profile shows one, which a template can draw as a grid of days. It counts every commit that the default branch of a repository on the site reaches, whoever wrote it, and a commit that two repositories reach counts once. A repository with other people's commits fills the calendar with theirs too.

- A commit's day is the date of its author time in the offset that git recorded, so a commit at 23:30 at `-08:00` counts on that day, not on the next day in UTC. A commit whose date RFC 3339 cannot write, outside the years 0000 to 9999, does not count.
- The window is the 365 days that end on the newest day with a commit, drawn as 53 weeks from Sunday to Saturday. The days of the first week before the window and the days of the last week after it are outside it, and count nothing. The window ends at the newest commit, not on the day of the build, so the same repositories always give the same index. A commit dated in the future moves the window to its day.
- A day's level is 0 without commits. Otherwise it is 1 plus the number of quartiles of the window's days with commits that are less than its count. The quartiles are nearest-rank: of the `n` counts in ascending order, the `k`th, for `k` from 1 to 3, is the one at position `ceil(k × n / 4)`, counted from 1. So a single day with commits, or days that all have the same count, are all level 1.
- A day has an LLM-assisted commit when its trailers mark it, as [LLM-assisted commits](#llm-assisted-commits) describes, so a template can color those days apart. The calendar shows the help that the trailers disclose, and nothing else.

To build the calendar, the scan reads the author and the trailers of every commit of each default branch on every build, also when it keeps the index.

### Documents

The home of a repository shows the root of its default branch, the newest commit of that branch, the clone URL, and its documents, which a template can show as tabs. Each tree page shows the documents of its directory in the same way. `surge` looks for these documents, in this order:

| Kind | Flag | Base names |
| --- | --- | --- |
| `readme` | `is_readme` | `README` |
| `license` | `is_license` | `LICENSE`, `LICENCE`, `COPYING` |
| `code-of-conduct` | `is_code_of_conduct` | `CODE_OF_CONDUCT` |
| `contributing` | `is_contributing` | `CONTRIBUTING` |
| `security` | `is_security` | `SECURITY` |
| `accessibility` | `is_accessibility` | `ACCESSIBILITY` |

`surge` looks for each kind in the page's directory, and for the root also in `docs/`. GitHub also looks in `.github/`, and `surge` does not. A document's file name is a base name of its kind, in any ASCII letter case, with no extension or with `.md`, `.markdown`, `.org` or `.txt` in any letter case. The first match in byte order wins. `surge` skips a symlink and a file over `blob_len_max`. It renders these formats:

- `.md` or `.markdown`: Markdown, with md4c.
- `.org`: Org, with org4c.
- Any other file: escaped text in a `<pre>`.

`surge` does not read the front matter of a Markdown file, TOML between `+++` lines or YAML between `---` lines at its start. It shows it as a `toml` or `yaml` code block. So a file that starts with a `---` thematic break and has another one later shows the text between them as YAML, as GitHub does.

A heading's level is its level in the source, as on GitHub. Markdown's `#` and Org's `*` are `<h1>`, and each lower level adds one, up to `<h6>`. Each heading has an `id`, so a link to `#building` in a README finds `## Building`:

- An Org headline with a `CUSTOM_ID` uses it.
- Otherwise the id is the heading text with ASCII letters in lowercase, digits and `_` kept, and each run of spaces and `-` as one `-`, with none at either end. Every other character is dropped, a non-ASCII letter included.
- A heading with no such text is `section`. A repeated id gets `-1`, `-2` and so on.

The id matches GitHub's for ASCII text without punctuation between words. GitHub keeps such a gap as `--`, so `## Build & test` is `#build-test` here and `#build--test` on GitHub. Each heading is in a `<div class="heading">`, followed by an empty link to itself for a stylesheet to draw, `<a class="anchor" href="#<id>" aria-labelledby="<id>"></a>`. The link takes its name from the heading. A template's own ids should contain a character that these ids never contain, such as `.`, so that a heading cannot take one.

### Links in documents

A Markdown or Org link in a README, a document, or a Markdown or Org blob resolves against the page's branch. A relative target resolves against the file's directory, and a target that starts with `/` against the repository root. `surge` decodes `%XX` escapes before it looks up the target, and then links one of these:

- A file: its blob page.
- A directory: its tree page.
- A target that names a file only when `.md` is added: that file's blob page.
- An image target that names a file: its raw copy, or its blob page when the file is over `raw_len_max`.

The link keeps its query and fragment. An Org `file:` link's `::#<id>` search option becomes the fragment `#<id>`, and `surge` drops its other search options. `surge` writes every other target as written, such as an absolute URL, another scheme, or a relative target that names no path. A verbose build counts, for each repository, the relative links that name no path on the pages that it renders. A kept page adds none, and a document on two pages counts on each.

Raw HTML resolves the same way, as on GitHub, in Markdown and in Org's `#+BEGIN_EXPORT html` blocks and `@@html:...@@` snippets. So a README that centers its logo with `<div align="center"><img src="media/logo.svg"></div>`, or picks one with a `<picture>`, shows it on every page that shows the README:

- These embedded files resolve as an image does: the `src` of an `<img>`, `<source>`, `<video>`, `<audio>`, `<track>`, `<iframe>` or `<embed>`, each URL of the `srcset` of an `<img>` or `<source>`, a `<video>`'s `poster`, and an `<object>`'s `data`. A file over `raw_len_max` has no raw copy, so it links its blob page, which a video or an iframe cannot show.
- The `href` of an `<a>` or an `<area>` resolves as a link does.

`surge` double-quotes each attribute with a resolved target, and keeps the descriptors of a `srcset`, such as `2x`. It leaves every other attribute and element, comments, and the contents of `<script>` and `<style>` as written.

### Files

A tree page lists the directories first and then the files, each group in byte order, with breadcrumbs from the repository to the directory. A symlink row shows at most 4096 bytes of its target text, with a warning for a longer target. A submodule row shows its commit id.

A symlink has no page of its own, and its row links its target when the target is in the branch: the blob page of a file, the tree page of a directory, and for another symlink the tree page of the directory that holds it, one hop only. The target is a path relative to the symlink's directory, as the filesystem reads it, so `surge` decodes no `%XX` escape, keeps `?` and `#` as part of the name, and adds no `.md`. A target that starts with `/`, leaves the repository root, was cut at 4096 bytes, or names no path with a page gets no link.

A submodule row also shows the submodule's URL, and links its commit where the URL allows. `surge` reads `.gitmodules` from the root of the branch's tree, as a file and not through a symlink, with git's config syntax. A gitlink gets the `url` of the submodule whose `path` is the gitlink's path. A path that two submodules name gets neither URL, and a `.gitmodules` that does not parse gives no submodule a URL on that branch, each with a warning. `surge` follows no `include.path`. The row shows the URL as written, without the user and password of an `http://` or `https://` URL, and without a `user:password@` of another URL. The commit links to one of these:

- A repository of the site, named by its clone URL or by a URL that starts with `./` or `../`, which resolves against the clone URL of the submodule's repository as git resolves it: that repository's commit page. The link is not checked, so it leads nowhere when no ref of that repository reaches the commit.
- A repository on `github.com`, `gitlab.com` or `codeberg.org`, as `https://<host>/<path>`, `git@<host>:<path>` or `ssh://[<user>@]<host>/<path>`, with the host in any letter case and a path of at least two segments: `https://github.com/<path>/tree/<oid>`, `https://gitlab.com/<path>/-/tree/<oid>` or `https://codeberg.org/<path>/src/commit/<oid>`, without the path's trailing `/` and `.git`.

Any other URL gets no link.

A blob page shows the mode and size of the file, links its raw copy, and shows its lines with `#L<n>` anchors, highlighted as [Syntax highlighting](#syntax-highlighting) describes. libgit2's binary check decides whether a file is binary, and a binary file shows no lines. A template can show an image file with `<img>` from its raw copy. An image file has the extension `.png`, `.jpg`, `.jpeg`, `.gif`, `.webp`, `.svg`, `.avif`, `.bmp` or `.ico`, in any letter case. A Markdown or Org file has the rendered view and the highlighted source on one page. The fixture site's web component, [surge-view.js](tests/fixtures/site/static/surge-view.js), shows the rendered view, or the source when the fragment names a line, such as `#L12`. Without JavaScript, the page shows both views.

The raw copy of a file is its bytes at `<r>/raw/<ref>/<p>`, for every file up to `raw_len_max`, SVG included.

### History

A branch's log lists its commits newest first, in the order of `git log --date-order`: by committer time, with no commit before one of its children. Each page holds `log_page_size` commits, with links to the first, previous, next and last pages. Each commit shows the branches and tags that name it. A new commit moves the page boundaries, so it makes all of the branch's commit list pages again.

Every commit that a branch or a tag reaches has a commit page. It shows the message, its trailers and co-authors, the author's and committer's names and dates, a link to each parent, a diffstat, a tree of the changed files, and the diffs in the order of that tree. It shows no branch or tag that names the commit. A page shows the author and the committer by name, and shows the message as the commit has it, so an address in the message, such as in a `Signed-off-by` trailer, shows too. The clone export holds every address of every commit.

A changed image shows its old and new sides, embedded in the commit page as `data:` URIs. An image here is a file with an [image extension](#files) whose diff the page shows and whose type did not change. Its sides go in, in the files' order, old side first, while their bytes stay within `image_len_max` in total. An SVG also shows its text diff.

The diff of a commit is against its first parent, or against nothing for a root commit. Rename detection is git's:

- A deleted file and an added file whose contents are at least 50% alike are a rename, with git's default `diff.renameLimit` of 1000.
- When the deleted files times the added files pass 1000 × 1000, the diff finds only exact renames, for up to 10000 changed files.
- `surge` does not read the repository's own `diff.renameLimit`.

A file that changed type, such as a file that became a symlink, is one changed file. A side over `blob_len_max` shows no lines. Each file has the anchor `#diff-<n>`, numbered from 0 in the order of the changed-files tree. Each line has `#diff-<n>-hunk-<h>-L<old>` for its old line number and `-R<new>` for its new one, with hunks numbered from 0. The caps apply in this order:

1. A file past `diff_file_count_max` is listed without its diff.
2. The file whose lines would pass `diff_line_count_max` or `diff_len_max` is cut there, and every later file is listed without its diff.

`diff_len_max` counts the bytes of the lines that the page shows, and the diffstat counts those lines.

### LLM-assisted commits

A commit is LLM-assisted when one of its trailers has a key in `llm_trailer_keys` and a value that holds a text of `llm_trailer_values`. Both compare in any ASCII letter case, as git compares trailer keys, so `Co-authored-by` and `Co-Authored-By` are one key. The value includes the address, so a text can name the tool or its address. Only the trailers count: a commit whose subject, body or author names a tool, such as `Fix the Claude API client`, is not marked. Every commit summary and every commit feed entry has `is_llm_assisted` and the tools that the trailers name, so a template can mark such a commit on the commit list, the commit page and the feed. The fixture's feeds give it `<category term="llm-assisted"/>`.

These tools write these trailers:

| Tool | What it writes |
| --- | --- |
| Claude Code | `Co-Authored-By: Claude <model> <noreply@anthropic.com>` |
| Linux kernel convention | `Assisted-by: <agent>:<model> [tools]` |
| aider | A `Co-authored-by:` trailer that names aider and the model, or an `(aider)` suffix on the author name |
| GitHub Copilot coding agent | Commits by a `Copilot` identity at `users.noreply.github.com` |
| Others | `Generated-by:`, free text |

So a site that wants to mark them sets values such as these:

```toml
llm_trailer_values = ["Claude", "Cursor", "Copilot", "aider"]
```

A short value can match more than you mean. `"Claude"` also matches a human co-author named Claude, and an address such as `"noreply@anthropic.com"` narrows it. `surge` cannot see an LLM's help that no trailer discloses, such as a commit whose only sign of the tool is its author, like a commit of GitHub's Copilot agent or an `(aider)` author. Tools change their trailers, so add a key or a value when you see a new one.

### Names without pages

A tree can hold names that a page cannot use. Each such entry keeps its row in the listing, without a link, and gets no blob page, raw copy or tree page. `surge` does not enter such a directory. Each case is one warning that names every path involved:

- `.`, `..`, and `.git` in any letter case.
- A name that contains `/`, `\`, a control byte, or bytes that are not UTF-8.
- A directory named `index.html`, whose tree page would be inside its parent's.
- Names in one directory that differ only in ASCII letter case, such as `xt_CONNMARK.h` and `xt_connmark.h`. `surge` picks no winner. It does not check names that differ only in Unicode normalization or in non-ASCII case.
- A name whose pages would pass the output path limits: 1024 bytes for the path below `output_dir`, or 255 bytes for one segment.

`surge` shows every name, ref and message as valid UTF-8, as [Templates](#templates) describes. A warning shows a control byte or an invalid byte as `\xNN`.

## Releases

### Release directories

The `releases_dir` of a repository holds one directory for each release, named after its tag. You or your release job fill it, and `surge` publishes what it finds. A release is a tag with a directory `<releases_dir>/<tag>/`. A tag without such a directory, or with a file of that name, is only a tag.

- The notes of a release are the message of its annotated tag, rendered as Markdown. Their links stay as written. As git does, `surge` cuts the message at the last line that opens a PGP, X.509 or SSH signature, such as `-----BEGIN PGP SIGNATURE-----`. The release map holds the signature apart. `surge` does not verify a tag's or a commit's signature.
- The assets of a release are the regular files in its directory, in byte order of name. A subdirectory is not an asset. The build copies each asset to `<r>/releases/download/<tag>/<asset>`, with no size limit.
- The date of a release is the date of its annotated tag, or the committer date of the commit of a lightweight tag. A tag of a tag uses the outer tag.

`releases_dir` does not have to exist. If it exists, it must be a directory.

A release project groups releases by tag prefix. A repository's release projects are its projects with a `tag_prefix`, in config order. A repository with a `releases_dir` and no project with a `tag_prefix` has one release project of its own, named after the repository, whose tags can start with `v`. The version of a release is its tag without the project's prefix, or, for the repository's own project, without a leading `v`. For example, the tag `sosig-v0.1.0` of the project with `tag_prefix = "sosig-v"` is the version `0.1.0`.

A release whose tag fits no project's prefix keeps its release page and its row on the release list, belongs to no project, and gets a warning. A project with a `tag_prefix` in a repository without `releases_dir` has a project page with no releases.

### Version lists and mise

Each release project has two files that mise's `http:` backend reads:

- `<r>/releases/<project>/versions.txt` lists the project's versions, one for each line, by Semantic Versioning precedence, newest last. A version must be `MAJOR.MINOR.PATCH` with an optional `-PRE`, as Semantic Versioning 2.0.0 writes them, without build metadata. A release with another version keeps its page, is left out of the list, and gets a warning. When two tags of one project give the same version, such as `v1.0.0` and `1.0.0`, the list has the first tag in byte order. The other tag keeps its page and gets a warning.
- `<r>/releases/<project>/versions.json` lists the same versions in the same order, with the RFC 3339 date of each release, on one line: `{"versions":[{"version":"0.1.0","date":"2026-01-01T13:00:00+01:00"},…]}`. mise reads the dates for its `minimum_release_age` check.

Every release project has its version lists. A release project shows a mise install only with `mise = true`, on its `[[repo.project]]` table, or on the `[[repo]]` table for the repository's own project. A project can publish releases that mise cannot install, such as a library.

The data of each release list, release project page and release page holds `mise_toml`, a `mise.toml` example built from `base_url`, for each project with `mise`. It is empty for any other project. For the project `alpha` of the repository `monorepo` in the fixture site, it is this:

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

The example expects the archive names of this repository's [release.yml](../../.github/workflows/release.yml), `<project>-<version>-<target>.tar.gz`, with the binary at `<root>/bin/<project>`. It takes the archives and the part of the tag before the version from one release: the newest listed release that is not a pre-release, or the newest listed release when all are pre-releases. mise's `latest` passes over a version with a `-PRE`. The example has these lines:

- A platform line for each target whose archive that release has: `linux-x64` for `x86_64-linux-musl`, `linux-arm64` for `aarch64-linux-musl`, and `macos-arm64` for `aarch64-macos`.
- `checksum_url`, only when that release has `SHA256SUMS`.

The example is empty when the project lists no version, or when that release has none of these archives.

### Checksums

The build copies a release's `SHA256SUMS` file with the other assets, and the mise example sets `checksum_url` to it. `surge` computes no checksums. You or your pipeline write this file with the assets, as `release.yml` does. A release without `SHA256SUMS` still publishes, with a warning when its project has `mise`. When the example takes its archives from that release, the example has no `checksum_url`.

mise checks the checksums only through its lock file. `mise install` never reads `checksum_url`. `mise lock` records the digest of each platform's archive in `mise.lock`, and later installs refuse a download that does not match. The fixture templates say this next to the example.

The release scan gives these warnings:

```text
warning: release tag fits no project's 'tag_prefix', so no 'versions.txt' lists it: '<tag>' (in repo '<repo>')
warning: release version is not MAJOR.MINOR.PATCH[-PRE], so 'versions.txt' leaves it out: '<version>' (in repo '<repo>', tag '<tag>')
warning: release tags name the same version, so 'versions.txt' lists only the first: '<tag>', '<tag>' (in repo '<repo>')
warning: release has no 'SHA256SUMS', so 'mise lock' records no checksums: '<tag>' (in repo '<repo>')
```

## Feeds

Each repository has these Atom feeds:

| Feed | Entries |
| --- | --- |
| `<r>/commits/<ref>.atom` | The newest `feed_count` commits of the branch. An entry is a commit page, with the subject as title, the committer date, the author, and the rest of the message as text. A branch that names no commit has no feed. |
| `<r>/tags.atom` | The newest `feed_count` tags by date, a tie in byte order of name. An entry has the tag name as title, and links the tag's release page, else its commit page, else the tag list. Its id is the tag list with the tag name as fragment, so two tags of one commit are two entries. Its text is the tag message without its signature. |
| `<r>/releases.atom` | With a release project only. The newest `feed_count` releases by date, a tie in byte order of tag. An entry has the tag as title, its release page as link and id, and the notes as HTML. |

The author of a tag or release entry is the tagger, or else the author of its commit. A feed's `modified` date is the date of its newest entry. A feed or an entry without a date has the Unix epoch, because Atom requires a date. Every feed link is absolute, below `base_url`. Every text holds only characters that XML 1.0 allows, as [Templates](#templates) describes.

## Clone export

Each repository gets a clone export, `<r>.git/`, that git can clone over dumb HTTP from any static host:

```sh
git clone https://git.example.org/apps-c17.git
```

The export holds `HEAD`, `info/refs`, `objects/info/packs`, and one pack with its idx. It publishes only `refs/heads/*` and `refs/tags/*`, and never the repository's `config` or hooks:

- The pack holds every object that a branch or a tag reaches, annotated and nested tags included, and tags of trees or blobs.
- `info/refs` lists each ref sorted by name, with a `^{}` line after each annotated tag, as `git update-server-info` writes it.
- `HEAD` names the default branch, so the export never has a detached `HEAD`.
- An empty repository has no pack, an empty `info/refs` and an empty `objects/info/packs`.

The pack's name is the hash of its content, and the same refs give the same pack. A build keeps the pack while the refs do not change, and a change of the default branch alone rewrites only `HEAD`. When the refs change, the build writes a new pack. It first writes the pack in a new directory below `TMPDIR`, or below `/tmp` when `TMPDIR` is unset or empty. It keeps the previous pack for one more build, unlisted, so a client that read an older `info/refs` from a cache can still finish its clone. The build after that removes it, also when a build between them failed before it wrote the new pack. A `--force` build does not read the output cache, so it removes the previous pack at once. A dry run does not make a changed pack. See [Dry run](#dry-run).

## Templates

Templates use [Mustache](https://mustache.github.io/) syntax and are in `templates_dir`. `surge` has no default templates, so each site supplies the templates that the keys of the [`[templates]` table](#configuration) name. [tests/fixtures/site/templates/](tests/fixtures/site/templates/) holds a complete set, which the golden test renders.

| Key | Required | Renders |
| --- | --- | --- |
| `index` | Always | The site index, `index.html`. |
| `tree` | Always | Each repository home, `<r>/index.html`, and each tree page. The home is the tree page of the default branch's root. |
| `blob` | Always | Each blob page. |
| `branch_list` | Always | Each branch list, `<r>/branches/index.html`. |
| `tag_list` | Always | Each tag list, `<r>/tags/index.html`. |
| `tag_feed` | Always | Each tag feed. |
| `commit` | Always | Each commit page. |
| `commit_list` | Always | Each commit list page. |
| `commit_feed` | Always | Each commit feed. |
| `release` | With a release project | Each release page, `<r>/releases/tag/<tag>/index.html`. |
| `release_list` | With a release project | Each release list, `<r>/releases/index.html`. |
| `release_project` | With a release project | Each release project page, `<r>/releases/<project>/index.html`. |
| `release_feed` | With a release project | Each release feed. |
| `not_found` | Always | The not-found page, at `not_found_path`. |

A site has a release project when a repository has a `releases_dir`, or a project with a `tag_prefix`. A site without one needs none of the four release templates. Every required template must exist. A missing one fails the build with an error that names the template and the first key in table order that names it, such as `(for 'templates.tree')`.

`{{value}}` escapes its output for HTML. `{{{value}}}` and `{{& value}}` write it without escaping, and work only for a key whose name ends in `_html`: `rendered_html`, `source_html`, `diff_html`, `notes_html` and `content_html`. `surge` makes the HTML of these keys itself. An unescaped tag of any other key that the data holds fails the render, so a value such as a title cannot add markup. A name that the data does not hold writes nothing. In an Atom template, `{{content_html}}` inside `<content type="html">` escapes the HTML, as Atom expects.

Every page and feed is valid UTF-8 without the characters that XML 1.0 forbids. `surge` replaces each invalid sequence in a value or a template with U+FFFD. It drops U+FFFE, U+FFFF, and the C0 control characters except tab, line feed and carriage return.

A partial reference `{{> name}}` loads `templates_dir/partials/<name>.html`. A partial name contains only letters, digits, `_` and `-`. A reference to another name fails the build. `surge` ignores the other files of `partials/` and its subdirectories. Before it writes anything, the build compiles each template that it renders and each partial once, and reports every template that it cannot read or compile. The feeds also use the `.html` partials. A partial can include itself, as the fixture's changed-files tree does. A partial that never stops including itself fails at the expansion limit in [Limits](#limits).

### Data trees

`surge` builds the data of each page in C. Every map holds every key that the tables below list, with empty text or `false` where a value is absent, because Mustache looks up a missing key in the enclosing maps. These rules apply to the values:

- Empty text is false in a section, so `{{#description}}…{{/description}}` shows its contents only for a description that is not empty.
- A flag such as `is_last` is `true` when it holds and empty otherwise, so it works as a section and as text.
- A date is RFC 3339 in the offset that git recorded, such as `2026-01-01T13:00:00+01:00`, with `Z` for a zero offset. An offset over 23:59 either way is `Z`, and a date outside the years 0000 to 9999 is `1970-01-01T00:00:00Z`, because RFC 3339 cannot write them.
- A size is a number of bytes.
- Every link is relative to the page and already holds its `path_to_root`. In the feeds, every link is absolute.

Every template gets `site`, with `title`, `description`, `author` and `base_url` from the config (`base_url` without a trailing `/`). It also gets `generator`, the name and version of `surge`, for a tag such as `<meta name="generator" content="{{generator}}">`, and `path_to_root`, the `../` prefix from the page to `output_dir`, empty at the top. A link to a static file is `{{path_to_root}}style.css`. A host serves the not-found page at any depth, so its `path_to_root` is the path of `base_url` followed by `/`, such as `/` for `https://git.example.org` and `/code/` for `https://git.example.org/code`. It is root-relative, so the not-found template can use the same partials as every other page, and it also works on a preview host.

The templates share these maps:

| Map | Keys |
| --- | --- |
| `repo` | Every page of a repository: `name`, `description`, `default_branch`, `url` (the home), `clone_url` (`<base_url>/<r>.git`), `branches_url`, `tags_url`, `tags_feed_url`, `commits_url` (the default branch's first commit list page, empty when it names no commit), `has_releases` (whether the repository has a [release project](#release-directories)), `releases_url` and `releases_feed_url` (each empty without one). |
| `ref` | The pages of a branch: `name`, `tree_url` (its root's tree page), `commits_url` (its first commit list page) and `commits_feed_url`. |
| Commit summary | `oid`, `short_oid` (7 hex digits), `subject`, `author` (a name), `date` (the committer date), `signature` (of a signed commit, or empty), `signature_kind` (`PGP`, `SSH` or `X.509`, from the line that opens the signature, or empty), `is_llm_assisted` (whether its trailers mark it, as [LLM-assisted commits](#llm-assisted-commits) describes), `llm_tools` (each with `name`: the value of each trailer that marks it, in message order, without the `<…>` at its end unless it is only that, once for each different value) and `url` (its commit page). It is `false` for a ref that names no commit, so `{{#commit}}…{{/commit}}` shows its contents only for a commit. |
| Entry | One row of a directory: `name`, `kind` (`dir`, `file`, `executable`, `symlink` or `gitlink`), `is_dir`, `is_file`, `is_executable`, `is_symlink`, `is_gitlink`, `url` (empty without a page), `is_linked`, `link_target` (a symlink's target), `link_url` (the page of a symlink's target, as [Files](#files) describes, or empty), `submodule_url` (a submodule's URL without its credentials, or empty), `submodule_commit_url` (the link to a submodule's commit, or empty), `oid` and `size` (empty for a directory or a submodule). |
| Breadcrumb | `name`, `url` and `is_last`. The last breadcrumb is the page's own directory or file, with an empty `url`. |
| Project map | `name`, `url` (its page), `versions_url`, `versions_json_url`, `mise_toml` (the [mise example](#version-lists-and-mise), empty without `mise`), and `releases`: release maps of its listed releases newest first, then of its other releases in byte order of tag. |
| Release map | `tag`, `version`, `date` (of its tag, as on the tag list), `url` (its page), `is_listed` (whether `versions.txt` lists it), `project_name` and `project_url` (empty for no project), `commit` (the tag's commit summary, `false` for a tag of a tree or a blob), `notes_html` (the tag message as Markdown, without its signature, with relative links that work on the release page), `signature` (of a signed tag, or empty), `signature_kind`, `has_sums` (whether it has `SHA256SUMS`), `is_mise` (whether its project has `mise`) and `assets`, each with `name`, `url` and `size`. |

Each kind of page also gets these keys, by the key of its template:

| Key | Keys |
| --- | --- |
| `index` | `repos`, one map for each repository in config order: `name`, `description`, `url` (its home), `clone_url`, `default_branch`, `modified` (the committer date of the default branch's newest commit, empty for an empty repository) and `projects`. A project, in config order, has `name`, `description`, `url` (its directory's tree page on the default branch), `has_releases` (whether it has a `tag_prefix`) and `releases_url` (its project page, or empty). And `contributions`, the [contributions calendar](#contributions), `false` when no commit counts. |
| `index` `contributions` | `count` (the commits in the window), `llm_count` (the LLM-assisted ones), `day_count` (the days with commits), `llm_day_count` (the days with LLM-assisted commits), `start_date` and `end_date` (the window's first and last day, `YYYY-MM-DD`), and `weeks`, 53 of them, oldest first. A week has `month` (the short English name of the month, such as `Jan`, when one of its days in the window is the 1st, else empty) and `days`, always 7, Sunday first. A day has `date` (`YYYY-MM-DD`), `weekday` (`0` for Sunday to `6`), `count`, `llm_count`, `level` (`0` to `4`), `has_llm` (whether it has an LLM-assisted commit) and `is_outside` (whether it is before the window's first day or after its last). |
| `not_found` | None: the not-found page gets only `site`, `generator` and `path_to_root`. |
| `tree` | `repo`; `ref` (for an empty repository, the default branch with empty links); `is_home` (whether the page is `<r>/index.html`); `is_empty` (whether the default branch has no commit, so the home has no tree page); `commit` (the branch's newest commit summary on the root's page, `false` elsewhere); `path` (empty for the root); `is_root`; `breadcrumbs` (to the directory); `tree_entries` (directories first, then files, each group in byte order); and `documents`. |
| `tree` `documents` | One map for each document, in [Documents](#documents) order: `kind`, the six kind flags (true for this one's kind), `is_main` (whether the page shows it first: the README, or else the first document), `path`, `url` (its blob page) and `rendered_html`. |
| `blob` | `repo`; `ref`; `path`; `name` (the file name); `breadcrumbs` (to the file); `mode` (`100644` or `100755`); `is_executable`; `size`; `raw_url` and `has_raw` (empty and `false` over `raw_len_max`); `is_too_large` (over `blob_len_max`, so the page has no contents); `is_binary` (libgit2's check, false when too large); `is_image` (an [image extension](#files)); `is_markup` (Markdown or Org); `rendered_html` (the rendered view, or empty); `source_html` (the lines, empty when binary or too large); and `line_count`. |
| `branch_list` | `repo`, and `branches` in byte order of name, each with `name`, `is_default`, `author` and `date` (of its commit), `tree_url`, `commits_url` and `commit` (its commit summary). A branch that names a tree or a blob has an empty `author`, `date` and links, and a `commit` of `false`. |
| `tag_list` | `repo`, and `tags`, newest first by the tagger's date, else the commit's committer date. A tie sorts in byte order of name, and a tag with neither date comes last. Each tag has `name`, `is_annotated`, `author` (the tagger, else the commit's author), `date` (or empty), `signature` (of a signed tag, or empty), `signature_kind`, `release_url` (or empty) and `commit` (the summary of the commit that the tag peels to, `false` for a tag of a tree or a blob). |
| `commit_list` | `repo`; `ref`; `page`; and `commits`, newest first. `page` has `number` (from 1), `count`, `is_first`, `is_last`, and `first_url`, `previous_url`, `next_url` and `last_url`, each empty when there is no such page or it is this page. A commit has the commit summary keys and `refs`, the branches and tags that name it, each with `name`, `kind` (`branch` or `tag`), `is_branch`, `is_tag` and `url` (a branch's tree page, or the tag list). |
| `commit` | `repo`; `commit`; `stats` (`file_count`, `additions` and `deletions`, which count the lines that the page shows); `is_truncated` (whether a cap cut the diff); `files` (in changed-files tree order); and `tree`. `commit` has the commit summary keys and `body` (the message after the subject, its trailers included), `trailers` (the `Key: value` lines of the message's last paragraph, as git finds them, in message order, each with `key` as written and `value`, its address included and its continuation lines joined with a space), `co_authors` (each with `name`: the value of each `Co-authored-by` trailer, its key in any letter case, without the `<…>` at its end, once for each different value), `author_date`, `committer` (a name), `is_root`, `is_merge` and `parents`, each with `oid`, `short_oid` and `url`, and an empty `subject`, `author` and `date`. |
| `commit` `files` | `index` (from 0), `anchor` (`diff-<index>`), `path`, `old_path` and `similarity` (in percent, `100` for an exact rename, both empty unless renamed), `status` (`added`, `deleted`, `modified`, `renamed` or `type-changed`), `is_added`, `is_deleted`, `is_modified`, `is_renamed`, `is_type_changed`, `is_too_large`, `is_binary`, `is_shown` (whether the page shows its diff), `is_truncated` (whether a cap cut its diff), `additions`, `deletions`, `diff_html`, `is_image` (whether the page shows it as an [image](#history)), `old_image_url` and `new_image_url` (each side as a `data:` URI, empty for a missing side or one that `image_len_max` leaves out) and `is_image_omitted` (whether a side with bytes was left out). |
| `commit` `tree` | The root node of the changed-files tree. A node has `name`, `is_dir`, `children` (empty for a file) and `file` (a file's map from `files`, `false` for a directory), so a partial that includes itself can render it. |
| `release_list` | `repo`; `projects` (a project map for each release project, in config order); and `releases` (a release map for every release: each project's releases in project order, then the releases of no project in byte order of tag). |
| `release_project` | `repo`, and `project`, the project map. |
| `release` | `repo`; `release`, the release map; and `project`, the project map of its project, `false` for a release of no project. |
| `tag_feed`, `commit_feed`, `release_feed` | `repo` with absolute links; in `commit_feed` only, `ref` with absolute links; `feed`; and `feed_entries`, newest first. `feed` has `id` and `url` (the feed's own URL), `html_url` (the first commit list page, the tag list or the release list) and `modified`. An entry has `id`, `title`, `url`, `modified`, `author`, `content` (text, empty for a release), `content_html` (HTML, empty for a commit or a tag), and `is_llm_assisted` and `llm_tools` as a commit summary has them (`false` and empty for a tag or a release). A relative link in `content_html` is relative to the entry's page, so the fixture's feeds set `xml:base="{{url}}"` on `<content>`. |

`source_html` is a `<pre class="surge-code"><code>` with a code line for each line of the blob. The line's number is an `<a id="L<n>" href="#L<n>">`.

`diff_html` is a `<pre class="surge-diff"><code>` with these code lines:

- A code line for each hunk header, with the class `surge-hunk`.
- A code line for each diff line, with the class `surge-add`, `surge-del`, `surge-context` or `surge-no-newline`. It has two numbers, the old line's and the new line's, and a number is empty when one side lacks the line. Each number is an `<a>` with the id `<anchor>-hunk-<h>-L<old>` or `<anchor>-hunk-<h>-R<new>`, and links to itself.

`diff_html` is empty for a file whose diff the page does not show.

A code line, in `source_html` and `diff_html`, has the markup of a line of an Org code block with line numbers. It is a `<span class="code-line">` that holds a `<span class="code-line-numbers">` with a `code-line-number` for each number, and then the line's text with its line end in a `<span class="code-line-text">`. A stylesheet can lay out the lines as grid rows, with a column for each number and one for the text.

The fixture templates also give each feed an `<author>`, the site's `author`, because Atom needs one for an entry without one.

### Syntax highlighting

A blob page and a diff highlight a file when the binary includes a grammar for the language that its path names. `surge` writes each token as a `<span>` whose class names the kind of token:

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

`surge` supplies no stylesheet, so each site sets the colors of these classes. A span never crosses a line, so every line of `source_html` and `diff_html` is whole HTML.

The path of a file names its language by the exact file name, or else by its extension in any letter case. `surge` does not read the file's contents to choose:

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

The release binaries include no grammar for Make, CMake or Dockerfile, so those files are escaped text. A build from source chooses its grammars with `APPS_GRAMMARS`, as [BUILD.md](../../BUILD.md#grammars) describes. `surge --version` lists the grammars of a binary. `surge` highlights each file in one language, so in an HTML file it only escapes the contents of `<script>` and `<style>`. A code block in a README, a document or release notes uses the language that the block names, such as `c` or `bash`, in any letter case.

A diff highlights the whole old blob and the whole new blob of a file once each. A deleted line takes the HTML of the old side, and an added or context line the HTML of the new side. So a line inside a comment or a string that starts above the hunk keeps its color. A diff is highlighted only when the whole diff of the commit is under its caps. `surge` writes a file as escaped text when its path names no grammar, when it is over `blob_len_max`, or when tree-sitter would need more than 128 MiB for it.

## Host notes

`surge` writes plain files that any static host serves. A few host settings decide whether every page and every clone works. [Host notes](../../README.md#host-notes) in the repository's README lists the path that each host serves its not-found page from.

### 404 handling

A dumb-HTTP clone asks for paths that do not exist. git first asks for each object as a loose object, and only then reads the pack. So a host must answer a missing path with a 404. `surge` always writes a not-found page, at `not_found_path`, and a static host that reads it from that path serves it for a missing path with status 404. Set `not_found_path` to the path that the host reads, such as `bunnycdn_errors/404.html` on bunny.net. Cloudflare Pages treats a project without a top-level `404.html` as a single-page application, and answers every missing path with 200 and the index page. Against such a host, git 2.56 reports the page as a corrupt object (`inflate: data stream error`) for each loose object that it asks for, then finds the objects in the pack and finishes the clone.

### Single-page-app fallback

If a host has a fallback that answers every missing path with one page, turn it off for the site. On Cloudflare Pages, the top-level `404.html` turns it off.

### File-size limits

Hosts limit the size and the number of files:

- Cloudflare Pages allows 25 MiB for each file, and 20,000 files on the free plan or 100,000 on a paid plan.
- GitHub blocks files over 100 MiB in a repository, and limits a published Pages site to 1 GB.
- pgs.sh allows 100 MB for each file on pico+.

The default `raw_len_max` is GitHub's 100 MiB, and the default `pack_len_max` is Cloudflare Pages' 25 MiB. A pack over `pack_len_max` still builds, with a warning that names the pack:

```text
warning: clone export pack exceeds max pack length (26214400 bytes) at 31457280 bytes, so a host with that file limit cannot serve it: '<r>.git/objects/pack/pack-<id>.pack' (in repo '<r>')
```

`surge` puts no size limit on a release asset, so check large archives against the host's limit. Every branch gets its own blob pages and raw copies, and every commit a page, so many long-lived branches multiply the number of files.

### Dotfiles and underscores

Nothing that a clone needs starts with `.` or `_`. The blob pages and raw copies of a tree's dotfiles do, such as `blob/main/.gitignore/index.html`, and so do `_headers` and `_surge/`. pgs.sh rejects dotfiles unless an `_pgs_ignore` allows them.

GitHub Pages runs Jekyll when it publishes from a branch, and Jekyll drops every file or directory whose name starts with `_`, `.` or `#`. The pages of tree entries with such names then disappear. Publish the site with a GitHub Actions workflow that uploads `output_dir`, or put an empty `.nojekyll` file in `static_dir`. A `.nojekyll` that you add to `output_dir` by hand does not survive the next build.

### Trailing slashes

Every link that `surge` writes names an `index.html`, so the site works on any host. A GitHub URL with the host and owner replaced, such as `https://git.example.org/apps-c17/tree/main/docs`, works only on a host that redirects `/x` to `/x/`:

- Documented: Netlify (Pretty URLs, on by default), S3 website endpoints (302), pgs.sh (301), and Cloudflare's Workers static assets (307 under `auto-trailing-slash`). The Cloudflare Pages documentation shows only the reverse cases.
- Observed but not documented: GitHub Pages (301) and `python3 -m http.server` (301).

### Cache headers

A clone reads `HEAD`, `info/refs` and `objects/info/packs` first, and these files change when the refs move. A pack never changes, because its name is the hash of its content. With `headers_file = true`, the build writes `_headers`, which Cloudflare Pages and Netlify read. It has one rule for each small file of every clone export, and one for the packs of each repository. For a site whose `base_url` is `https://example.org/git`, with one repository, `apps-c17`, it is this:

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

Each path starts with the path of `base_url`, which is empty for a site at the host's root. Cloudflare Pages reads at most 100 rules, with one splat in each, so a site of up to 97 repositories fits. GitHub Pages' documentation does not mention custom headers. With `headers_file = true`, a `static/_headers` file collides with the generated one and makes the build fail. With `headers_file = false`, the build copies a `static/_headers` like any static file.

## Trust model

`surge` publishes repositories that you own and trust, and does not sanitize their contents:

> [!warning]
> READMEs, documents, release notes, and Markdown and Org files render with raw HTML enabled, and `surge` writes that HTML without sanitizing it. Link targets stay as written, `javascript:` included. `raw/` holds every blob up to `raw_len_max`, SVG files included, on the site's origin. `surge` writes no Content Security Policy, because the templates are yours. Build only repositories whose contents you trust.

libgit2's owner check is off, so `surge` reads a repository that another user owns, such as `/srv/git` owned by `git` while the build runs as you. The checks that protect the output tree stay on, because a trusted repository can still hold long or case-colliding paths, and a crafted object can hold any name. [Output directory](#output-directory) and [Names without pages](#names-without-pages) describe them.

## Development

[BUILD.md](../../BUILD.md#surge) describes how to build, test and package `surge`, [ARCHITECTURE.md](ARCHITECTURE.md) describes its code, and [STYLE.md](../../STYLE.md) describes the code style.

## Dependencies

The root [vendor/](../../vendor/) directory contains these dependencies, except the libraries in `libs/`. [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists the licenses of the vendored dependencies.

- [libs/](../../libs/): The libraries that `surge` shares with the other apps: `cli`, `core`, `os`, `runtime`, `formats`, `markup`, `output` and `templates`, and for the unit tests `testing` and `os_memory`. [libs/ARCHITECTURE.md](../../libs/ARCHITECTURE.md) describes them.
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
