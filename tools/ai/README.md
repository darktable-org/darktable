# GPU-Accelerated ONNX Runtime for darktable

darktable bundles a CPU-only ONNX Runtime on Linux, DirectML on Windows,
and CoreML on macOS. To enable GPU acceleration for AI features (denoise,
upscale, segmentation), install a GPU-enabled ORT build using the
preferences UI or one of the install scripts in this directory.

## What's bundled by default

| Platform | Bundled ORT | GPU support |
|----------|------------|-------------|
| Linux | CPU only | None – install GPU ORT below |
| Windows | DirectML | AMD, NVIDIA, Intel via DirectX 12 |
| macOS | CoreML | Apple Neural Engine |

## Easiest: install from darktable preferences

1. Open darktable preferences (Ctrl+,)
2. Go to the **AI** tab
3. Click **install** – darktable detects your GPU and downloads
   the correct ORT package automatically
4. Restart darktable

Click **detect** instead to find a previously installed or
system-packaged ORT library.

## Installing via script

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

### Requirements

**NVIDIA (CUDA)** – Pascal-or-newer GPU (compute 6.0+), driver 525+,
CUDA 12.x or 13.x toolkit, cuDNN 9.x.

**AMD (MIGraphX)** – ROCm-supported GPU (Radeon RX 6000+ / Instinct
MI100+), ROCm 7.x with MIGraphX. Wheels are manylinux-repaired and
bundle their own ROCm runtime.

**Intel (OpenVINO)** – Intel iGPU (HD/UHD/Iris Xe) or Arc discrete,
GPU driver with OpenCL (`intel-opencl-icd`) and/or Level Zero. The
OpenVINO runtime ships in the package.

### AMD: building from source

If the prebuilt package doesn't work (ABI mismatch, unsupported ROCm
version), build ORT against your installed ROCm:

```bash
./tools/ai/install-ort-amd-build.sh
```

Requires cmake 3.26+, gcc/g++, python3, git. Takes 10–20 minutes.

## Enabling the custom ORT in darktable

After running the script or built-in installer:

1. Open darktable preferences (Ctrl+,)
2. Go to the **AI** tab
3. Click **detect**, or use the browse button to select the library
   manually
4. Restart darktable

Or set `DT_ORT_LIBRARY` in the environment:

```bash
# Linux
DT_ORT_LIBRARY=~/.local/lib/onnxruntime-cuda/libonnxruntime.so.1.24.4 darktable
```
```powershell
# Windows
$env:DT_ORT_LIBRARY="$env:LOCALAPPDATA\onnxruntime-cuda\onnxruntime.dll"; darktable
```

If neither preference nor env var is set, darktable uses the bundled
ORT (CPU on Linux, DirectML on Windows, CoreML on macOS).

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
