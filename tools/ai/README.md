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

`data/ort_gpu.json` lists the upstream ONNX Runtime URLs the install scripts and
preferences UI pull from. It needs to be refreshed whenever Microsoft
ships a new ONNX Runtime release or AMD ships a new ROCm patch. Use the
script in this directory:

```bash
# show what would change (no writes); exits non-zero if updates exist
python3 tools/ai/refresh-ort-gpu.py --check

# apply the updates in place
python3 tools/ai/refresh-ort-gpu.py --update

# apply and open a PR via gh CLI (CI mode; needs GITHUB_TOKEN)
python3 tools/ai/refresh-ort-gpu.py --update --pr

# verbose progress (otherwise quiet by default in --update mode)
python3 tools/ai/refresh-ort-gpu.py --check -v
```

What it does:

- Queries `api.github.com/repos/microsoft/onnxruntime/releases` for the
  latest non-prerelease that has all four expected GPU assets
  (linux/windows × CUDA 12/13). Updates the four NVIDIA entries.
- Scrapes `https://repo.radeon.com/rocm/manylinux/`, finds the cp312
  ONNX Runtime wheel in each `rocm-rel-X.Y.Z/` directory, and keeps the
  latest patch per ROCm minor. Updates the AMD entries with
  range-based matching (`rocm_min: "7.2"`, `rocm_max: "7.3"` covers
  every 7.2.x patch).
- Computes SHA256 only for wheels whose URL changed since the last
  refresh – a no-op run does no downloads.
- Preserves vendors it doesn't manage (e.g. Intel/OpenVINO) and any
  manual fields (`required_libs`, `lib_pattern`, `install_subdir`).

Stdlib only – no extra Python deps. Network access required.

A monthly CI job (`.github/workflows/refresh-ort-gpu.yml`) runs the
script in `--update --pr` mode on the 1st of each month and opens a
PR if anything upstream moved. Maintainer reviews and merges; nothing
is auto-merged.
