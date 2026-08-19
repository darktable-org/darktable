#!/usr/bin/env bash
# Build darktable-mobile as a native Linux desktop binary.
#
# Prerequisites:
#   Qt 6.5+ for Linux (gcc_64) — typically from the Qt online installer
#   Go 1.21+ — only needed if you want a bundled daemon binary
#
# Usage:
#   ./build-desktop.sh [--release]
#
# The resulting binary is:
#   build-desktop/darktable-mobile
#
# On the desktop the app attaches to the darktable daemon that darktable itself
# already runs (at $XDG_RUNTIME_DIR/darktable-p2p.sock), so no bundled daemon
# is required.  If that socket does not exist the app will try to start a
# system-installed dt-p2p-daemon binary.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# ── configuration ──────────────────────────────────────────────────────────────
# Default to the Qt installer path; override with QT_DESKTOP_ROOT.
: "${QT_DESKTOP_ROOT:=$HOME/Qt/6.7.3/gcc_64}"

if [[ ! -f "$QT_DESKTOP_ROOT/bin/qt-cmake" ]]; then
    echo "ERROR: qt-cmake not found at $QT_DESKTOP_ROOT/bin/qt-cmake"
    echo "Set QT_DESKTOP_ROOT to your Qt gcc_64 kit, e.g.:"
    echo "  QT_DESKTOP_ROOT=~/Qt/6.7.3/gcc_64 ./build-desktop.sh"
    exit 1
fi

BUILD_TYPE="Debug"
if [[ "${1:-}" == "--release" ]]; then
    BUILD_TYPE="Release"
fi

BUILD_DIR="$SCRIPT_DIR/build-desktop"

# ── configure ─────────────────────────────────────────────────────────────────
echo "==> Configuring for Linux desktop (Qt at $QT_DESKTOP_ROOT)…"
mkdir -p "$BUILD_DIR"

"$QT_DESKTOP_ROOT/bin/qt-cmake" \
    -S "$SCRIPT_DIR" \
    -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"

# ── build ─────────────────────────────────────────────────────────────────────
echo "==> Building…"
cmake --build "$BUILD_DIR" --parallel "$(nproc)"

BINARY="$BUILD_DIR/darktable-mobile"
echo ""
echo "Binary ready: $BINARY"
echo ""
echo "To run (with darktable already running on the desktop):"
echo "  $BINARY"
