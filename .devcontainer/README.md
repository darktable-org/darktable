# Darktable Container Build Environment

A Docker/Podman image that **exactly mirrors the CI compile check**, with all
build dependencies pre-installed. It is an **optional** complement to building
natively — every contributor can continue using their own environment as before.

## Use cases

- **Immutable/atomic Linux** (Fedora Silverblue, NixOS, SteamOS, etc.): avoids
  installing and layering 50+ build packages that break on weekly OS rebuilds.
- **Infrequent contributors**: get a working build environment without a
  permanent setup.
- **Reproducing CI failures**: your local environment matches the CI container
  exactly, so a build that passes here passes CI.
- **Sandboxing AI coding agents**: tools running inside the container can only
  access the mounted workspace — host SSH keys, credentials, private documents,
  and other projects remain invisible to them.

## Prerequisites: Docker or Podman

Docker (or Podman) is required for all usage options below. Install one:

| Platform | Command / link |
| -------- | -------------- |
| Debian / Ubuntu | `sudo apt install docker.io` or [Docker CE](https://docs.docker.com/engine/install/ubuntu/) (recommended — includes BuildKit) |
| Fedora / RHEL | `sudo dnf install docker` or [Docker CE](https://docs.docker.com/engine/install/fedora/) |
| Arch | `sudo pacman -S docker` |
| openSUSE | `sudo zypper install docker` |
| macOS | `brew install --cask docker` or `brew install podman` |
| Windows | [Docker Desktop](https://docs.docker.com/desktop/setup/install/windows-install/) |

On Linux, add your user to the `docker` group and log out/in before using it:

```bash
sudo usermod -aG docker "$USER"
```

**Podman** is a rootless drop-in replacement on Linux (`alias docker=podman`).
See the [Podman installation guide](https://podman.io/docs/installation).

## Option 1: Docker/Podman CLI (most lightweight)

No IDE, no extra tooling — just build and run the container directly.

```bash
# Pull the pre-built CI image
docker pull ghcr.io/darktable-org/darktable-build:latest

# Verify the build compiles cleanly (same environment as CI)
docker run --rm --user "$(id -u):$(id -g)" \
    -v "$PWD":/workspace -w /workspace \
    ghcr.io/darktable-org/darktable-build:latest \
    bash -lc './build.sh --prefix /tmp/dt --build-type Release'

# Build an AppImage for GUI testing on the host
docker run --rm --user "$(id -u):$(id -g)" \
    -v "$PWD":/workspace -w /workspace \
    -e APPIMAGE_EXTRACT_AND_RUN=1 \
    ghcr.io/darktable-org/darktable-build:latest \
    bash -lc './tools/appimage-build-script.sh'
```

The AppImage appears in `build/Darktable-*.AppImage` and can be run directly on the host.

> Replace `docker` with `podman` if you use Podman.

## Option 2: Dev Container (richest experience)

A [Dev Container](https://containers.dev/) adds IDE integration on top of the
same Docker image: editor extensions, CMake integration, debugger support, etc.

### VS Code

Install the [Dev Containers extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers),
then open this repository and click **"Reopen in Container"** when prompted
(or F1 → *Dev Containers: Reopen in Container*). Git submodules are initialized
automatically.

### JetBrains IDEs (CLion, etc.)

Install the Dev Containers plugin and follow the
[JetBrains Dev Containers guide](https://www.jetbrains.com/help/idea/connect-to-devcontainer.html).

### Other editors and terminal

Install the [`devcontainer` CLI](https://github.com/devcontainers/cli):

```bash
npm install -g @devcontainers/cli
```

Then:

```bash
# Start the container
devcontainer up --workspace-folder .

# Open a shell inside it
devcontainer exec --workspace-folder . bash
```

Then build as usual (see [Building](#building)).

> **VS Code and JetBrains bundle their own devcontainer implementation** — you
> only need to install the CLI separately when using other editors or working
> purely in a terminal.

The VS Code extensions listed in [`devcontainer.json`](devcontainer.json) are
all from Microsoft (`ms-vscode.*`) or well-established publishers.

## Building darktable

Inside the container (any option):

```bash
# Standard development build
./build.sh --prefix /tmp/dt --build-type RelWithDebInfo

# Debug build
./build.sh --prefix /tmp/dt --build-type Debug

# Switch to Clang 22 (matches CI LLVM22 path)
export CC=clang-22 CXX=clang++-22
./build.sh --prefix /tmp/dt --build-type RelWithDebInfo
```

## Testing with AppImage

The container has no display. GUI testing uses an AppImage built inside the
container and run on the host.

```bash
# APPIMAGE_EXTRACT_AND_RUN=1 is required — FUSE is not available in containers
APPIMAGE_EXTRACT_AND_RUN=1 ./tools/appimage-build-script.sh
```

The AppImage is created in `build/Darktable-*.AppImage`. Run it on the host:

```bash
chmod +x build/Darktable-*.AppImage
./build/Darktable-*.AppImage --configdir ~/.config/darktable-test
```

Using `--configdir` avoids touching your production darktable configuration.

## Running unit tests

`libcmocka-dev` is already installed:

```bash
./build.sh --prefix /tmp/dt --build-type RelWithDebInfo -- -DBUILD_TESTING=ON
cd build && ctest
```

## CI environment

The [Dockerfile](Dockerfile) is the single source of truth for the build
environment. The `.github/workflows/build-docker.yml` workflow implements a
**build → test → push** sequence: it builds a candidate image from the
Dockerfile, runs a smoke-test build of darktable inside it, and only pushes
to GHCR if the build succeeds. Linux CI jobs always pull the last tested
`:latest` image.

### Pre-built images on GHCR

The `:latest` tag on `ghcr.io/darktable-org/darktable-build` is updated
whenever `.devcontainer/Dockerfile` changes on `master`, after the candidate
image passes a smoke-test build of darktable. Each release is also tagged
`YYYY-MM-DD-SHORTSHA` for pinned auditing.

`workflow_dispatch` on `build-docker.yml` lets maintainers trigger a manual
rebuild — useful when the upstream `ubuntu:26.04` base image gains security
patches without any change to the Dockerfile.

### Customising the build environment

To add or remove packages, edit `.devcontainer/Dockerfile` and submit it as a
normal PR. When the change merges to `master`, `build-docker.yml` runs
automatically, builds and smoke-tests the new image, and pushes it to GHCR
only if the build succeeds.

## Troubleshooting

### `docker build` prints a deprecation warning about the legacy builder

This happens with older Docker installations (e.g. Ubuntu's `docker.io` package)
that don't use BuildKit by default. Fix by installing Docker CE via the
[official Docker Engine docs](https://docs.docker.com/engine/install/ubuntu/)
(which includes `docker-buildx-plugin`), or just add the plugin to an existing
installation:

```bash
sudo apt install docker-buildx-plugin
```

### AppImage build fails with FUSE error

Always set `APPIMAGE_EXTRACT_AND_RUN=1` — FUSE is not available inside containers.

### Git submodules not initialized

```bash
git submodule update --init --recursive
```

### Git says the mounted repository has dubious ownership inside the container

Pass `--user "$(id -u):$(id -g)"` to `docker run` (as shown in the examples
above), or mark the path as safe inside the container:

```bash
git config --global --add safe.directory /workspace
```

### Need to install an extra package temporarily

```bash
sudo apt-get update && sudo apt-get install <package>
```

### Rebuild the container after Dockerfile changes

VS Code: F1 → *Dev Containers: Rebuild Container*
CLI: `devcontainer up --workspace-folder . --remove-existing-container`

## File structure

```text
.devcontainer/
├── Dockerfile           # Build environment (mirrors CI)
├── devcontainer.json    # IDE/tooling configuration
└── README.md            # This file
.github/workflows/
├── ci.yml               # Linux jobs run against the published :latest image
└── build-docker.yml     # Build → test → push :latest (on Dockerfile changes or workflow_dispatch)
```
