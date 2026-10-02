param([string]$Stage = '.cache\inference-candidate')
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Stage = [IO.Path]::GetFullPath((Join-Path $Root $Stage))
if (-not $Stage.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase) -or
    $Stage -eq (Join-Path $Root 'dist')) { throw 'Candidate must be a separate directory inside the project.' }
$Build = Join-Path $Root 'build'
$cache = Get-Content (Join-Path $Build 'CMakeCache.txt') -Raw
$Gufo = [regex]::Match($cache, '(?m)^GUFO_ROOT:PATH=(.+)$').Groups[1].Value.Trim()
if (-not (Test-Path $Gufo)) { throw 'Configure the normal pinned build before creating a candidate.' }
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.14.44.17.14.x86.x64 -property installationPath
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
$lines = & $env:ComSpec /d /s /c "call `"$vcvars`" amd64 -vcvars_ver=14.44 >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Could not activate MSVC 14.44.' }
foreach ($line in $lines) {
    $index = $line.IndexOf('=')
    if ($index -gt 0) { [Environment]::SetEnvironmentVariable($line.Substring(0, $index), $line.Substring($index + 1), 'Process') }
}
$runtime = Get-Content (Join-Path $Root 'dist\runtime.json') -Raw | ConvertFrom-Json
$env:ROCM_PATH = $runtime.RocmPath; $env:HIP_PATH = $runtime.RocmPath
$env:HIP_DEVICE_LIB_PATH = $runtime.DeviceLibPath; $env:DEVICE_LIB_PATH = $runtime.DeviceLibPath
$env:ROCBLAS_TENSILE_LIBPATH = $runtime.RocblasTensileLibPath
$env:HIPBLASLT_TENSILE_LIBPATH = $runtime.HipblasltTensileLibPath
$env:PATH = "$($runtime.RocmPath)\bin;$($runtime.RocmPath)\lib\llvm\bin;$($runtime.VcpkgBin);$env:PATH"
& (Join-Path $PSScriptRoot 'apply-gufo-overlay.ps1') -GufoRoot $Gufo -OverlayRoot (Join-Path $Root 'gufo-overlay')
$cmake = 'C:\Program Files\CMake\bin\cmake.exe'
& $cmake --build $Build --target FlashNextVelocityEngine FlashNextVelocityUnitTests FlashNextQ8ShallowTest FlashNextMtpReplayTest FlashNextMtpCostProbe -j 12
if ($LASTEXITCODE -ne 0) { throw 'Candidate native build failed.' }
New-Item -ItemType Directory -Force $Stage, (Join-Path $Stage 'engine') | Out-Null
& 'C:\Program Files\dotnet\dotnet.exe' publish (Join-Path $Root 'src\desktop\FlashNextVelocity.Desktop.csproj') -c Release -r win-x64 --self-contained false -o $Stage
if ($LASTEXITCODE -ne 0) { throw 'Candidate desktop publish failed.' }
# Retain the exact pinned runtime DLLs; never rebuild the live installation.
Get-ChildItem (Join-Path $Root 'dist\engine') -File | Copy-Item -Destination (Join-Path $Stage 'engine') -Force
Get-ChildItem (Join-Path $Build 'bin') -File | Copy-Item -Destination (Join-Path $Stage 'engine') -Force
foreach ($name in @('config.json', 'ui.json', 'runtime.json', 'project-root.txt')) {
    Copy-Item -LiteralPath (Join-Path $Root "dist\$name") -Destination (Join-Path $Stage $name) -Force
}
$binary = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes((Join-Path $Stage 'engine\FlashNextVelocity.Engine.exe')))
if (-not $binary.Contains('fnv-mtp-cache-replay-v13')) { throw 'Candidate contains a stale native engine.' }
Write-Output "Candidate ready: $Stage"
