# GPU-Accelerated ONNX Runtime for darktable

darktable bundles a CPU-only ONNX Runtime by default. On Linux, it also
bundles DirectML on Windows and CoreML on macOS. These scripts install a
GPU-enabled ORT build to accelerate AI features (denoise, upscale,
segmentation).

## What's bundled by default

| Platform | Bundled ORT | GPU support |
|----------|------------|-------------|
| Linux | CPU only | None – use scripts below |
| Windows | DirectML | AMD, NVIDIA, Intel via DirectX 12 |
| macOS | CoreML | Apple Silicon Neural Engine |

## Installing GPU-accelerated ORT

### NVIDIA (CUDA) – Linux & Windows

**Requirements:**

- NVIDIA GPU with compute capability 6.0+ (GeForce GTX 1000 "Pascal" or newer)
- NVIDIA driver 525 or later
- CUDA 12.x runtime – included with the driver on Windows; on Linux install
  the CUDA toolkit (`nvidia-cuda-toolkit` on Ubuntu/Debian, `cuda` on Arch)
- cuDNN 9.x – download from https://developer.nvidia.com/cudnn-downloads or
  install via package manager (`libcudnn9-cuda-12` on Ubuntu/Debian, `cudnn`
  on Arch)

Linux:
```bash
./tools/ai/install-ort-nvidia.sh
```

Windows (PowerShell):
```powershell
.\tools\ai\install-ort-nvidia.ps1
```

Downloads a prebuilt ORT with CUDA EP from GitHub (~200 MB, ~30 sec).
On Windows, use this instead of the bundled DirectML for potentially
better NVIDIA performance.

### AMD (MIGraphX) – Linux

**Requirements:**

- AMD GPU supported by ROCm:
  - Consumer: Radeon RX 6000 series (RDNA2) or newer
  - Data center: Instinct MI100 (CDNA) or newer
- ROCm 6.0 or later – install from AMD's repo:
  https://rocm.docs.amd.com/projects/install-on-linux/en/latest/
  - Ubuntu/Debian: `sudo apt install rocm`
  - Arch: `sudo pacman -S rocm-hip-sdk`
  - Fedora: `sudo dnf install rocm`
- MIGraphX (included in ROCm, or install separately):
  - Ubuntu/Debian: `sudo apt install migraphx migraphx-dev`
  - Arch: `sudo pacman -S migraphx`
- For building from source: cmake 3.26+, gcc/g++, python3, git

Prebuilt (fast, ~30 sec):
```bash
./tools/ai/install-ort-amd.sh
```

Build from source (fallback if prebuilt doesn't work, 10-20 min):
```bash
./tools/ai/install-ort-amd-build.sh
```

The prebuilt script downloads a wheel from AMD's package repository. The
build script compiles ORT against your installed ROCm headers and
libraries – use it if the prebuilt version has ABI compatibility issues.
Both auto-detect your ROCm version and select the matching ORT release:

| ROCm | ORT version |
|------|-------------|
| 7.2 | 1.23.2 |
| 7.1 | 1.23.1 |
| 7.0 | 1.22.1 |
| 6.4 | 1.21.0 |
| 6.3 | 1.19.0 |
| 6.2 | 1.18.0 |
| 6.1 | 1.17.0 |
| 6.0 | 1.16.0 |

### Intel (OpenVINO) – Linux

**Requirements:**

- Intel GPU or any x86_64 CPU:
  - Integrated: HD Graphics, UHD Graphics, Iris Xe (Gen9+)
  - Discrete: Intel Arc A-series (A770, A750, A580, etc.)
  - CPU-only mode works on any x86_64 processor (Intel or AMD)
- For GPU acceleration: Intel compute runtime with Level Zero
  - Ubuntu/Debian: `sudo apt install intel-opencl-icd level-zero`
  - Arch: `sudo pacman -S intel-compute-runtime level-zero-loader`
  - For Arc GPUs: kernel 6.2 or later recommended
- pip3 (for downloading the wheel)
- OpenVINO runtime is bundled in the package – no separate install needed

```bash
./tools/ai/install-ort-intel.sh
```

Downloads a prebuilt ORT with OpenVINO EP from PyPI (~60 MB, ~30 sec).
Includes all OpenVINO runtime libraries.

## Using the custom ORT

All scripts install to `~/.local/lib/onnxruntime-<provider>/` and print
the path to use. Set the `DT_ORT_LIBRARY` environment variable to point
darktable to the custom build:

```bash
DT_ORT_LIBRARY=~/.local/lib/onnxruntime-cuda/libonnxruntime.so.1.24.4 darktable
```

Or add to `~/.bashrc` for persistence:
```bash
export DT_ORT_LIBRARY=~/.local/lib/onnxruntime-cuda/libonnxruntime.so.1.24.4
```

On Windows (PowerShell):
```powershell
$env:DT_ORT_LIBRARY="C:\Users\you\AppData\Local\onnxruntime-cuda\onnxruntime.dll"
darktable
```

Or set permanently via System → Environment Variables.

If `DT_ORT_LIBRARY` is not set, darktable uses the bundled ORT (CPU on
Linux, DirectML on Windows, CoreML on macOS).

## Manual installation (without scripts)

If you prefer to install manually or the scripts don't work for your setup:

1. **Get an ORT shared library with your desired EP compiled in:**
   - NVIDIA CUDA: download `onnxruntime-linux-x64-gpu-VERSION.tgz` (Linux)
     or `onnxruntime-win-x64-gpu-VERSION.zip` (Windows) from
     https://github.com/microsoft/onnxruntime/releases
   - AMD MIGraphX: download `onnxruntime_rocm` wheel from
     https://repo.radeon.com/rocm/manylinux/ (match your ROCm version)
     or build from source: `./build.sh --config Release --build_shared_lib --use_migraphx --migraphx_home /opt/rocm`
   - Intel OpenVINO: `pip download --no-deps onnxruntime-openvino`

2. **Extract the shared library:**
   - `.tgz`/`.zip`: extract `lib/libonnxruntime.so*` (or `lib/onnxruntime.dll`)
   - `.whl`: rename to `.zip` and extract `onnxruntime/capi/libonnxruntime.so*`
     and any `libonnxruntime_providers_*.so` files

3. **Point darktable to it:**
   ```bash
   export DT_ORT_LIBRARY=/path/to/libonnxruntime.so.X.Y.Z
   ```

## Verifying

Run darktable with AI debug output to confirm which ORT is loaded:

```bash
DT_ORT_LIBRARY=... darktable -d ai
```

Look for:
```
[darktable_ai] loaded ORT 1.24.4 from '/home/user/.local/lib/onnxruntime-cuda/libonnxruntime.so.1.24.4'
```

Then check Preferences → Processing → AI execution provider to select
your GPU provider (CUDA, MIGraphX, OpenVINO).
