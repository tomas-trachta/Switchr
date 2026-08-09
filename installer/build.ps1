# installer/build.ps1 -- produce Switchr installer .exe.
#
# Steps:
#   1. Locate MSBuild via vswhere.
#   2. Build Switchr.sln in Release|x64.
#   3. Stage Switchr.exe and the VC++ runtime DLLs into stage\.
#   4. Compile installer\Switchr.iss with ISCC, emitting output\Switchr-setup-x64.exe.
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$Platform      = 'x64',
    # Defaults to the SwitchrVersion property in Switchr\Switchr.vcxproj
    # (the single source of truth). Pass -Version X.Y.Z to override for a
    # one-off build without editing the project file.
    [string]$Version       = ''
)

$ErrorActionPreference = 'Stop'
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Resolve-Path (Join-Path $ScriptDir '..')

# Read the version from Switchr.vcxproj if the caller didn't override.
if (-not $Version) {
    $projPath = Join-Path $RepoRoot 'Switchr\Switchr.vcxproj'
    $projXml  = [xml](Get-Content $projPath)
    $vMajor = $null; $vMinor = $null; $vPatch = $null
    foreach ($pg in $projXml.Project.PropertyGroup) {
        if ($pg.SwitchrVersionMajor) { $vMajor = $pg.SwitchrVersionMajor.Trim() }
        if ($pg.SwitchrVersionMinor) { $vMinor = $pg.SwitchrVersionMinor.Trim() }
        if ($pg.SwitchrVersionPatch) { $vPatch = $pg.SwitchrVersionPatch.Trim() }
    }
    if ($null -eq $vMajor -or $null -eq $vMinor -or $null -eq $vPatch) {
        throw "Could not locate SwitchrVersionMajor/Minor/Patch in $projPath. Either set them there or pass -Version."
    }
    $Version = "$vMajor.$vMinor.$vPatch"
}

# --- Locate MSBuild via vswhere -------------------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found at $vswhere -- install Visual Studio Build Tools."
}
$vsRoot = & $vswhere -latest -products * `
    -requires Microsoft.Component.MSBuild `
    -property installationPath
if (-not $vsRoot) { throw "No Visual Studio instance with MSBuild found." }
$msbuild = Join-Path $vsRoot 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path $msbuild)) { throw "MSBuild not found at $msbuild." }

# --- Locate ISCC (Inno Setup compiler) ------------------------------------
$iscc = $null
$candidates = @(
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "${env:ProgramFiles}\Inno Setup 6\ISCC.exe",
    "${env:LOCALAPPDATA}\Programs\Inno Setup 6\ISCC.exe"
)
foreach ($p in $candidates) {
    if (Test-Path $p) { $iscc = $p; break }
}
if (-not $iscc) {
    $cmd = Get-Command iscc -ErrorAction SilentlyContinue
    if ($cmd) { $iscc = $cmd.Source }
}
if (-not $iscc) {
    throw "ISCC.exe not found. Install Inno Setup 6 from https://jrsoftware.org/isdl.php (or: winget install JRSoftware.InnoSetup)."
}

# --- Build ----------------------------------------------------------------
Write-Host "==> Building Switchr $Version ($Configuration|$Platform)" -ForegroundColor Cyan

& $msbuild (Join-Path $RepoRoot 'Switchr.sln') `
    -nologo "-p:Configuration=$Configuration" "-p:Platform=$Platform" `
    -v:minimal
if ($LASTEXITCODE -ne 0) { throw "MSBuild failed (exit $LASTEXITCODE)." }

$exePath = Join-Path $RepoRoot "build\$Platform\$Configuration\Switchr.exe"
if (-not (Test-Path $exePath)) { throw "Build did not produce $exePath." }

# --- Stage payload --------------------------------------------------------
Write-Host "==> Staging payload" -ForegroundColor Cyan
$stage = Join-Path $ScriptDir 'stage'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null

Copy-Item $exePath $stage

# Copy VC++ runtime DLLs app-local. Required because the project links the
# dynamic CRT (/MD by default); without these, Switchr.exe will fail to
# start on machines that don't already have the matching VC redistributable.
$redistRoot = Join-Path $vsRoot 'VC\Redist\MSVC'
if (Test-Path $redistRoot) {
    $latestVer = Get-ChildItem $redistRoot -Directory |
        Where-Object { $_.Name -match '^\d' } |
        Sort-Object Name -Descending | Select-Object -First 1
    if ($latestVer) {
        $crtDir = Join-Path $latestVer.FullName "$Platform\Microsoft.VC143.CRT"
        if (Test-Path $crtDir) {
            foreach ($dll in @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
                $src = Join-Path $crtDir $dll
                if (Test-Path $src) { Copy-Item $src $stage }
                else { Write-Warning "Missing runtime DLL: $src" }
            }
        } else {
            Write-Warning "VC redist CRT directory not found: $crtDir"
        }
    }
} else {
    Write-Warning "VC redist root not found: $redistRoot"
}

# --- Installer ------------------------------------------------------------
Write-Host "==> Compiling installer with ISCC" -ForegroundColor Cyan
$outDir = Join-Path $ScriptDir 'output'
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
& $iscc "/Q" "/DAppVersion=$Version" (Join-Path $ScriptDir 'Switchr.iss')
if ($LASTEXITCODE -ne 0) { throw "ISCC failed (exit $LASTEXITCODE)." }

$setupExe = Join-Path $outDir "Switchr-setup-$Platform.exe"
if (Test-Path $setupExe) {
    Write-Host "    installer -> $setupExe" -ForegroundColor Green
} else {
    Write-Warning "Expected installer not found at $setupExe."
}
