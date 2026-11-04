<#
.SYNOPSIS
    Builds BlackCamera (Release), stages it in dist\BlackCamera and compiles the Inno Setup installer.

.DESCRIPTION
    Requirements: Visual Studio with the C++ workload, vcpkg (VCPKG_ROOT or %USERPROFILE%\vcpkg),
    Inno Setup 6 (ISCC.exe). Run from any folder:
        powershell -ExecutionPolicy Bypass -File tools\make-installer.ps1
#>
param(
    [string]$Version = "1.2.0"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not $env:VCPKG_ROOT) {
    $env:VCPKG_ROOT = Join-Path $env:USERPROFILE "vcpkg"
    Write-Host "VCPKG_ROOT not set, using $env:VCPKG_ROOT"
}

# CMake ships with Visual Studio; use it when cmake is not on PATH
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
}

Write-Host "== Build (Release) and test"
& $cmake --workflow --preset release
if ($LASTEXITCODE) { throw "build failed" }

Write-Host "== Stage dist\BlackCamera"
Remove-Item -Recurse -Force dist\BlackCamera -ErrorAction SilentlyContinue
& $cmake --install build --config Release --prefix dist/BlackCamera
if ($LASTEXITCODE) { throw "install failed" }

Write-Host "== VC++ Redistributable"
New-Item -ItemType Directory -Force build\redist | Out-Null
$redist = "build\redist\vc_redist.x64.exe"
if (-not (Test-Path $redist)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -property installationPath
    $local = Get-ChildItem (Join-Path $vs "VC\Redist\MSVC") -Recurse -Filter vc_redist.x64.exe -ErrorAction SilentlyContinue |
             Sort-Object FullName -Descending | Select-Object -First 1
    if ($local) {
        Copy-Item $local.FullName $redist
    } else {
        Invoke-WebRequest "https://aka.ms/vs/17/release/vc_redist.x64.exe" -OutFile $redist
    }
}

Write-Host "== Inno Setup"
$iscc = (Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source
if (-not $iscc) {
    $iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
              "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $iscc) { throw "Inno Setup 6 not found (https://jrsoftware.org/isdl.php)" }
& $iscc "/DMyAppVersion=$Version" installer.iss
if ($LASTEXITCODE) { throw "ISCC failed" }
Write-Host "Done: dist\BlackCamera_Setup_v$Version.exe"
