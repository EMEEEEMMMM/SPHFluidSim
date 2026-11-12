<#
.SYNOPSIS
    Build a portable Windows (MSYS2 UCRT64 / Qt 6) package for SPHFluidSim.

.DESCRIPTION
    Stages the executable, Qt runtime / plugins / QML modules (via windeployqt),
    the GLSL shaders, a relocatable qt.conf, the MinGW UCRT64 runtime, Assimp,
    the OpenCL ICD loader and the redistributable NVRTC runtime, then writes a
    .zip archive.

    Shared by CI (.github/workflows/release.yml) and local builds: every path
    has a default and can be overridden with a parameter, so the exact same
    command works on a runner and on a developer machine.

    CUDA NOTE (important):
    The CUDA Driver API (nvcuda.dll on Windows, libcuda.so.1 on Linux) is
    loaded at RUNTIME from the NVIDIA display driver. nvcuda.dll is NOT
    redistributable and MUST NOT be bundled in this package. The only CUDA
    pieces shipped are the redistributable CUDA-toolkit DLLs needed to JIT the
    embedded kernels: nvrtc64_*.dll (NVRTC) and nvrtc-builtins64_*.dll. On a
    machine without an NVIDIA driver the app simply falls back to the CPU
    backend; the driver supplies nvcuda.dll, not this installer.

    Windows PowerShell 5.1 compatible:
      - no ??, no ?., no ternary operator
      - paths built with Join-Path
      - ASCII only (no non-ASCII characters) to avoid encoding issues

.PARAMETER BuildDir
    Build tree that holds SPHFluidSim.exe (default 'build').
.PARAMETER ExeName
    Executable file name (default 'SPHFluidSim.exe').
.PARAMETER Output
    Output .zip path (default derived from VERSION, e.g.
    'dist/SPHFluidSim-v1.11.0-win64.zip').
.PARAMETER StageDir
    Staging directory that becomes the zip root (default 'dist/SPHFluidSim').
.PARAMETER QtBin
    Directory containing windeployqt.exe. When omitted it is auto-detected
    from $env:Qt6_DIR or from windeployqt on PATH.
.PARAMETER Version
    Version string. When omitted it is read from the root VERSION file.
.PARAMETER Msys2Bin
    (CI helper) MSYS2 UCRT64 bin dir holding the MinGW runtime, Assimp and
    OpenCL ICD loader DLLs. Defaults to the resolved -QtBin (they are the same
    ucrt64/bin on MSYS2).
.PARAMETER CudaBin
    (CI helper) CUDA toolkit bin dir holding nvrtc64_*.dll. Defaults to
    $env:CUDA_PATH/bin / $env:CUDA_HOME/bin.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\deploy\windows.ps1

.EXAMPLE
    .\deploy\windows.ps1 -BuildDir build \
        -Output dist/SPHFluidSim-v1.11.0-win64.zip \
        -QtBin C:/msys64/ucrt64/bin -CudaBin "$env:CUDA_PATH/bin"
#>
[CmdletBinding()]
param(
    [string]$BuildDir = 'build',
    [string]$ExeName  = 'SPHFluidSim.exe',
    [string]$Output   = 'dist/SPHFluidSim-v1.11.0-win64.zip',
    [string]$StageDir = 'dist/SPHFluidSim',
    [string]$QtBin    = '',
    [string]$Version  = '',
    [string]$Msys2Bin = '',
    [string]$CudaBin  = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# ---------------------------------------------------------------------------
# Locate the repository root. This script lives in <root>/deploy/, so all
# relative paths are resolved against <root> and the script can be launched
# from any working directory (locally or from CI).
# ---------------------------------------------------------------------------
$ScriptDir = $PSScriptRoot
if (-not $ScriptDir) { $ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition }
$RepoRoot = Split-Path -Parent $ScriptDir

function Resolve-RepoPath {
    param([string]$Path)
    if ([System.IO.Path]::IsPathRooted($Path)) { return $Path }
    return (Join-Path $RepoRoot $Path)
}

$BuildDir = Resolve-RepoPath $BuildDir
$Output   = Resolve-RepoPath $Output
$StageDir = Resolve-RepoPath $StageDir

# ---------------------------------------------------------------------------
# Version: the root VERSION file is the single source of truth. When -Output
# was not supplied explicitly, derive the archive name from MAJOR.MINOR.
# ---------------------------------------------------------------------------
if ([string]::IsNullOrWhiteSpace($Version)) {
    $versionFile = Join-Path $RepoRoot 'VERSION'
    if (-not (Test-Path -LiteralPath $versionFile)) {
        throw "VERSION file not found at '$versionFile'; pass -Version explicitly."
    }
    $Version = (Get-Content -LiteralPath $versionFile -Raw).Trim()
}
if (-not $PSBoundParameters.ContainsKey('Output')) {
    $Output = Resolve-RepoPath ('dist/SPHFluidSim-v' + $Version + '-win64.zip')
}

Write-Host '=== SPHFluidSim Windows packaging ==='
Write-Host ('Version : ' + $Version)
Write-Host ('BuildDir: ' + $BuildDir)
Write-Host ('StageDir: ' + $StageDir)
Write-Host ('Output  : ' + $Output)

# ---------------------------------------------------------------------------
# Resolve the Qt bin directory (windeployqt.exe).
#   1) explicit -QtBin
#   2) $env:Qt6_DIR  -> <prefix>/lib/cmake/Qt6, bin is <prefix>/bin
#   3) windeployqt.exe found on PATH
# ---------------------------------------------------------------------------
function Find-WindeployqtDir {
    if (-not [string]::IsNullOrWhiteSpace($QtBin)) { return $QtBin }

    $q6 = $env:Qt6_DIR
    if ($q6) {
        $p = $q6
        $p = Split-Path -Parent $p     # lib/cmake
        $p = Split-Path -Parent $p     # lib
        $p = Split-Path -Parent $p     # <prefix>
        $candidate = Join-Path $p 'bin'
        if (Test-Path -LiteralPath (Join-Path $candidate 'windeployqt.exe')) {
            return $candidate
        }
    }

    $cmd = Get-Command windeployqt -ErrorAction SilentlyContinue
    if ($cmd) { return (Split-Path -Parent $cmd.Source) }
    return ''
}

$QtBin = Find-WindeployqtDir
if ([string]::IsNullOrWhiteSpace($QtBin)) {
    throw "Could not locate windeployqt. Pass -QtBin <qt6 bin dir>, set Qt6_DIR, or put windeployqt on PATH."
}
$windeployqt = Join-Path $QtBin 'windeployqt.exe'
if (-not (Test-Path -LiteralPath $windeployqt)) {
    throw "windeployqt.exe not found in '$QtBin'. Check -QtBin."
}

# Msys2Bin holds the MinGW runtime / Assimp / OpenCL ICD DLLs; on MSYS2 this is
# the very same ucrt64/bin directory as QtBin.
if ([string]::IsNullOrWhiteSpace($Msys2Bin)) { $Msys2Bin = $QtBin }

# CudaBin holds NVRTC. Fall back to the standard CUDA env vars when not given.
if ([string]::IsNullOrWhiteSpace($CudaBin)) {
    foreach ($root in @($env:CUDA_PATH, $env:CUDA_HOME)) {
        if ($root) {
            $cand = Join-Path $root 'bin'
            if (Test-Path -LiteralPath $cand) { $CudaBin = $cand; break }
        }
    }
}

# ---------------------------------------------------------------------------
# 1) Clean the staging directory, refusing to ever wipe the repository root.
# ---------------------------------------------------------------------------
$stageFull = [System.IO.Path]::GetFullPath($StageDir)
$rootFull  = [System.IO.Path]::GetFullPath($RepoRoot)
if ($stageFull -eq $rootFull) {
    throw "Refusing to clean StageDir equal to the repository root ('$stageFull')."
}
if (Test-Path -LiteralPath $StageDir) {
    Write-Host ('Cleaning stage dir: ' + $StageDir)
    Remove-Item -LiteralPath $StageDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $StageDir | Out-Null

# ---------------------------------------------------------------------------
# 2) Copy the executable (hard requirement).
# ---------------------------------------------------------------------------
$exeSrc = Join-Path $BuildDir $ExeName
if (-not (Test-Path -LiteralPath $exeSrc)) {
    throw "Executable not found: '$exeSrc'. Build first (cmake --build build) or pass -BuildDir / -ExeName."
}
$exeDst = Join-Path $StageDir $ExeName
Copy-Item -LiteralPath $exeSrc -Destination $exeDst -Force
Write-Host ('Copied exe: ' + $ExeName)

# ---------------------------------------------------------------------------
# 3) Copy shaders. Two possible sources, prefer the POST_BUILD output next to
#    the exe (<BuildDir>/shaders, see CMakeLists.txt) and fall back to the
#    checked-in sources (src/ShaderProgram/). Both are supported.
# ---------------------------------------------------------------------------
$shaderNames = @(
    'vertex_shader.vert',
    'fragment_shader.frag',
    'surface_vertex.glsl',
    'surface_fragment.glsl'
)
$stageShaders = Join-Path $StageDir 'shaders'
New-Item -ItemType Directory -Force -Path $stageShaders | Out-Null
$buildShaders = Join-Path $BuildDir 'shaders'
$srcShaders   = Join-Path $RepoRoot 'src/ShaderProgram'
Write-Host 'Copying shaders ...'
foreach ($n in $shaderNames) {
    $candA = Join-Path $buildShaders $n
    $candB = Join-Path $srcShaders $n
    $src = ''
    if (Test-Path -LiteralPath $candA)      { $src = $candA }
    elseif (Test-Path -LiteralPath $candB)  { $src = $candB }
    if (-not $src) {
        throw "Shader '$n' not found (looked in '$buildShaders' and '$srcShaders'). Build the project so the POST_BUILD copy runs, or keep the .glsl/.vert/.frag sources in src/ShaderProgram/."
    }
    Copy-Item -LiteralPath $src -Destination $stageShaders -Force
    Write-Host ('  + ' + $n + '  <- ' + (Split-Path -Parent $src))
}

# ---------------------------------------------------------------------------
# 4) Run windeployqt for Qt 6 (QML imports come from --qmldir qml).
#    --compiler-runtime also pulls the MinGW runtime DLLs; it is best effort,
#    so a failure there is retried without the flag. windeployqt resolves the
#    full transitive Qt dependency closure (Qt6*.dll + glib/icu/freetype/...
#    helpers), the platform/imageformats plugins and the QML modules.
# ---------------------------------------------------------------------------
$qmlDir = Join-Path $RepoRoot 'qml'
Write-Host ('Running windeployqt: ' + $windeployqt)

# Windows PowerShell 5.1 turns anything a native program writes to stderr into
# a terminating error while $ErrorActionPreference is 'Stop'. windeployqt
# prints harmless warnings (a missing dxcompiler.dll, for instance), so relax
# the preference for the call and judge the result by the exit code instead.
function Invoke-NativeTool {
    param([string]$FilePath, [string[]]$ToolArgs)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $code = 0
    try {
        # Out-Host keeps the tool's chatter out of this function's return value.
        & $FilePath @ToolArgs | Out-Host
        if ($null -ne $LASTEXITCODE) { $code = $LASTEXITCODE }
    } finally {
        $ErrorActionPreference = $previous
    }
    return $code
}

$deployArgs = @('--qmldir', $qmlDir, '--release', '--compiler-runtime', $exeDst)
$windeployExit = Invoke-NativeTool -FilePath $windeployqt -ToolArgs $deployArgs
if ($windeployExit -ne 0) {
    Write-Warning "windeployqt --compiler-runtime returned $windeployExit; retrying without it."
    $deployArgs = @('--qmldir', $qmlDir, '--release', $exeDst)
    $windeployExit = Invoke-NativeTool -FilePath $windeployqt -ToolArgs $deployArgs
    if ($windeployExit -ne 0) {
        throw "windeployqt failed with exit code $windeployExit."
    }
}

# ---------------------------------------------------------------------------
# 5) Collect the non-Qt DLLs that windeployqt cannot know about.
#
#    NONE of these are fatal if missing: a CI runner may legitimately lack the
#    OpenCL ICD loader or the CUDA toolkit, and the app falls back to the CPU
#    backend at runtime. Missing files are reported as warnings only.
#
#    Remember: nvcuda.dll is DELIBERATELY NOT collected - see the CUDA note at
#    the top of this file. Only NVRTC / CUDA-toolkit redistributables are.
# ---------------------------------------------------------------------------
function Copy-DllMatches {
    param(
        [string[]]$Dirs,
        [string[]]$Patterns,
        [string]$Dest,
        [string]$Label
    )
    $count = 0
    foreach ($pat in $Patterns) {
        foreach ($d in $Dirs) {
            if ([string]::IsNullOrWhiteSpace($d)) { continue }
            if (-not (Test-Path -LiteralPath $d)) { continue }
            $items = @(Get-ChildItem -LiteralPath $d -Filter $pat -File -ErrorAction SilentlyContinue)
            foreach ($it in $items) {
                Copy-Item -LiteralPath $it.FullName -Destination $Dest -Force
                Write-Host ('  + ' + $it.Name)
                $count++
            }
        }
    }
    if ($count -eq 0) {
        Write-Warning ('[' + $Label + '] no DLL matched [' + ($Patterns -join ', ') + ']; optional dependency, continuing.')
    }
    return $count
}

$systemDllDirs = @($Msys2Bin, $QtBin, $BuildDir)

Write-Host 'Collecting MinGW UCRT64 runtime DLLs ...'
Copy-DllMatches -Dirs $systemDllDirs `
    -Patterns @('libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll', 'libgomp-1.dll') `
    -Dest $StageDir -Label 'MinGW runtime' | Out-Null

Write-Host 'Collecting Assimp DLL ...'
Copy-DllMatches -Dirs $systemDllDirs -Patterns @('libassimp*.dll') `
    -Dest $StageDir -Label 'assimp' | Out-Null

Write-Host 'Collecting OpenCL ICD loader DLL ...'
Copy-DllMatches -Dirs $systemDllDirs -Patterns @('OpenCL*.dll', 'libOpenCL*.dll') `
    -Dest $StageDir -Label 'OpenCL ICD' | Out-Null
# MSYS2's ocl-icd package may expose only libOpenCL-*.dll while the executable
# imports OpenCL.dll (the CUDA import library name). Provide the canonical name
# as a copy so the portable package resolves its import table on clean systems.
$canonicalOpenCL = Join-Path $StageDir 'OpenCL.dll'
if (-not (Test-Path -LiteralPath $canonicalOpenCL)) {
    foreach ($d in $systemDllDirs) {
        if ([string]::IsNullOrWhiteSpace($d) -or -not (Test-Path -LiteralPath $d)) { continue }
        $candidate = Get-ChildItem -LiteralPath $d -Filter 'libOpenCL*.dll' -File -ErrorAction SilentlyContinue |
                     Select-Object -First 1
        if ($candidate) {
            Copy-Item -LiteralPath $candidate.FullName -Destination $canonicalOpenCL -Force
            Write-Host ('  + ' + $candidate.Name + ' -> OpenCL.dll')
            break
        }
    }
}

Write-Host 'Collecting NVRTC runtime DLLs (CUDA-toolkit redistributables) ...'
$cudaDllDirs = @()
if ($CudaBin) { $cudaDllDirs += $CudaBin }
foreach ($root in @($env:CUDA_PATH, $env:CUDA_HOME)) {
    if ($root) { $cudaDllDirs += (Join-Path $root 'bin') }
}
$cudaDllDirs += $Msys2Bin
$cudaDllDirs += $QtBin
Copy-DllMatches -Dirs $cudaDllDirs `
    -Patterns @('nvrtc64_*.dll', 'nvrtc-builtins64_*.dll') `
    -Dest $StageDir -Label 'NVRTC' | Out-Null

# ---------------------------------------------------------------------------
# 6) qt.conf - written AFTER windeployqt so it is the final word and cannot be
#    overwritten by windeployqt. Prefix=. / Plugins=. make the copied Qt tree
#    relocatable; Qml2Imports=qml points at the deployed QML modules.
# ---------------------------------------------------------------------------
$qtConf = Join-Path $StageDir 'qt.conf'
Set-Content -LiteralPath $qtConf -Value @('[Paths]', 'Prefix=.', 'Plugins=.', 'Qml2Imports=qml') -Encoding Ascii
Write-Host 'Wrote qt.conf'

# ---------------------------------------------------------------------------
# 7) Copy README.md (hard requirement).
# ---------------------------------------------------------------------------
$readme = Join-Path $RepoRoot 'README.md'
if (-not (Test-Path -LiteralPath $readme)) {
    throw "README.md not found at '$readme'."
}
Copy-Item -LiteralPath $readme -Destination $StageDir -Force
Write-Host 'Copied README.md'

# ---------------------------------------------------------------------------
# 8) Compress the whole staging directory into the final .zip. Compressing the
#    directory (not "$StageDir/*") keeps a single SPHFluidSim/ root folder in
#    the archive, so extracting it does not spill files into the cwd.
# ---------------------------------------------------------------------------
$outParent = Split-Path -Parent $Output
if ($outParent -and -not (Test-Path -LiteralPath $outParent)) {
    New-Item -ItemType Directory -Force -Path $outParent | Out-Null
}
if (Test-Path -LiteralPath $Output) {
    Remove-Item -LiteralPath $Output -Force
}
Write-Host ('Creating archive: ' + $Output)
Compress-Archive -Path $StageDir -DestinationPath $Output -CompressionLevel Optimal -Force

Write-Host '=== Packaging done ==='
Write-Output $Output
