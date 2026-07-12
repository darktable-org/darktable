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

.PARAMETER Ep
    Force a specific execution provider, skipping auto-detection.
    One EP per install. Values:

      cuda12    NVIDIA, CUDA 12 (onnxruntime-gpu from PyPI)
      cuda13    NVIDIA, CUDA 13 (onnxruntime release zip from GitHub)
      openvino  Intel,  OpenVINO (onnxruntime-openvino from PyPI)

    AMD ROCm / MIGraphX are not available on Windows. Without this flag
    the script auto-detects vendor (nvidia-smi / Win32_VideoController)
    and CUDA version (nvcc), and prompts when more than one GPU vendor
    is present.

.PARAMETER Manifest
    Use a custom ort_gpu.json manifest. Without this flag the script
    picks the first found:
      1. <script>\..\..\{data,share\darktable}\ort_gpu.json
         (source checkout or installed-alongside-script layout)
      2. $env:ProgramFiles\darktable\share\darktable\ort_gpu.json
      3. $env:LOCALAPPDATA\darktable\share\darktable\ort_gpu.json
         (system or per-user installs of darktable)
      4. https://raw.githubusercontent.com/.../master/data/ort_gpu.json
         (fetched as fallback for the `irm ... | iex` one-liner)

.EXAMPLE
    .\install-ort-gpu.ps1
    Detect the GPU, prompt for confirmation, install.

.EXAMPLE
    .\install-ort-gpu.ps1 -Yes
    Same as above without confirmation.

.EXAMPLE
    .\install-ort-gpu.ps1 -Ep cuda13 -Yes
    Install the NVIDIA CUDA 13 package without GPU detection.
#>
param(
    [switch]$Yes,
    [ValidateSet("", "cuda12", "cuda13", "openvino")]
    [string]$Ep = "",
    [string]$Manifest = "",
    [switch]$Help
)

# Map -Ep to internal vendor + CUDA version overrides.
$Vendor = ""
$CudaForce = ""
switch ($Ep) {
    "cuda12"   { $Vendor = "nvidia"; $CudaForce = "12" }
    "cuda13"   { $Vendor = "nvidia"; $CudaForce = "13" }
    "openvino" { $Vendor = "intel" }
}

if ($Help) {
    Get-Help $MyInvocation.MyCommand.Path -Detailed
    exit 0
}

$ErrorActionPreference = "Stop"

# --- Locate manifest ---
if (-not $Manifest) {
    # MyInvocation.MyCommand.Path is null when run via irm | iex (no file on disk)
    $ScriptPath = $MyInvocation.MyCommand.Path
    $candidates = @()
    if ($ScriptPath) {
        $ScriptDir = Split-Path -Parent $ScriptPath
        $candidates += @(
            (Join-Path $ScriptDir "..\..\data\ort_gpu.json"),
            (Join-Path $ScriptDir "..\..\share\darktable\ort_gpu.json")
        )
    }
    $candidates += @(
        (Join-Path $env:ProgramFiles "darktable\share\darktable\ort_gpu.json"),
        (Join-Path $env:LOCALAPPDATA "darktable\share\darktable\ort_gpu.json")
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $Manifest = $c; break }
    }
    if (-not $Manifest) {
        # No local copy found; fetch from GitHub so the script works when
        # downloaded and run directly from a raw GitHub URL.
        $GhManifestUrl = "https://raw.githubusercontent.com/darktable-org/darktable/refs/heads/master/data/ort_gpu.json"
        $tmp = Join-Path ([System.IO.Path]::GetTempPath()) "ort_gpu_$(New-Guid).json"
        try {
            Invoke-WebRequest -Uri $GhManifestUrl -OutFile $tmp -UseBasicParsing
            $Manifest = $tmp
        } catch {
            Write-Host "Error: cannot find or download ort_gpu.json." -ForegroundColor Red
            Write-Host "  Use -Manifest <path> to specify it manually."
            exit 1
        }
    }
}

if (-not (Test-Path $Manifest)) {
    Write-Host "Error: manifest not found: $Manifest" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "ONNX Runtime GPU acceleration installer"
Write-Host "========================================"

$Platform = "windows"
$Arch = "x86_64"

# --- Load manifest ---
$ManifestData = Get-Content $Manifest -Raw | ConvertFrom-Json
$MinOrtVersion = $ManifestData.min_ort_version

# Resolve a PyPI source block to (url, sha256, version, size_mb).
function Resolve-PypiSource($source) {
    $url = "https://pypi.org/pypi/$($source.package)/json"
    Write-Host "Resolving latest $($source.package) ..."
    try {
        $data = Invoke-RestMethod -Uri $url -UseBasicParsing
    } catch {
        Write-Host "Error: failed to query PyPI for $($source.package)" -ForegroundColor Red
        exit 1
    }
    $ver = $data.info.version
    if ($MinOrtVersion -and ([version]$ver -lt [version]$MinOrtVersion)) {
        Write-Host "Error: PyPI $($source.package) latest ($ver) is below min_ort_version ($MinOrtVersion)" -ForegroundColor Red
        exit 1
    }
    $pythonTag = if ($source.python_tag) { $source.python_tag } else { "cp312" }
    $files = $data.releases.$ver

    $wheel = $null
    foreach ($tag in $source.platform_tags) {
        $wheel = $files | Where-Object {
            $_.packagetype -eq "bdist_wheel" -and
            $_.filename -like "*$pythonTag*" -and
            $_.filename -like "*$tag*"
        } | Select-Object -First 1
        if ($wheel) { break }
    }

    if (-not $wheel) {
        Write-Host "Error: no matching wheel in $($source.package) $ver" -ForegroundColor Red
        exit 1
    }

    return @{
        url = $wheel.url
        sha256 = $wheel.digests.sha256
        version = $ver
        size_mb = [int]([math]::Ceiling($wheel.size / 1MB / 10) * 10)
    }
}

# Resolve a github_release source block. SHA is empty — GitHub doesn't
# publish per-asset SHAs in the API response; file-magic check catches
# HTML error pages at extract time.
function Resolve-GitHubReleaseSource($source) {
    $apiUrl = "https://api.github.com/repos/$($source.repo)/releases/latest"
    Write-Host "Resolving latest $($source.repo) ..."

    $headers = @{ 'Accept' = 'application/vnd.github+json' }
    if ($env:GITHUB_TOKEN) { $headers['Authorization'] = "Bearer $env:GITHUB_TOKEN" }

    try {
        $data = Invoke-RestMethod -Uri $apiUrl -Headers $headers -UseBasicParsing
    } catch {
        $code = $null
        if ($_.Exception.Response) { $code = [int]$_.Exception.Response.StatusCode }
        if ($code -eq 403 -or $code -eq 429) {
            $installSubdir = if ($Package.install_subdir) { $Package.install_subdir } else { 'onnxruntime' }
            $hintDir = Join-Path $env:LOCALAPPDATA $installSubdir
            Write-Host @"

GitHub API rate limit hit (60 req/hr per IP, unauthenticated).

You're seeing this because the install script needs api.github.com to
discover the latest ONNX Runtime release. Shared NAT (corporate networks,
VPNs, cloud VMs) often blows through 60/hr from one source IP.

Workarounds, easiest first:

  1. Wait ~1 hour for the limit to reset and re-run this script.

  2. Set GITHUB_TOKEN with any personal access token (no scopes
     required for public repos) - bumps the limit to 5000 req/hr:
       `$env:GITHUB_TOKEN = 'ghp_yourtoken'
       .\install-ort-gpu.ps1

  3. Install manually - easy for this package:
       a. Open https://github.com/$($source.repo)/releases/latest in a browser
       b. Find the asset named like: $($source.asset_pattern)
          (where {version} is whatever release tag is shown at the top)
       c. Download it (zip, no auth needed)
       d. Expand-Archive <file>.zip -DestinationPath .
       e. Copy the extracted lib\onnxruntime.dll (and LICENSE,
          ThirdPartyNotices.txt from the extracted root) into:
            $hintDir
       f. Open darktable -> Preferences -> AI tab -> point at the .dll

"@ -ForegroundColor Yellow
        } else {
            Write-Host "Error: GitHub API request failed: $($_.Exception.Message)" -ForegroundColor Red
        }
        exit 1
    }

    $tag = $data.tag_name
    $ver = $tag -replace '^v', ''

    if ($MinOrtVersion -and ([version]$ver -lt [version]$MinOrtVersion)) {
        Write-Host "Error: latest $($source.repo) release ($ver) is below min_ort_version ($MinOrtVersion)" -ForegroundColor Red
        exit 1
    }

    $expected = $source.asset_pattern -replace '\{version\}', $ver
    $asset = $data.assets | Where-Object { $_.name -eq $expected } | Select-Object -First 1
    if (-not $asset) {
        Write-Host "Error: no asset named '$expected' in release $tag of $($source.repo)" -ForegroundColor Red
        exit 1
    }

    # GitHub's `digest` field arrived in 2024; older releases may not have it,
    # in which case sha256 stays empty and the file-magic check kicks in.
    $sha = ""
    if ($asset.digest -and $asset.digest.StartsWith("sha256:")) {
        $sha = $asset.digest.Substring(7)
    }

    return @{
        url = $asset.browser_download_url
        sha256 = $sha
        version = $ver
        size_mb = [int]([math]::Ceiling($asset.size / 1MB / 10) * 10)
    }
}

# --- Detect GPU (skipped with -Ep) ---
$Vendors = @()
$Selected = $null

if ($Vendor) {
    Write-Host "EP override: forcing $Ep (skipping GPU detection)"
    $Selected = @{ vendor = $Vendor; label = $Vendor; driver = "" }
} else {
    # NVIDIA
    $NvidiaSmi = Get-Command nvidia-smi -ErrorAction SilentlyContinue
    if ($NvidiaSmi) {
        # nvidia-smi writes "driver not loaded" to stdout; require CSV
        $NvidiaInfo = (nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>$null |
                       Select-Object -First 1)
        if ($NvidiaInfo -and $NvidiaInfo.Contains(",")) {
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
        Write-Host "Ensure GPU drivers are installed, or use -Ep <cuda12|cuda13|openvino>."
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
# Apply EP-driven CUDA override (set by -Ep cuda12/cuda13).
if ($CudaForce -and $Selected.vendor -eq "nvidia") { $CudaMM = "$CudaForce.0" }

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

# Resolve source blocks to concrete URL/SHA/version at install time.
if ($Package.source) {
    $r = switch ($Package.source.type) {
        'pypi'           { Resolve-PypiSource          $Package.source }
        'github_release' { Resolve-GitHubReleaseSource $Package.source }
        default { Write-Host "Error: unknown source type '$($Package.source.type)'" -ForegroundColor Red; exit 1 }
    }
    $Package | Add-Member -NotePropertyName url          -NotePropertyValue $r.url     -Force
    $Package | Add-Member -NotePropertyName sha256       -NotePropertyValue $r.sha256  -Force
    $Package | Add-Member -NotePropertyName ort_version  -NotePropertyValue $r.version -Force
    $Package | Add-Member -NotePropertyName size_mb      -NotePropertyValue $r.size_mb -Force
}
if ($Package.runtime_source -and $Package.runtime_source.type -eq "pypi") {
    $r = Resolve-PypiSource $Package.runtime_source
    $Package | Add-Member -NotePropertyName runtime_url    -NotePropertyValue $r.url    -Force
    $Package | Add-Member -NotePropertyName runtime_sha256 -NotePropertyValue $r.sha256 -Force
}

$InstallDir = Join-Path $env:LOCALAPPDATA $Package.install_subdir

# --- Info & confirmation ---
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
} else {
    # GitHub source has no published SHA; catch HTML error pages with file magic
    $magic = [System.IO.File]::ReadAllBytes($ArchivePath)[0..3]
    $isZip = ($magic[0] -eq 0x50 -and $magic[1] -eq 0x4B -and $magic[2] -eq 0x03 -and $magic[3] -eq 0x04)
    $isGz  = ($magic[0] -eq 0x1F -and $magic[1] -eq 0x8B)
    $ok = switch ($Package.format) {
        'zip' { $isZip } 'whl' { $isZip } 'tgz' { $isGz } default { $true }
    }
    if (-not $ok) {
        Write-Host "Error: download is not a $($Package.format) (file magic mismatch)" -ForegroundColor Red
        Remove-Item -Recurse -Force $TempDir
        exit 1
    }
}

# --- Extract ---
Write-Host "Extracting..."

# preserve_layout: keep the archive's relative paths (required for wheels
# whose DLLs reference sibling files via relative paths).
# flat (default): copy matched DLLs straight into the install dir.
$preserveLayout = [bool]$Package.preserve_layout
$libPattern = $Package.lib_pattern
$libPatterns = @($libPattern) + @($Package.lib_extra_patterns | Where-Object { $_ })

# nuke any prior install — preserve_layout creates subdirs that a
# per-pattern depth-1 clean wouldn't catch
if ($preserveLayout) {
    if (Test-Path $InstallDir) {
        Remove-Item -Path $InstallDir -Recurse -Force
    }
}
New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null

if (-not $preserveLayout) {
    # flat install: drop old versioned DLLs of the same family so they
    # don't shadow the new install
    foreach ($pat in $libPatterns) {
        Get-ChildItem -Path $InstallDir -Filter "${pat}*.dll" -File -ErrorAction SilentlyContinue |
            Remove-Item -Force -ErrorAction SilentlyContinue
    }
}

$ExtractDir = Join-Path $TempDir "extracted"

# copy DLLs matching any of $patterns from $searchRoot into $InstallDir,
# either flat or preserving relative paths
function Copy-MatchingLibs([string]$searchRoot, [string[]]$patterns, [bool]$preserve) {
    $count = 0
    Get-ChildItem -Path $searchRoot -Recurse -File |
        Where-Object { $_.Extension -eq ".dll" } |
        Where-Object {
            $name = $_.Name
            $null -ne ($patterns | Where-Object { $name -like "${_}*" } | Select-Object -First 1)
        } |
        ForEach-Object {
            if ($preserve) {
                $rel = $_.FullName.Substring($searchRoot.Length).TrimStart('\','/')
                $dest = Join-Path $InstallDir $rel
                $destDir = Split-Path -Parent $dest
                if (-not (Test-Path $destDir)) { New-Item -ItemType Directory -Path $destDir -Force | Out-Null }
                Copy-Item $_.FullName -Destination $dest -Force
            } else {
                Copy-Item $_.FullName -Destination $InstallDir -Force
            }
            $count++
        }
    return $count
}

# Copy LICENSE / NOTICE / ThirdPartyNotices alongside the libs so the
# install directory is self-documenting and MIT/Apache-2.0 attribution
# travels with the binaries. Prefixed with the wheel's top-level package
# directory so multiple wheels (e.g. onnxruntime + openvino on Intel
# Windows) don't overwrite each other's notices.
function Copy-Licenses([string]$searchRoot) {
    Get-ChildItem -Path $searchRoot -Recurse -File -Depth 4 |
        Where-Object {
            # skip *.dist-info/ — pip-generated duplicates of the same files in the package root
            $_.FullName -notmatch '\.dist-info[\\/]' -and
            $_.Name -match '^(LICENSE|NOTICE|ThirdPartyNotices|COPYING)' -and
            $_.Extension -in @('', '.txt', '.md')
        } |
        ForEach-Object {
            $parent = Split-Path -Leaf (Split-Path -Parent $_.FullName)
            $dest = Join-Path $InstallDir ("$parent-$($_.Name)")
            if (-not (Test-Path $dest)) {
                Copy-Item $_.FullName -Destination $dest -Force
            }
        }
}

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

$copied = Copy-MatchingLibs $ExtractDir $libPatterns $preserveLayout
Copy-Licenses $ExtractDir

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
            $rtPatterns = @($rtPattern) + @($Package.runtime_extra_patterns | Where-Object { $_ })
            $rtCopied = Copy-MatchingLibs $rtExtract $rtPatterns $preserveLayout
            Copy-Licenses $rtExtract
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
# preserve_layout: recurse, skip auditwheel-bundled *.libs/ dirs (Linux
# convention; harmless on Windows). flat: only look at depth 1.
if ($preserveLayout) {
    $ortDll = Get-ChildItem -Path $InstallDir -Recurse -File -Filter "$libPattern*.dll" |
        Where-Object { $_.FullName -notmatch '\\[^\\]+\.libs\\' } |
        Select-Object -First 1
    $listing = Get-ChildItem -Path $InstallDir -Recurse -File |
        Where-Object { $_.FullName -notmatch '\\[^\\]+\.libs\\' }
} else {
    $ortDll = Get-ChildItem -Path $InstallDir -File -Filter "$libPattern*.dll" |
        Select-Object -First 1
    $listing = Get-ChildItem -Path $InstallDir -File
}

if (-not $ortDll) {
    Write-Host "Error: no $libPattern*.dll found after extraction." -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "Done. Installed to: $InstallDir"
$listing | Format-Table Name, Length -AutoSize
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
