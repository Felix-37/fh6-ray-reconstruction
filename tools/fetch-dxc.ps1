# Downloads the DirectX Shader Compiler release the build uses into third_party/dxc (bin/x64/dxc.exe,
# dxcompiler.dll, dxil.dll and the headers in inc/) and checks its SHA-256. The patched RTGI variants are
# only guaranteed byte-identical with this exact release.
# Usage, from the repository root:
#   powershell -ExecutionPolicy Bypass -File tools/fetch-dxc.ps1 [-Force] [-Destination <folder>]
param(
    [switch]$Force,
    [string]$Destination
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is very slow with the progress bar

$tag = 'v1.9.2609'
$asset = 'dxc_2026_09_29.zip'
$sha256 = 'ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1'
$url = "https://github.com/microsoft/DirectXShaderCompiler/releases/download/$tag/$asset"

$root = Split-Path -Parent $PSScriptRoot
if (-not $Destination) { $Destination = Join-Path $root 'third_party\dxc' }
if ((Test-Path (Join-Path $Destination 'bin\x64\dxc.exe')) -and -not $Force) {
    Write-Host "DXC is already in $Destination (use -Force to download it again)."
    exit 0
}

$tmp = Join-Path ([IO.Path]::GetTempPath()) ('rr-forza-dxc-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    $zip = Join-Path $tmp $asset
    Write-Host "Downloading $url"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
    $hash = (Get-FileHash -Algorithm SHA256 -Path $zip).Hash.ToLowerInvariant()
    if ($hash -ne $sha256) {
        throw "SHA-256 mismatch for ${asset}: expected $sha256, got $hash"
    }
    if (Test-Path $Destination) { Remove-Item -Recurse -Force $Destination }
    Expand-Archive -Path $zip -DestinationPath $Destination
    Write-Host "DXC $tag extracted to $Destination"
}
finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
