param(
  [ValidateSet('distribution','halo_greedy')][string]$Mode = 'distribution',
  [string]$Stage = '.cache\inference-candidate',
  [switch]$CostAudit
)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Stage = [IO.Path]::GetFullPath((Join-Path $Root $Stage))
$runtime = Get-Content (Join-Path $Stage 'runtime.json') -Raw | ConvertFrom-Json
$cfg = Get-Content (Join-Path $Stage 'config.json') -Raw | ConvertFrom-Json
$env:ROCM_PATH=$runtime.RocmPath; $env:HIP_PATH=$runtime.RocmPath
$env:HIP_DEVICE_LIB_PATH=$runtime.DeviceLibPath; $env:DEVICE_LIB_PATH=$runtime.DeviceLibPath
$env:ROCBLAS_TENSILE_LIBPATH=$runtime.RocblasTensileLibPath
$env:HIPBLASLT_TENSILE_LIBPATH=$runtime.HipblasltTensileLibPath
$env:PATH="$Stage\engine;$($runtime.RocmPath)\bin;$($runtime.VcpkgBin);$env:PATH"
if ($CostAudit) {
  & (Join-Path $Stage 'engine\FlashNextMtpCostProbe.exe') --model $cfg.model --mtp-model $cfg.mtp --batch $cfg.prefill_batch --cost-audit 1
} else {
  & (Join-Path $Stage 'engine\FlashNextMtpReplayTest.exe') $cfg.model $cfg.mtp $Mode
}
if ($LASTEXITCODE -ne 0) { throw "MTP replay failed: $Mode" }
