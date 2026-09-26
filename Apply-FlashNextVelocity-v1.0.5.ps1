param(
  [Parameter(Position=0)][string]$ProjectRoot,
  [switch]$Force
)
$ErrorActionPreference = 'Stop'
$PackageRoot = $PSScriptRoot
$ManifestPath = Join-Path $PackageRoot 'PAYLOAD-MANIFEST.json'
$PayloadRoot = Join-Path $PackageRoot 'payload'

function Get-Sha256([string]$Path) {
  return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
}
function Is-ProjectRoot([string]$Path) {
  if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path -PathType Container)) { return $false }
  return (Test-Path -LiteralPath (Join-Path $Path 'VERSION.json') -PathType Leaf) -and
         (Test-Path -LiteralPath (Join-Path $Path 'CMakeLists.txt') -PathType Leaf) -and
         (Test-Path -LiteralPath (Join-Path $Path 'gufo-overlay') -PathType Container)
}
function Resolve-Target([string]$Requested) {
  if (-not [string]::IsNullOrWhiteSpace($Requested)) {
    $resolved = (Resolve-Path -LiteralPath $Requested).Path
    if (-not (Is-ProjectRoot $resolved)) { throw "Not a FlashNextVelocity source root: $resolved" }
    return $resolved
  }
  $candidates = @(
    (Split-Path $PackageRoot -Parent),
    (Get-Location).Path,
    'C:\FlashNextVelocity'
  ) | Select-Object -Unique
  foreach ($candidate in $candidates) {
    if (Is-ProjectRoot $candidate) { return (Resolve-Path -LiteralPath $candidate).Path }
  }
  $entered = Read-Host 'Enter the FlashNextVelocity project folder (the folder containing VERSION.json and BUILD.bat)'
  if ([string]::IsNullOrWhiteSpace($entered)) { throw 'No project folder was supplied.' }
  $resolved = (Resolve-Path -LiteralPath $entered).Path
  if (-not (Is-ProjectRoot $resolved)) { throw "Not a FlashNextVelocity source root: $resolved" }
  return $resolved
}

if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) { throw "Patch manifest is missing: $ManifestPath" }
if (-not (Test-Path -LiteralPath $PayloadRoot -PathType Container)) { throw "Patch payload is missing: $PayloadRoot" }
$Manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
if ($Manifest.schema -ne 'flashnextvelocity-dropin-patch-v1') { throw "Unsupported patch manifest schema: $($Manifest.schema)" }
$ProjectRoot = Resolve-Target $ProjectRoot

Write-Host "FlashNextVelocity drop-in patch" -ForegroundColor Cyan
Write-Host "Target:  $ProjectRoot"
Write-Host "Patch:   $($Manifest.patch_name)"
Write-Host "Runtime: $($Manifest.runtime_revision)"
Write-Host ''

# Validate every payload byte and every target file before writing anything.
$mismatches = New-Object System.Collections.Generic.List[string]
$plan = @()
foreach ($entry in $Manifest.files) {
  $rel = [string]$entry.path
  $payload = Join-Path $PayloadRoot ($rel -replace '/', '\')
  $target = Join-Path $ProjectRoot ($rel -replace '/', '\')
  if (-not (Test-Path -LiteralPath $payload -PathType Leaf)) {
    throw "Payload file missing: $rel"
  }
  $payloadHash = Get-Sha256 $payload
  if ($payloadHash -ne ([string]$entry.payload_sha256).ToUpperInvariant()) {
    throw "Payload hash mismatch for $rel. ZIP contents are damaged or incomplete."
  }

  $exists = Test-Path -LiteralPath $target -PathType Leaf
  $currentHash = if ($exists) { Get-Sha256 $target } else { $null }
  $baseHash = if ($null -ne $entry.base_sha256) { ([string]$entry.base_sha256).ToUpperInvariant() } else { $null }
  $alreadyCurrent = $exists -and $currentHash -eq $payloadHash
  $matchesBase = $exists -and $null -ne $baseHash -and $currentHash -eq $baseHash
  $isAddition = [string]$entry.action -eq 'add'

  if (-not $alreadyCurrent) {
    if ($isAddition) {
      if ($exists -and -not $Force) { $mismatches.Add("$rel (new patch file already exists with different contents)") }
    } elseif (-not $matchesBase) {
      if (-not $exists) { $mismatches.Add("$rel (expected base file is missing)") }
      elseif (-not $Force) { $mismatches.Add("$rel (local file differs from the supplied v$($Manifest.expected_base_version) bundle)") }
    }
  }
  $plan += [pscustomobject]@{
    Path = $rel
    Payload = $payload
    Target = $target
    Exists = $exists
    CurrentHash = $currentHash
    PayloadHash = $payloadHash
    AlreadyCurrent = $alreadyCurrent
  }
}
if ($mismatches.Count -gt 0) {
  Write-Host 'Patch stopped before changing any files.' -ForegroundColor Red
  Write-Host 'These files do not match the source bundle this patch was built from:' -ForegroundColor Yellow
  $mismatches | ForEach-Object { Write-Host "  - $_" }
  Write-Host ''
  Write-Host 'If those differences are intentional, rerun Apply-FlashNextVelocity-v1.0.5.ps1 with -Force. A backup is still created first.' -ForegroundColor Yellow
  exit 2
}

$toChange = @($plan | Where-Object { -not $_.AlreadyCurrent })
if ($toChange.Count -eq 0) {
  Write-Host 'Patch is already fully applied; every payload hash matches.' -ForegroundColor Green
  Write-Host 'Next: run BUILD.bat if you have not rebuilt this source yet.' -ForegroundColor Yellow
  exit 0
}

$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$backupParent = Join-Path $ProjectRoot '.patch-backups'
$backupRoot = Join-Path $backupParent "FlashNextVelocity-v1.0.5-StrixNative-$timestamp"
New-Item -ItemType Directory -Force -Path $backupRoot | Out-Null
$rollback = [ordered]@{
  schema = 'flashnextvelocity-rollback-v1'
  patch_name = [string]$Manifest.patch_name
  patch_version = [string]$Manifest.patch_version
  runtime_revision = [string]$Manifest.runtime_revision
  project_root = $ProjectRoot
  applied_at = (Get-Date).ToString('o')
  entries = @()
}

$changed = 0
$skipped = 0
try {
  # Phase 1: back up every file that will be replaced before writing any
  # project source. This makes even an interrupted copy phase recoverable.
  foreach ($item in $plan) {
    if ($item.AlreadyCurrent) {
      ++$skipped
      continue
    }
    $relativeWindows = $item.Path -replace '/', '\'
    $backupFile = Join-Path $backupRoot (Join-Path 'files' $relativeWindows)
    $entryRecord = [ordered]@{
      path = $item.Path
      existed_before = [bool]$item.Exists
      previous_sha256 = $item.CurrentHash
      payload_sha256 = $item.PayloadHash
    }
    if ($item.Exists) {
      $backupDir = Split-Path $backupFile -Parent
      New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
      Copy-Item -LiteralPath $item.Target -Destination $backupFile -Force
      if ((Get-Sha256 $backupFile) -ne $item.CurrentHash) {
        throw "Backup hash verification failed: $($item.Path)"
      }
    }
    $rollback['entries'] += [pscustomobject]$entryRecord
  }

  $rollbackPath = Join-Path $backupRoot 'ROLLBACK-MANIFEST.json'
  [IO.File]::WriteAllText($rollbackPath, ($rollback | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
  New-Item -ItemType Directory -Force -Path $backupParent | Out-Null
  [IO.File]::WriteAllText((Join-Path $backupParent 'LATEST.txt'), $backupRoot, [Text.UTF8Encoding]::new($false))

  # Phase 2: apply payload only after the complete rollback set is durable.
  foreach ($item in $plan) {
    if ($item.AlreadyCurrent) { continue }
    $targetDir = Split-Path $item.Target -Parent
    New-Item -ItemType Directory -Force -Path $targetDir | Out-Null
    Copy-Item -LiteralPath $item.Payload -Destination $item.Target -Force
    ++$changed
  }

  # Verify the installed bytes before declaring success.
  foreach ($item in $plan) {
    if (-not (Test-Path -LiteralPath $item.Target -PathType Leaf)) { throw "Installed file missing: $($item.Path)" }
    if ((Get-Sha256 $item.Target) -ne $item.PayloadHash) { throw "Installed hash verification failed: $($item.Path)" }
  }
} catch {
  Write-Host "Patch application failed: $($_.Exception.Message)" -ForegroundColor Red
  Write-Host "Rollback data (if backup phase completed): $backupRoot" -ForegroundColor Yellow
  Write-Host 'If LATEST.txt exists under .patch-backups, run ROLLBACK-LAST-PATCH.bat before retrying.' -ForegroundColor Yellow
  throw
}

Write-Host ''
Write-Host "PATCH APPLIED AND VERIFIED." -ForegroundColor Green
Write-Host "Files changed: $changed; already current: $skipped" -ForegroundColor Green
Write-Host "Backup: $backupRoot" -ForegroundColor Cyan
Write-Host ''
Write-Host 'Next: run BUILD.bat from the project root.' -ForegroundColor Yellow
Write-Host 'BUILD.bat will compile/run the native context-lookup + profiler unit tests before building the engine.' -ForegroundColor Yellow
