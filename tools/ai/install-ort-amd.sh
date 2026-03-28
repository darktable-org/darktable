#!/bin/bash
#
# Install ONNX Runtime with MIGraphX ExecutionProvider for darktable
# AI acceleration on AMD GPUs.
#
# This extracts native .so files from the onnxruntime_rocm wheel hosted
# on AMD's repo. Much faster than building from source (~30 sec vs
# 10-20 min) but requires the wheel's ROCm version to match your system.
#
# If this doesn't work, use install-ort-amd-build.sh to build from source.
#
# Requirements:
#   - AMD GPU supported by ROCm (RDNA2+, CDNA+)
#   - ROCm 6.x+ with MIGraphX installed
#   - wget or curl
#
# Usage: install-ort-amd.sh [-y|--yes] [install-dir]

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

# --- Platform checks ---
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
echo "ONNX Runtime - MIGraphX ExecutionProvider installer (prebuilt)"
echo "==============================================================="
echo ""
echo "This will download a prebuilt ONNX Runtime with AMD MIGraphX support"
echo "from AMD's package repository. Much faster than building from source."
echo ""
echo "Note: the prebuilt wheel is compiled against a specific ROCm version."
echo "      If darktable fails to detect MIGraphX at runtime, use"
echo "      install-ort-amd-build.sh to build from source instead."
echo ""
echo "Requirements:"
echo "  - AMD GPU supported by ROCm (Radeon RX 6000+, Instinct MI100+)"
echo "  - ROCm 6.x+ with MIGraphX"
echo "  - wget or curl"
echo ""
echo "Actions:"
echo "  - Download onnxruntime-migraphx wheel from AMD repo (~60 MB)"
echo "  - Extract native shared libraries to: $INSTALL_DIR"
echo ""

if [ "$YES" = false ]; then
  read -rp "Continue? [y/N] " answer
  if [[ ! "$answer" =~ ^[Yy] ]]; then
    echo "Aborted."
    exit 0
  fi
  echo ""
fi

# --- Check ROCm ---
if ! command -v rocminfo &>/dev/null || [ ! -d "$ROCM_HOME" ]; then
  echo "Error: ROCm not found. Install ROCm first:"
  echo "  https://rocm.docs.amd.com/projects/install-on-linux/en/latest/"
  echo ""
  exit 1
fi

ROCM_VERSION="unknown"
if [ -f "$ROCM_HOME/.info/version" ]; then
  ROCM_VERSION=$(cat "$ROCM_HOME/.info/version")
fi
echo "ROCm: $ROCM_VERSION ($ROCM_HOME)"

# --- Select wheel repo matching ROCm ---
ROCM_MAJOR_MINOR=$(echo "$ROCM_VERSION" | grep -oP '^\d+\.\d+')
case "$ROCM_MAJOR_MINOR" in
  7.2*) ROCM_REPO="rocm-rel-7.2"; ORT_VERSION="1.23.2" ;;
  7.1*) ROCM_REPO="rocm-rel-7.1"; ORT_VERSION="1.23.1" ;;
  7.0*) ROCM_REPO="rocm-rel-7.0"; ORT_VERSION="1.22.1" ;;
  6.4*) ROCM_REPO="rocm-rel-6.4"; ORT_VERSION="1.21.0" ;;
  6.3*) ROCM_REPO="rocm-rel-6.3"; ORT_VERSION="1.19.0" ;;
  6.2*) ROCM_REPO="rocm-rel-6.2"; ORT_VERSION="1.18.0" ;;
  6.1*) ROCM_REPO="rocm-rel-6.1"; ORT_VERSION="1.17.0" ;;
  6.0*) ROCM_REPO="rocm-rel-6.0"; ORT_VERSION="1.16.0" ;;
  *)
    echo "Error: unsupported ROCm version $ROCM_VERSION"
    echo "  Try install-ort-amd-build.sh to build from source instead."
    echo ""
    exit 1
    ;;
esac
REPO_URL="https://repo.radeon.com/rocm/manylinux/$ROCM_REPO/"
echo "AMD repo: $REPO_URL"


# --- Download ---
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

# AMD publishes wheels as onnxruntime_rocm (not onnxruntime-migraphx) with
# platform tag linux_x86_64 (not manylinux). Download directly via URL.
WHEEL_NAME="onnxruntime_rocm-${ORT_VERSION}-cp312-cp312-linux_x86_64.whl"
WHEEL_URL="${REPO_URL}${WHEEL_NAME}"
WHEEL="$TMPDIR/$WHEEL_NAME"

echo "Downloading $WHEEL_NAME..."
if command -v wget &>/dev/null; then
  wget -q --show-progress -O "$WHEEL" "$WHEEL_URL"
elif command -v curl &>/dev/null; then
  curl -fL --progress-bar -o "$WHEEL" "$WHEEL_URL"
else
  echo "Error: neither wget nor curl found." >&2
  exit 1
fi

if [ ! -s "$WHEEL" ]; then
  echo "Error: failed to download from $WHEEL_URL"
  echo "  Try install-ort-amd-build.sh to build from source instead."
  echo ""
  exit 1
fi

echo "Wheel: $(basename "$WHEEL")"

# --- Extract native libraries ---
echo "Extracting shared libraries..."
unzip -q -o "$WHEEL" -d "$TMPDIR/wheel"

mkdir -p "$INSTALL_DIR"

# Copy ORT core library
cp "$TMPDIR/wheel/onnxruntime/capi/"libonnxruntime.so* "$INSTALL_DIR/" 2>/dev/null || true
# Copy all provider libraries (MIGraphX, ROCm, shared)
cp "$TMPDIR/wheel/onnxruntime/capi/"libonnxruntime_providers_*.so "$INSTALL_DIR/" 2>/dev/null || true

ORT_SO=$(ls "$INSTALL_DIR/libonnxruntime.so."* 2>/dev/null | head -1)
if [ -z "$ORT_SO" ]; then
  ORT_SO=$(ls "$INSTALL_DIR/libonnxruntime.so" 2>/dev/null | head -1)
fi

if [ -z "$ORT_SO" ]; then
  echo "Error: libonnxruntime.so not found in wheel."
  echo "  Try install-ort-amd-build.sh to build from source instead."
  exit 1
fi

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
echo ""
echo "If MIGraphX is not detected at runtime, build from source instead:"
echo "  ./tools/ai/install-ort-amd-build.sh"
