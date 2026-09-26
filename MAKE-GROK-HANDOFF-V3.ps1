param(
  [string]$Root = (Resolve-Path $PSScriptRoot).Path,
  [int]$LatestLogs = 3,
  [int]$LatestBenchmarks = 8
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$Root = [IO.Path]::GetFullPath(
  [Environment]::ExpandEnvironmentVariables($Root).Trim().Trim('"')
)

if (-not (Test-Path (Join-Path $Root 'CMakeLists.txt'))) {
  throw "This does not look like the FlashNextVelocity project root: $Root"
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$projectName = Split-Path $Root -Leaf
$parent = Split-Path $Root -Parent
$outZip = Join-Path $parent ("{0}-Grok-Handoff-{1}.zip" -f $projectName, $stamp)

$tempRoot = Join-Path $env:TEMP ("FNV-Grok-Handoff-" + [guid]::NewGuid().ToString('N'))
$stage = Join-Path $tempRoot $projectName
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)

# Generated/rebuildable/heavy directories.
# Everything else is copied by default.
$skipDirNames = @(
  '.git',
  '.deps',
  '.cache',
  'build',
  'bin',
  'obj',
  '.vs',
  '.idea',
  'node_modules',
  'packages',
  'downloads',
  '.patch-backups',
  'payload',
  '__pycache__',
  '.pytest_cache',
  '.mypy_cache',
  '.ruff_cache'
)

# Never carry giant archives/models/generated native binaries.
$skipExtensions = @(
  '.zip', '.7z', '.rar', '.tar', '.gz', '.tgz', '.bz2', '.xz',
  '.gguf', '.safetensors',
  '.dll', '.exe', '.pdb', '.obj', '.lib', '.exp', '.ilk',
  '.so', '.dylib', '.a', '.pyc'
)

function Get-NormalizedBase([string]$Path) {
  $base = [IO.Path]::GetFullPath($Path)
  while ($base.EndsWith('\') -or $base.EndsWith('/')) {
    $base = $base.Substring(0, $base.Length - 1)
  }
  return $base
}

$RootBase = Get-NormalizedBase $Root
$RootPrefix = $RootBase + [IO.Path]::DirectorySeparatorChar

function Rel([string]$Path) {
  # Windows PowerShell 5.1/.NET Framework compatible.
  # Do NOT use [IO.Path]::GetRelativePath(), which is unavailable there.
  $full = [IO.Path]::GetFullPath($Path)

  if (-not $full.StartsWith($RootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to process path outside project root: $full"
  }

  return $full.Substring($RootPrefix.Length).Replace('\','/')
}

function Should-Skip([string]$RelativePath) {
  $r = $RelativePath.Replace('\','/')
  $parts = $r.Split('/', [StringSplitOptions]::RemoveEmptyEntries)

  foreach ($part in $parts) {
    if ($skipDirNames -contains $part) {
      return $true
    }
  }

  # dist is generated. Live config/UI are copied separately if safe.
  if ($r -match '^dist(/|$)') {
    return $true
  }

  # Keep only selected recent history, copied separately below.
  if ($r -match '^(logs|benchmarks)(/|$)') {
    return $true
  }

  # Serena project config/memories are useful; machine indexes/caches are not.
  if ($r -match '^\.serena/(cache|caches|index|indexes|symbols|lsp|tmp|temp)(/|$)') {
    return $true
  }

  # Grok runtime/session caches should not be carried forward.
  if ($r -match '^\.grok/(sessions|logs|cache|caches|crash)(/|$)') {
    return $true
  }

  $ext = [IO.Path]::GetExtension($r).ToLowerInvariant()
  if ($skipExtensions -contains $ext) {
    return $true
  }

  return $false
}

function Looks-Like-SecretFile([string]$Path) {
  $name = [IO.Path]::GetFileName($Path).ToLowerInvariant()

  if ($name -match '(^|[._-])(api[-_]?key|secret|token|credentials?|password)([._-]|$)') {
    return $true
  }

  $ext = [IO.Path]::GetExtension($Path).ToLowerInvariant()
  if (@('.json','.yml','.yaml','.toml','.ini','.config','.txt','.env') -notcontains $ext -and $name -ne '.env') {
    return $false
  }

  try {
    $item = Get-Item -LiteralPath $Path -ErrorAction Stop
    if ($item.Length -gt 2MB) {
      return $false
    }

    $text = Get-Content -LiteralPath $Path -Raw -ErrorAction Stop
    $secretProperty = '(?i)(api[_-]?key|secret|password|access[_-]?token|auth[_-]?token|bearer)\s*["'']?\s*[:=]\s*["''][^"'']{8,}["'']'
    return [regex]::IsMatch($text, $secretProperty)
  }
  catch {
    return $false
  }
}

function Copy-One([string]$Source, [string]$Relative) {
  if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
    return
  }

  if (Should-Skip $Relative) {
    return
  }

  if (Looks-Like-SecretFile $Source) {
    Write-Warning "Skipped possible credential file: $Relative"
    return
  }

  $dest = Join-Path $stage $Relative
  $destDir = Split-Path $dest -Parent
  New-Item -ItemType Directory -Force $destDir | Out-Null
  Copy-Item -LiteralPath $Source -Destination $dest -Force
}

function Copy-LatestFiles([string]$SourceDir, [string]$DestDir, [int]$Count) {
  if ($Count -le 0 -or -not (Test-Path -LiteralPath $SourceDir -PathType Container)) {
    return
  }

  $files = Get-ChildItem -LiteralPath $SourceDir -File -Recurse -ErrorAction SilentlyContinue |
    Where-Object {
      $_.Length -gt 0 -and
      $_.Length -le 8MB -and
      @('.log','.txt','.md','.json','.csv') -contains $_.Extension.ToLowerInvariant()
    } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First $Count

  foreach ($f in $files) {
    $name = "{0}-{1}" -f $f.LastWriteTime.ToString('yyyyMMdd-HHmmss'), $f.Name
    $dest = Join-Path $stage (Join-Path $DestDir $name)
    New-Item -ItemType Directory -Force (Split-Path $dest -Parent) | Out-Null
    Copy-Item -LiteralPath $f.FullName -Destination $dest -Force
  }
}

try {
  New-Item -ItemType Directory -Force $stage | Out-Null

  Write-Host "Scanning project-owned files..." -ForegroundColor Cyan

  # SAFETY:
  # This enumeration only READS the live project and COPIES into our private
  # %TEMP% staging folder. It never moves/renames/deletes source-project files.
  Get-ChildItem -LiteralPath $Root -File -Recurse -Force | ForEach-Object {
    $rel = Rel $_.FullName

    if (-not (Should-Skip $rel)) {
      Copy-One $_.FullName $rel
    }
  }

  # Copy current live runtime settings separately because generated dist/
  # is otherwise excluded.
  foreach ($rel in @('dist\config.json','dist\ui.json')) {
    $src = Join-Path $Root $rel

    if (Test-Path -LiteralPath $src -PathType Leaf) {
      if (Looks-Like-SecretFile $src) {
        Write-Warning "Skipped $rel because it appears to contain a credential."
      }
      else {
        $dest = Join-Path $stage $rel
        New-Item -ItemType Directory -Force (Split-Path $dest -Parent) | Out-Null
        Copy-Item -LiteralPath $src -Destination $dest -Force
      }
    }
  }

  # A few newest useful diagnostics only.
  Copy-LatestFiles (Join-Path $Root 'logs') '_handoff\logs' $LatestLogs
  Copy-LatestFiles (Join-Path $Root 'benchmarks') '_handoff\benchmarks' $LatestBenchmarks

  $handoff = @'
# FlashNextVelocity Grok handoff

This is a CLEAN, REBUILDABLE SOURCE SNAPSHOT of the current working project.

Generated dependencies/build output are intentionally excluded. BUILD.bat and
the project scripts are expected to recreate them.

## Current runtime state
- Windows / gfx1151
- Qwen3.8 Flash-Next
- v1.0.10 async halo pipeline
- per-HC-stream MTP hidden normalization
- halo-greedy MTP
- Top-64/128/256 exact compact verification
- GPU-retained verification frontier with lazy full-row D2H
- context lookup
- Gufo overlay is project-owned and re-applied during build

## Last known benchmark baseline
- Prompt tokens: 1165
- Completion tokens: 493
- Decode: 33.46 tok/s
- Prefill: 1150.22 tok/s
- MTP max: 3
- MTP confidence: 0.00
- MTP-only acceptance: 66.6%
- MTP avg proposed width: 1.37
- MTP verify: 48.34 ms/round
- MTP verify: 25.247 ms/output
- Async halo chains: 246
- Retained frontier downloads: 0
- hipStreamSynchronize: 635
- Sync wait: 27.412 ms/output
- Depth 1: 156 cycles, 32.36 tok/s, 66.7% acceptance
- Depth 2: 88 cycles, 37.33 tok/s, 66.5% acceptance
- Depth 3: 2 cycles, 36.96 tok/s, 66.7% acceptance

Important:
Synchronization wait includes queued GPU execution; it is not all removable host idle time.

## Immediate next optimization target
Re-evaluate adaptive MTP depth economics for the asynchronous pipeline.

Do not blindly force depth 2.
Instrument expected cost/yield for depths 1/2/3 and adapt from measured recent
acceptance/prefix survival while preserving exact target semantics.

## Preserve
Do not regress:
- exact target sampling/output semantics
- Top-64/128/256 compact verification
- full-vocabulary fallback correctness
- rejection/frontier correctness
- per-HC-stream MTP normalization
- async GPU-feedback halo chain
- GPU-retained verification frontier
- context lookup
- gfx1151/Windows build
- tests

## Build workflow
1. Read scripts/build.ps1 and overlay scripts before changing Gufo integration.
2. Make durable changes in project-owned source/overlay files.
3. Do not make the only copy of a fix inside generated .deps/gufo.
4. Run BUILD.bat.
5. Benchmark against 33.46 tok/s.
6. Keep only changes that improve performance, correctness, or useful diagnostics.

## Intentionally omitted
- .deps
- .cache
- build output
- generated native binaries/DLLs
- downloaded archives
- model GGUFs
- stale log history
- Serena machine indexes/caches
- old patch payload/backups

Serena project configuration and memories are retained when present; its indexes
should be rebuilt in the new workspace.
'@

  [IO.File]::WriteAllText(
    (Join-Path $stage 'GROK-HANDOFF.md'),
    $handoff,
    $Utf8NoBom
  )

  # Extend ignore rules IN THE COPY ONLY.
  $gitignore = Join-Path $stage '.gitignore'
  $existing = if (Test-Path -LiteralPath $gitignore) {
    Get-Content -LiteralPath $gitignore -Raw
  }
  else {
    ''
  }

  $extraIgnore = @'

# Generated/heavy content excluded from Grok handoff
.deps/
.cache/
build/
dist/*
!dist/config.json
!dist/ui.json
logs/
benchmarks/
.patch-backups/
payload/
*.zip
*.7z
*.rar
*.tar
*.gz
*.gguf
*.safetensors
**/bin/
**/obj/
**/__pycache__/
.serena/cache/
.serena/caches/
.serena/index/
.serena/indexes/
.serena/symbols/
.serena/lsp/
.grok/sessions/
.grok/logs/
.grok/cache/
'@

  [IO.File]::WriteAllText(
    $gitignore,
    ($existing.TrimEnd() + "`r`n" + $extraIgnore.TrimStart()),
    $Utf8NoBom
  )

  # Grok workspace rules, in generated copy only.
  $ruleDir = Join-Path $stage '.grok\rules'
  New-Item -ItemType Directory -Force $ruleDir | Out-Null

  $rule = @'
# FlashNextVelocity handoff rules
- Read GROK-HANDOFF.md before substantial work.
- Treat the extracted tree as project-owned source.
- Never make a durable fix only inside .deps/, build/, or generated dist files.
- Preserve exact target sampling and verification correctness.
- Use BUILD.bat for Windows/gfx1151 validation.
- Prefer measured benchmark evidence over speculative tuning.
'@

  [IO.File]::WriteAllText(
    (Join-Path $ruleDir 'flashnext-handoff.md'),
    $rule,
    $Utf8NoBom
  )

  $init = @'
@echo off
setlocal
cd /d "%~dp0"

where git >nul 2>nul
if errorlevel 1 (
  echo ERROR: git is not on PATH.
  pause
  exit /b 1
)

if not exist ".git" git init

git add -A
git diff --cached --quiet
if errorlevel 1 (
  git -c user.name="FlashNextVelocity Handoff" -c user.email="handoff@local" commit -m "FlashNextVelocity Grok handoff baseline"
)

echo.
echo Fresh Git baseline ready.
pause
'@

  [IO.File]::WriteAllText(
    (Join-Path $stage 'INIT-GROK-WORKSPACE.bat'),
    $init,
    [Text.Encoding]::ASCII
  )

  $run = @'
@echo off
setlocal
cd /d "%~dp0"

set GROK_RESPECT_GITIGNORE=1

where grok >nul 2>nul
if errorlevel 1 (
  echo ERROR: grok is not on PATH.
  pause
  exit /b 1
)

grok
'@

  [IO.File]::WriteAllText(
    (Join-Path $stage 'RUN-GROK.bat'),
    $run,
    [Text.Encoding]::ASCII
  )

  Write-Host "Creating ZIP..." -ForegroundColor Cyan
  Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $outZip -CompressionLevel Optimal

  $zipInfo = Get-Item -LiteralPath $outZip
  $hash = (Get-FileHash -LiteralPath $outZip -Algorithm SHA256).Hash
  $sizeMB = [math]::Round($zipInfo.Length / 1MB, 2)

  Write-Host ""
  Write-Host "Grok handoff bundle created successfully." -ForegroundColor Green
  Write-Host "ZIP:    $outZip"
  Write-Host "Size:   $sizeMB MB"
  Write-Host "SHA256: $hash"
  Write-Host ""
  Write-Host "IMPORTANT: The live project was READ ONLY." -ForegroundColor Green
  Write-Host "Nothing in the source project was moved, renamed, modified, or deleted." -ForegroundColor Green
  Write-Host ""
  Write-Host "Extract the ZIP into a NEW folder."
  Write-Host "Run INIT-GROK-WORKSPACE.bat once, then RUN-GROK.bat."
}
finally {
  # SAFETY:
  # The only deletion this script can perform is its OWN randomly named
  # temporary staging folder under %TEMP%.
  #
  # It never calls Remove-Item against $Root or anything below $Root.
  if (Test-Path -LiteralPath $tempRoot) {
    $tempFull = [IO.Path]::GetFullPath($tempRoot)
    $rootFull = [IO.Path]::GetFullPath($Root)

    if (-not $tempFull.Equals($rootFull, [StringComparison]::OrdinalIgnoreCase) -and
        -not $rootFull.StartsWith($tempFull + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
      Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
  }
}
