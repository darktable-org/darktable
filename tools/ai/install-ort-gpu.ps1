<#
.SYNOPSIS
    Install GPU-accelerated ONNX Runtime for darktable.

.DESCRIPTION
    Reads the package manifest (data/ort_gpu.json) to determine the
    correct download URL for the detected GPU, downloads the ORT
    package, and installs the libraries to
    %LOCALAPPDATA%\onnxruntime-<vendor>\.

    For NVIDIA, additionally bundles cuDNN and the required CUDA
    toolkit DLLs (cublas, cublasLt, cudart, cufft, curand) from the
    user's installed CUDA toolkit so the resulting directory is
    self-contained.

    Supported vendors on Windows:
      - NVIDIA via CUDA EP   (requires CUDA 12.x or 13.x + cuDNN 9.x)
      - Intel  via OpenVINO  (requires Intel GPU driver)

    GPU auto-detection uses:
      - nvidia-smi                       (NVIDIA)
      - Get-CimInstance Win32_VideoController (Intel)

    After installing, point darktable at the new library:
      Preferences -> AI -> ONNX Runtime library -> "detect" or "browse"
    Restart darktable to apply.  Or set DT_ORT_LIBRARY=<path> in the
    environment.

.PARAMETER Yes
    Skip the interactive "Continue?" prompt.

.PARAMETER Force
    Skip the vendor-specific dependency check (CUDA toolkit, cuDNN).
    The download proceeds regardless; if dependencies are missing at
    runtime, ORT will fall back to CPU.

.PARAMETER Vendor
    Force a specific GPU vendor and skip auto-detection.
    Valid values: "nvidia", "intel".

.PARAMETER Manifest
    Use a custom ort_gpu.json manifest.  Default search order:
        <script>\..\..\data\ort_gpu.json
        <script>\..\..\share\darktable\ort_gpu.json
        $env:ProgramFiles\darktable\share\darktable\ort_gpu.json
        $env:LOCALAPPDATA\darktable\share\darktable\ort_gpu.json

.EXAMPLE
    .\install-ort-gpu.ps1
    Detect the GPU, prompt for confirmation, install.

.EXAMPLE
    .\install-ort-gpu.ps1 -Yes
    Same as above without confirmation.

.EXAMPLE
    .\install-ort-gpu.ps1 -Vendor nvidia -Yes
    Install the NVIDIA package without GPU detection.

.EXAMPLE
    .\install-ort-gpu.ps1 -Force -Yes
    Install without checking dependencies.
#>
param(
    [switch]$Yes,
    [switch]$Force,
    [string]$Vendor = "",
    [string]$Manifest = "",
    [switch]$Help
)

if ($Help) {
    Get-Help $MyInvocation.MyCommand.Path -Detailed
    exit 0
}

$ErrorActionPreference = "Stop"

# --- Locate manifest ---
if (-not $Manifest) {
    $ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
    $candidates = @(
        (Join-Path $ScriptDir "..\..\data\ort_gpu.json"),
        (Join-Path $ScriptDir "..\..\share\darktable\ort_gpu.json"),
        (Join-Path $env:ProgramFiles "darktable\share\darktable\ort_gpu.json"),
        (Join-Path $env:LOCALAPPDATA "darktable\share\darktable\ort_gpu.json")
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $Manifest = $c; break }
    }
    if (-not $Manifest) {
        Write-Host "Error: cannot find ort_gpu.json manifest." -ForegroundColor Red
        Write-Host "  Use -Manifest <path> to specify it manually."
        exit 1
    }
}

if (-not (Test-Path $Manifest)) {
    Write-Host "Error: manifest not found: $Manifest" -ForegroundColor Red
    exit 1
}

$Platform = "windows"
$Arch = "x86_64"

# --- Load manifest ---
$ManifestData = Get-Content $Manifest -Raw | ConvertFrom-Json

# --- Detect GPU (skipped with -Vendor) ---
$Vendors = @()
$Selected = $null

if ($Vendor) {
    if ($Vendor -notin @("nvidia", "intel")) {
        Write-Host "Error: unknown vendor '$Vendor'. Use: nvidia, intel" -ForegroundColor Red
        exit 1
    }
    Write-Host "Vendor override: $Vendor (skipping GPU detection)"
    $Selected = @{ vendor = $Vendor; label = $Vendor; driver = "" }
} else {
    # NVIDIA
    $NvidiaSmi = Get-Command nvidia-smi -ErrorAction SilentlyContinue
    if ($NvidiaSmi) {
        $NvidiaInfo = (nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>$null |
                       Select-Object -First 1)
        if ($NvidiaInfo) {
            $fields = $NvidiaInfo -split ","
            $Vendors += @{
                vendor = "nvidia"
                label = $fields[0].Trim()
                driver = if ($fields.Count -gt 1) { $fields[1].Trim() } else { "unknown" }
            }
        }
    }

    # Intel
    $IntelGPU = Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match 'Intel' }
    if ($IntelGPU) {
        $Vendors += @{
            vendor = "intel"
            label = $IntelGPU.Name
            driver = $IntelGPU.DriverVersion
        }
    }

    if ($Vendors.Count -eq 0) {
        Write-Host ""
        Write-Host "No supported GPU detected." -ForegroundColor Red
        Write-Host ""
        Write-Host "Supported: NVIDIA (CUDA), Intel (OpenVINO)"
        Write-Host "Ensure GPU drivers are installed, or use -Vendor <nvidia|intel>."
        Write-Host ""
        exit 1
    }

    # --- Handle multiple vendors ---
    if ($Vendors.Count -eq 1) {
        $Selected = $Vendors[0]
    } else {
        Write-Host ""
        Write-Host "Multiple GPUs detected:"
        for ($i = 0; $i -lt $Vendors.Count; $i++) {
            $v = $Vendors[$i]
            $ep = switch ($v.vendor) { "nvidia" { "CUDA" } "intel" { "OpenVINO" } }
            Write-Host "  $($i+1)) $($v.label) ($($v.driver)) [$ep]"
        }
        Write-Host ""
        $choice = Read-Host "Select GPU [1-$($Vendors.Count)]"
        $idx = [int]$choice - 1
        if ($idx -lt 0 -or $idx -ge $Vendors.Count) {
            Write-Host "Invalid selection." -ForegroundColor Red
            exit 1
        }
        $Selected = $Vendors[$idx]
    }
}

# --- Detect CUDA version for NVIDIA package matching ---
$CudaMM = ""
if ($Selected.vendor -eq "nvidia") {
    $nvcc = Get-Command nvcc -ErrorAction SilentlyContinue
    if ($nvcc) {
        $nvccOut = nvcc --version 2>$null | Select-String 'V(\d+\.\d+)' |
            ForEach-Object { $_.Matches[0].Groups[1].Value }
        if ($nvccOut) { $CudaMM = $nvccOut }
    }
}

# --- Find matching package in manifest ---
$Package = $null
foreach ($p in $ManifestData.packages) {
    if ($p.vendor -ne $Selected.vendor) { continue }
    if ($p.platform -ne $Platform) { continue }
    if ($p.arch -and $p.arch -ne $Arch) { continue }
    # NVIDIA: match CUDA version range
    if ($CudaMM -and $p.cuda_min) {
        if ([version]$CudaMM -lt [version]$p.cuda_min) { continue }
        if ($p.cuda_max -and [version]$CudaMM -gt [version]$p.cuda_max) { continue }
    }
    $Package = $p
    break
}

if (-not $Package) {
    Write-Host ""
    Write-Host "Error: no matching package found for $($Selected.vendor)/$Platform/$Arch" -ForegroundColor Red
    if ($CudaMM) { Write-Host "  CUDA version: $CudaMM" }
    exit 1
}

$InstallDir = Join-Path $env:LOCALAPPDATA $Package.install_subdir

# --- Info & confirmation ---
Write-Host ""
Write-Host "ONNX Runtime $($Package.ort_version) - GPU acceleration installer"
Write-Host "============================================================"
Write-Host ""
Write-Host "GPU: $($Selected.label)"
if ($Selected.driver) { Write-Host "Driver: $($Selected.driver)" }
Write-Host "ORT version: $($Package.ort_version)"
Write-Host "Download size: ~$($Package.size_mb) MB"
Write-Host "Install to: $InstallDir"
if ($Package.requirements) { Write-Host "Requirements: $($Package.requirements)" }
Write-Host ""

if (-not $Yes) {
    $answer = Read-Host "Continue? [y/N]"
    if ($answer -notmatch '^[Yy]') {
        Write-Host "Aborted."
        exit 0
    }
    Write-Host ""
}

# --- Download ---
$TempDir = Join-Path $env:TEMP "ort-gpu-$(Get-Random)"
New-Item -ItemType Directory -Path $TempDir -Force | Out-Null

$ext = switch ($Package.format) { "zip" { ".zip" } "whl" { ".zip" } "tgz" { ".tgz" } default { "" } }
$ArchivePath = Join-Path $TempDir "ort-package$ext"

Write-Host "Downloading..."
try {
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest -Uri $Package.url -OutFile $ArchivePath -UseBasicParsing
} catch {
    Write-Host "Error: download failed." -ForegroundColor Red
    Write-Host "  URL: $($Package.url)"
    Write-Host "  $($_.Exception.Message)"
    Remove-Item -Recurse -Force $TempDir
    exit 1
}

# --- Verify checksum ---
if ($Package.sha256) {
    Write-Host "Verifying checksum..."
    $ActualHash = (Get-FileHash -Path $ArchivePath -Algorithm SHA256).Hash.ToLower()
    if ($ActualHash -ne $Package.sha256) {
        Write-Host "Error: checksum mismatch!" -ForegroundColor Red
        Write-Host "  Expected: $($Package.sha256)"
        Write-Host "  Got:      $ActualHash"
        Remove-Item -Recurse -Force $TempDir
        exit 1
    }
    Write-Host "Checksum OK."
}

# --- Extract ---
Write-Host "Extracting..."
New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null

# clean prior install of the same library family so old versioned DLLs
# don't shadow the new install. matches the shell-script behavior on
# Linux: remove ${libPattern}*.dll before extracting the new package
$libPattern = $Package.lib_pattern
Get-ChildItem -Path $InstallDir -Filter "${libPattern}*.dll" -File -ErrorAction SilentlyContinue |
    Remove-Item -Force -ErrorAction SilentlyContinue
foreach ($extra in @($Package.lib_extra_patterns)) {
    if ($extra) {
        Get-ChildItem -Path $InstallDir -Filter "${extra}*.dll" -File -ErrorAction SilentlyContinue |
            Remove-Item -Force -ErrorAction SilentlyContinue
    }
}

$ExtractDir = Join-Path $TempDir "extracted"

switch ($Package.format) {
    "zip" {
        Expand-Archive -Path $ArchivePath -DestinationPath $ExtractDir -Force
    }
    "whl" {
        # wheels are zip files
        Expand-Archive -Path $ArchivePath -DestinationPath $ExtractDir -Force
    }
    "tgz" {
        # PowerShell doesn't natively handle tgz; use tar if available
        $tar = Get-Command tar -ErrorAction SilentlyContinue
        if ($tar) {
            New-Item -ItemType Directory -Path $ExtractDir -Force | Out-Null
            tar xzf $ArchivePath -C $ExtractDir
        } else {
            Write-Host "Error: tar not found, cannot extract .tgz" -ForegroundColor Red
            Remove-Item -Recurse -Force $TempDir
            exit 1
        }
    }
    default {
        Write-Host "Error: unsupported format: $($Package.format)" -ForegroundColor Red
        Remove-Item -Recurse -Force $TempDir
        exit 1
    }
}

# Copy matching libraries
$copied = 0
Get-ChildItem -Path $ExtractDir -Recurse -File |
    Where-Object { $_.Name -like "${libPattern}*" -and $_.Extension -eq ".dll" } |
    ForEach-Object {
        Copy-Item $_.FullName -Destination $InstallDir -Force
        $copied++
    }

# Clean up
Remove-Item -Recurse -Force $TempDir

if ($copied -eq 0) {
    Write-Host "Error: no libraries found after extraction." -ForegroundColor Red
    exit 1
}

# --- Bundle runtime dependencies from a separate wheel (e.g. OpenVINO) ---
if ($Package.runtime_url) {
    Write-Host "Downloading runtime dependencies..."
    $rtTempDir = Join-Path $env:TEMP "ort-runtime-$(Get-Random)"
    New-Item -ItemType Directory -Path $rtTempDir -Force | Out-Null
    $rtArchive = Join-Path $rtTempDir "runtime.zip"

    try {
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $Package.runtime_url -OutFile $rtArchive -UseBasicParsing
    } catch {
        Write-Host "Warning: runtime download failed: $($_.Exception.Message)" -ForegroundColor Yellow
        Remove-Item -Recurse -Force $rtTempDir
    }

    if (Test-Path $rtArchive) {
        # verify checksum
        $rtOk = $true
        if ($Package.runtime_sha256) {
            $rtHash = (Get-FileHash -Path $rtArchive -Algorithm SHA256).Hash.ToLower()
            if ($rtHash -ne $Package.runtime_sha256) {
                Write-Host "Warning: runtime checksum mismatch, skipping." -ForegroundColor Yellow
                $rtOk = $false
            }
        }
        if ($rtOk) {
            $rtExtract = Join-Path $rtTempDir "extracted"
            Expand-Archive -Path $rtArchive -DestinationPath $rtExtract -Force
            $rtPattern = if ($Package.runtime_lib_pattern) { $Package.runtime_lib_pattern } else { "openvino" }
            $rtCopied = 0
            Get-ChildItem -Path $rtExtract -Recurse -File |
                Where-Object { $_.Extension -eq ".dll" } |
                Where-Object { $_.Name -like "${rtPattern}*" -or $_.Name -like "tbb*.dll" } |
                ForEach-Object {
                    Copy-Item $_.FullName -Destination $InstallDir -Force
                    $rtCopied++
                }
            if ($rtCopied -gt 0) {
                Write-Host "Bundled $rtCopied runtime DLLs."
            }
        }
        Remove-Item -Recurse -Force $rtTempDir
    }
}

# --- Bundle cuDNN DLLs for NVIDIA (avoids PATH issues) ---
if ($Selected.vendor -eq "nvidia") {
    # determine CUDA major version to pick the right cuDNN subdirectory
    $cudaMajor = ""
    if ($CudaMM) { $cudaMajor = ($CudaMM -split '\.')[0] }

    $cudnnSrc = $null
    $cudnnRoot = Join-Path $env:ProgramFiles "NVIDIA\CUDNN"
    if (Test-Path $cudnnRoot) {
        # find the cuDNN bin subdirectory matching our CUDA major version
        # layout: CUDNN\v9.20\bin\13.2\x64\cudnn64_9.dll
        Get-ChildItem -Path $cudnnRoot -Directory | ForEach-Object {
            $binDir = Join-Path $_.FullName "bin"
            if (Test-Path $binDir) {
                Get-ChildItem -Path $binDir -Directory | ForEach-Object {
                    if ($cudaMajor -and $_.Name.StartsWith($cudaMajor)) {
                        $x64 = Join-Path $_.FullName "x64"
                        if (Test-Path $x64) { $cudnnSrc = $x64 }
                        else { $cudnnSrc = $_.FullName }
                    }
                }
            }
        }
        # fallback: find any cudnn64_*.dll recursively
        if (-not $cudnnSrc) {
            $fallback = Get-ChildItem -Path $cudnnRoot -Filter "cudnn64_*.dll" -Recurse -ErrorAction SilentlyContinue |
                Select-Object -First 1
            if ($fallback) { $cudnnSrc = $fallback.DirectoryName }
        }
    }
    # also check CUDA_PATH\bin
    if (-not $cudnnSrc -and $env:CUDA_PATH) {
        $cudaBin = Join-Path $env:CUDA_PATH "bin"
        if (Get-ChildItem -Path $cudaBin -Filter "cudnn64_*.dll" -ErrorAction SilentlyContinue |
            Select-Object -First 1) { $cudnnSrc = $cudaBin }
    }

    if ($cudnnSrc) {
        $cudnnCopied = 0
        Get-ChildItem -Path $cudnnSrc -Filter "cudnn*.dll" | ForEach-Object {
            Copy-Item $_.FullName -Destination $InstallDir -Force
            $cudnnCopied++
        }
        if ($cudnnCopied -gt 0) {
            Write-Host "Bundled $cudnnCopied cuDNN DLLs from: $cudnnSrc"
        }
    } else {
        Write-Host "Warning: cuDNN not found, CUDA EP may fail at runtime." -ForegroundColor Yellow
    }

    # bundle required CUDA runtime DLLs from toolkit
    if ($env:CUDA_PATH) {
        $cudaCopied = 0
        $cudaLibs = @("cublas*.dll", "cublasLt*.dll", "cudart*.dll",
                       "cufft*.dll", "curand*.dll")
        # CUDA 13+ puts DLLs in bin\x64\, older in bin\
        @("bin\x64", "bin") | ForEach-Object {
            $dir = Join-Path $env:CUDA_PATH $_
            if (Test-Path $dir) {
                foreach ($pattern in $cudaLibs) {
                    Get-ChildItem -Path $dir -Filter $pattern -ErrorAction SilentlyContinue | ForEach-Object {
                        if (-not (Test-Path (Join-Path $InstallDir $_.Name))) {
                            Copy-Item $_.FullName -Destination $InstallDir -Force
                            $cudaCopied++
                        }
                    }
                }
            }
        }
        if ($cudaCopied -gt 0) {
            Write-Host "Bundled $cudaCopied CUDA runtime DLLs from toolkit."
        }
    }
}

# --- Verify ---
$ortDll = Get-ChildItem "$InstallDir\$libPattern*" -File |
    Where-Object { $_.Extension -eq ".dll" } |
    Select-Object -First 1

Write-Host ""
Write-Host "Done. Installed to: $InstallDir"
Get-ChildItem "$InstallDir\*" -File | Format-Table Name, Length -AutoSize
Write-Host ""
Write-Host "To enable in darktable:"
Write-Host ""
Write-Host "  1. Open darktable preferences (Ctrl+,)"
Write-Host "  2. Go to the AI tab"
Write-Host "  3. Click 'detect' to find the installed library automatically,"
Write-Host "     or set 'ONNX Runtime library' to:"
Write-Host "     $($ortDll.FullName)"
Write-Host "  4. Restart darktable"
Write-Host ""
Write-Host "Or via command line:"
Write-Host ""
Write-Host "  `$env:DT_ORT_LIBRARY=`"$($ortDll.FullName)`"; darktable"
