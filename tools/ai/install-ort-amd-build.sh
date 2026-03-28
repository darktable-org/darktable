#!/bin/bash
#
# Build and install ONNX Runtime with MIGraphX ExecutionProvider
# for darktable AI acceleration on AMD GPUs.
#
# Unlike NVIDIA (which has pre-built packages), MIGraphX EP must be
# built from source to match the installed ROCm version.
#
# Requirements:
#   - AMD GPU supported by ROCm (RDNA2+, CDNA+)
#   - ROCm 6.x+ with MIGraphX installed
#   - Build tools: cmake 3.26+, gcc/g++, python3
#
# Usage: install-ort-amd-build.sh [-y|--yes] [install-dir]

set -euo pipefail

YES=false
while [ $# -gt 0 ]; do
  case "$1" in
    -y|--yes) YES=true; shift ;;
    *) break ;;
  esac
done

INSTALL_DIR="${1:-$HOME/.local/lib/onnxruntime-migraphx}"
ROCM_HOME="${ROCM_HOME:-/opt/rocm}"
BUILD_DIR="${TMPDIR:-/tmp}/ort-migraphx-build"

# --- Platform checks (before user prompt) ---
if [ "$(uname -s)" != "Linux" ]; then
  echo "Error: this script is for Linux only." >&2
  exit 1
fi

if [ "$(uname -m)" != "x86_64" ]; then
  echo "Error: MIGraphX EP is only available for x86_64 (got $(uname -m))." >&2
  exit 1
fi

# --- Info & confirmation ---
echo ""
echo "ONNX Runtime - MIGraphX ExecutionProvider builder"
echo "==================================================="
echo ""
echo "This will build ONNX Runtime from source with AMD MIGraphX support"
echo "to enable GPU acceleration for darktable AI features"
echo "(denoise, upscale, segmentation)."
echo ""
echo "Unlike NVIDIA, there is no pre-built package - ORT must be compiled"
echo "against the ROCm version installed on your system."
echo ""
echo "Requirements:"
echo "  - AMD GPU supported by ROCm (Radeon RX 6000+, Instinct MI100+)"
echo "  - ROCm 6.x+ with MIGraphX"
echo "  - cmake 3.26+, gcc/g++, python3, git"
echo ""
echo "Actions:"
echo "  - Clone ORT source (~300 MB)"
echo "  - Build with MIGraphX EP (10-20 min depending on hardware)"
echo "  - Install shared libraries to: $INSTALL_DIR"
echo ""

if [ "$YES" = false ]; then
  read -rp "Continue? [y/N] " answer
  if [[ ! "$answer" =~ ^[Yy] ]]; then
    echo "Aborted."
    exit 0
  fi
  echo ""
fi

# --- Helper: distro-specific install hint ---
distro_hint() {
  local pkg_deb="$1" pkg_rpm="$2" pkg_arch="$3" pkg_suse="$4" fallback_url="$5"
  if [ -f /etc/os-release ]; then
    . /etc/os-release
    case "$ID" in
      ubuntu|debian|linuxmint|pop)
        echo "  Install on $NAME:"
        echo "    $pkg_deb"
        ;;
      fedora|rhel|centos|rocky|alma)
        echo "  Install on $NAME:"
        echo "    $pkg_rpm"
        ;;
      arch|manjaro|endeavouros)
        echo "  Install on $NAME:"
        echo "    $pkg_arch"
        ;;
      opensuse*|sles)
        echo "  Install on $NAME:"
        echo "    $pkg_suse"
        ;;
      *)
        echo "  Download from: $fallback_url"
        return
        ;;
    esac
  else
    echo "  Download from: $fallback_url"
  fi
}

# --- Check ROCm ---
if ! command -v rocminfo &>/dev/null || [ ! -d "$ROCM_HOME" ]; then
  echo "Error: ROCm not found at $ROCM_HOME"
  echo ""
  distro_hint \
    "sudo apt install rocm  (add AMD repo first: https://rocm.docs.amd.com/projects/install-on-linux/en/latest/)" \
    "sudo dnf install rocm  (add AMD repo first: https://rocm.docs.amd.com/projects/install-on-linux/en/latest/)" \
    "sudo pacman -S rocm-hip-sdk" \
    "sudo zypper install rocm  (add AMD repo first: https://rocm.docs.amd.com/projects/install-on-linux/en/latest/)" \
    "https://rocm.docs.amd.com/projects/install-on-linux/en/latest/"
  echo ""
  exit 1
fi

ROCM_VERSION="unknown"
if [ -f "$ROCM_HOME/.info/version" ]; then
  ROCM_VERSION=$(cat "$ROCM_HOME/.info/version")
fi
echo "ROCm: $ROCM_VERSION ($ROCM_HOME)"

# --- Select ORT version matching ROCm ---
ROCM_MAJOR_MINOR=$(echo "$ROCM_VERSION" | grep -oP '^\d+\.\d+')
case "$ROCM_MAJOR_MINOR" in
  # 7.3+) ORT_VERSION="1.24.4" ;;  # TODO: confirm when docs are updated
  7.2*) ORT_VERSION="1.23.2" ;;
  7.1*) ORT_VERSION="1.23.1" ;;
  7.0*) ORT_VERSION="1.22.1" ;;
  6.4*) ORT_VERSION="1.21.0" ;;
  6.3*) ORT_VERSION="1.19.0" ;;
  6.2*) ORT_VERSION="1.18.0" ;;
  6.1*) ORT_VERSION="1.17.0" ;;
  6.0*) ORT_VERSION="1.16.0" ;;
  *)
    echo ""
    echo "Error: unsupported ROCm version $ROCM_VERSION"
    echo "  Supported: ROCm 6.0 - 7.2"
    echo "  Update ROCm or set ORT_VERSION manually and re-run."
    echo ""
    exit 1
    ;;
esac
echo "ORT version: $ORT_VERSION (matched to ROCm $ROCM_MAJOR_MINOR)"

# --- Check MIGraphX ---
if ! command -v migraphx-driver &>/dev/null \
   && [ ! -f "$ROCM_HOME/lib/libmigraphx.so" ] \
   && [ ! -f "$ROCM_HOME/lib64/libmigraphx.so" ]; then
  echo ""
  echo "Error: MIGraphX not found in $ROCM_HOME"
  echo ""
  distro_hint \
    "sudo apt install migraphx migraphx-dev" \
    "sudo dnf install migraphx migraphx-devel" \
    "sudo pacman -S migraphx" \
    "sudo zypper install migraphx migraphx-devel" \
    "https://rocm.docs.amd.com/projects/install-on-linux/en/latest/"
  echo ""
  exit 1
fi
echo "MIGraphX: found"

# --- Check build tools ---
MISSING=""
command -v cmake &>/dev/null || MISSING="$MISSING cmake"
command -v g++ &>/dev/null   || MISSING="$MISSING g++"
command -v git &>/dev/null   || MISSING="$MISSING git"
command -v python3 &>/dev/null || MISSING="$MISSING python3"

if [ -n "$MISSING" ]; then
  echo ""
  echo "Error: missing build tools:$MISSING"
  echo ""
  distro_hint \
    "sudo apt install$MISSING" \
    "sudo dnf install$MISSING" \
    "sudo pacman -S$MISSING" \
    "sudo zypper install$MISSING" \
    ""
  echo ""
  exit 1
fi

CMAKE_VERSION=$(cmake --version | head -1 | grep -oP '[0-9]+\.[0-9]+')
echo "cmake: $CMAKE_VERSION"

# --- Clone & build ---
echo ""
echo "Cloning ONNX Runtime v${ORT_VERSION}..."

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

git clone --depth 1 --branch "v${ORT_VERSION}" \
  https://github.com/microsoft/onnxruntime.git "$BUILD_DIR/onnxruntime"

cd "$BUILD_DIR/onnxruntime"

# Patch Eigen hash mismatch - GitLab regenerates zip archives, breaking the
# hardcoded SHA1 in older ORT releases. Remove the URL_HASH line from
# eigen.cmake so FetchContent downloads without verification.
if [ -f "cmake/external/eigen.cmake" ]; then
  sed -i '/URL_HASH/d' cmake/external/eigen.cmake
  echo "Patched Eigen: removed URL_HASH check (GitLab zip archive mismatch)"
fi

echo ""
echo "Building with MIGraphX EP (this will take 30-60 minutes)..."
echo ""

./build.sh \
  --config Release \
  --build_shared_lib \
  --parallel \
  --skip_tests \
  --use_migraphx \
  --migraphx_home "$ROCM_HOME"

# --- Install ---
BUILD_LIB_DIR="$BUILD_DIR/onnxruntime/build/Linux/Release"

mkdir -p "$INSTALL_DIR"
cp "$BUILD_LIB_DIR/"libonnxruntime*.so* "$INSTALL_DIR/"

# Clean up build tree (~2 GB)
rm -rf "$BUILD_DIR"

ORT_SO=$(ls "$INSTALL_DIR/libonnxruntime.so."* 2>/dev/null | head -1)

echo ""
echo "Done. Installed to: $INSTALL_DIR"
ls -lh "$INSTALL_DIR/"*.so* 2>/dev/null
echo ""
echo "To use with darktable:"
echo ""
echo "  DT_ORT_LIBRARY=$ORT_SO darktable"
echo ""
echo "Or add to ~/.bashrc:"
echo ""
echo "  export DT_ORT_LIBRARY=$ORT_SO"
