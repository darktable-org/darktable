# GPU-Accelerated ONNX Runtime for darktable

darktable bundles a CPU-only ONNX Runtime on Linux, DirectML on Windows,
and CoreML on macOS. To enable GPU acceleration for AI features (denoise,
upscale, segmentation), you can install a GPU-enabled ORT build using
the install script, the built-in installer in darktable preferences,
or manually.

## What's bundled by default

| Platform | Bundled ORT | GPU support |
|----------|------------|-------------|
| Linux | CPU only | None – install GPU ORT below |
| Windows | DirectML | AMD, NVIDIA, Intel via DirectX 12 |
| macOS | CoreML | Apple Neural Engine |

## Available execution providers

| EP | Linux | Windows | macOS |
|----|-------|---------|-------|
| CPU | yes | yes | yes |
| CoreML | - | - | yes |
| CUDA (NVIDIA) | yes | yes | - |
| MIGraphX (AMD) | yes | - | - |
| OpenVINO (Intel) | yes | yes | - |
| DirectML | - | yes (bundled) | - |

On Windows, CUDA and OpenVINO can be installed as alternatives to the
bundled DirectML for potentially better performance on NVIDIA or Intel GPUs.

## Easiest: install from darktable preferences

1. Open darktable preferences (Ctrl+,)
2. Go to the **AI** tab
3. Click **install** – darktable detects your GPU and downloads
   the correct ORT package automatically
4. Restart darktable

Alternatively, click **detect** to find a previously installed or
system-packaged ORT library.

## Installing via script

The unified install script reads a package manifest (`ort_gpu.json`)
to determine the correct download for your GPU. It detects your hardware
automatically and supports NVIDIA, AMD, and Intel GPUs.

The Linux script requires `jq` for JSON parsing (`sudo apt install jq`).

Scripts support `-y` / `--yes` (skip confirmation), `-f` / `--force`
(skip dependency checks), `--vendor <nvidia|amd|intel>` (skip GPU detection),
and `--manifest <path>` (custom manifest path).
PowerShell uses `-Yes`, `-Force`, and `-Vendor`.

Linux:
```bash
./tools/ai/install-ort-gpu.sh
```

Windows (PowerShell):
```powershell
.\tools\ai\install-ort-gpu.ps1
```

By default Windows blocks unsigned PowerShell scripts. If you see
"cannot be loaded because running scripts is disabled on this system"
or similar, unblock the script and invoke the interpreter directly
with a one-off bypass:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\ai\install-ort-gpu.ps1
```

The script will:
1. Detect available GPUs (NVIDIA via nvidia-smi, AMD via rocminfo, Intel via lspci)
2. Let you choose if multiple GPUs are found
3. Show package details (version, size, requirements)
4. Download and extract the matching ORT package
5. Print instructions for enabling it in darktable

### GPU-specific requirements

**NVIDIA (CUDA):**
- NVIDIA GPU with compute capability 6.0+ (GeForce GTX 1000 "Pascal" or newer)
- NVIDIA driver 525+
- CUDA 12.x toolkit
- cuDNN 9.x

**AMD (MIGraphX):**
- AMD GPU supported by ROCm (Radeon RX 6000+ or Instinct MI100+)
- ROCm 6.3 or later with MIGraphX
- See the ROCm-to-ORT version mapping in `ort_gpu.json`
- ROCm 7.1 / 7.2 wheels are manylinux-repaired and bundle their own
  ROCm runtime, so they work on any glibc-recent distro.
  ROCm 6.3 / 6.4 / 7.0 wheels rely on system-installed ROCm libraries
  with AMD's upstream SONAME versions (`librocm_smi64.so.7`,
  `libmigraphx_c.so.3`). Distros that rebuild ROCm with their own
  SONAMEs (e.g. Fedora ships `librocm_smi64.so.1`) will fail to load
  these wheels. On those distros, install AMD's upstream ROCm from the
  RHEL repo (see below) or build from source.

**Installing AMD's upstream ROCm on non-supported distros (e.g. Fedora):**

AMD's RPM repo targets RHEL / Rocky / Oracle Linux but works on Fedora
too. Don't install `amdgpu-dkms` – Fedora has in-tree `amdgpu`.

```bash
sudo tee /etc/yum.repos.d/rocm.repo > /dev/null <<'EOF'
[ROCm-7.2]
name=ROCm 7.2 (RHEL 10)
baseurl=https://repo.radeon.com/rocm/rhel10/7.2/main
enabled=1
priority=50
gpgcheck=1
gpgkey=https://repo.radeon.com/rocm/rocm.gpg.key
EOF
sudo rpm --import https://repo.radeon.com/rocm/rocm.gpg.key
sudo dnf install --nobest --skip-broken \
  rocm-hip-libraries rocm-smi-lib migraphx rccl

# AMD installs to a versioned prefix like /opt/rocm-7.2.0, and the
# RHEL 10 packages don't create the /opt/rocm symlink or register the
# lib dir with ldconfig on Fedora. do both explicitly:
latest=$(ls -d /opt/rocm-* 2>/dev/null | sort -V | tail -1)
sudo ln -snf "$latest" /opt/rocm
echo "$latest/lib" | sudo tee /etc/ld.so.conf.d/rocm.conf
sudo ldconfig

# verify – both should print a match
ldconfig -p | grep -E 'libmigraphx_c|librocm_smi64'
```

**Intel (OpenVINO):**
- Intel GPU (HD/UHD/Iris Xe integrated, or Arc A-series discrete)
- Intel GPU driver with OpenCL (`intel-opencl-icd`) and/or Level Zero (`level-zero`)
- OpenVINO runtime is bundled in the package

### AMD: building from source

If the prebuilt AMD package doesn't work (ABI mismatch, unsupported
ROCm version), build ORT from source:

```bash
./tools/ai/install-ort-amd-build.sh
```

This compiles ORT against your installed ROCm headers and libraries
(requires cmake 3.26+, gcc/g++, python3, git; takes 10-20 minutes).

## Enabling the custom ORT in darktable

After running the script or the built-in installer:

1. Open darktable preferences (Ctrl+,)
2. Go to the **AI** tab
3. Click **detect** to find the installed library automatically,
   or use the browse button to select it manually
4. Restart darktable

The `DT_ORT_LIBRARY` environment variable also works as an alternative:

```bash
DT_ORT_LIBRARY=~/.local/lib/onnxruntime-cuda/libonnxruntime.so.1.24.4 darktable
```

On Windows (PowerShell):
```powershell
$env:DT_ORT_LIBRARY="C:\Users\you\AppData\Local\onnxruntime-cuda\onnxruntime.dll"
darktable
```

If neither the preference nor the env var is set, darktable uses the
bundled ORT (CPU on Linux, DirectML on Windows, CoreML on macOS).

## Package manifest (`ort_gpu.json`)

The scripts and the built-in installer read `data/ort_gpu.json` to
determine download URLs, archive formats, and library patterns. This
manifest is shipped with darktable and can be updated without rebuilding.

Each entry specifies:
- `vendor` / `platform` / `arch` – matching criteria
- `url` – download URL
- `sha256` – SHA-256 checksum for integrity verification
- `format` – archive type (`tgz`, `zip`, `whl`)
- `lib_pattern` – filename prefix for library extraction
- `install_subdir` – target directory name under `~/.local/lib/`
- `rocm_min` / `rocm_max` – ROCm version range (AMD only)
- `size_mb` – approximate download size
- `requirements` – human-readable dependency list

## Manual installation (without scripts)

If you prefer to install manually:

1. **Get an ORT shared library with your desired EP compiled in:**
   - NVIDIA CUDA: download `onnxruntime-linux-x64-gpu-VERSION.tgz` (Linux)
     or `onnxruntime-win-x64-gpu-VERSION.zip` (Windows) from
     https://github.com/microsoft/onnxruntime/releases
   - AMD MIGraphX: download `onnxruntime_migraphx` (ROCm 7.x) or
     `onnxruntime_rocm` (ROCm 6.x) wheel from
     https://repo.radeon.com/rocm/manylinux/ (match your ROCm version)
     or build from source: `./build.sh --config Release --build_shared_lib --use_migraphx --migraphx_home /opt/rocm`
   - Intel OpenVINO: `pip download --no-deps onnxruntime-openvino`

2. **Extract the shared library:**
   - `.tgz`/`.zip`: extract `lib/libonnxruntime.so*` (or `lib/onnxruntime.dll`)
   - `.whl`: rename to `.zip` and extract `onnxruntime/capi/libonnxruntime.so*`
     and any `libonnxruntime_providers_*.so` files

3. **Point darktable to it** via preferences or env var (see above).

## Verifying

Run darktable with AI debug output to confirm which ORT is loaded:

```bash
darktable -d ai
```

Look for:
```
[darktable_ai] loaded ORT 1.24.4 from '/home/user/.local/lib/onnxruntime-cuda/libonnxruntime.so.1.24.4'
[darktable_ai] execution provider: CUDA
```

Then check Preferences → AI → execution provider to select your GPU
provider (CUDA, MIGraphX, OpenVINO, or auto).
