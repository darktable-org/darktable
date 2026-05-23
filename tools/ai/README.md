# GPU-Accelerated ONNX Runtime for darktable

darktable bundles a CPU-only ONNX Runtime on Linux, DirectML on Windows,
and CoreML on macOS. To enable GPU acceleration for AI features (denoise,
upscale, segmentation), install a GPU-enabled ONNX Runtime build using one
of the install scripts in this directory.

macOS users don't need to install anything – CoreML acceleration is
bundled with darktable.

## What's bundled by default

| Platform | Bundled ONNX Runtime | GPU support |
|----------|------------|-------------|
| Linux | CPU only | None – install GPU ONNX Runtime below |
| Windows | DirectML | AMD, NVIDIA, Intel via DirectX 12 |
| macOS | CoreML | Apple Neural Engine |

## Installing via script

### Local install

Linux:
```bash
./tools/ai/install-ort-gpu.sh --help    # see all flags
./tools/ai/install-ort-gpu.sh
```

Windows (PowerShell):
```powershell
.\tools\ai\install-ort-gpu.ps1 -Help    # see all flags
.\tools\ai\install-ort-gpu.ps1
```

If Windows blocks the script ("running scripts is disabled on this
system"), bypass once:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\ai\install-ort-gpu.ps1
```

### One-line install

Linux:
```bash
curl -fsSL https://raw.githubusercontent.com/darktable-org/darktable/refs/heads/master/tools/ai/install-ort-gpu.sh | bash
```

Windows (PowerShell):
```powershell
irm https://raw.githubusercontent.com/darktable-org/darktable/refs/heads/master/tools/ai/install-ort-gpu.ps1 | iex
```

When no local `ort_gpu.json` is found the scripts automatically fetch the
manifest from GitHub, so no extra files are needed.

### Script prerequisites

**Linux** – `bash`, `curl`, `jq`.

**Windows** – PowerShell 5.1+.

### GPU / driver requirements

**NVIDIA (CUDA)** – Pascal-or-newer GPU (compute 6.0+), driver 525+,
CUDA Toolkit 12.x or 13.x, cuDNN 9.x.

**AMD (MIGraphX)** – ROCm-supported GPU (Radeon RX 7700+ / Instinct
MI100+), ROCm 6.x+ with MIGraphX.

**Intel (OpenVINO)** – Intel iGPU (HD/UHD/Iris Xe) or Arc discrete,
GPU driver with OpenCL (`intel-opencl-icd`) and/or Level Zero. The
OpenVINO runtime ships in the package.

### AMD: building from source

If the prebuilt package doesn't work (ABI mismatch, unsupported ROCm
version), build ONNX Runtime against your installed ROCm:

```bash
./tools/ai/install-ort-amd-build.sh
```

Requires cmake 3.26+, gcc/g++, python3, git. Takes 10–20 minutes.

## Installing manually

If the scripts won't run for you (locked-down environment, custom audit
requirements, GitHub API rate-limited – see below) you can fetch and
place the files yourself. The end goal is just a `libonnxruntime.so.*`
/ `onnxruntime.dll` on disk that darktable can be pointed at.

### Where to download

| Vendor | Source |
|--------|--------|
| NVIDIA CUDA 12 | [GitHub Releases](https://github.com/microsoft/onnxruntime/releases/latest) – `onnxruntime-{linux\|win}-x64-gpu-X.Y.Z.{tgz\|zip}` |
| NVIDIA CUDA 13 | [GitHub Releases](https://github.com/microsoft/onnxruntime/releases/latest) – `onnxruntime-{linux\|win}-x64-gpu_cuda13-X.Y.Z.{tgz\|zip}` |
| AMD ROCm 6.x | [PyPI `onnxruntime-rocm`](https://pypi.org/project/onnxruntime-rocm/#files) – `cp312` `manylinux` wheel |
| AMD ROCm 7.x | [PyPI `onnxruntime-migraphx`](https://pypi.org/project/onnxruntime-migraphx/#files) – `cp312` `manylinux` wheel |
| Intel OpenVINO (Linux) | [PyPI `onnxruntime-openvino`](https://pypi.org/project/onnxruntime-openvino/#files) – `cp312` `manylinux_2_28_x86_64` wheel |
| Intel OpenVINO (Windows) | [PyPI `onnxruntime-openvino`](https://pypi.org/project/onnxruntime-openvino/#files) + [PyPI `openvino`](https://pypi.org/project/openvino/#files) – both `cp312` `win_amd64` |

For NVIDIA we recommend GitHub Releases for manual installs of either
CUDA version – the tarball/zip layout is simpler to extract by hand than
the `onnxruntime-gpu` PyPI wheel (which buries the library under
`onnxruntime/capi/`). The install script uses PyPI for CUDA 12 because
GitHub's API rate-limited discovery is unnecessary when PyPI exposes
the same artifact via a friendlier JSON endpoint.

### Extracting

The goal: one directory containing the main library, all
`libonnxruntime_providers_*` plugins, any bundled native deps, and the
`LICENSE` / `ThirdPartyNotices.txt` files. The install script uses
these conventional locations and `darktable -d ai` auto-detects them:

| Vendor / EP | Linux | Windows |
|-------------|-------|---------|
| NVIDIA CUDA | `~/.local/lib/onnxruntime-cuda/` | `%LOCALAPPDATA%\onnxruntime-cuda\` |
| AMD ROCm 6 | `~/.local/lib/onnxruntime-rocm/` | n/a |
| AMD ROCm 7 / MIGraphX | `~/.local/lib/onnxruntime-migraphx/` | n/a |
| Intel OpenVINO | `~/.local/lib/onnxruntime-openvino/` | `%LOCALAPPDATA%\onnxruntime-openvino\` |

Any path works – the table is just what the script uses. Substitute as
you like; the only requirement is keeping all files from one extraction
together.

#### NVIDIA – tarball / zip (CUDA 12 or CUDA 13)

```bash
# Linux. Pick the asset matching your CUDA major from the release page.
# CUDA 12: onnxruntime-linux-x64-gpu-<ver>.tgz
# CUDA 13: onnxruntime-linux-x64-gpu_cuda13-<ver>.tgz
tar xzf onnxruntime-linux-x64-gpu*-<ver>.tgz
src=onnxruntime-linux-x64-gpu*-<ver>
dst=~/.local/lib/onnxruntime-cuda
mkdir -p "$dst"
cp "$src"/lib/libonnxruntime*.so* "$dst"/
cp "$src"/LICENSE "$src"/ThirdPartyNotices.txt "$dst"/
```

```powershell
# Windows. Same asset naming as above, .zip suffix.
Expand-Archive .\onnxruntime-win-x64-gpu*-<ver>.zip -DestinationPath .
$src = (Get-ChildItem -Directory onnxruntime-win-x64-gpu*).FullName
$dst = "$env:LOCALAPPDATA\onnxruntime-cuda"
New-Item -ItemType Directory $dst -Force | Out-Null
Copy-Item "$src\lib\onnxruntime*.dll" $dst
Copy-Item "$src\LICENSE", "$src\ThirdPartyNotices.txt" $dst
```

#### AMD – PyPI wheel (`onnxruntime-rocm` for ROCm 6, `onnxruntime-migraphx` for ROCm 7)

```bash
# Pick the latest cp312 manylinux wheel from the project's "Download files" page.
unzip -q onnxruntime_*-*.whl -d wheel/

dst=~/.local/lib/onnxruntime-migraphx     # or onnxruntime-rocm
mkdir -p "$dst"
cp wheel/onnxruntime/capi/libonnxruntime*.so* "$dst"/
cp wheel/onnxruntime/LICENSE "$dst"/
cp wheel/onnxruntime/ThirdPartyNotices.txt "$dst"/
```

If the wheel contains an `onnxruntime.libs/` directory (auditwheel-bundled
ROCm libs), preserve the relative layout instead of the flat copy above –
the providers `.so` files have an RPATH of `$ORIGIN/../../onnxruntime.libs`
and will fail to load if the directory is renamed or detached:

```bash
# only when wheel/onnxruntime.libs/ exists
cp -r wheel/onnxruntime "$dst"/
cp -r wheel/onnxruntime.libs "$dst"/
# then point darktable at $dst/onnxruntime/capi/libonnxruntime.so.<ver>
```

Quick check whether your wheel needs the layout-preserving path:

```bash
unzip -l onnxruntime_*-*.whl | grep -q '\.libs/' && echo "preserve layout" || echo "flat copy"
```

Current ROCm 6 (`onnxruntime-rocm`) and ROCm 7 (`onnxruntime-migraphx`)
releases are flat – no `.libs/`. Future releases may change; the check
above is the safe answer.

#### AMD – alternative: ONNX Runtime from AMD's ROCm repository

PyPI's `onnxruntime-rocm` / `onnxruntime-migraphx` wheels ship a fixed
set of HIP kernel binaries (typically the desktop gfx targets upstream
tracks). If your GPU's `gfx*` ID isn't in that set, ONNX Runtime loads but model
compilation fails at runtime with:

```
migraphx_program_compile: Error: ... no kernel image is available
for execution on the device
```

This commonly hits some RDNA 3 iGPUs (e.g. Radeon 780M / gfx1103). AMD's
own ROCm repository at https://repo.radeon.com/rocm/manylinux/ ships
builds with broader per-architecture coverage. Browse the
`rocm-rel-X.Y.Z/` directory matching your installed ROCm version,
download the `onnxruntime_rocm-*.whl` matching `cp312`, and extract it
exactly as in the AMD PyPI section above (to
`~/.local/lib/onnxruntime-rocm/`). The install script's **detect**
button picks up any `onnxruntime-*` directory.

Two cheaper workarounds worth trying first:

- **Hide the unsupported GPU** from MIGraphX so it binds to a supported
  card (find indices with `rocminfo | grep -E "Marketing Name|gfx"`):
  ```bash
  HIP_VISIBLE_DEVICES=0 darktable
  ```
- **Override the `gfx` version** – RDNA 3 ISA is mostly binary-compatible
  with gfx1100, so the bundled gfx1100 kernels often work:
  ```bash
  HSA_OVERRIDE_GFX_VERSION=11.0.0 darktable
  ```

#### Intel OpenVINO – PyPI wheel (Linux)

The `onnxruntime-openvino` wheel bundles all OpenVINO + TBB libs inside
its `capi/` directory, so one extraction covers everything.

```bash
unzip -q onnxruntime_openvino-*.whl -d wheel/

dst=~/.local/lib/onnxruntime-openvino
mkdir -p "$dst"
cp wheel/onnxruntime/capi/libonnxruntime*.so* "$dst"/
cp wheel/onnxruntime/capi/libopenvino*.so*    "$dst"/
cp wheel/onnxruntime/capi/libtbb*.so*         "$dst"/
cp wheel/onnxruntime/LICENSE "$dst"/
cp wheel/onnxruntime/ThirdPartyNotices.txt "$dst"/
```

#### Intel OpenVINO – PyPI wheels (Windows)

Windows needs two wheels: `onnxruntime-openvino` plus the matching
`openvino` runtime (Microsoft bundles the runtime inside ONNX Runtime on Linux
but ships it separately for Windows).

```powershell
Expand-Archive .\onnxruntime_openvino-*.whl -DestinationPath ort\
Expand-Archive .\openvino-*-win_amd64.whl   -DestinationPath rt\

$dst = "$env:LOCALAPPDATA\onnxruntime-openvino"
New-Item -ItemType Directory $dst -Force | Out-Null
Copy-Item ort\onnxruntime\capi\onnxruntime*.dll $dst
Copy-Item rt\openvino\libs\*.dll $dst
Copy-Item ort\onnxruntime\LICENSE, ort\onnxruntime\ThirdPartyNotices.txt $dst
```

Then point darktable at the resulting library as described in
[Enabling the custom ONNX Runtime in darktable](#enabling-the-custom-onnx-runtime-in-darktable)
below.

### Why you might need the manual route: GitHub API rate limit

For CUDA 13 the install script asks `api.github.com` which release is
latest. GitHub limits unauthenticated callers to **60 requests per hour
per source IP**. On shared NAT (corporate networks, VPNs, cloud VMs)
that limit gets exhausted quickly by all the other users behind the
same IP, and the script will fail with a clear message pointing here.

Two ways out:

- **Set `GITHUB_TOKEN`** with any personal access token (no scopes
  required for public repos) before re-running – that raises the limit
  to 5000 requests/hour authenticated.
- **Install manually** following the table above – for the CUDA 13
  tarball this is a one-line `tar xzf` plus a copy. The install script
  prints the same instructions on failure.

PyPI sources (AMD, Intel, NVIDIA CUDA 12) have no published rate limit
and are unaffected.

## Enabling the custom ONNX Runtime in darktable

After running the script or built-in installer:

1. Open darktable preferences (Ctrl+,)
2. Go to the **AI** tab
3. Click **detect**, or use the browse button to select the library
   manually
4. Restart darktable

Or set `DT_ORT_LIBRARY` in the environment:

Linux:
```bash
DT_ORT_LIBRARY=~/.local/lib/onnxruntime-cuda/libonnxruntime.so.1.24.4 darktable
```

Windows (PowerShell):
```powershell
$env:DT_ORT_LIBRARY="$env:LOCALAPPDATA\onnxruntime-cuda\onnxruntime.dll"; darktable
```

If neither preference nor env var is set, darktable uses the bundled
ONNX Runtime (CPU on Linux, DirectML on Windows, CoreML on macOS).

## Verifying

```bash
darktable -d ai
```

Look for:
```
[darktable_ai] loaded ORT 1.24.4 from '/home/user/.local/lib/onnxruntime-cuda/libonnxruntime.so.1.24.4'
[darktable_ai] execution provider: CUDA
[darktable_ai] NVIDIA CUDA enabled successfully on device 0: NVIDIA GeForce RTX 4090
```

## Maintaining the GPU package registry

`data/ort_gpu.json` describes how the install scripts and preferences
UI find each vendor's ONNX Runtime build. Every entry now carries a
`source` block instead of a pinned URL:

- **PyPI** sources (AMD, Intel, NVIDIA CUDA 12) – install scripts
  query `pypi.org/pypi/<package>/json` at install time, pick the
  latest wheel matching the declared platform tags + python tag, and
  verify the SHA from PyPI's JSON response.
- **GitHub release** sources (NVIDIA CUDA 13 – Microsoft hasn't
  published a PyPI package for CUDA 13 yet) – install scripts query
  `api.github.com/repos/<repo>/releases/latest` and pick the asset
  matching the declared filename pattern. No SHA available in the API
  response; download integrity is gated by HTTPS + a file-magic check.

The top-level `min_ort_version` field is the floor the install scripts
enforce against any resolved version – bump it whenever darktable's C
bindings start requiring a newer ONNX Runtime.