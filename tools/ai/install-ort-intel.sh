#!/bin/bash
#
# Install ONNX Runtime with OpenVINO ExecutionProvider for darktable AI
# acceleration on Intel GPUs (and CPUs).
#
# The onnxruntime-openvino pip wheel bundles OpenVINO runtime libraries,
# so no separate OpenVINO installation is needed. This script extracts
# the native shared libraries from the wheel - no Python required at runtime.
#
# Requirements:
#   - Intel GPU (HD/UHD/Iris/Arc) or CPU
#   - Level Zero runtime for GPU acceleration (optional, CPU works without)
#
# Usage: install-ort-intel.sh [-y|--yes] [install-dir]

set -euo pipefail

YES=false
while [ $# -gt 0 ]; do
  case "$1" in
    -y|--yes) YES=true; shift ;;
    *) break ;;
  esac
done

ORT_OPENVINO_VERSION="1.24.1"
INSTALL_DIR="${1:-$HOME/.local/lib/onnxruntime-openvino}"

# --- Platform checks ---
if [ "$(uname -s)" != "Linux" ]; then
  echo "Error: this script is for Linux only." >&2
  exit 1
fi

ARCH=$(uname -m)
if [ "$ARCH" != "x86_64" ]; then
  echo "Error: OpenVINO EP is only available for x86_64 (got $ARCH)." >&2
  exit 1
fi

# --- Info & confirmation ---
echo ""
echo "ONNX Runtime ${ORT_OPENVINO_VERSION} - OpenVINO ExecutionProvider installer"
echo "========================================================================="
echo ""
echo "This will download and install ONNX Runtime with Intel OpenVINO support"
echo "to enable GPU/CPU acceleration for darktable AI features"
echo "(denoise, upscale, segmentation)."
echo ""
echo "OpenVINO runtime libraries are bundled - no separate install needed."
echo ""
echo "Requirements:"
echo "  - Intel GPU (HD/UHD/Iris Xe/Arc) or any x86_64 CPU"
echo "  - OpenCL runtime for GPU acceleration (optional, CPU works without)"
echo ""
echo "Actions:"
echo "  - Download onnxruntime-openvino wheel from PyPI (~60 MB)"
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

# --- Check Intel GPU compute runtime (optional, for GPU) ---
if ! ldconfig -p 2>/dev/null | grep -q libze_loader; then
  echo "Note: Intel Level Zero runtime not found. OpenVINO will use CPU only."
  echo "      For Intel GPU acceleration, install the compute runtime:"
  echo ""
  distro_hint \
    "sudo apt install intel-opencl-icd level-zero" \
    "sudo dnf install intel-opencl level-zero" \
    "sudo pacman -S intel-compute-runtime level-zero-loader" \
    "sudo zypper install intel-opencl level-zero" \
    "https://github.com/intel/compute-runtime/releases"
  echo ""
fi

# --- Determine wheel URL ---
# PyPI wheel naming: onnxruntime_openvino-VERSION-cpXX-cpXX-manylinux_2_28_x86_64.whl
# We pick cp312 as a common target - the native .so files are the same for all Python versions.
WHEEL_NAME="onnxruntime_openvino-${ORT_OPENVINO_VERSION}-cp312-cp312-manylinux_2_28_x86_64.whl"
WHEEL_URL="https://files.pythonhosted.org/packages/cp312/${WHEEL_NAME}"

# PyPI doesn't have stable direct URLs - use pip download to get the wheel.
if ! command -v pip3 &>/dev/null && ! command -v pip &>/dev/null; then
  echo "Error: pip is required to download the wheel."
  echo ""
  distro_hint \
    "sudo apt install python3-pip" \
    "sudo dnf install python3-pip" \
    "sudo pacman -S python-pip" \
    "sudo zypper install python3-pip" \
    ""
  echo ""
  exit 1
fi

PIP=$(command -v pip3 || command -v pip)

# --- Download ---
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

echo "Downloading onnxruntime-openvino ${ORT_OPENVINO_VERSION}..."
$PIP download \
  --no-deps \
  --only-binary=:all: \
  --platform manylinux_2_28_x86_64 \
  --python-version 312 \
  --dest "$TMPDIR" \
  "onnxruntime-openvino==${ORT_OPENVINO_VERSION}" 2>&1 | tail -3

WHEEL=$(ls "$TMPDIR"/*.whl 2>/dev/null | head -1)
if [ -z "$WHEEL" ]; then
  echo "Error: failed to download wheel." >&2
  exit 1
fi

# --- Extract native libraries ---
echo "Extracting shared libraries..."
unzip -q -o "$WHEEL" -d "$TMPDIR/wheel"

mkdir -p "$INSTALL_DIR"

# Copy ORT core library
cp "$TMPDIR/wheel/onnxruntime/capi/"libonnxruntime.so* "$INSTALL_DIR/" 2>/dev/null || true
# Copy OpenVINO provider library
cp "$TMPDIR/wheel/onnxruntime/capi/"libonnxruntime_providers_openvino.so "$INSTALL_DIR/" 2>/dev/null || true
# Copy bundled OpenVINO runtime libraries
cp "$TMPDIR/wheel/onnxruntime/capi/"libopenvino*.so* "$INSTALL_DIR/" 2>/dev/null || true
# Copy any other dependency .so files
cp "$TMPDIR/wheel/onnxruntime/capi/"libtbb*.so* "$INSTALL_DIR/" 2>/dev/null || true

ORT_SO=$(ls "$INSTALL_DIR/libonnxruntime.so."* 2>/dev/null | head -1)
if [ -z "$ORT_SO" ]; then
  # Some wheels use plain libonnxruntime.so without version suffix
  ORT_SO=$(ls "$INSTALL_DIR/libonnxruntime.so" 2>/dev/null | head -1)
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
