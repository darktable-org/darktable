# Darktable Container Build Environment

A Docker/Podman image that **exactly mirrors the CI compile check** (ubuntu:26.04,
GCC 16 / Clang 22) with all build dependencies pre-installed.

This is an **optional** complement to building natively. Every contributor can
continue building in their own environment as before. It exists for those who find
it useful — see [Use cases](#use-cases) below.

## Use cases

- **Immutable/atomic Linux** (Fedora Silverblue, NixOS, SteamOS, etc.): avoids
  installing and layering 50+ packages on the host system that break on weekly OS rebuilds.
- **Infrequent contributors**: get a working build environment without permanent setup.
- **Reproducing CI failures**: your local environment matches the CI container exactly,
  so a build that passes here passes CI.
- **Sandboxing AI coding agents**: tools running inside the container can only access
  the mounted workspace — host SSH keys, credentials, private documents, and other
  projects are not visible to them.

## Installing Docker or Podman

Any OCI-compatible runtime works. Install one for your platform:

| Platform | Command |
| -------- | ------- |
| Debian / Ubuntu | `sudo apt install docker.io` or [Docker Engine docs](https://docs.docker.com/engine/install/ubuntu/) |
| Fedora / RHEL | `sudo dnf install docker` or [Docker Engine docs](https://docs.docker.com/engine/install/fedora/) |
| Arch | `sudo pacman -S docker` |
| openSUSE | `sudo zypper install docker` |
| macOS | `brew install --cask docker` (Docker Desktop) or `brew install podman` |
| Windows | [Docker Desktop](https://docs.docker.com/desktop/setup/install/windows-install/) |

On Linux, **Podman** is a rootless drop-in replacement (`alias docker=podman`).
See the [Podman installation guide](https://podman.io/docs/installation) for all distros.

After installing, make sure your user is in the `docker` group (Linux) or that
Docker Desktop is running (macOS/Windows) before running the commands below.

## Usage tiers

| Tier | Requirements | Good for |
| ---- | ------------ | -------- |
| [Docker/Podman CLI](#tier-1-dockerpodman-cli-only) | Docker or Podman | Verify build, create AppImage — no IDE needed |
| [devcontainer CLI](#tier-2-devcontainer-cli-terminal) | Docker + `devcontainer` CLI | Full development from a terminal |
| [IDE integration](#tier-3-ide-integration) | Docker + any Dev Container-capable IDE | Full development with editor |

## Tier 1: Docker/Podman CLI only

The most lightweight option. No IDE, no extra tooling.

```bash
# Build the image once (from the repository root)
docker build -t darktable-dev -f .devcontainer/Dockerfile .

# Verify the build compiles cleanly (same environment as CI)
docker run --rm --user "$(id -u):$(id -g)" \
    -v "$PWD":/workspace -w /workspace \
    darktable-dev bash -lc './build.sh --prefix /tmp/dt --build-type Release'

# Build an AppImage for GUI testing on the host
docker run --rm --user "$(id -u):$(id -g)" \
    -v "$PWD":/workspace -w /workspace \
    -e APPIMAGE_EXTRACT_AND_RUN=1 \
    darktable-dev bash -lc './tools/appimage-build-script.sh'
```

The AppImage appears in `build/Darktable-*.AppImage` and can be run directly on the host.

> Replace `docker` with `podman` if you use Podman.

## Tier 2: devcontainer CLI (terminal)

```bash
# Start the container
devcontainer up --workspace-folder .

# Open a shell inside it
devcontainer exec --workspace-folder . bash
```

Then build as usual (see [Building](#building)).

## Tier 3: IDE integration

The container follows the open [Dev Container specification](https://containers.dev/)
and works with any supporting tool:

- **VS Code** — Dev Containers extension → "Reopen in Container"
- **JetBrains IDEs** (CLion, etc.) — Dev Containers plugin
- **Neovim / other editors** — via `devcontainer` CLI above

The [VS Code extensions listed in `devcontainer.json`](devcontainer.json) are all
from Microsoft (`ms-vscode.*`) or well-established publishers.

## Building

```bash
# Standard development build
./build.sh --prefix /tmp/dt --build-type RelWithDebInfo

# Debug build
./build.sh --prefix /tmp/dt --build-type Debug

# Use Clang 22 instead of GCC 16 (matches CI LLVM22 path)
export CC=clang-22 CXX=clang++-22
./build.sh --prefix /tmp/dt --build-type RelWithDebInfo
```

## Testing with AppImage

The container has no display, so GUI testing uses an AppImage run on the host.

```bash
# Build the AppImage
# APPIMAGE_EXTRACT_AND_RUN=1 is required inside any Docker/devcontainer (no FUSE)
APPIMAGE_EXTRACT_AND_RUN=1 ./tools/appimage-build-script.sh
```

The AppImage is created in `build/Darktable-*.AppImage`.

**Run on the host** (outside the container):

```bash
chmod +x build/Darktable-*.AppImage

# Use a separate config dir to avoid affecting your production darktable
./build/Darktable-*.AppImage --configdir ~/.config/darktable-test
```

## Running unit tests

`libcmocka-dev` is already installed:

```bash
./build.sh --prefix /tmp/dt --build-type RelWithDebInfo -- -DBUILD_TESTING=ON
cd build && ctest
```

## CI environment

The [Dockerfile](Dockerfile) is the single source of truth for the build
environment. It mirrors the "Install Base Dependencies" step in
`.github/workflows/ci.yml` exactly. Check the Dockerfile for the current base
image, compiler versions, and package list.

## Troubleshooting

### AppImage fails with FUSE error

Always set `APPIMAGE_EXTRACT_AND_RUN=1` — FUSE is not available inside containers.

### Git submodules not initialized

```bash
git submodule update --init --recursive
```

### Git says the mounted repository has dubious ownership inside the container

Use the same UID/GID as the host when starting Docker, or configure the mounted repo as safe inside the container:

```bash
docker run --rm --user "$(id -u):$(id -g)" \
    -v "$PWD":/workspace -w /workspace \
    darktable-dev bash -lc './build.sh --prefix /tmp/dt --build-type Release'

# or, if you keep the default root user inside the container:
git config --global --add safe.directory /workspace
```

### Need to install an extra package temporarily

```bash
sudo apt-get update && sudo apt-get install <package>
```

### Rebuild the container after Dockerfile changes

VS Code: F1 → "Dev Containers: Rebuild Container"
CLI: `devcontainer up --workspace-folder . --remove-existing-container`

## File structure

```text
.devcontainer/
├── Dockerfile           # Build environment (mirrors CI ubuntu:26.04)
├── devcontainer.json    # IDE/tooling configuration
└── README.md            # This file
```
