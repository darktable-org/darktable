# Darktable Development Container

A complete development environment for building and testing darktable with all dependencies pre-installed.

## Purpose

This devcontainer provides:
- ✅ Build darktable from source in an environment **identical to the CI compile check** (`ubuntu:26.04`, GCC 16 / Clang 22)
- ✅ Create AppImage for GUI testing on host
- ✅ Debug with GDB
- ✅ Sandbox AI coding agents so they cannot access host SSH keys, credentials, or private files
- ❌ Does NOT run GUI inside container (use AppImage on host)

## Prerequisites

- [Docker](https://www.docker.com/products/docker-desktop/) (or any OCI-compatible runtime)
- Any [Dev Container-compatible tool](https://containers.dev/supporting):
  - VS Code with [Dev Containers extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers)
  - JetBrains IDEs (CLion, IntelliJ, etc.) via Dev Containers plugin
  - Neovim / other editors via [devcontainer CLI](https://github.com/devcontainers/cli)
  - Plain terminal: `devcontainer up --workspace-folder .`

## Quick Start

### VS Code

1. Open this repository in VS Code
2. Click "Reopen in Container" when prompted (or F1 → "Dev Containers: Reopen in Container")
3. Wait for container build (~5-10 minutes first time)
4. Git submodules are initialized automatically

### Terminal / other IDEs

```bash
# Build and start the container
devcontainer up --workspace-folder .

# Open a shell inside it
devcontainer exec --workspace-folder . bash
```

## Building Darktable

### Basic Build (for development)

```bash
# Build darktable (binaries in ./build/bin/)
./build.sh --prefix /tmp/dt --build-type RelWithDebInfo

# Run from build directory
./build/bin/darktable --version
```

### Build with Debug Symbols

```bash
./build.sh --prefix /tmp/dt --build-type Debug
```

### Clean Build

```bash
rm -rf build
./build.sh --prefix /tmp/dt --build-type RelWithDebInfo
```

## Testing

### GUI Testing with AppImage

GUI testing requires building an AppImage and running it on your host system:

```bash
# 1. Create lensfun directory (avoids warnings)
sudo mkdir -p /var/lib/lensfun-updates

# 2. Build AppImage (takes ~10-15 minutes)
APPIMAGE_EXTRACT_AND_RUN=1 ./tools/appimage-build-script.sh

# 3. The AppImage is created in: build/Darktable-*.AppImage
```

**To run on your host:**

```bash
# From host terminal (not in container)
# If workspace is mounted, the AppImage is accessible at:
cd <repo-path>/build

# Make executable and run
chmod +x Darktable-*.AppImage
./Darktable-*.AppImage --configdir ~/.config/darktable-test
```

**Using `--configdir`** creates a separate configuration to avoid conflicts with your production darktable installation.

## CI Environment Alignment

This container uses `ubuntu:26.04` and the **exact same packages** installed by the CI workflow
(`.github/workflows/ci.yml`), so builds here are directly comparable to CI results.

Compilers available (matching CI):
- **Primary:** GCC 16 (`gcc-16` / `g++-16`) — default CI compiler
- **Alternative:** Clang 22 (`clang-22` / `clang++-22`)

To switch compilers:
```bash
export CC=clang-22 CXX=clang++-22
./build.sh --prefix /tmp/dt --build-type RelWithDebInfo
```

## Development Tools Included

- **Compilers:** GCC 16, Clang 22
- **Build Systems:** CMake, Ninja
- **Libraries:** GTK3, GLib, SQLite, libcurl, Exiv2, libavif, libheif, libjxl, WebP, ONNX Runtime, and more
- **VS Code Extensions:** C/C++ tools, CMake Tools, clang-format
- **Editors:** vim, nano

## Troubleshooting

### Build fails with missing dependencies

```bash
# Rebuild the container
# F1 → "Dev Containers: Rebuild Container"
```

### Git submodules not initialized

```bash
git submodule init && git submodule update
```

### AppImage build fails with FUSE error

Always use the environment variable:
```bash
APPIMAGE_EXTRACT_AND_RUN=1 ./tools/appimage-build-script.sh
```

### Need to install additional packages

```bash
sudo apt-get update
sudo apt-get install <package-name>
```

### Want to run unit tests?

```bash
# Build with tests enabled (libcmocka-dev is already installed)
./build.sh --prefix /tmp/dt --build-type RelWithDebInfo -- -DBUILD_TESTING=ON

# Run tests
cd build && ctest
```

## File Structure

```
.devcontainer/
├── devcontainer.json    # Container configuration
├── Dockerfile           # Environment setup with all build dependencies
└── README.md           # This file
```

## Build Options Reference

```bash
# Build types
--build-type Release          # Optimized, no debug info
--build-type Debug            # Debug symbols, no optimization
--build-type RelWithDebInfo   # Optimized + debug symbols (recommended)

# Parallel jobs
-j N                          # Use N parallel jobs (default: all CPUs)

# Installation
--prefix <path>               # Set installation prefix (not required for development)
--install                     # Actually install files (not recommended in container)
```

## Notes

- Workspace folder mounted at `/workspaces/darktable`
- Build artifacts go to `build/` directory
- Container runs as user `vscode` (UID 1000)
- Git submodules initialized automatically on container creation

## Resources

- [Darktable Build Instructions](https://github.com/darktable-org/darktable#building)
- [Developer's Guide](https://github.com/darktable-org/darktable/wiki/Developer's-guide)
- [User Manual](https://docs.darktable.org/)
