#!/usr/bin/env bash
# Build darktable-mobile as an Android APK.
#
# Prerequisites:
#   Qt 6.5+  for Android (arm64-v8a) installed, e.g. via Qt online installer
#   Android NDK r25c+
#   Android SDK (build-tools 34)
#   Go 1.21+  (for cross-compiling dt-p2p-daemon)
#
# Usage:
#   ./build-android.sh [--release]
#
# The script:
#   1. Cross-compiles dt-p2p-daemon for android/arm64
#   2. Runs qt-cmake to configure the project
#   3. Builds and packages the APK via androiddeployqt
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# ── configuration ──────────────────────────────────────────────────────────────
: "${QT_ANDROID_ROOT:?Set QT_ANDROID_ROOT to Qt's Android arm64-v8a kit, e.g. ~/Qt/6.7.0/android_arm64_v8a}"
: "${ANDROID_NDK:?Set ANDROID_NDK to your NDK root, e.g. ~/Android/Sdk/ndk/25.2.9519653}"
: "${ANDROID_SDK:?Set ANDROID_SDK to your SDK root, e.g. ~/Android/Sdk}"

BUILD_TYPE="Debug"
if [[ "${1:-}" == "--release" ]]; then
    BUILD_TYPE="Release"
fi

BUILD_DIR="$SCRIPT_DIR/build-android"
ASSETS_DIR="$SCRIPT_DIR/assets"
DAEMON_SRC="$REPO_ROOT/src/p2p"
DAEMON_OUT="$ASSETS_DIR/dt-p2p-daemon"

# ── 1. cross-compile the Go daemon for Android arm64 ──────────────────────────
echo "==> Building dt-p2p-daemon for android/arm64…"
mkdir -p "$ASSETS_DIR"

GOOS=android GOARCH=arm64 CGO_ENABLED=0 \
    go build -o "$DAEMON_OUT" "$DAEMON_SRC"

echo "    daemon → $DAEMON_OUT ($(du -sh "$DAEMON_OUT" | cut -f1))"

# ── 2. configure ──────────────────────────────────────────────────────────────
echo "==> Configuring with qt-cmake…"
mkdir -p "$BUILD_DIR"

# Use realpath so no tilde reaches CMake (tilde in CMake cache values is not
# expanded by /bin/sh and causes androiddeployqt to fail with "not found").
QT_ANDROID_ROOT="$(realpath "$QT_ANDROID_ROOT")"
QT_HOST_ROOT="$(dirname "$QT_ANDROID_ROOT")/gcc_64"
ANDROID_NDK="$(realpath "$ANDROID_NDK")"
ANDROID_SDK="$(realpath "$ANDROID_SDK")"

export ANDROID_SDK_ROOT="$ANDROID_SDK"

"$QT_ANDROID_ROOT/bin/qt-cmake" \
    -S "$SCRIPT_DIR" \
    -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-26 \
    -DANDROID_SDK_ROOT="$ANDROID_SDK" \
    -DANDROID_NDK_ROOT="$ANDROID_NDK" \
    -DQT_HOST_PATH="$QT_HOST_ROOT" \
    -DQT_HOST_PATH_CMAKE_DIR="$QT_HOST_ROOT/lib/cmake"

# ── 3. build & package APK ────────────────────────────────────────────────────
echo "==> Building and packaging APK…"
# AGP 8.x dropped android.bundle.enableUncompressedNativeLibs; strip it from
# the generated gradle.properties so Gradle doesn't reject the build.
cmake --build "$BUILD_DIR" --parallel "$(nproc)" 2>&1 | grep -v "enableUncompressedNativeLibs" || true
sed -i '/enableUncompressedNativeLibs/d' \
    "$BUILD_DIR/android-build/gradle.properties" 2>/dev/null || true
cmake --build "$BUILD_DIR" --target apk

APK_PATH=$(find "$BUILD_DIR" -name "*.apk" | head -1)
echo ""
echo "APK ready: $APK_PATH"
echo ""
echo "To install on a connected device:"
echo "  adb install -r \"$APK_PATH\""
