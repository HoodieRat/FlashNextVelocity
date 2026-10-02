param([string]$Stage='.cache\inference-candidate', [int]$Port=8081)
$ErrorActionPreference='Stop'
$Root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Stage=[IO.Path]::GetFullPath((Join-Path $Root $Stage))
if (-not $Stage.StartsWith($Root+'\',[StringComparison]::OrdinalIgnoreCase) -or $Stage -eq "$Root\dist") {throw 'Use an isolated candidate directory.'}
$runtime=Get-Content "$Stage\runtime.json" -Raw | ConvertFrom-Json
$cfg=Get-Content "$Stage\config.json" -Raw | ConvertFrom-Json
$cfg.port=$Port
$desired="$Stage\acceptance-config.json"
$snapshot="$Stage\acceptance-launch.json"
$cfg | ConvertTo-Json -Depth 20 | Set-Content $desired -Encoding utf8NoBOM
Copy-Item -LiteralPath $desired -Destination $snapshot -Force
$env:ROCM_PATH=$runtime.RocmPath; $env:HIP_PATH=$runtime.RocmPath
$env:HIP_DEVICE_LIB_PATH=$runtime.DeviceLibPath; $env:DEVICE_LIB_PATH=$runtime.DeviceLibPath
$env:ROCBLAS_TENSILE_LIBPATH=$runtime.RocblasTensileLibPath
$env:HIPBLASLT_TENSILE_LIBPATH=$runtime.HipblasltTensileLibPath
$env:PATH="$Stage\engine;$($runtime.RocmPath)\bin;$($runtime.VcpkgBin);$env:PATH"
$p=Start-Process -FilePath "$Stage\engine\FlashNextVelocity.Engine.exe" -ArgumentList @('--config', ('"'+$snapshot+'"'), '--studio-config', ('"'+$desired+'"')) -WorkingDirectory "$Stage\engine" -WindowStyle Hidden -PassThru -RedirectStandardOutput "$Stage\acceptance-stdout.log" -RedirectStandardError "$Stage\acceptance-stderr.log"
$p.Id | Set-Content "$Stage\acceptance-pid.txt"
Write-Output "Candidate PID $($p.Id), port $Port"
