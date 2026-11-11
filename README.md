<div align="center">
  <img src="media/logo.webp" height="300" alt="">
</div>

<h1 align="center"><code>apps</code></h1>

This repository holds four small programs written in C17, and the libraries that they share.

## LLM disclosure

> [!warning]
> This is all vibe-coded slop, produced with Claude Opus 4.8, 5, and 5.5.

## Contents

| Directory | Contents |
| --- | --- |
| [`apps/sosig`](apps/sosig) | `sosig`, a static site generator for Markdown and Org files. |
| [`apps/fram`](apps/fram) | `fram`, a static photo and video gallery generator. |
| [`apps/cwrap`](apps/cwrap) | `cwrap`, a formatter that refills the prose in C comments. |
| [`apps/surge`](apps/surge) | `surge`, a static site generator for git repositories. |
| [`libs`](libs) | The libraries that the apps share: `core`, `testing`, `os`, `runtime`, `cli`, `formats`, `markup`, `output` and `templates`, which [libs/ARCHITECTURE.md](libs/ARCHITECTURE.md) describes. |
| [`vendor`](vendor) | The sources of the third-party libraries that the apps and `libs` use. |
| [`cmake`](cmake) | The CMake modules, presets and toolchains that every project uses. |

Each app and `libs` is its own CMake project, with its own presets. To build one, configure it from its directory, for example `cmake --preset debug` in `apps/sosig`. [BUILD.md](BUILD.md) gives the commands that build, test and package each project, and describes CI. [STYLE.md](STYLE.md) describes the code style.

Each app has its own version and its own releases. A release tag has the app's name, for example `sosig-v0.3.0`, and starts [release.yml](.github/workflows/release.yml) for that app.

## Host notes

`sosig`, `fram` and `surge` each write a not-found page, which a static host serves for a path that does not exist. Its template is `templates.not_found`, and its output path below `output_dir` is `not_found_path`. Both default to `404.html`. A host reads the page from a path of its own, so set `not_found_path` to that path:

| Host | `not_found_path` | Without the page |
| --- | --- | --- |
| bunny.net | `"bunnycdn_errors/404.html"` | The host answers with its own page. |
| Cloudflare Pages | `"404.html"`, the default | The host treats the project as a single-page application, and answers every missing path with status 200 and the index page. |

GitHub Pages and Netlify also read a top-level `404.html`. Moving a site to another host changes only `not_found_path`, and the template stays where it is. The page's `path_to_root` is the path of `base_url`, so the page loads its styles at every depth below the site. Each app's README describes its page, and [surge's README](apps/surge/README.md#404-handling) why a clone needs the status 404.

## License

All projects use the MIT License, as [LICENSE.txt](LICENSE.txt) states.
