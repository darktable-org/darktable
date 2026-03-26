#!/bin/bash
#
# Install ONNX Runtime with CUDA ExecutionProvider for darktable AI acceleration.
#
# Requirements:
#   - NVIDIA GPU with CUDA compute capability 6.0+
#   - CUDA 12.x runtime (driver 525+)
#   - cuDNN 9.x
#
# The prebuilt ORT GPU package from GitHub includes the CUDA and cuDNN
# execution provider libraries. darktable probes for CUDA EP at runtime
# via dlsym - no rebuild required.
#
# Usage: install-ort-nvidia.sh [-y|--yes] [install-dir]

set -euo pipefail

YES=false
while [ $# -gt 0 ]; do
  case "$1" in
    -y|--yes) YES=true; shift ;;
    *) break ;;
  esac
done

ORT_VERSION="1.24.4"
INSTALL_DIR="${1:-$HOME/.local/lib/onnxruntime-cuda}"
PACKAGE="onnxruntime-linux-x64-gpu-${ORT_VERSION}"
URL="https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/${PACKAGE}.tgz"

# --- Platform checks (before user prompt) ---
if [ "$(uname -s)" != "Linux" ]; then
  echo "Error: this script is for Linux only." >&2
  exit 1
fi

if [ "$(uname -m)" != "x86_64" ]; then
  echo "Error: CUDA EP is only available for x86_64 (got $(uname -m))." >&2
  exit 1
fi

# --- Info & confirmation ---
echo ""
echo "ONNX Runtime ${ORT_VERSION} - CUDA ExecutionProvider installer"
echo "================================================================"
echo ""
echo "This will download and install a GPU-accelerated ONNX Runtime build"
echo "to enable NVIDIA CUDA acceleration for darktable AI features"
echo "(denoise, upscale, segmentation)."
echo ""
echo "Requirements:"
echo "  - NVIDIA GPU with compute capability 6.0+ (Pascal or newer)"
echo "  - NVIDIA driver 525+ with CUDA 12.x support"
echo "  - cuDNN 9.x (install via your distro packages or NVIDIA repos)"
echo ""
echo "Actions:"
echo "  - Download prebuilt package from GitHub (~200 MB)"
echo "    $URL"
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

# --- Check NVIDIA driver ---
if ! command -v nvidia-smi &>/dev/null; then
  echo "Warning: nvidia-smi not found - NVIDIA driver may not be installed."
  echo ""
  distro_hint \
    "sudo apt install nvidia-driver-550" \
    "sudo dnf install akmod-nvidia  (RPM Fusion required: https://rpmfusion.org/Configuration)" \
    "sudo pacman -S nvidia" \
    "sudo zypper install nvidia-driver-G06" \
    "https://www.nvidia.com/drivers"
  echo ""
  echo "  A reboot is typically required after driver installation."
  echo "  Re-run this script afterwards."
  echo ""
  exit 1
fi

DRIVER_VERSION=$(nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>/dev/null | head -1)
echo "NVIDIA driver: $DRIVER_VERSION"

# --- Check CUDA toolkit ---
if command -v nvcc &>/dev/null; then
  CUDA_VERSION=$(nvcc --version | grep -oP 'release \K[0-9]+\.[0-9]+')
  echo "CUDA toolkit: $CUDA_VERSION"
else
  echo ""
  echo "Note: CUDA toolkit not found. ORT bundles its own CUDA runtime libraries,"
  echo "      but installing the toolkit ensures full compatibility."
  echo ""
  distro_hint \
    "sudo apt install nvidia-cuda-toolkit" \
    "sudo dnf install cuda-toolkit  (NVIDIA repo: https://developer.nvidia.com/cuda-downloads)" \
    "sudo pacman -S cuda" \
    "sudo zypper install cuda-toolkit  (NVIDIA repo: https://developer.nvidia.com/cuda-downloads)" \
    "https://developer.nvidia.com/cuda-downloads"
  echo ""
fi

# --- Check cuDNN ---
CUDNN_FOUND=false
if ldconfig -p 2>/dev/null | grep -q libcudnn; then
  CUDNN_FOUND=true
  CUDNN_LIB=$(ldconfig -p 2>/dev/null | grep 'libcudnn.so' | head -1 | awk '{print $NF}')
  echo "cuDNN: $CUDNN_LIB"
fi

if [ "$CUDNN_FOUND" = false ]; then
  echo ""
  echo "Warning: cuDNN not found. CUDA EP requires cuDNN 9.x to be installed."
  echo ""
  distro_hint \
    "sudo apt install libcudnn9-cuda-12" \
    "sudo dnf install libcudnn9-cuda-12" \
    "sudo pacman -S cudnn" \
    "sudo zypper install libcudnn9-cuda-12" \
    "https://developer.nvidia.com/cudnn-downloads"
  echo ""
  echo "  If the package is not found, add the NVIDIA repo first:"
  echo "    https://developer.nvidia.com/cudnn-downloads"
  echo ""
  echo "  You can install cuDNN after this script finishes - darktable will"
  echo "  detect it at startup."
  echo ""
fi

# --- Download ---
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

echo "Downloading ORT ${ORT_VERSION} with CUDA EP..."
if command -v wget &>/dev/null; then
  wget -q --show-progress -O "$TMPDIR/ort-gpu.tgz" "$URL"
elif command -v curl &>/dev/null; then
  curl -fL --progress-bar -o "$TMPDIR/ort-gpu.tgz" "$URL"
else
  echo "Error: neither wget nor curl found." >&2
  exit 1
fi

# --- Install ---
tar xzf "$TMPDIR/ort-gpu.tgz" -C "$TMPDIR"

mkdir -p "$INSTALL_DIR"
cp "$TMPDIR/${PACKAGE}/lib/"*.so* "$INSTALL_DIR/"

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
