#!/bin/bash
#
# Install GPU-accelerated ONNX Runtime for darktable.

set -euo pipefail

usage() {
  cat <<EOF
NAME
    install-ort-gpu.sh – install GPU-accelerated ONNX Runtime for darktable

SYNOPSIS
    install-ort-gpu.sh [-y|--yes]
                       [--ep <cuda12|cuda13|rocm|migraphx|openvino>]
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

    --ep <name>
        Force a specific execution provider, skipping auto-detection.
        One EP per install. Values:

          cuda12    NVIDIA, CUDA 12 (onnxruntime-gpu from PyPI)
          cuda13    NVIDIA, CUDA 13 (onnxruntime release tarball from GitHub)
          rocm      AMD,    ROCm 6.x (onnxruntime-rocm from PyPI)
          migraphx  AMD,    ROCm 7.x (onnxruntime-migraphx from PyPI)
          openvino  Intel,  OpenVINO (onnxruntime-openvino from PyPI)

        Without this flag the script auto-detects vendor (nvidia-smi /
        rocminfo / lspci) and version (nvcc / /opt/rocm/.info/version),
        and prompts when more than one GPU vendor is present.

    --manifest <path>
        Use a custom ort_gpu.json manifest. Without this flag the
        script picks the first found:
          1. <script>/../../{data,share/darktable}/ort_gpu.json
             (source checkout or installed-alongside-script layout)
          2. /usr/{share,local/share}/darktable/ort_gpu.json
             (system install of darktable)
          3. https://raw.githubusercontent.com/.../master/data/ort_gpu.json
             (fetched as fallback for the "curl ... | bash" one-liner)

    -h, --help
        Show this help and exit.

EXAMPLES
    install-ort-gpu.sh
        Detect the GPU, prompt for confirmation, install.

    install-ort-gpu.sh -y
        Same as above without confirmation.

    install-ort-gpu.sh --ep migraphx -y
        Install the AMD/MIGraphX package without GPU detection.

    install-ort-gpu.sh --ep cuda13 -y
        Install the NVIDIA CUDA 13 package without GPU detection.
EOF
}

YES=false
MANIFEST=""
VENDOR_OVERRIDE=""
CUDA_OVERRIDE=""
ROCM_OVERRIDE=""

# Map an --ep value to internal vendor + version overrides.
parse_ep() {
  case "$1" in
    cuda12)   VENDOR_OVERRIDE=nvidia; CUDA_OVERRIDE=12 ;;
    cuda13)   VENDOR_OVERRIDE=nvidia; CUDA_OVERRIDE=13 ;;
    rocm)     VENDOR_OVERRIDE=amd;    ROCM_OVERRIDE=6  ;;
    migraphx) VENDOR_OVERRIDE=amd;    ROCM_OVERRIDE=7  ;;
    openvino) VENDOR_OVERRIDE=intel ;;
    *) echo "Error: --ep requires one of: cuda12, cuda13, rocm, migraphx, openvino (got '$1')" >&2; exit 1 ;;
  esac
}

# read user input from the controlling tty, not stdin -- when run via
# `curl ... | bash`, stdin is the pipe and `read` returns immediately
# with empty input. usage: ask "Prompt: " VAR_NAME
ask() {
  local prompt="$1" var="$2"
  if [ -r /dev/tty ]; then
    read -rp "$prompt" "$var" </dev/tty
  else
    echo "Cannot prompt for input (no tty). Re-run with --ep and -y" >&2
    exit 1
  fi
}
while [ $# -gt 0 ]; do
  case "$1" in
    -y|--yes) YES=true; shift ;;
    --manifest)
      [ $# -ge 2 ] || { echo "Error: --manifest requires a path argument" >&2; exit 1; }
      MANIFEST="$2"; shift 2 ;;
    --ep)
      [ $# -ge 2 ] || { echo "Error: --ep requires one of: cuda12, cuda13, rocm, migraphx, openvino" >&2; exit 1; }
      parse_ep "$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) break ;;
  esac
done

# --- Platform checks ---
if [ "$(uname -s)" != "Linux" ]; then
  echo "Error: this script is for Linux only." >&2
  exit 1
fi

# --- Prerequisite check ---
check_prereqs() {
  local missing=() cmd
  for cmd in jq curl tar unzip; do
    command -v "$cmd" >/dev/null 2>&1 || missing+=("$cmd")
  done
  [ ${#missing[@]} -eq 0 ] && return
  echo "Error: missing required commands: ${missing[*]}" >&2
  echo "Install them via your distro's package manager and re-run." >&2
  exit 1
}
check_prereqs

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
    # No local copy found; fetch from GitHub so the script works when
    # downloaded and run directly from a raw GitHub URL.
    GH_MANIFEST_URL="https://raw.githubusercontent.com/darktable-org/darktable/refs/heads/master/data/ort_gpu.json"
    MANIFEST="$(mktemp --suffix=.json)"
    curl -fsSL "$GH_MANIFEST_URL" -o "$MANIFEST" || { echo "Error: cannot download ort_gpu.json from GitHub." >&2; exit 1; }
  fi
fi

if [ ! -f "$MANIFEST" ]; then
  echo "Error: manifest not found: $MANIFEST" >&2
  exit 1
fi

MIN_ORT_VERSION=$(jq -r '.min_ort_version // ""' "$MANIFEST" 2>/dev/null || true)

# Compare semver-ish strings: returns 0 iff $1 >= $2.
_version_ge() {
  [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | head -1)" = "$2" ]
}

# Resolve a PyPI source block in $PKG_JSON to concrete URL/SHA/version.
# Sets PKG_URL, PKG_SHA256, PKG_ORT_VERSION, PKG_SIZE_MB.
resolve_pypi_source() {
  local pkg python_tag json ver wheel size tag
  pkg=$(echo "$PKG_JSON" | jq -r '.source.package')
  python_tag=$(echo "$PKG_JSON" | jq -r '.source.python_tag // "cp312"')

  echo "Resolving latest $pkg ..."
  json=$(curl -fsSL "https://pypi.org/pypi/$pkg/json") || {
    echo "Error: failed to query PyPI for $pkg" >&2; exit 1; }

  ver=$(echo "$json" | jq -r '.info.version')
  if [ -n "$MIN_ORT_VERSION" ] && ! _version_ge "$ver" "$MIN_ORT_VERSION"; then
    echo "Error: PyPI $pkg latest ($ver) is below min_ort_version ($MIN_ORT_VERSION)" >&2
    exit 1
  fi

  # Walk platform_tags newest-first; first match wins.
  wheel=""
  while IFS= read -r tag; do
    wheel=$(echo "$json" | jq -c --arg v "$ver" --arg pt "$python_tag" --arg t "$tag" '
      [.releases[$v][] | select(.packagetype == "bdist_wheel")
                       | select(.filename | contains($pt))
                       | select(.filename | contains($t))][0]
    ')
    [ -n "$wheel" ] && [ "$wheel" != "null" ] && break
    wheel=""
  done < <(echo "$PKG_JSON" | jq -r '.source.platform_tags[]')

  if [ -z "$wheel" ]; then
    echo "Error: no matching wheel in $pkg $ver for $python_tag + platform tags" >&2
    exit 1
  fi

  PKG_URL=$(echo "$wheel" | jq -r '.url')
  PKG_SHA256=$(echo "$wheel" | jq -r '.digests.sha256')
  PKG_ORT_VERSION="$ver"
  size=$(echo "$wheel" | jq -r '.size')
  PKG_SIZE_MB=$(( (size / (1024*1024) + 9) / 10 * 10 ))
}

# Resolve a github_release source block in $PKG_JSON. Sets PKG_URL,
# PKG_ORT_VERSION, PKG_SIZE_MB. PKG_SHA256 is left empty — GitHub does
# not publish per-asset SHA256 in the API response, so we fall back to
# file-magic verification at extract time.
resolve_github_release_source() {
  local repo asset_pattern api_url tmp http_code json tag ver expected url size hdr=()
  repo=$(echo "$PKG_JSON" | jq -r '.source.repo')
  asset_pattern=$(echo "$PKG_JSON" | jq -r '.source.asset_pattern')
  api_url="https://api.github.com/repos/$repo/releases/latest"

  echo "Resolving latest $repo ..."
  [ -n "${GITHUB_TOKEN:-}" ] && hdr=(-H "Authorization: Bearer $GITHUB_TOKEN")

  tmp=$(mktemp)
  http_code=$(curl -fsS -o "$tmp" -w '%{http_code}' \
    "${hdr[@]}" -H 'Accept: application/vnd.github+json' "$api_url" || echo "$?")

  if [ "$http_code" = "403" ] || [ "$http_code" = "429" ]; then
    rm -f "$tmp"
    cat <<EOF >&2

GitHub API rate limit hit (60 req/hr per IP, unauthenticated).

You're seeing this because the install script needs api.github.com to
discover the latest CUDA 13 release. Shared NAT (corporate networks,
VPNs, cloud VMs) often blows through 60/hr from one source IP.

Workarounds, easiest first:

  1. Wait ~1 hour for the limit to reset and re-run this script.

  2. Set GITHUB_TOKEN with any personal access token (no scopes
     required for public repos) — bumps the limit to 5000 req/hr:
       GITHUB_TOKEN=ghp_yourtoken ./install-ort-gpu.sh

  3. Install manually — easy for this package:
       a. Open https://github.com/$repo/releases/latest in a browser
       b. Find the asset named like: $asset_pattern
          (where {version} is whatever release tag is shown at the top)
       c. Download it (tarball, no auth needed)
       d. tar xzf <file>.tgz
       e. Copy the extracted lib/libonnxruntime.so.* (and LICENSE,
          ThirdPartyNotices.txt from the extracted root) into:
            $INSTALL_DIR
       f. Open darktable -> Preferences -> AI tab -> point at the .so

EOF
    exit 1
  fi

  if [ "$http_code" != "200" ]; then
    echo "Error: GitHub API returned HTTP $http_code for $api_url" >&2
    rm -f "$tmp"
    exit 1
  fi

  json=$(cat "$tmp"); rm -f "$tmp"
  tag=$(echo "$json" | jq -r '.tag_name')
  ver="${tag#v}"

  if [ -n "$MIN_ORT_VERSION" ] && ! _version_ge "$ver" "$MIN_ORT_VERSION"; then
    echo "Error: latest $repo release ($ver) is below min_ort_version ($MIN_ORT_VERSION)" >&2
    exit 1
  fi

  expected="${asset_pattern//\{version\}/$ver}"
  local asset
  asset=$(echo "$json" | jq -c --arg n "$expected" '.assets[] | select(.name == $n)')
  if [ -z "$asset" ] || [ "$asset" = "null" ]; then
    echo "Error: no asset named '$expected' in release $tag of $repo" >&2
    exit 1
  fi
  url=$(echo "$asset" | jq -r '.browser_download_url')
  size=$(echo "$asset" | jq -r '.size')
  # GitHub's `digest` field arrived in 2024; older releases may not have it,
  # in which case PKG_SHA256 stays empty and the file-magic check kicks in.
  local digest
  digest=$(echo "$asset" | jq -r '.digest // ""')
  PKG_SHA256="${digest#sha256:}"
  [ "$PKG_SHA256" = "$digest" ] && PKG_SHA256=""  # non-sha256 digest → don't trust

  PKG_URL="$url"
  PKG_ORT_VERSION="$ver"
  PKG_SIZE_MB=$(( (size / (1024*1024) + 9) / 10 * 10 ))
}

ARCH=$(uname -m)
PLATFORM="linux"

echo ""
echo "ONNX Runtime GPU acceleration installer"
echo "========================================"

# --- Detect GPU (skipped with --ep) ---
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
  VENDOR="$VENDOR_OVERRIDE"
  echo "EP override: forcing $VENDOR_OVERRIDE (skipping GPU detection)"
  # auto-detect version only when --ep didn't already pin one
  [ "$VENDOR_OVERRIDE" = "amd"    ] && [ -z "$ROCM_OVERRIDE" ] && detect_rocm_version
  [ "$VENDOR_OVERRIDE" = "nvidia" ] && [ -z "$CUDA_OVERRIDE" ] && detect_cuda_version
else
  # NVIDIA
  if command -v nvidia-smi &>/dev/null; then
    # nvidia-smi writes "driver not loaded" to stdout; require CSV
    NVIDIA_INFO=$(nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null || true)
    if [ -n "$NVIDIA_INFO" ] && echo "$NVIDIA_INFO" | head -1 | grep -q ','; then
      # comma-join names across all detected GPUs; driver version is
      # identical per row, take from the first
      GPU_LABEL=$(echo "$NVIDIA_INFO" | awk -F, '{
          gsub(/^[[:space:]]+|[[:space:]]+$/, "", $1)
          printf "%s%s", (NR>1 ? ", " : ""), $1
        }')
      DRIVER_VERSION=$(echo "$NVIDIA_INFO" | head -1 | cut -d, -f2 \
        | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
      VENDOR="nvidia"
      detect_cuda_version
    fi
  fi

  # AMD
  detect_rocm_version
  if [ -n "$ROCM_VERSION" ]; then
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
    echo "Ensure GPU drivers are installed, or use --ep <cuda12|cuda13|rocm|migraphx|openvino>."
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
  ask "Select GPU [1-${#VENDORS[@]}]: " choice
  idx=$((choice - 1))
  if [ "$idx" -lt 0 ] || [ "$idx" -ge ${#VENDORS[@]} ]; then
    echo "Invalid selection." >&2
    exit 1
  fi
  SELECTED="${VENDORS[$idx]}"
fi

# set labels for selected vendor (with fallbacks for --ep override)
case "$SELECTED" in
  nvidia) SEL_LABEL="${GPU_LABEL:-NVIDIA GPU}"; SEL_DRIVER="$DRIVER_VERSION" ;;
  amd)    SEL_LABEL="${GPU_LABEL_AMD:-AMD GPU}"; SEL_DRIVER="${DRIVER_VERSION_AMD:-}" ;;
  intel)  SEL_LABEL="${GPU_LABEL_INTEL:-Intel GPU}"; SEL_DRIVER="" ;;
esac

# Apply EP-driven version overrides (set by parse_ep). Both are no-ops
# when --ep wasn't given or the selected vendor doesn't match.
[ -n "$CUDA_OVERRIDE" ] && [ "$SELECTED" = "nvidia" ] && CUDA_MM="${CUDA_OVERRIDE}.0"
[ -n "$ROCM_OVERRIDE" ] && [ "$SELECTED" = "amd"    ] && ROCM_MM="${ROCM_OVERRIDE}.0"

# --- Find matching package in manifest ---
PKG_JSON=$(jq -c --arg v "$SELECTED" --arg p "$PLATFORM" --arg a "$ARCH" \
  --arg r "${ROCM_MM:-}" --arg c "${CUDA_MM:-}" \
  '[.packages[] | select(.vendor==$v and .platform==$p and (.arch // "x86_64")==$a) |
   if ($r != "" and .rocm_min != null) then select(.rocm_min <= $r and (.rocm_max // "99") >= $r) else . end |
   if ($c != "" and .cuda_min != null) then select(.cuda_min <= $c and (.cuda_max // "99") >= $c) else . end] | first' \
  "$MANIFEST" 2>/dev/null || true)

if [ -z "$PKG_JSON" ] || [ "$PKG_JSON" = "null" ]; then
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
if [ "$SELECTED" = "nvidia" ] && [ -z "${CUDA_MM:-}" ] && [ -z "$CUDA_OVERRIDE" ]; then
  echo "Warning: could not detect installed CUDA toolkit version."
  echo "  Proceeding with: Requirements: $(_field requirements '')"
  echo "  (use --ep cuda12 or --ep cuda13 to pick explicitly)"
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
PKG_INSTALL_SUBDIR=$(_field install_subdir "onnxruntime")
PKG_SIZE_MB=$(_field size_mb "0")
PKG_ORT_VERSION=$(_field ort_version "")
PKG_REQUIREMENTS=$(_field requirements "")
PKG_SHA256=$(_field sha256 "")

# If entry uses a source block, resolve URL/SHA/version at install time.
PKG_SOURCE_TYPE=$(echo "$PKG_JSON" | jq -r '.source.type // ""')
case "$PKG_SOURCE_TYPE" in
  pypi)           resolve_pypi_source ;;
  github_release) resolve_github_release_source ;;
esac

INSTALL_DIR="$HOME/.local/lib/$PKG_INSTALL_SUBDIR"

# --- Info & confirmation ---
echo ""
echo "GPU: $SEL_LABEL"
[ -n "$SEL_DRIVER" ] && echo "Driver: $SEL_DRIVER"
echo "ORT version: $PKG_ORT_VERSION"
echo "Download size: ~${PKG_SIZE_MB} MB"
echo "Install to: $INSTALL_DIR"
[ -n "$PKG_REQUIREMENTS" ] && echo "Requirements: $PKG_REQUIREMENTS"
echo ""

if [ "$YES" = false ]; then
  ask "Continue? [y/N] " answer
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
curl -fL --progress-bar -o "$ARCHIVE" "$PKG_URL"

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
else
  # GitHub source has no published SHA; catch HTML error pages with file magic.
  # Use od (coreutils, always present) rather than xxd (vim-common, often missing).
  MAGIC=$(head -c 4 "$ARCHIVE" | od -An -tx1 | tr -d ' \n')
  case "$PKG_FORMAT" in
    whl|zip) [ "$MAGIC" = "504b0304" ] || { echo "Error: download is not a zip/wheel (magic=$MAGIC)" >&2; exit 1; } ;;
    tgz)     [[ "$MAGIC" =~ ^1f8b ]]   || { echo "Error: download is not a gzip tarball (magic=$MAGIC)" >&2; exit 1; } ;;
  esac
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

# Copy LICENSE / NOTICE / ThirdPartyNotices alongside the libs so the
# install directory is self-documenting and MIT/Apache-2.0 attribution
# travels with the binaries.
_copy_licenses() {
  local search_root="$1"
  # skip *.dist-info/ — pip-generated duplicates of the same files in the package root
  find "$search_root" -maxdepth 4 -type f -not -path '*/*.dist-info/*' \
    \( -iname 'LICENSE' -o -iname 'LICENSE.txt' \
       -o -iname 'NOTICE' -o -iname 'NOTICE.txt' \
       -o -iname 'ThirdPartyNotices*' -o -iname 'COPYING' \) \
    -exec cp -n {} "$INSTALL_DIR/" \; 2>/dev/null || true
}

case "$PKG_FORMAT" in
  tgz)
    tar xzf "$ARCHIVE" -C "$TMPDIR"
    _extract_libs "$TMPDIR"
    _copy_licenses "$TMPDIR"
    ;;
  zip|whl)
    unzip -q -o "$ARCHIVE" -d "$TMPDIR/extracted"
    _extract_libs "$TMPDIR/extracted"
    _copy_licenses "$TMPDIR/extracted"
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
