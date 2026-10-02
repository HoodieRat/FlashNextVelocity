param(
    [Parameter(Mandatory=$true)][string]$DestinationRoot,
    [string]$RocmRoot = '',
    [string]$VcpkgShare = ''
)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Notices = Join-Path $DestinationRoot 'third-party'
New-Item -ItemType Directory -Force $Notices | Out-Null
foreach ($name in @('LICENSE', 'NOTICE', 'THIRD_PARTY.md')) {
    Copy-Item -LiteralPath (Join-Path $Root $name) -Destination $Notices -Force
}
Copy-Item -LiteralPath (Join-Path $Root 'licenses') -Destination $Notices -Recurse -Force

# Keep the notices supplied with the selected runtime, including transitive
# components. Preserve their relative paths rather than merging license texts.
if ($RocmRoot) {
    $Share = Join-Path $RocmRoot 'share'
    if (-not (Test-Path -LiteralPath $Share)) { throw "ROCm notice directory missing: $Share" }
    foreach ($file in Get-ChildItem -LiteralPath $Share -Recurse -File) {
        if ($file.Name -notmatch '^(LICENSE|NOTICE|COPYING|copyright)') { continue }
        $relative = $file.FullName.Substring($Share.Length).TrimStart([char[]]'\/')
        $target = Join-Path $Notices "licenses\runtime\rocm\$relative"
        New-Item -ItemType Directory -Force (Split-Path $target -Parent) | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $target -Force
    }
}
if ($VcpkgShare) {
    if (-not (Test-Path -LiteralPath $VcpkgShare)) { throw "vcpkg notice directory missing: $VcpkgShare" }
    foreach ($file in Get-ChildItem -LiteralPath $VcpkgShare -Filter copyright -Recurse -File) {
        $relative = $file.FullName.Substring($VcpkgShare.Length).TrimStart([char[]]'\/')
        $target = Join-Path $Notices "licenses\runtime\vcpkg\$relative"
        New-Item -ItemType Directory -Force (Split-Path $target -Parent) | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $target -Force
    }
}
