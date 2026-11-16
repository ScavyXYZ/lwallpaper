[CmdletBinding()]
param(
    [string]$Version = '1.3',
    [ValidateSet('Gui', 'Rose')][string]$Edition = 'Gui',
    [string]$BuildDirectory,
    [string]$OutputDirectory,
    [string]$RedistPath,
    [string]$Compiler
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if ($Version -notmatch '^\d+\.\d+(\.\d+)?$') { throw 'Version must be numeric (e.g. 1.3).' }
if (-not $BuildDirectory) { $BuildDirectory = "$root/build/bin/Release" }
if (-not $OutputDirectory) { $OutputDirectory = "$root/build/installer" }
if (-not (Test-Path "$BuildDirectory/lwallpaper.exe")) { throw 'Build the Release application first.' }
if (-not $Compiler) {
    $Compiler = @(
        "${env:ProgramFiles(x86)}/Inno Setup 6/ISCC.exe",
        "$env:ProgramFiles/Inno Setup 6/ISCC.exe",
        (Get-Command iscc.exe -ErrorAction SilentlyContinue).Source
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
}
if (-not $Compiler) { throw 'Inno Setup 6 compiler was not found.' }
if (-not $RedistPath) {
    $RedistPath = "$root/build/installer-tools/VC_redist.x64.exe"
    New-Item -ItemType Directory -Force (Split-Path $RedistPath) | Out-Null
    Invoke-WebRequest https://aka.ms/vs/17/release/vc_redist.x64.exe -OutFile $RedistPath
}
$signature = Get-AuthenticodeSignature -LiteralPath $RedistPath
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
    throw 'VC++ prerequisite does not have a valid Microsoft Authenticode signature.'
}
$arguments = @('/Q', "/DAppVersion=$Version",
    "/DBuildDir=$((Resolve-Path $BuildDirectory).Path)",
    "/DRedistPath=$((Resolve-Path $RedistPath).Path)", "/O$OutputDirectory")
if ($Edition -eq 'Rose') { $arguments += '/DRoseEdition' }
& $Compiler @arguments "$root/packaging/lwall.iss"
if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed.' }
$prefix = if ($Edition -eq 'Rose') { 'lwallpaper-vgui' } else { 'lwallpaper' }
$installer = Join-Path $OutputDirectory "$prefix-$Version-windows-x64-setup.exe"
if (-not (Test-Path -LiteralPath $installer)) { throw 'Compiled installer not found.' }
$hash = (Get-FileHash $installer -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  $(Split-Path $installer -Leaf)" | Set-Content "$installer.sha256" -Encoding ascii
Write-Host "Built installer: $installer"
