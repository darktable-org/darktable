#!/bin/bash
#
# Install GPU-accelerated ONNX Runtime for darktable.

set -euo pipefail

usage() {
  cat <<EOF
NAME
    install-ort-gpu.sh – install GPU-accelerated ONNX Runtime for darktable

SYNOPSIS
    install-ort-gpu.sh [-y|--yes] [-f|--force]
                       [--vendor <nvidia|amd|intel>]
                       [--manifest <path>]
                       [-h|--help]

DESCRIPTION
    Reads the package manifest (data/ort_gpu.json) to determine the
    correct download URL for the detected GPU, downloads the ORT
    package, and installs the libraries to ~/.local/lib/onnxruntime-<vendor>/.

    Supported vendors on Linux:
      - NVIDIA via CUDA EP        (requires CUDA 12.x or 13.x + cuDNN 9.x)
      - AMD    via MIGraphX/ROCm  (requires ROCm 7.x with MIGraphX)
      - Intel  via OpenVINO       (requires Intel GPU driver with OpenCL)

    GPU auto-detection uses:
      - nvidia-smi             (NVIDIA)
      - /opt/rocm/.info/version (AMD)
      - lspci                  (Intel)

    After installing, point darktable at the new library:
      Preferences -> AI -> ONNX Runtime library -> "detect" or "browse"
    Restart darktable to apply.  Or set DT_ORT_LIBRARY=<path> in the
    environment.

OPTIONS
    -y, --yes
        Skip the interactive "Continue?" prompt.

    -f, --force
        Skip the vendor-specific dependency check (CUDA toolkit, cuDNN
        SO, ROCm install, OpenCL ICD).  The download proceeds regardless;
        if dependencies are missing at runtime, ORT will fall back to CPU.

    --vendor <nvidia|amd|intel>
        Force a specific GPU vendor and skip auto-detection.

    --manifest <path>
        Use a custom ort_gpu.json manifest.  Default search order:
            <script>/../../data/ort_gpu.json
            <script>/../../share/darktable/ort_gpu.json
            /usr/share/darktable/ort_gpu.json
            /usr/local/share/darktable/ort_gpu.json

    -h, --help
        Show this help and exit.

EXAMPLES
    install-ort-gpu.sh
        Detect the GPU, prompt for confirmation, install.

    install-ort-gpu.sh -y
        Same as above without confirmation.

    install-ort-gpu.sh --vendor amd -y
        Install the AMD/MIGraphX package without GPU detection.

    install-ort-gpu.sh --force -y
        Install without checking dependencies.
EOF
}

YES=false
FORCE=false
MANIFEST=""
VENDOR_OVERRIDE=""
while [ $# -gt 0 ]; do
  case "$1" in
    -y|--yes) YES=true; shift ;;
    -f|--force) FORCE=true; shift ;;
    --manifest) MANIFEST="$2"; shift 2 ;;
    --vendor) VENDOR_OVERRIDE="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) break ;;
  esac
done

# --- Locate manifest ---
if [ -z "$MANIFEST" ]; then
  # try relative to script, then installed location
  SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
  if [ -f "$SCRIPT_DIR/../../data/ort_gpu.json" ]; then
    MANIFEST="$SCRIPT_DIR/../../data/ort_gpu.json"
  elif [ -f "$SCRIPT_DIR/../../share/darktable/ort_gpu.json" ]; then
    MANIFEST="$SCRIPT_DIR/../../share/darktable/ort_gpu.json"
  elif [ -f "/usr/share/darktable/ort_gpu.json" ]; then
    MANIFEST="/usr/share/darktable/ort_gpu.json"
  elif [ -f "/usr/local/share/darktable/ort_gpu.json" ]; then
    MANIFEST="/usr/local/share/darktable/ort_gpu.json"
  else
    echo "Error: cannot find ort_gpu.json manifest." >&2
    echo "  Use --manifest <path> to specify it manually." >&2
    exit 1
  fi
fi

if [ ! -f "$MANIFEST" ]; then
  echo "Error: manifest not found: $MANIFEST" >&2
  exit 1
fi

# --- Platform checks ---
if [ "$(uname -s)" != "Linux" ]; then
  echo "Error: this script is for Linux only." >&2
  exit 1
fi

ARCH=$(uname -m)
PLATFORM="linux"

# --- Detect GPU (skipped with --vendor) ---
VENDOR=""
GPU_LABEL=""
DRIVER_VERSION=""
GPU_LABEL_AMD=""
DRIVER_VERSION_AMD=""
GPU_LABEL_INTEL=""
ROCM_MM=""
CUDA_MM=""

# Detect ROCm version using a distro-agnostic cascade. Each source is tried
# in order until one yields a usable X.Y[.Z] version string.
# Sets ROCM_VERSION and ROCM_MM if found.
detect_rocm_version() {
  ROCM_VERSION=""
  local v=""

  # Cascade of detection strategies, ordered from most reliable to fallback.
  # All strategies are distro-independent except where explicitly noted.
  local sources=(
    # 1. Canonical AMD installer version file (all distros, AMD repo install)
    'cat /opt/rocm/.info/version 2>/dev/null'
    # 2. Versioned install dir symlink target (e.g. /opt/rocm -> rocm-6.2.0)
    'readlink -f /opt/rocm 2>/dev/null | grep -oE "rocm-[0-9]+\.[0-9]+(\.[0-9]+)?" | sed "s/rocm-//"'
    # 3. Any /opt/rocm-X.Y[.Z] directory (side-by-side installs)
    'ls -d /opt/rocm-[0-9]*.[0-9]* 2>/dev/null | sed "s|.*/rocm-||" | sort -V | tail -1'
    # 4. hipconfig (works wherever HIP is on PATH)
    'command -v hipconfig >/dev/null && hipconfig --version 2>/dev/null'
    # 5. pkg-config (works for any distro that ships .pc files)
    'command -v pkg-config >/dev/null && pkg-config --modversion rocm-core 2>/dev/null'
    # 6. dpkg (Debian/Ubuntu)
    'command -v dpkg-query >/dev/null && dpkg-query -W -f="\${Version}" rocm-core 2>/dev/null'
    # 7. rpm (Fedora/RHEL/openSUSE)
    'command -v rpm >/dev/null && rpm -q --qf "%{VERSION}" rocm-core 2>/dev/null'
    'command -v rpm >/dev/null && rpm -q --qf "%{VERSION}" rocm-runtime 2>/dev/null'
    # 8. pacman (Arch)
    'command -v pacman >/dev/null && pacman -Q rocm-core 2>/dev/null | awk "{print \$2}"'
  )

  for cmd in "${sources[@]}"; do
    v=$(eval "$cmd" 2>/dev/null | tr -d '[:space:]' || true)
    # strip distro suffixes like "-1.fc41" or "-1ubuntu1"
    v="${v%%-*}"
    if [[ "$v" =~ ^[0-9]+\.[0-9]+ ]]; then
      ROCM_VERSION="$v"
      break
    fi
  done

  if [ -n "$ROCM_VERSION" ]; then
    ROCM_MM=$(echo "$ROCM_VERSION" | grep -oP '^\d+\.\d+')
  fi
}

# Detect installed CUDA toolkit version. Sets CUDA_MM (major.minor) if found.
# nvcc is often absent on non-developer systems; fall back to library/path probes.
detect_cuda_version() {
  CUDA_MM=""
  local v=""

  # nvcc — most authoritative when available
  if command -v nvcc &>/dev/null; then
    v=$(nvcc --version 2>/dev/null | grep -oP 'V\K\d+\.\d+' || true)
    [[ "$v" =~ ^[0-9]+\.[0-9]+ ]] && { CUDA_MM="$v"; return; }
  fi

  # version.json (CUDA 11.1+) or version.txt (older)
  v=$(grep -oP '"version"\s*:\s*"\K\d+\.\d+' /usr/local/cuda/version.json 2>/dev/null \
      || grep -oP 'CUDA Version \K\d+\.\d+' /usr/local/cuda/version.txt 2>/dev/null \
      || true)
  [[ "$v" =~ ^[0-9]+\.[0-9]+ ]] && { CUDA_MM="$v"; return; }

  # libcudart.so major version from ldconfig — works regardless of install method
  v=$(ldconfig -p 2>/dev/null | grep -oP 'libcudart\.so\.\K\d+' | sort -V | tail -1 || true)
  [[ "$v" =~ ^[0-9]+$ ]] && { CUDA_MM="${v}.0"; return; }
}

if [ -n "$VENDOR_OVERRIDE" ]; then
  case "$VENDOR_OVERRIDE" in
    nvidia|amd|intel) VENDOR="$VENDOR_OVERRIDE" ;;
    *)
      echo "Error: unknown vendor '$VENDOR_OVERRIDE'. Use: nvidia, amd, intel" >&2
      exit 1
      ;;
  esac
  echo "Vendor override: $VENDOR_OVERRIDE (skipping GPU detection)"
  # still collect version info for package matching
  if [ "$VENDOR_OVERRIDE" = "amd" ]; then
    detect_rocm_version
  fi
  if [ "$VENDOR_OVERRIDE" = "nvidia" ]; then
    detect_cuda_version
  fi
else
  # NVIDIA
  if command -v nvidia-smi &>/dev/null; then
    NVIDIA_INFO=$(nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null || true)
    if [ -n "$NVIDIA_INFO" ]; then
      # comma-join names across all detected GPUs; driver version is
      # identical per row, take from the first
      GPU_LABEL=$(echo "$NVIDIA_INFO" | awk -F, '{
          gsub(/^[[:space:]]+|[[:space:]]+$/, "", $1)
          printf "%s%s", (NR>1 ? ", " : ""), $1
        }')
      DRIVER_VERSION=$(echo "$NVIDIA_INFO" | head -1 | cut -d, -f2 | xargs)
      VENDOR="nvidia"
      detect_cuda_version
    fi
  fi

  # AMD
  detect_rocm_version
  if [ -n "$ROCM_VERSION" ] || command -v rocminfo &>/dev/null; then
    # rocminfo lists multiple agents (CPU, GPU, sometimes NPU on Ryzen AI);
    # use Device Type to pick the GPU. Marketing Name appears before Device
    # Type within each agent block, so we hold the most recent name and
    # print it when we hit a GPU agent.
    # `|| true` swallows the SIGPIPE-induced non-zero exit status the
    # pipeline gets under `pipefail` when awk's `exit` closes stdin
    # before rocminfo finishes writing — without it, $() would also
    # capture a fallback echo and concatenate two values into one var
    AMD_NAME=$(rocminfo 2>/dev/null | awk '
        /Marketing Name:/  { sub(/.*Marketing Name:[[:space:]]*/, ""); sub(/[[:space:]]+$/, ""); mn=$0 }
        /Device Type:.*GPU/{ if(out) out = out ", " mn; else out = mn }
        END                { print out }
      ' || true)
    [ -z "$AMD_NAME" ] && AMD_NAME="AMD GPU"
    if [ -n "$VENDOR" ]; then
      VENDOR="$VENDOR amd"
    else
      VENDOR="amd"
    fi
    GPU_LABEL_AMD="${AMD_NAME}"
    DRIVER_VERSION_AMD="ROCm ${ROCM_VERSION}"
  fi

  # Intel
  INTEL_GPU=$(lspci 2>/dev/null | grep -i 'VGA.*Intel\|Display.*Intel' | sed 's/.*: //' | awk '{
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", $0)
      printf "%s%s", (NR>1 ? ", " : ""), $0
    }' || true)
  if [ -n "$INTEL_GPU" ] || ldconfig -p 2>/dev/null | grep -q libze_loader; then
    if [ -n "$VENDOR" ]; then
      VENDOR="$VENDOR intel"
    else
      VENDOR="intel"
    fi
    GPU_LABEL_INTEL="${INTEL_GPU:-Intel GPU}"
  fi

  if [ -z "$VENDOR" ]; then
    echo ""
    echo "No supported GPU detected."
    echo ""
    echo "Supported: NVIDIA (CUDA), AMD (ROCm/MIGraphX), Intel (OpenVINO)"
    echo "Ensure GPU drivers are installed, or use --vendor <nvidia|amd|intel>."
    echo ""
    exit 1
  fi
fi

# --- Handle multiple vendors ---
VENDORS=($VENDOR)
SELECTED=""

if [ ${#VENDORS[@]} -eq 1 ]; then
  SELECTED="${VENDORS[0]}"
else
  echo ""
  echo "Multiple GPUs detected:"
  for i in "${!VENDORS[@]}"; do
    v="${VENDORS[$i]}"
    case "$v" in
      nvidia) echo "  $((i+1))) NVIDIA: $GPU_LABEL ($DRIVER_VERSION)" ;;
      amd)    echo "  $((i+1))) AMD: $GPU_LABEL_AMD ($DRIVER_VERSION_AMD)" ;;
      intel)  echo "  $((i+1))) Intel: $GPU_LABEL_INTEL" ;;
    esac
  done
  echo ""
  read -rp "Select GPU [1-${#VENDORS[@]}]: " choice
  idx=$((choice - 1))
  if [ "$idx" -lt 0 ] || [ "$idx" -ge ${#VENDORS[@]} ]; then
    echo "Invalid selection." >&2
    exit 1
  fi
  SELECTED="${VENDORS[$idx]}"
fi

# set labels for selected vendor (with fallbacks for --vendor override)
case "$SELECTED" in
  nvidia) SEL_LABEL="${GPU_LABEL:-NVIDIA GPU}"; SEL_DRIVER="$DRIVER_VERSION" ;;
  amd)    SEL_LABEL="${GPU_LABEL_AMD:-AMD GPU}"; SEL_DRIVER="${DRIVER_VERSION_AMD:-}" ;;
  intel)  SEL_LABEL="${GPU_LABEL_INTEL:-Intel GPU}"; SEL_DRIVER="" ;;
esac

# --- Find matching package in manifest ---
if ! command -v jq &>/dev/null; then
  echo "Error: jq is required to parse the manifest." >&2
  echo "  Install with: sudo apt install jq" >&2
  exit 1
fi

PKG_JSON=$(jq -c --arg v "$SELECTED" --arg p "$PLATFORM" --arg a "$ARCH" \
  --arg r "${ROCM_MM:-}" --arg c "${CUDA_MM:-}" \
  '[.packages[] | select(.vendor==$v and .platform==$p and (.arch // "x86_64")==$a) |
   if ($r != "" and .rocm_min != null) then select(.rocm_min <= $r and (.rocm_max // "99") >= $r) else . end |
   if ($c != "" and .cuda_min != null) then select(.cuda_min <= $c and (.cuda_max // "99") >= $c) else . end] | first' \
  "$MANIFEST" 2>/dev/null || true)

if [ -z "$PKG_JSON" ]; then
  echo ""
  echo "Error: no matching package found in manifest for $SELECTED/$PLATFORM/$ARCH"
  [ -n "${ROCM_MM:-}" ] && echo "  ROCm version: $ROCM_MM"
  [ -n "${CUDA_MM:-}" ] && echo "  CUDA version: $CUDA_MM"
  echo "  Check ort_gpu.json for supported configurations."
  echo ""
  exit 1
fi

# Warn when CUDA version could not be detected — package was selected
# without version filtering and may not match the installed toolkit.
if [ "$SELECTED" = "nvidia" ] && [ -z "${CUDA_MM:-}" ]; then
  echo "Warning: could not detect installed CUDA toolkit version."
  echo "  Proceeding with: Requirements: $(_field requirements '')"
  echo ""
fi

# Extract fields from JSON
_field() {
  echo "$PKG_JSON" | jq -r ".$1 // \"$2\""
}

PKG_URL=$(_field url "")
PKG_FORMAT=$(_field format "tgz")
PKG_LIB_PATTERN=$(_field lib_pattern "libonnxruntime")
# additional name prefixes to copy out of the same archive — used for
# wheels that bundle their own dependencies alongside ORT (e.g. the
# Linux onnxruntime-openvino wheel ships libopenvino*, libtbb*)
PKG_LIB_EXTRA_PATTERNS=$(echo "$PKG_JSON" | jq -r '.lib_extra_patterns // [] | .[]' 2>/dev/null || true)
# preserve_layout: TRUE for manylinux wheels with auditwheel-bundled deps
# whose providers .so RPATH is `$ORIGIN/../../<ep>.libs` (e.g. AMD MIGraphX).
# default flat extraction matches OpenVINO's $ORIGIN-only RPATH
PKG_PRESERVE_LAYOUT=$(echo "$PKG_JSON" | jq -r '.preserve_layout // false' 2>/dev/null || echo false)
PKG_INSTALL_SUBDIR=$(_field install_subdir "onnxruntime-gpu")
PKG_SIZE_MB=$(_field size_mb "0")
PKG_ORT_VERSION=$(_field ort_version "")
PKG_REQUIREMENTS=$(_field requirements "")
PKG_SHA256=$(_field sha256 "")

INSTALL_DIR="$HOME/.local/lib/$PKG_INSTALL_SUBDIR"

# --- Info & confirmation ---
echo ""
echo "ONNX Runtime $PKG_ORT_VERSION - GPU acceleration installer"
echo "============================================================"
echo ""
echo "GPU: $SEL_LABEL"
[ -n "$SEL_DRIVER" ] && echo "Driver: $SEL_DRIVER"
echo "ORT version: $PKG_ORT_VERSION"
echo "Download size: ~${PKG_SIZE_MB} MB"
echo "Install to: $INSTALL_DIR"
[ -n "$PKG_REQUIREMENTS" ] && echo "Requirements: $PKG_REQUIREMENTS"
echo ""

if [ "$YES" = false ]; then
  read -rp "Continue? [y/N] " answer
  if [[ ! "$answer" =~ ^[Yy] ]]; then
    echo "Aborted."
    exit 0
  fi
  echo ""
fi

# --- Download ---
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

ARCHIVE="$TMPDIR/ort-package"

echo "Downloading..."
if command -v wget &>/dev/null; then
  wget -q --show-progress -O "$ARCHIVE" "$PKG_URL"
elif command -v curl &>/dev/null; then
  curl -fL --progress-bar -o "$ARCHIVE" "$PKG_URL"
else
  echo "Error: neither wget nor curl found." >&2
  exit 1
fi

if [ ! -s "$ARCHIVE" ]; then
  echo "Error: download failed from $PKG_URL" >&2
  exit 1
fi

# --- Verify checksum ---
if [ -n "$PKG_SHA256" ]; then
  echo "Verifying checksum..."
  ACTUAL_SHA256=$(sha256sum "$ARCHIVE" | cut -d' ' -f1)
  if [ "$ACTUAL_SHA256" != "$PKG_SHA256" ]; then
    echo "Error: checksum mismatch!" >&2
    echo "  Expected: $PKG_SHA256" >&2
    echo "  Got:      $ACTUAL_SHA256" >&2
    exit 1
  fi
  echo "Checksum OK."
fi

# --- Extract ---
echo "Extracting..."

# nuke any prior install — preserve_layout creates subdirs that a
# per-pattern depth-1 clean wouldn't catch
rm -rf "$INSTALL_DIR"
mkdir -p "$INSTALL_DIR"

# flat: copy matched files to INSTALL_DIR/<basename>
# preserve_layout: keep archive's relative paths (required for manylinux
# wheels whose providers .so RPATH is `$ORIGIN/../../<ep>.libs`)
_extract_libs() {
  local search_root="$1"
  local pattern f rel
  for pattern in "$PKG_LIB_PATTERN" $PKG_LIB_EXTRA_PATTERNS; do
    find "$search_root" -name "${pattern}*" -type f | while read -r f; do
      if [ "$PKG_PRESERVE_LAYOUT" = "true" ]; then
        rel="${f#$search_root/}"
        mkdir -p "$INSTALL_DIR/$(dirname "$rel")"
        cp "$f" "$INSTALL_DIR/$rel"
      else
        cp "$f" "$INSTALL_DIR/"
      fi
    done
  done
}

case "$PKG_FORMAT" in
  tgz)
    tar xzf "$ARCHIVE" -C "$TMPDIR"
    _extract_libs "$TMPDIR"
    ;;
  zip|whl)
    unzip -q -o "$ARCHIVE" -d "$TMPDIR/extracted"
    _extract_libs "$TMPDIR/extracted"
    ;;
  *)
    echo "Error: unsupported archive format: $PKG_FORMAT" >&2
    exit 1
    ;;
esac

# --- Clear executable-stack flag (Linux only) ---
# Some upstream ORT builds (notably AMD's ROCm/MIGraphX packages) ship
# libonnxruntime.so with PT_GNU_STACK marked RWE. glibc >= 2.41 refuses to
# dlopen such objects with "cannot enable executable stack as shared object
# requires", so darktable's probe reports the library as invalid. The flag
# is purely a linker-metadata mistake; clearing it is safe.
clear_execstack() {
  local f="$1"
  if ! command -v readelf &>/dev/null; then return 0; fi
  readelf -lW "$f" 2>/dev/null | grep -q 'GNU_STACK.*RWE' || return 0
  echo "  patching executable-stack flag on $(basename "$f")"
  if command -v execstack &>/dev/null; then
    execstack -c "$f" 2>/dev/null && return 0
  fi
  # Fallback: clear the X bit in PT_GNU_STACK p_flags directly. ELF64 only.
  # We locate the program header by reading e_phoff/e_phentsize/e_phnum from
  # the ELF header and scan for PT_GNU_STACK (p_type == 0x6474e551).
  python3 - "$f" <<'PY' 2>/dev/null && return 0
import struct, sys
p = sys.argv[1]
with open(p, 'r+b') as fh:
    data = fh.read(64)
    if data[:4] != b'\x7fELF' or data[4] != 2:  # ELF64 only
        sys.exit(1)
    e_phoff = struct.unpack_from('<Q', data, 32)[0]
    e_phentsize = struct.unpack_from('<H', data, 54)[0]
    e_phnum = struct.unpack_from('<H', data, 56)[0]
    PT_GNU_STACK = 0x6474e551
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        fh.seek(off)
        ph = fh.read(e_phentsize)
        p_type, p_flags = struct.unpack_from('<II', ph, 0)
        if p_type == PT_GNU_STACK:
            new_flags = p_flags & ~0x1  # clear PF_X
            fh.seek(off + 4)
            fh.write(struct.pack('<I', new_flags))
            sys.exit(0)
sys.exit(1)
PY
  echo "  warning: could not clear execstack flag (install 'execstack' or python3)" >&2
  echo "           the library may fail to load on glibc >= 2.41" >&2
  return 1
}

if [ "$PLATFORM" = "linux" ]; then
  find "$INSTALL_DIR" -name "*.so*" -type f | while read -r f; do
    clear_execstack "$f" || true
  done
fi

# --- Verify ---
# preserve_layout: recurse, skip auditwheel-bundled *.libs/ dirs
# flat: only look at depth 1
if [ "$PKG_PRESERVE_LAYOUT" = "true" ]; then
  _find_args=(! -path '*/*.libs/*')
else
  _find_args=(-maxdepth 1)
fi
ORT_SO=$(find "$INSTALL_DIR" "${_find_args[@]}" -name "${PKG_LIB_PATTERN}.so.*" -type f 2>/dev/null \
           | sort -V | tail -1)
[ -z "$ORT_SO" ] && ORT_SO=$(find "$INSTALL_DIR" "${_find_args[@]}" -name "${PKG_LIB_PATTERN}*.so*" -type f 2>/dev/null \
                              | grep -v providers | sort -V | tail -1)

if [ -z "$ORT_SO" ]; then
  echo "Error: no library found after extraction." >&2
  exit 1
fi

echo ""
echo "Done. Installed to: $INSTALL_DIR"
echo "Library: $ORT_SO"
echo ""
echo "To enable in darktable:"
echo ""
echo "  1. Open darktable preferences (Ctrl+,)"
echo "  2. Go to the AI tab"
echo "  3. Click 'detect' to find the installed library automatically,"
echo "     or set 'ONNX Runtime library' to:"
echo "     $ORT_SO"
echo "  4. Restart darktable"
echo ""
echo "Or via command line:"
echo ""
echo "  DT_ORT_LIBRARY=$ORT_SO darktable"
