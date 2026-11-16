[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Configuration = 'Release',
    [string]$QtRoot = $env:QTDIR,
    [string]$Generator = 'Visual Studio 18 2026',
    [string]$BuildDirectory
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $BuildDirectory) {
    $BuildDirectory = Join-Path $root 'build'
}

$configureArgs = @('-S', $root, '-B', $BuildDirectory, '-G', $Generator, '-A', 'x64')
if ($QtRoot) {
    if (-not (Test-Path (Join-Path $QtRoot 'lib/cmake/Qt6/Qt6Config.cmake'))) {
        throw "Qt6 was not found in '$QtRoot'. Pass -QtRoot pointing to the MSVC x64 Qt installation."
    }
    $configureArgs += "-DQt6_ROOT=$QtRoot"
}

& cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& cmake --build $BuildDirectory --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }

Write-Output "Built: $(Join-Path $BuildDirectory "bin/$Configuration/lwallpaper.exe")"
