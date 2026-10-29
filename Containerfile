FROM docker.io/library/ubuntu:26.04

ARG LLVM_VERSION=23
ARG GCC_VERSION=14

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates \
      cmake \
      cppcheck \
      ffmpeg \
      "gcc-${GCC_VERSION}" \
      git \
      libc6-dev \
      make \
      nasm \
      ninja-build \
      wget \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /etc/apt/keyrings \
    && wget --quiet https://apt.llvm.org/llvm-snapshot.gpg.key \
      --output-document=/etc/apt/keyrings/apt.llvm.org.asc \
    && echo \
      "deb [signed-by=/etc/apt/keyrings/apt.llvm.org.asc] https://apt.llvm.org/resolute/ llvm-toolchain-resolute-${LLVM_VERSION} main" \
      > /etc/apt/sources.list.d/apt.llvm.org.list \
    && apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      "clang-${LLVM_VERSION}" \
      "clang-format-${LLVM_VERSION}" \
      "clang-tidy-${LLVM_VERSION}" \
      "libclang-rt-${LLVM_VERSION}-dev" \
    && rm -rf /var/lib/apt/lists/*

# cwrap has no distribution package, so the image installs a pinned release and checks its archive
# against the release's `SHA256SUMS`.
ARG CWRAP_VERSION=0.3.1
RUN case "$(uname -m)" in \
      x86_64) target=x86_64-linux-musl ;; \
      aarch64) target=aarch64-linux-musl ;; \
      *) echo "no cwrap release for $(uname -m)" >&2; exit 1 ;; \
    esac \
    && archive="cwrap-${CWRAP_VERSION}-${target}.tar.gz" \
    && release="https://github.com/unindented/apps-c17/releases/download/cwrap-v${CWRAP_VERSION}" \
    && dir="$(mktemp -d)" \
    && cd "${dir}" \
    && wget --quiet "${release}/${archive}" "${release}/SHA256SUMS" \
    && awk -v archive="${archive}" '$2 == archive' SHA256SUMS > cwrap.sha256 \
    && if [ ! -s cwrap.sha256 ]; then \
      echo "no digest for '${archive}' in the SHA256SUMS of cwrap-v${CWRAP_VERSION}" >&2; exit 1; \
    fi \
    && sha256sum --check --quiet cwrap.sha256 \
    && tar -xzf "${archive}" -C /usr/local --strip-components=1 --no-same-owner \
      "cwrap-${CWRAP_VERSION}-${target}/bin/cwrap" \
    && cd / \
    && rm -rf "${dir}"

# mise has no distribution package either. surge's golden test installs a release project with it,
# so the image installs a pinned release and checks its archive against `SHASUMS256.txt`.
ARG MISE_VERSION=2026.11.0
RUN case "$(uname -m)" in \
      x86_64) target=linux-x64-musl ;; \
      aarch64) target=linux-arm64-musl ;; \
      *) echo "no mise release for $(uname -m)" >&2; exit 1 ;; \
    esac \
    && archive="mise-v${MISE_VERSION}-${target}.tar.gz" \
    && release="https://github.com/jdx/mise/releases/download/v${MISE_VERSION}" \
    && dir="$(mktemp -d)" \
    && cd "${dir}" \
    && wget --quiet "${release}/${archive}" "${release}/SHASUMS256.txt" \
    && awk -v archive="./${archive}" '$2 == archive { print $1 "  " substr($2, 3) }' \
      SHASUMS256.txt > mise.sha256 \
    && if [ ! -s mise.sha256 ]; then \
      echo "no digest for '${archive}' in the SHASUMS256.txt of mise v${MISE_VERSION}" >&2; exit 1; \
    fi \
    && sha256sum --check --quiet mise.sha256 \
    && tar -xzf "${archive}" -C /usr/local --strip-components=1 --no-same-owner mise/bin/mise \
    && cd / \
    && rm -rf "${dir}"

ENV CC=clang-23

# The image holds the whole repository. Each project has its own presets, so a run selects the
# project's directory with `--workdir`, for example `--workdir /src/apps/sosig`.
COPY . /src
WORKDIR /src

ENTRYPOINT ["cmake", "--workflow", "--preset"]
CMD ["ci-debug"]
