#
# Install ONNX Runtime with CUDA ExecutionProvider for darktable AI acceleration.
#
# Requirements:
#   - NVIDIA GPU with CUDA compute capability 6.0+
#   - CUDA 12.x runtime (driver 525+)
#   - cuDNN 9.x
#
# Usage: .\install-ort-nvidia.ps1 [-Yes] [-InstallDir <path>]

param(
    [switch]$Yes,
    [string]$InstallDir = "$env:LOCALAPPDATA\onnxruntime-cuda"
)

$ErrorActionPreference = "Stop"

$OrtVersion = "1.24.4"
$Package = "onnxruntime-win-x64-gpu-$OrtVersion"
$Url = "https://github.com/microsoft/onnxruntime/releases/download/v$OrtVersion/$Package.zip"

# --- Info & confirmation ---
Write-Host ""
Write-Host "ONNX Runtime $OrtVersion - CUDA ExecutionProvider installer"
Write-Host "================================================================"
Write-Host ""
Write-Host "This will download and install a GPU-accelerated ONNX Runtime build"
Write-Host "to enable NVIDIA CUDA acceleration for darktable AI features"
Write-Host "(denoise, upscale, segmentation)."
Write-Host ""
Write-Host "Requirements:"
Write-Host "  - NVIDIA GPU with compute capability 6.0+ (Pascal or newer)"
Write-Host "  - NVIDIA driver 525+ with CUDA 12.x support"
Write-Host "  - cuDNN 9.x (download from https://developer.nvidia.com/cudnn-downloads)"
Write-Host ""
Write-Host "Actions:"
Write-Host "  - Download prebuilt package from GitHub (~200 MB)"
Write-Host "    $Url"
Write-Host "  - Install to: $InstallDir"
Write-Host ""

if (-not $Yes) {
    $answer = Read-Host "Continue? [y/N]"
    if ($answer -notmatch '^[Yy]') {
        Write-Host "Aborted."
        exit 0
    }
    Write-Host ""
}

# --- Check NVIDIA driver ---
$nvidiaSmi = Get-Command nvidia-smi -ErrorAction SilentlyContinue
if (-not $nvidiaSmi) {
    Write-Host "Warning: nvidia-smi not found - NVIDIA driver may not be installed."
    Write-Host ""
    Write-Host "  Download NVIDIA driver from: https://www.nvidia.com/drivers"
    Write-Host "  A reboot is typically required after driver installation."
    Write-Host "  Re-run this script afterwards."
    Write-Host ""
    exit 1
}

$driverVersion = (nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>$null | Select-Object -First 1).Trim()
Write-Host "NVIDIA driver: $driverVersion"

# --- Check CUDA toolkit ---
$nvcc = Get-Command nvcc -ErrorAction SilentlyContinue
if ($nvcc) {
    $cudaVersion = (nvcc --version | Select-String 'release (\d+\.\d+)').Matches.Groups[1].Value
    Write-Host "CUDA toolkit: $cudaVersion"
} else {
    Write-Host ""
    Write-Host "Note: CUDA toolkit not found. ORT bundles its own CUDA runtime libraries,"
    Write-Host "      but installing the toolkit ensures full compatibility."
    Write-Host ""
    Write-Host "  Download from: https://developer.nvidia.com/cuda-downloads"
    Write-Host ""
}

# --- Check cuDNN ---
$cudnnFound = $false
$cudnnPaths = @(
    "$env:CUDA_PATH\bin\cudnn*.dll",
    "$env:ProgramFiles\NVIDIA\CUDNN\*\bin\cudnn*.dll"
)
foreach ($pattern in $cudnnPaths) {
    if (Test-Path $pattern) {
        $cudnnDll = (Get-Item $pattern | Select-Object -First 1).FullName
        Write-Host "cuDNN: $cudnnDll"
        $cudnnFound = $true
        break
    }
}
if (-not $cudnnFound) {
    Write-Host ""
    Write-Host "Warning: cuDNN not found. CUDA EP requires cuDNN 9.x."
    Write-Host ""
    Write-Host "  Download from: https://developer.nvidia.com/cudnn-downloads"
    Write-Host ""
    Write-Host "  You can install cuDNN after this script finishes - darktable will"
    Write-Host "  detect it at startup."
    Write-Host ""
}

# --- Download ---
$tmpDir = Join-Path $env:TEMP "ort-cuda-install"
if (Test-Path $tmpDir) { Remove-Item -Recurse -Force $tmpDir }
New-Item -ItemType Directory -Path $tmpDir | Out-Null

$zipPath = Join-Path $tmpDir "ort-gpu.zip"

Write-Host "Downloading ORT $OrtVersion with CUDA EP..."
try {
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest -Uri $Url -OutFile $zipPath -UseBasicParsing
} catch {
    Write-Host "Error: download failed from $Url" -ForegroundColor Red
    Write-Host $_.Exception.Message
    exit 1
}

# --- Install ---
Write-Host "Extracting..."
Expand-Archive -Path $zipPath -DestinationPath $tmpDir -Force

if (-not (Test-Path $InstallDir)) {
    New-Item -ItemType Directory -Path $InstallDir | Out-Null
}

$srcLib = Join-Path $tmpDir $Package "lib"
Copy-Item "$srcLib\*.dll" -Destination $InstallDir -Force

# Clean up
Remove-Item -Recurse -Force $tmpDir

$ortDll = Get-Item "$InstallDir\onnxruntime.dll" -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "Done. Installed to: $InstallDir"
Get-ChildItem "$InstallDir\*.dll" | Format-Table Name, Length -AutoSize
Write-Host ""
Write-Host "To use with darktable:"
Write-Host ""
Write-Host "  `$env:DT_ORT_LIBRARY=`"$($ortDll.FullName)`"; darktable"
Write-Host ""
Write-Host "Or set it permanently:"
Write-Host ""
Write-Host "  [Environment]::SetEnvironmentVariable('DT_ORT_LIBRARY', '$($ortDll.FullName)', 'User')"
