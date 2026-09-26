@echo off
setlocal EnableExtensions DisableDelayedExpansion
title FlashNextVelocity - Make AI Context Bundle
cd /d "%~dp0"

rem ============================================================================
rem  FlashNextVelocity AI Context Bundle
rem
rem  Put this .bat in the PROJECT ROOT and run it.
rem
rem  DESIGN:
rem    - DEFAULT-INCLUDE: new source/docs/config/assets are picked up automatically.
rem    - EXCLUDE only known dependency, cache, generated-build, model, archive,
rem      secret, and compiled-output material.
rem    - Keeps project-owned folders such as src, scripts, assets, benchmarks,
rem      gufo-overlay, root docs/patches/config examples, and future code folders.
rem    - Writes the ZIP to:  _AI-Bundles\
rem
rem  Optional:
rem    MAKE-FlashNextVelocity-ContextBundle.bat --no-pause
rem ============================================================================

set "SELF=%~f0"
set "PROJECT_ROOT=%CD%"
set "TMPPS=%TEMP%\FlashNextVelocity_Bundle_%RANDOM%_%RANDOM%.ps1"

powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command ^
 "$lines=[IO.File]::ReadAllLines($env:SELF); $marker='#===POWERSHELL_PAYLOAD===#'; $i=[Array]::IndexOf($lines,$marker); if($i -lt 0){throw 'Internal PowerShell payload not found.'}; $enc=New-Object -TypeName Text.UTF8Encoding -ArgumentList $false; [IO.File]::WriteAllLines($env:TMPPS,$lines[($i+1)..($lines.Length-1)],$enc)"

if errorlevel 1 (
    echo.
    echo ERROR: Could not prepare the bundler.
    if /I not "%~1"=="--no-pause" pause
    exit /b 1
)

powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%TMPPS%" -ProjectRoot "%PROJECT_ROOT%"
set "RC=%ERRORLEVEL%"

del /q "%TMPPS%" >nul 2>&1

if not "%RC%"=="0" (
    echo.
    echo ERROR: Bundle creation failed.
    if /I not "%~1"=="--no-pause" pause
    exit /b %RC%
)

echo.
if /I not "%~1"=="--no-pause" pause
exit /b 0

#===POWERSHELL_PAYLOAD===#
param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectRoot
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# -----------------------------------------------------------------------------
# EDITABLE POLICY
# -----------------------------------------------------------------------------
# These directory NAMES are pruned anywhere in the tree.
# Do not add project-owned source folders here.
$ExcludedDirectoryNames = @(
    '.git',
    '.deps',
    '_deps',
    '.cache',
    '.serena',
    '.vs',
    '.idea',
    'node_modules',
    '.pnpm-store',
    '.yarn',
    '.nuget',
    'vcpkg_installed',
    'bin',
    'obj',
    'build',
    'Build',
    'dist',
    'out',
    'logs',
    '__pycache__',
    '.pytest_cache',
    '.mypy_cache',
    '.ruff_cache',
    '.venv',
    'venv',
    '.tox',
    '.gradle',
    '.next',
    '.turbo',
    'coverage',
    'CMakeFiles',
    'target',
    '_AI-Bundles'
)

# File types that are never useful as AI source/context bundles.
$ExcludedExtensions = @(
    '.zip', '.7z', '.rar', '.tar', '.gz', '.bz2', '.xz',
    '.exe', '.dll', '.pdb', '.obj', '.lib', '.exp', '.ilk',
    '.so', '.dylib', '.a', '.o', '.class', '.jar', '.war',
    '.nupkg', '.snupkg',
    '.gguf', '.safetensors', '.onnx', '.pt', '.pth', '.ckpt',
    '.iso', '.dmg', '.dmp', '.core',
    '.db', '.sqlite', '.sqlite3',
    '.log', '.trace', '.cache', '.bak', '.old',
    '.user', '.suo',
    '.tmp', '.temp', '.part'
)

# These are source/text/context formats. They are kept even when unusually large.
$SourceLikeExtensions = @(
    '.c', '.cc', '.cpp', '.cxx',
    '.h', '.hh', '.hpp', '.hxx',
    '.hip', '.cu', '.cuh',
    '.cs', '.csproj', '.sln', '.slnx',
    '.vcxproj', '.props', '.targets', '.filters',
    '.fs', '.fsx', '.vb',
    '.ps1', '.psm1', '.psd1', '.bat', '.cmd',
    '.py', '.pyi',
    '.js', '.jsx', '.ts', '.tsx', '.mjs', '.cjs',
    '.rs', '.go', '.java', '.kt', '.kts', '.swift',
    '.rb', '.php', '.pl', '.lua', '.r',
    '.json', '.jsonc', '.toml', '.yaml', '.yml',
    '.xml', '.ini', '.cfg', '.config', '.manifest',
    '.md', '.txt', '.rst',
    '.cmake', '.jinja', '.jinja2',
    '.html', '.htm', '.css', '.scss', '.sass', '.less',
    '.sql', '.proto', '.graphql', '.gql',
    '.sh', '.zsh', '.fish',
    '.patch', '.diff',
    '.csv', '.tsv',
    '.svg', '.resx', '.editorconfig'
)

# Safety valve for unknown non-source blobs. Source-like files above are exempt.
$MaxNonSourceFileBytes = 50MB

# -----------------------------------------------------------------------------
# HELPERS
# -----------------------------------------------------------------------------
$ProjectRoot = [IO.Path]::GetFullPath($ProjectRoot).TrimEnd([char[]]"\/")
if (-not (Test-Path -LiteralPath $ProjectRoot -PathType Container)) {
    throw "Project root does not exist: $ProjectRoot"
}

$ProjectName = Split-Path -Leaf $ProjectRoot
if ([string]::IsNullOrWhiteSpace($ProjectName)) {
    $ProjectName = 'Project'
}

$BundleDirectory = Join-Path $ProjectRoot '_AI-Bundles'
New-Item -ItemType Directory -Force -Path $BundleDirectory | Out-Null

$Timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$ZipPath = Join-Path $BundleDirectory ("{0}-AI-Context-{1}.zip" -f $ProjectName, $Timestamp)

function Get-RelativeProjectPath {
    param([Parameter(Mandatory = $true)][string]$FullPath)

    $relative = $FullPath.Substring($ProjectRoot.Length)
    return $relative.TrimStart([char[]]"\/")
}

function Add-SkipExample {
    param(
        [Parameter(Mandatory = $true)][string]$Reason,
        [Parameter(Mandatory = $true)][string]$Path
    )

    if ($script:SkipExamples.Count -lt 200) {
        [void]$script:SkipExamples.Add(("{0}: {1}" -f $Reason, $Path))
    }
}

function Test-SourceLike {
    param([Parameter(Mandatory = $true)][IO.FileInfo]$File)

    $ext = $File.Extension.ToLowerInvariant()

    if ($SourceLikeExtensions -contains $ext) {
        return $true
    }

    switch -Regex ($File.Name) {
        '^(README|LICENSE|LICENCE|NOTICE|COPYING|AUTHORS|CHANGELOG|CHANGES|VERSION)$' { return $true }
        '^(Dockerfile|Containerfile|Makefile|GNUmakefile)$' { return $true }
        '^CMakeLists\.txt$' { return $true }
        '^\.gitignore$' { return $true }
        '^\.gitattributes$' { return $true }
        '^\.dockerignore$' { return $true }
        default { return $false }
    }
}

function Test-SecretOrMachineLocal {
    param(
        [Parameter(Mandatory = $true)][IO.FileInfo]$File,
        [Parameter(Mandatory = $true)][string]$RelativePath
    )

    $name = $File.Name

    # Keep templates/examples, skip live .env files.
    if ($name -match '^\.env($|\.)' -and
        $name -notmatch '\.(example|sample|template)$') {
        return $true
    }

    if ($RelativePath -ieq 'config.json') {
        return $true
    }

    if ($name -ieq '.rocm-path' -or
        $name -ieq '.rocm-path.txt' -or
        $name -ieq 'api-key.txt' -or
        $name -ieq 'apikey.txt' -or
        $name -ieq 'secrets.json' -or
        $name -ieq 'credentials.json') {
        return $true
    }

    $ext = $File.Extension.ToLowerInvariant()
    if ($ext -in @('.pem', '.key', '.pfx', '.p12')) {
        return $true
    }

    return $false
}

# -----------------------------------------------------------------------------
# PRUNED RECURSIVE SCAN
# -----------------------------------------------------------------------------
$Files = New-Object 'System.Collections.Generic.List[System.IO.FileInfo]'
$SkipExamples = New-Object 'System.Collections.Generic.List[string]'

$script:SkippedDirectories = 0
$script:SkippedReparsePoints = 0
$script:SkippedByExtension = 0
$script:SkippedSecrets = 0
$script:SkippedLargeFiles = 0
$script:SkippedSystemJunk = 0

function Walk-ProjectDirectory {
    param([Parameter(Mandatory = $true)][string]$Directory)

    foreach ($path in [IO.Directory]::EnumerateFileSystemEntries($Directory)) {
        try {
            $attributes = [IO.File]::GetAttributes($path)
        }
        catch {
            throw "Could not inspect '$path': $($_.Exception.Message)"
        }

        $isDirectory = (($attributes -band [IO.FileAttributes]::Directory) -ne 0)
        $isReparse = (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)

        if ($isDirectory) {
            $name = [IO.Path]::GetFileName($path)
            $relative = Get-RelativeProjectPath $path

            if ($ExcludedDirectoryNames -contains $name) {
                $script:SkippedDirectories++
                Add-SkipExample 'excluded directory' $relative
                continue
            }

            # Avoid following junctions/symlinks outside the project or into loops.
            if ($isReparse) {
                $script:SkippedReparsePoints++
                Add-SkipExample 'reparse/junction directory' $relative
                continue
            }

            Walk-ProjectDirectory $path
            continue
        }

        $file = New-Object -TypeName IO.FileInfo -ArgumentList $path
        $relativeFile = Get-RelativeProjectPath $file.FullName

        if ($file.Name -ieq 'Thumbs.db' -or
            $file.Name -ieq 'desktop.ini' -or
            $file.Name -ieq '.DS_Store' -or
            $file.Name -ieq 'CMakeCache.txt' -or
            $file.Name -ieq 'cmake_install.cmake' -or
            $file.Name -ieq 'build.ninja' -or
            $file.Name -ieq '.ninja_deps' -or
            $file.Name -ieq '.ninja_log' -or
            $file.Name -ieq 'compile_commands.json') {
            $script:SkippedSystemJunk++
            Add-SkipExample 'system junk' $relativeFile
            continue
        }

        if (Test-SecretOrMachineLocal -File $file -RelativePath $relativeFile) {
            $script:SkippedSecrets++
            Add-SkipExample 'secret/machine-local' $relativeFile
            continue
        }

        $ext = $file.Extension.ToLowerInvariant()
        if ($ExcludedExtensions -contains $ext) {
            $script:SkippedByExtension++
            Add-SkipExample 'binary/archive/model extension' $relativeFile
            continue
        }

        if ($file.Length -gt $MaxNonSourceFileBytes -and -not (Test-SourceLike -File $file)) {
            $script:SkippedLargeFiles++
            Add-SkipExample ("> {0} MB non-source file" -f [int]($MaxNonSourceFileBytes / 1MB)) $relativeFile
            continue
        }

        [void]$Files.Add($file)
    }
}

Write-Host ""
Write-Host "Scanning project:"
Write-Host "  $ProjectRoot"
Write-Host ""

Walk-ProjectDirectory $ProjectRoot

if ($Files.Count -eq 0) {
    throw 'No files qualified for the context bundle.'
}

$Files = @(
    $Files |
    Sort-Object { Get-RelativeProjectPath $_.FullName }
)

# -----------------------------------------------------------------------------
# BUILD ZIP DIRECTLY -- NO STAGING COPY
# -----------------------------------------------------------------------------
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Add-TextZipEntry {
    param(
        [Parameter(Mandatory = $true)]
        [System.IO.Compression.ZipArchive]$Archive,

        [Parameter(Mandatory = $true)]
        [string]$EntryName,

        [Parameter(Mandatory = $true)]
        [string]$Text
    )

    $entry = $Archive.CreateEntry(
        $EntryName,
        [System.IO.Compression.CompressionLevel]::Optimal
    )

    $stream = $entry.Open()
    $encoding = New-Object -TypeName Text.UTF8Encoding -ArgumentList $false
    $writer = New-Object -TypeName IO.StreamWriter -ArgumentList $stream, $encoding

    try {
        $writer.Write($Text)
    }
    finally {
        $writer.Dispose()
    }
}

$totalBytes = [Int64]0
foreach ($file in $Files) {
    $totalBytes += $file.Length
}

$treeLines = New-Object 'System.Collections.Generic.List[string]'
foreach ($file in $Files) {
    $relative = (Get-RelativeProjectPath $file.FullName).Replace('\', '/')
    [void]$treeLines.Add(("{0}`t{1}" -f $relative, $file.Length))
}

$manifest = New-Object Text.StringBuilder
[void]$manifest.AppendLine("$ProjectName AI context bundle")
[void]$manifest.AppendLine("")
[void]$manifest.AppendLine("Created: $((Get-Date).ToString('yyyy-MM-dd HH:mm:ss zzz'))")
[void]$manifest.AppendLine("Source root: $ProjectRoot")
[void]$manifest.AppendLine("Included files: $($Files.Count)")
[void]$manifest.AppendLine(("Included source/context size: {0:N2} MB" -f ($totalBytes / 1MB)))
[void]$manifest.AppendLine("")
[void]$manifest.AppendLine("POLICY")
[void]$manifest.AppendLine("- Default behavior is INCLUDE. New project files/folders are automatically picked up.")
[void]$manifest.AppendLine("- Dependency caches, generated build output, compiled binaries, models, archives, secrets, and machine-local files are intentionally omitted.")
[void]$manifest.AppendLine("- Project-owned source such as src, scripts, assets, benchmarks, gufo-overlay, root docs/patches/config examples, and future normal code folders is retained unless it matches an exclusion rule.")
[void]$manifest.AppendLine("- .deps is intentionally omitted. gufo-overlay is NOT omitted.")
[void]$manifest.AppendLine("- The ZIP is a source/context handoff, not a runnable binary distribution.")
[void]$manifest.AppendLine("")
[void]$manifest.AppendLine("EXCLUDED DIRECTORY NAMES")
foreach ($item in ($ExcludedDirectoryNames | Sort-Object -Unique)) {
    [void]$manifest.AppendLine("- $item")
}
[void]$manifest.AppendLine("")
[void]$manifest.AppendLine("EXCLUDED FILE EXTENSIONS")
foreach ($item in ($ExcludedExtensions | Sort-Object -Unique)) {
    [void]$manifest.AppendLine("- $item")
}
[void]$manifest.AppendLine("")
[void]$manifest.AppendLine("SKIP COUNTS")
[void]$manifest.AppendLine("- Excluded directories: $script:SkippedDirectories")
[void]$manifest.AppendLine("- Reparse/junction directories: $script:SkippedReparsePoints")
[void]$manifest.AppendLine("- Binary/archive/model extensions: $script:SkippedByExtension")
[void]$manifest.AppendLine("- Secrets/machine-local files: $script:SkippedSecrets")
[void]$manifest.AppendLine("- Oversized non-source files: $script:SkippedLargeFiles")
[void]$manifest.AppendLine("- System junk files: $script:SkippedSystemJunk")
[void]$manifest.AppendLine("")
[void]$manifest.AppendLine("FIRST SKIPPED EXAMPLES (max 200)")
if ($SkipExamples.Count -eq 0) {
    [void]$manifest.AppendLine("- none")
}
else {
    foreach ($item in $SkipExamples) {
        [void]$manifest.AppendLine("- $item")
    }
}

if (Test-Path -LiteralPath $ZipPath) {
    Remove-Item -LiteralPath $ZipPath -Force
}

$zipStream = [IO.File]::Open(
    $ZipPath,
    [IO.FileMode]::CreateNew,
    [IO.FileAccess]::ReadWrite,
    [IO.FileShare]::None
)

$archive = New-Object -TypeName System.IO.Compression.ZipArchive -ArgumentList @(
    $zipStream,
    [System.IO.Compression.ZipArchiveMode]::Create,
    $false
)

try {
    $index = 0

    foreach ($file in $Files) {
        $index++
        $entryName = (Get-RelativeProjectPath $file.FullName).Replace('\', '/')

        if (($index % 100) -eq 0 -or $index -eq $Files.Count) {
            Write-Host ("Adding {0}/{1}: {2}" -f $index, $Files.Count, $entryName)
        }

        [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $archive,
            $file.FullName,
            $entryName,
            [System.IO.Compression.CompressionLevel]::Optimal
        )
    }

    Add-TextZipEntry `
        -Archive $archive `
        -EntryName '__AI_BUNDLE_METADATA__/MANIFEST.txt' `
        -Text $manifest.ToString()

    Add-TextZipEntry `
        -Archive $archive `
        -EntryName '__AI_BUNDLE_METADATA__/PROJECT_TREE.txt' `
        -Text (($treeLines -join [Environment]::NewLine) + [Environment]::NewLine)
}
finally {
    $archive.Dispose()
    $zipStream.Dispose()
}

$zipInfo = Get-Item -LiteralPath $ZipPath

Write-Host ""
Write-Host "DONE"
Write-Host "Bundle:"
Write-Host "  $ZipPath"
Write-Host ""
Write-Host ("Included files: {0:N0}" -f $Files.Count)
Write-Host ("Source/context: {0:N2} MB" -f ($totalBytes / 1MB))
Write-Host ("ZIP size:       {0:N2} MB" -f ($zipInfo.Length / 1MB))
Write-Host ""
Write-Host "Intentionally skipped:"
Write-Host ("  dependency/generated directories : {0}" -f $script:SkippedDirectories)
Write-Host ("  compiled/model/archive files     : {0}" -f $script:SkippedByExtension)
Write-Host ("  secrets/machine-local files      : {0}" -f $script:SkippedSecrets)
Write-Host ("  oversized non-source files       : {0}" -f $script:SkippedLargeFiles)
Write-Host ("  junction/reparse directories     : {0}" -f $script:SkippedReparsePoints)
Write-Host ""
Write-Host "Metadata inside ZIP:"
Write-Host "  __AI_BUNDLE_METADATA__/MANIFEST.txt"
Write-Host "  __AI_BUNDLE_METADATA__/PROJECT_TREE.txt"
