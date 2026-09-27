# apps-c17

This repository holds three small programs written in C17, and the libraries that they share.

| Directory | Contents |
| --- | --- |
| [`apps/sosig`](apps/sosig) | `sosig`, a static site generator for Markdown and Org files. |
| [`apps/fram`](apps/fram) | `fram`, a static photo and video gallery generator. |
| [`apps/cwrap`](apps/cwrap) | `cwrap`, a formatter that refills the prose in C comments. |
| [`libs`](libs) | The libraries that the apps share: `shared`, `core` and `testing`, which [libs/ARCHITECTURE.md](libs/ARCHITECTURE.md) describes. |
| [`vendor`](vendor) | The sources of the third-party libraries that the apps and `libs` use. |
| [`cmake`](cmake) | The CMake modules, presets and toolchains that every project uses. |

Each app and `libs` is its own CMake project, with its own presets. To build one, configure it from its directory, for example `cmake --preset debug` in `apps/sosig`. Each app has its own README and build instructions.

The CI workflow, [ci.yml](.github/workflows/ci.yml), checks each project that a change can affect, and its `Summary` job reports the result of every check. The root [Containerfile](Containerfile) builds the Linux CI image for every project. Build it in the repository root, and select the project's directory with `--workdir`:

```sh
podman build --tag apps-c17-linux-ci --file Containerfile .
podman run --rm --workdir /src/apps/sosig apps-c17-linux-ci ci-debug
```

Each app has its own version and its own releases. A release tag has the app's name, for example `sosig-v0.3.0`, and starts [release.yml](.github/workflows/release.yml) for that app.

## License

All projects use the MIT License, as [LICENSE.txt](LICENSE.txt) states.
