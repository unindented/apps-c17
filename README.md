# apps-c17

This repository holds three small programs written in C17, and the library that they share.

| Directory | Contents |
| --- | --- |
| [`apps/sosig`](apps/sosig) | `sosig`, a static site generator for Markdown and Org files. |
| [`apps/fram`](apps/fram) | `fram`, a static photo and video gallery generator. |
| [`apps/cwrap`](apps/cwrap) | `cwrap`, a formatter that refills the prose in C comments. |
| [`libs/shared`](libs/shared) | `sharedstuff`, an arena allocator, counted strings, buffers and an error log. |

Each directory is its own CMake project, with its own README, build instructions and presets. To build one, configure it from its directory, for example `cmake --preset debug` in `apps/sosig`.

Each app has its own version and its own releases. A release tag has the app's name, for example `sosig-v0.3.0`.

## License

Each project has its own `LICENSE.txt`. All of them use the MIT License.
