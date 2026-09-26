param(
  [Parameter(Position=0)][string]$ProjectRoot,
  [switch]$Force
)
$ErrorActionPreference = 'Stop'
function Get-Sha256([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant() }
function Is-ProjectRoot([string]$Path) {
  return -not [string]::IsNullOrWhiteSpace($Path) -and (Test-Path -LiteralPath $Path -PathType Container) -and
         (Test-Path -LiteralPath (Join-Path $Path 'VERSION.json') -PathType Leaf)
}
if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
  $candidates = @((Split-Path $PSScriptRoot -Parent), (Get-Location).Path, 'C:\FlashNextVelocity') | Select-Object -Unique
  foreach ($candidate in $candidates) { if (Is-ProjectRoot $candidate) { $ProjectRoot = $candidate; break } }
  if ([string]::IsNullOrWhiteSpace($ProjectRoot)) { $ProjectRoot = Read-Host 'Enter the FlashNextVelocity project folder' }
}
$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
if (-not (Is-ProjectRoot $ProjectRoot)) { throw "Not a FlashNextVelocity source root: $ProjectRoot" }
$backupParent = Join-Path $ProjectRoot '.patch-backups'
$latestFile = Join-Path $backupParent 'LATEST.txt'
if (-not (Test-Path -LiteralPath $latestFile -PathType Leaf)) { throw "No rollback pointer found: $latestFile" }
$backupRoot = (Get-Content -LiteralPath $latestFile -Raw).Trim()
$manifestPath = Join-Path $backupRoot 'ROLLBACK-MANIFEST.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw "Rollback manifest missing: $manifestPath" }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.schema -ne 'flashnextvelocity-rollback-v1') { throw "Unsupported rollback manifest: $($manifest.schema)" }

# Do not silently erase edits made after patching.
$conflicts = @()
foreach ($entry in $manifest.entries) {
  $target = Join-Path $ProjectRoot (([string]$entry.path) -replace '/', '\')
  if (Test-Path -LiteralPath $target -PathType Leaf) {
    $current = Get-Sha256 $target
    $patched = ([string]$entry.payload_sha256).ToUpperInvariant()
    if ($current -ne $patched -and -not $Force) { $conflicts += [string]$entry.path }
  } elseif ([bool]$entry.existed_before -and -not $Force) {
    $conflicts += [string]$entry.path
  }
}
if ($conflicts.Count -gt 0) {
  Write-Host 'Rollback stopped before changing files because these patched files were edited afterward:' -ForegroundColor Red
  $conflicts | ForEach-Object { Write-Host "  - $_" }
  Write-Host 'Rerun with -Force only if you want to discard those post-patch edits.' -ForegroundColor Yellow
  exit 2
}

foreach ($entry in $manifest.entries) {
  $rel = [string]$entry.path
  $target = Join-Path $ProjectRoot ($rel -replace '/', '\')
  if ([bool]$entry.existed_before) {
    $backup = Join-Path $backupRoot (Join-Path 'files' ($rel -replace '/', '\'))
    if (-not (Test-Path -LiteralPath $backup -PathType Leaf)) { throw "Backup file missing: $rel" }
    $dir = Split-Path $target -Parent
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Copy-Item -LiteralPath $backup -Destination $target -Force
    $expected = if ($null -ne $entry.previous_sha256) { ([string]$entry.previous_sha256).ToUpperInvariant() } else { $null }
    if ($expected -and (Get-Sha256 $target) -ne $expected) { throw "Restored hash verification failed: $rel" }
  } else {
    if (Test-Path -LiteralPath $target -PathType Leaf) { Remove-Item -LiteralPath $target -Force }
  }
}
[IO.File]::WriteAllText((Join-Path $backupRoot 'ROLLED-BACK.txt'), (Get-Date).ToString('o'), [Text.UTF8Encoding]::new($false))
Remove-Item -LiteralPath $latestFile -Force -ErrorAction SilentlyContinue
Write-Host ''
Write-Host 'ROLLBACK COMPLETE AND VERIFIED.' -ForegroundColor Green
Write-Host "Restored from: $backupRoot" -ForegroundColor Cyan
Write-Host 'Run BUILD.bat again to rebuild the restored source.' -ForegroundColor Yellow
