param([switch]$SkipBundledTests)
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$Root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if($Root.Length -gt 100){throw "Project path is too long for a reliable ROCm/C++ build. Put it somewhere short, e.g. C:\FlashNextVelocity. Current: $Root"}
$Deps=Join-Path $Root '.deps';$Vcpkg=Join-Path $Deps 'vcpkg';$ProjectRocm=Join-Path $Deps 'rocm';$Downloads=Join-Path $Deps 'downloads';$Cache=Join-Path $Root '.cache';$Build=Join-Path $Root 'build';$Dist=Join-Path $Root 'dist';$Logs=Join-Path $Root 'logs'
New-Item -ItemType Directory -Force $Deps,$Downloads,$Cache,$Logs | Out-Null
$Transcript=Join-Path $Logs ("build-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
Start-Transcript -Path $Transcript -Force | Out-Null
$GufoCommit='cff564964e8506c0abb3e530cecb55332187ac6c';$RocmVersion='10.0.0';$RocmUrl="https://stable.repo.amd.com/rocm/core/tarball/therock-dist-windows-gfx1151-$RocmVersion.tar.gz";$MsvcComponent='Microsoft.VisualStudio.Component.VC.14.44.17.14.x86.x64'
function Has([string]$n){return [bool](Get-Command $n -EA SilentlyContinue)}
function Winget([string]$id,[string[]]$extra=@()){if(-not(Has winget)){throw "winget is required to install $id"};& winget install --id $id -e --accept-package-agreements --accept-source-agreements @extra;if($LASTEXITCODE -ne 0){throw "winget failed installing $id"}}
if(-not(Has git)){Winget 'Git.Git';$env:PATH+=';C:\Program Files\Git\cmd'}
if(-not(Has cmake)){Winget 'Kitware.CMake';$env:PATH+=';C:\Program Files\CMake\bin'}
if(-not(Has ninja)){Winget 'Ninja-build.Ninja';$n=Get-ChildItem (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages') -Recurse -Filter ninja.exe -EA SilentlyContinue|Select-Object -First 1;if($n){$env:PATH="$($n.DirectoryName);$env:PATH"}}
if(-not(Has ninja)){throw 'Ninja is unavailable after installation.'}
if(-not(Has dotnet)){Winget 'Microsoft.DotNet.SDK.8';$env:PATH+=";$env:ProgramFiles\dotnet"}
if(-not(Has dotnet)){throw '.NET 8 SDK is unavailable.'}

# Pin HIP host headers to MSVC 14.44 even when newer Visual Studio is installed.
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if(-not(Test-Path $vswhere)){Winget 'Microsoft.VisualStudio.2022.BuildTools' @('--override',"--wait --passive --add $MsvcComponent --add Microsoft.VisualStudio.Component.Windows11SDK.26100")}
if(-not(Test-Path $vswhere)){throw 'Visual Studio Installer/vswhere not found.'}
function FindVs1444 { $x=& $vswhere -latest -products * -requires $MsvcComponent -property installationPath;if($LASTEXITCODE -eq 0 -and $x){return [string]$x};return $null }
$Vs=FindVs1444
if(-not $Vs){$existing=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath;$setup="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\setup.exe";if($existing -and (Test-Path $setup)){Write-Host 'Adding MSVC 14.44 for Windows HIP...' -ForegroundColor Yellow;$args="modify --installPath `"$existing`" --add $MsvcComponent --add Microsoft.VisualStudio.Component.Windows11SDK.26100 --passive --norestart";$p=Start-Process $setup -ArgumentList $args -Verb RunAs -Wait -PassThru;if($p.ExitCode -ne 0 -and $p.ExitCode -ne 3010){Write-Warning "Visual Studio Installer returned $($p.ExitCode)"}};$Vs=FindVs1444}
if(-not $Vs){Winget 'Microsoft.VisualStudio.2022.BuildTools' @('--override',"--wait --passive --add $MsvcComponent --add Microsoft.VisualStudio.Component.Windows11SDK.26100");$Vs=FindVs1444}
if(-not $Vs){throw 'MSVC 14.44 could not be installed.'}
$vcvars=Join-Path $Vs 'VC\Auxiliary\Build\vcvarsall.bat';$lines=& $env:ComSpec /d /s /c "call `"$vcvars`" amd64 -vcvars_ver=14.44 >nul && set";if($LASTEXITCODE -ne 0){throw 'Could not activate MSVC 14.44.'};foreach($line in $lines){$i=$line.IndexOf('=');if($i -gt 0){[Environment]::SetEnvironmentVariable($line.Substring(0,$i),$line.Substring($i+1),'Process')}};if(-not $env:VCToolsVersion.StartsWith('14.44.')){throw "Wrong MSVC toolset selected: $env:VCToolsVersion"};Write-Host "HIP host toolset: MSVC $env:VCToolsVersion" -ForegroundColor Green

# Pinned Gufo checkout. Re-prepare only when the pinned commit or our integrated Windows patch script changes.
$prepareScript=Join-Path $PSScriptRoot 'prepare-gufo.ps1'
$prepareHash=(Get-FileHash $prepareScript -Algorithm SHA256).Hash
$Gufo=Join-Path $env:LOCALAPPDATA "FlashNextVelocity\gufo-$($GufoCommit.Substring(0,12))-$($prepareHash.Substring(0,8))"
$prepareMarker=Join-Path $Gufo '.flashnextvelocity-prepared.json'
$needPrepare=$true
if(-not(Test-Path (Join-Path $Gufo '.git'))){
  & git -c core.longpaths=true clone --filter=blob:none --no-checkout https://github.com/pixmaate/gufo.git $Gufo
  if($LASTEXITCODE -ne 0){throw 'Gufo clone failed.'}
  & git -C $Gufo checkout --detach $GufoCommit
  if($LASTEXITCODE -ne 0){throw 'Pinned Gufo checkout failed.'}
}else{
  try{
    $head=(& git -C $Gufo rev-parse HEAD).Trim()
    if($LASTEXITCODE -eq 0 -and $head -eq $GufoCommit){
      if(Test-Path $prepareMarker){
        $marker=Get-Content $prepareMarker -Raw|ConvertFrom-Json
        if($marker.commit -eq $GufoCommit -and $marker.prepare_sha256 -eq $prepareHash){$needPrepare=$false}
      }else{
        # Adopt a tree prepared by v1.0.2 rather than resetting/recompiling it once more.
        $executor=Join-Path $Gufo 'src\models\qwen38_flash_next\kernels\rocm\executor.cpp'
        $vision=Join-Path $Gufo 'src\models\qwen\vision\encoder.hip'
        $winReader=Join-Path $Gufo 'src\core\gguf_reader.cpp'
        $alreadyPrepared=(Test-Path $executor) -and (Test-Path $vision) -and (Test-Path $winReader) -and
          [bool](Select-String -Path $executor -SimpleMatch 'hipBLASLt and hipBLAS prefill GEMM failed' -Quiet) -and
          [bool](Select-String -Path $vision -SimpleMatch 'VisionScalar' -Quiet) -and
          [bool](Select-String -Path $winReader -SimpleMatch 'win_file.hpp' -Quiet)
        if($alreadyPrepared){
          $needPrepare=$false
          $marker=[ordered]@{commit=$GufoCommit;prepare_sha256=$prepareHash}
          [IO.File]::WriteAllText($prepareMarker,($marker|ConvertTo-Json),[Text.UTF8Encoding]::new($false))
          Write-Host 'Adopted existing prepared Gufo tree from the previous successful native build.' -ForegroundColor Green
        }
      }
    }
  }catch{$needPrepare=$true}
}
if($needPrepare){
  $head=(& git -C $Gufo rev-parse HEAD).Trim()
  if($LASTEXITCODE -ne 0 -or $head -ne $GufoCommit){throw "Gufo checkout is not pinned to $GufoCommit`: $Gufo"}
  if(Test-Path $prepareMarker){throw "Gufo preparation marker does not match this build. Preserve and inspect the source before retrying: $Gufo"}
  & $prepareScript -GufoRoot $Gufo
  if($LASTEXITCODE -ne 0){throw 'Integrated Gufo Windows preparation failed.'}
  $marker=[ordered]@{commit=$GufoCommit;prepare_sha256=$prepareHash}
  [IO.File]::WriteAllText($prepareMarker,($marker|ConvertTo-Json),[Text.UTF8Encoding]::new($false))
}else{
  Write-Host 'Gufo Windows source preparation already current; reusing prepared tree.' -ForegroundColor Green
}

# Reapply project-owned Gufo performance/correctness sources on every build.
# This survives future source preparation/reset without maintaining a fragile
# numbered patch stack.
$overlayScript=Join-Path $PSScriptRoot 'apply-gufo-overlay.ps1'
$overlayRoot=Join-Path $Root 'gufo-overlay'
& $overlayScript -GufoRoot $Gufo -OverlayRoot $overlayRoot

# Source guard for the compact-verification confidence-stop repair.  The old
# fatal text must never survive source preparation/overlay application.
$executorSource=Join-Path $Gufo 'src\models\qwen38_flash_next\kernels\rocm\executor.cpp'
$engineSource=Join-Path $Gufo 'src\models\qwen38_flash_next\engine.cpp'
$legacyCompactFatal='compact verification requires at least one draft row'
if((Get-Content $executorSource -Raw).Contains($legacyCompactFatal) -or
   (Get-Content $engineSource -Raw).Contains($legacyCompactFatal)){
  throw 'Gufo overlay is stale: legacy anchor-only compact-verification fatal is still present in source.'
}
if(-not (Select-String -Path $executorSource -SimpleMatch 'compact_requested && n_logits >= 2' -Quiet)){
  throw 'Gufo overlay is missing the executor anchor-only compact fallback.'
}
if((Select-String -Path $executorSource -SimpleMatch 'UseEagerVerifyDiagnostic' -Quiet) -or
   (Select-String -Path $executorSource -SimpleMatch 'eager_verify_diagnostic' -Quiet)){
  throw 'Benchmark-only eager verification bypass is still present; normal HIP graph replay was not restored.'
}
Write-Host 'Normal HIP-graph verification path restored.' -ForegroundColor Green
$batchSource=Join-Path $Gufo 'src\models\qwen38_flash_next\kernels\rocm\batch.cpp'
$referenceSource=Join-Path $Gufo 'src\models\qwen38_flash_next\reference.cpp'
$executorText=Get-Content $executorSource -Raw
$engineText=Get-Content $engineSource -Raw
$batchText=Get-Content $batchSource -Raw
$referenceText=Get-Content $referenceSource -Raw
if(-not $executorText.Contains('RmsNormRows(s_.mtp_h, l.nextn_hnorm.f32(), s_.mtp_h, n, hc_dim, c.hc_count,') -or
   -not $batchText.Contains('RmsNormRows(base.mtp_h, l.nextn_hnorm.f32(), base.mtp_h, rows, c.HcDim(),') -or
   -not $batchText.Contains('c.hc_count, c.rms_eps, stream_);') -or
   -not $referenceText.Contains('const auto* hnorm = static_cast<const float*>(l.nextn_hnorm.data);') -or
   $referenceText.Contains('cpu::RmsNorm(norm, static_cast<const float*>(l.nextn_hnorm.data), c_.rms_eps);')){
  throw 'MTP hidden-state normalization regression: expected independent per-HC-stream RMSNorm in single, batched, and reference paths.'
}
Write-Host 'MTP hidden handoff verified: independent RMSNorm per hyper-connection stream.' -ForegroundColor Green
$mtpCostsSource=Join-Path $gufo 'src\models\qwen38_flash_next\mtp_costs.hpp'
if(-not(Test-Path $mtpCostsSource) -or
   (Select-String -Path $mtpCostsSource -SimpleMatch 'kFnvWindowsC1ShallowCosts' -Quiet) -or
   -not (Select-String -Path $mtpCostsSource -SimpleMatch 'kMtpCycleMilliseconds' -Quiet)){
  throw 'Gufo overlay did not restore the calibrated upstream MTP cost curves.'
}
Write-Host 'Gufo calibrated MTP cost curves restored; unvalidated Windows override removed.' -ForegroundColor Green
$mtpSamplingSource=Join-Path $Gufo 'src\models\qwen38_flash_next\mtp_sampling.hpp'
if(-not(Test-Path $mtpSamplingSource)){
  throw "Gufo overlay is missing sampled-MTP source: $mtpSamplingSource"
}
$mtpSamplingText=[IO.File]::ReadAllText($mtpSamplingSource)
$mtpSamplingRequired=[ordered]@{
  '256-candidate support'='kMtpCandidates = 256'
  'compact sampled frontier'='SampleMtpFrontierCompact'
  'adaptive exact verification width'='CompactMtpVerificationWidthForKeep'
}
$mtpSamplingMissing=@()
foreach($entry in $mtpSamplingRequired.GetEnumerator()){
  if(-not $mtpSamplingText.Contains([string]$entry.Value)){
    $mtpSamplingMissing += [string]$entry.Key
  }
}
if($mtpSamplingMissing.Count -gt 0){
  throw ("Gufo overlay sampled-MTP validation failed. Missing implementation marker(s): " + ($mtpSamplingMissing -join ', ') + ". Source: $mtpSamplingSource")
}
Write-Host 'Sampled-MTP path verified: 256 candidates + adaptive exact verification width.' -ForegroundColor Green
$kernelsSource=Join-Path $Gufo 'src\models\qwen38_flash_next\kernels\rocm\kernels.hip.cpp'
$kernelsHeader=Join-Path $Gufo 'src\models\qwen38_flash_next\kernels\rocm\kernels.hpp'
$executorHeader=Join-Path $Gufo 'src\models\qwen38_flash_next\kernels\rocm\executor.hpp'
if(-not(Test-Path $kernelsSource) -or -not(Test-Path $kernelsHeader) -or -not(Test-Path $executorHeader)){
  throw 'Gufo overlay compact-verification source set is incomplete (kernels.hip.cpp / kernels.hpp / executor.hpp).'
}
$kernelsText=[IO.File]::ReadAllText($kernelsSource)
$kernelsHeaderText=[IO.File]::ReadAllText($kernelsHeader)
$executorHeaderText=[IO.File]::ReadAllText($executorHeader)
$executorText=[IO.File]::ReadAllText($executorSource)
$compactVerifyRequired=[ordered]@{
  'Top-64 kernel definition'='void MtpTopVerificationCandidates64('
  'Top-128 kernel definition'='void MtpTopVerificationCandidates128('
  'Top-256 kernel definition'='void MtpTopVerificationCandidates256('
}
$compactVerifyMissing=@()
foreach($entry in $compactVerifyRequired.GetEnumerator()){
  if(-not $kernelsText.Contains([string]$entry.Value)){
    $compactVerifyMissing += [string]$entry.Key
  }
}
foreach($marker in @('MtpTopVerificationCandidates64(', 'MtpTopVerificationCandidates128(', 'MtpTopVerificationCandidates256(')){
  if(-not $kernelsHeaderText.Contains($marker)){
    $compactVerifyMissing += "kernel declaration $marker"
  }
  if(-not $executorText.Contains($marker)){
    $compactVerifyMissing += "executor dispatch $marker"
  }
}
foreach($marker in @('kCompactVerify64', 'kCompactVerify128', 'kCompactVerify256')){
  if(-not $executorHeaderText.Contains($marker)){
    $compactVerifyMissing += "transfer mode $marker"
  }
}
if(-not $executorText.Contains('verification frontier retain')){
  $compactVerifyMissing += 'GPU-retained full final frontier'
}
if(-not $executorText.Contains('DownloadRetainedFrontier(')){
  $compactVerifyMissing += 'lazy retained-frontier download fallback'
}
if($compactVerifyMissing.Count -gt 0){
  throw ("Gufo overlay compact-verification validation failed. Missing implementation marker(s): " + ($compactVerifyMissing -join ', ') + ". Reapply the v1.0.10 root overlay.")
}
if(-not (Select-String -Path $mtpSamplingSource -SimpleMatch 'HaloGreedyMtpProposal' -Quiet) -or
   -not (Select-String -Path $mtpSamplingSource -SimpleMatch 'VerifyDeterministicMtpProposalCompact' -Quiet) -or
   -not (Select-String -Path $engineSource -SimpleMatch 'SampledMtpProposalMode::kHaloGreedy' -Quiet) -or
   -not (Select-String -Path $engineSource -SimpleMatch 'only target verification' -Quiet) -or
   -not (Select-String -Path $engineSource -SimpleMatch 'MTP recurrent-state lifecycle mismatch after catch-up' -Quiet)){
  throw 'Gufo overlay is missing the halo-compatible sampled-MTP proposal path or recurrent-state invariant.'
}
Write-Host 'Adaptive Top-64/128/256 exact verification + retained final frontier + halo-compatible sampled-MTP path verified.' -ForegroundColor Green
$asyncPipelineRequired=[ordered]@{
  'GPU feedback MTP chain'='MtpHaloGreedyChain('
  'device-fed draft token path'='device-fed MTP chain does not transfer candidates'
  'device-resident chain token buffer'='mtp_chain_tokens'
  'single bulk chain download'='async MTP chain download'
  'session-owned retained frontier'='frontier_logits'
  'lazy retained-frontier fallback'='DownloadRetainedFrontier('
  'device-only rollback deferral'='Device-only restores are ordered by the same nonblocking stream'
}
$asyncPipelineMissing=@()
foreach($entry in $asyncPipelineRequired.GetEnumerator()){
  if(-not $executorText.Contains([string]$entry.Value) -and
     -not $executorHeaderText.Contains([string]$entry.Value)){
    $asyncPipelineMissing += [string]$entry.Key
  }
}
foreach($marker in @('async_halo_chain', 'DownloadRetainedFrontier(*session_', 'retained_frontier_downloads')){
  if(-not $engineText.Contains($marker)){
    $asyncPipelineMissing += "engine marker $marker"
  }
}
if($asyncPipelineMissing.Count -gt 0){
  throw ("Gufo overlay async-pipeline validation failed. Missing implementation marker(s): " + ($asyncPipelineMissing -join ', '))
}
Write-Host 'Async halo MTP feedback chain + GPU-retained lazy verification frontier + deferred device rollback verified.' -ForegroundColor Green
if(-not(Test-Path $kernelsSource) -or
   -not (Select-String -Path $kernelsSource -SimpleMatch 'LLVM 23 hoists the next K block' -Quiet)){
  throw 'Gufo overlay is missing the LLVM 23 gfx1151 W8A8 scheduler-spill fix.'
}
Write-Host 'LLVM 23 gfx1151 W8A8 scheduler-spill fix verified.' -ForegroundColor Green
if(-not (Select-String -Path $engineSource -SimpleMatch 'pending->chain.size() < 2' -Quiet)){
  throw 'Gufo overlay is missing the engine confidence-stop compact fallback.'
}
$contextLookupSource=Join-Path $Gufo 'src\models\qwen38_flash_next\context_lookup.hpp'
if(-not(Test-Path $contextLookupSource) -or
   -not (Select-String -Path $contextLookupSource -SimpleMatch 'FindContextLookup' -Quiet) -or
   -not (Select-String -Path $engineSource -SimpleMatch 'lookup_budget' -Quiet) -or
   -not (Select-String -Path $engineSource -SimpleMatch 'The lookup lane replaces MTP proposal generation, not MTP recurrent' -Quiet) -or
   -not (Select-String -Path $engineSource -SimpleMatch 'Lookup acceptance is deliberately excluded' -Quiet)){
  throw 'Gufo overlay is missing the native context-lookup proposal lane or its MTP-policy isolation.'
}
Write-Host 'Native context lookup + recurrent-state maintenance + MTP-policy isolation source verified.' -ForegroundColor Green

function ValidRocm([string]$p){if(-not $p -or -not(Test-Path $p)){return $false};$cc=Get-ChildItem $p -Recurse -Filter clang++.exe -EA SilentlyContinue|Where-Object {$_.FullName -match 'llvm\\bin\\clang\+\+\.exe$'}|Select-Object -First 1;$isa=Get-ChildItem $p -Recurse -Filter oclc_isa_version_1151.bc -EA SilentlyContinue|Select-Object -First 1;return [bool]($cc -and $isa)}
# Studio owns its ROCm SDK. Reuse/copy an existing SDK once, but never keep a runtime dependency on another project folder.
$ensureRocm=Join-Path $PSScriptRoot 'ensure-local-rocm.ps1'
if(-not(ValidRocm $ProjectRocm)){
  & $ensureRocm -Root $Root -AllowMissing
  if($LASTEXITCODE -ne 0){throw 'ROCm migration helper failed.'}
}
if(-not(ValidRocm $ProjectRocm)){
  $Rocm=$ProjectRocm;New-Item -ItemType Directory -Force $Rocm|Out-Null
  $archive=Join-Path $Downloads "therock-dist-windows-gfx1151-$RocmVersion.tar.gz"
  if(-not(Test-Path $archive)){Write-Host "Downloading AMD ROCm $RocmVersion gfx1151 into Studio..." -ForegroundColor Cyan;& curl.exe -L --fail --retry 3 -o $archive $RocmUrl;if($LASTEXITCODE -ne 0){throw 'ROCm download failed.'}}
  & tar.exe -xzf $archive -C $Rocm --strip-components=1
  if($LASTEXITCODE -ne 0){throw 'ROCm extraction failed.'}
}
if(-not(ValidRocm $ProjectRocm)){throw "Project-local ROCm is invalid: $ProjectRocm"}
$Rocm=(Resolve-Path $ProjectRocm).Path
$clangxx=Get-ChildItem $Rocm -Recurse -Filter clang++.exe|Where-Object {$_.FullName -match 'llvm\\bin\\clang\+\+\.exe$'}|Select-Object -First 1;if(-not $clangxx){throw 'ROCm clang++ missing.'}
$clang=Join-Path $clangxx.Directory.FullName 'clang.exe'
$bitcode=Get-ChildItem $Rocm -Recurse -Filter oclc_isa_version_1151.bc -EA SilentlyContinue|Select-Object -First 1
$rb=Get-ChildItem $Rocm -Recurse -Directory -EA SilentlyContinue|Where-Object {$_.FullName -match 'rocblas\\library\\gfx1151$'}|Select-Object -First 1
if(-not $rb){$rb=Get-ChildItem $Rocm -Recurse -Directory -EA SilentlyContinue|Where-Object {$_.FullName -match 'rocblas\\library$'}|Select-Object -First 1}
$hb=Get-ChildItem $Rocm -Recurse -Directory -EA SilentlyContinue|Where-Object {$_.FullName -match 'hipblaslt\\library\\gfx1151$'}|Select-Object -First 1
if(-not $hb){$hbf=Get-ChildItem $Rocm -Recurse -File -Filter 'TensileLibrary_lazy_gfx1151.*' -EA SilentlyContinue|Where-Object {$_.FullName -match 'hipblaslt'}|Select-Object -First 1;if($hbf){$hb=$hbf.Directory}}
if(-not $hb){throw "hipBLASLt gfx1151 library assets missing under $Rocm"}
$env:ROCM_PATH=$Rocm;$env:HIP_PATH=$Rocm;$env:HIP_CLANG_PATH=$clangxx.Directory.FullName;$env:HIP_PLATFORM='amd';$env:PATH="$(Join-Path $Rocm 'bin');$($clangxx.Directory.FullName);$env:PATH"
if($bitcode){$env:HIP_DEVICE_LIB_PATH=$bitcode.Directory.FullName;$env:DEVICE_LIB_PATH=$bitcode.Directory.FullName}
if($rb){$env:ROCBLAS_TENSILE_LIBPATH=$rb.FullName}
$env:HIPBLASLT_TENSILE_LIBPATH=$hb.FullName
Write-Host "Project-local ROCm: $Rocm" -ForegroundColor Green
Write-Host "hipBLASLt gfx1151 assets: $($hb.FullName)" -ForegroundColor Green
[IO.File]::WriteAllText((Join-Path $Root '.rocm-path.txt'),$Rocm,[Text.UTF8Encoding]::new($false))
$probe=Join-Path $Cache 'probe.hip';New-Item -ItemType Directory -Force $Cache|Out-Null;[IO.File]::WriteAllText($probe,"#include <hip/hip_runtime.h>`n__global__ void k() {}`nint main(){return 0;}`n",[Text.UTF8Encoding]::new($false));& $clangxx.FullName --offload-arch=gfx1151 -c $probe -o (Join-Path $Cache 'probe.obj');if($LASTEXITCODE -ne 0){throw 'ROCm gfx1151 compiler probe failed.'}
# Project-local vcpkg and cache.
Remove-Item Env:VCPKG_ROOT -EA SilentlyContinue;$env:VCPKG_DEFAULT_BINARY_CACHE=Join-Path $Cache 'vcpkg\archives';New-Item -ItemType Directory -Force $env:VCPKG_DEFAULT_BINARY_CACHE|Out-Null
if(-not(Test-Path (Join-Path $Vcpkg '.git'))){if(Test-Path $Vcpkg){Remove-Item $Vcpkg -Recurse -Force};& git -c core.longpaths=true clone --depth 1 https://github.com/microsoft/vcpkg.git $Vcpkg;if($LASTEXITCODE -ne 0){throw 'vcpkg clone failed.'}; & git -C $Vcpkg config core.longpaths true}
if(-not(Test-Path (Join-Path $Vcpkg 'vcpkg.exe'))){& (Join-Path $Vcpkg 'bootstrap-vcpkg.bat') -disableMetrics;if($LASTEXITCODE -ne 0){throw 'vcpkg bootstrap failed.'}}
& (Join-Path $Vcpkg 'vcpkg.exe') install --triplet x64-windows icu openssl libpng libjpeg-turbo curl;if($LASTEXITCODE -ne 0){throw 'vcpkg dependency install failed.'}

# Native engine.
$toolchain=Join-Path $Vcpkg 'scripts\buildsystems\vcpkg.cmake';$prefix=$Rocm.Replace('\','/')
# IMPORTANT: --fresh only clears CMake's cache. It does not remove Ninja object
# files. Project ZIP/overlay files can carry timestamps older than those objects,
# causing Ninja to relink stale native code. Always rebuild this small project
# from a genuinely empty build tree so the binary must match the current source.
if(Test-Path $Build){
  Write-Host 'Removing previous build tree to prevent stale native/.NET objects...' -ForegroundColor Cyan
  Remove-Item $Build -Recurse -Force
}
New-Item -ItemType Directory -Force $Build | Out-Null
# Always configure from a fresh CMake cache. Reusing a cache can retain absolute HIP/ROCm
# paths from an older project location (for example C:\flashfknworkalready) and can also
# trigger CMake's compiler-change auto-reset, which drops non-persistent configure state.
& cmake.exe --fresh -S $Root -B $Build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DGUFO_ROOT=$($Gufo.Replace('\','/'))" "-DCMAKE_TOOLCHAIN_FILE=$($toolchain.Replace('\','/'))" -DVCPKG_TARGET_TRIPLET=x64-windows "-DCMAKE_PREFIX_PATH=$prefix" "-DCMAKE_C_COMPILER=$($clang.Replace('\','/'))" "-DCMAKE_CXX_COMPILER=$($clangxx.FullName.Replace('\','/'))" "-DCMAKE_HIP_COMPILER=$($clangxx.FullName.Replace('\','/'))" "-DCMAKE_HIP_COMPILER_ROCM_ROOT=$prefix" -DCMAKE_HIP_ARCHITECTURES=gfx1151;if($LASTEXITCODE -ne 0){throw 'CMake configure failed.'}
$CmakeCache=Join-Path $Build 'CMakeCache.txt'
if(-not(Test-Path $CmakeCache)){throw 'CMake configure completed without producing CMakeCache.txt.'}
$CmakeCacheText=Get-Content $CmakeCache -Raw
if($CmakeCacheText -notmatch '(?m)^CMAKE_HIP_ARCHITECTURES:STRING=gfx1151\r?$'){throw 'Fresh CMake cache is not pinned to gfx1151.'}
if($CmakeCacheText -match '(?i)C:\\flashfknworkalready'){throw 'Fresh CMake cache still contains a legacy C:\flashfknworkalready path.'}
Write-Host 'Fresh CMake cache verified: gfx1151 + project-local ROCm.' -ForegroundColor Green
if (-not $SkipBundledTests) {
& cmake.exe --build $Build --target FlashNextVelocityUnitTests -j 16;if($LASTEXITCODE -ne 0){throw 'Native unit-test build failed.'}
$UnitTests=Join-Path $Build 'bin\FlashNextVelocityUnitTests.exe'
if(-not(Test-Path $UnitTests)){throw 'Native unit-test executable missing after build.'}
& $UnitTests
if($LASTEXITCODE -ne 0){throw 'Native MTP/context-lookup/profiler unit tests failed.'}
Write-Host 'Native MTP/context-lookup/profiler unit tests passed.' -ForegroundColor Green
& cmake.exe --build $Build --target FlashNextQ8ShallowTest -j 16;if($LASTEXITCODE -ne 0){throw 'Q8 shallow operator-test build failed.'}
$Q8Test=Join-Path $Build 'bin\FlashNextQ8ShallowTest.exe'
if(-not(Test-Path $Q8Test)){throw 'Q8 shallow operator-test executable missing after build.'}
& $Q8Test
if($LASTEXITCODE -ne 0){throw 'Q8 shallow operator tests failed.'}
Write-Host 'Q8 shallow operator tests passed.' -ForegroundColor Green
}
& cmake.exe --build $Build --target FlashNextVelocityEngine -j 16;if($LASTEXITCODE -ne 0){throw 'Native engine build failed.'}
$EngineBin=Join-Path $Build 'bin';$EngineExe=Join-Path $EngineBin 'FlashNextVelocity.Engine.exe';if(-not(Test-Path $EngineExe)){throw 'Native engine executable missing after build.'}
# Prove that the executable was rebuilt from the repaired compact-verification
# source rather than silently reusing the old object file. The legacy diagnostic
# was a literal in the old executor and must not exist in this binary.
$engineAscii=[Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($EngineExe))
if($engineAscii.Contains($legacyCompactFatal)){
  throw 'Native engine validation failed: built EXE still contains the legacy compact-verification fatal. Stale object reuse detected.'
}
if(-not $engineAscii.Contains('fnv-mtp-cache-replay-v13')){
  throw 'Native engine validation failed: current runtime revision marker is missing from the built EXE.'
}
Write-Host 'Native engine source revision verified: fnv-mtp-cache-replay-v13; async MTP chain, grouped MTP hnorm, and retained verification frontier active.' -ForegroundColor Green
$engineAscii=$null

# Desktop app.
$DesktopProject=Join-Path $Root 'src\desktop\FlashNextVelocity.Desktop.csproj';$DesktopOut=Join-Path $Build 'desktop'
& dotnet publish $DesktopProject -c Release -r win-x64 --self-contained false -o $DesktopOut;if($LASTEXITCODE -ne 0){throw 'Desktop dashboard build failed.'}

# Preserve the live next-to-EXE settings before rebuilding dist. A rebuild must
# never silently reset user-selected MTP/sampling/thinking settings or UI startup
# preferences. Refuse to overwrite malformed JSON instead of replacing it with
# defaults.
$PreservedConfig=$null;$PreservedUi=$null
$ExistingDistConfig=Join-Path $Dist 'config.json';$ExistingUi=Join-Path $Dist 'ui.json'
if(Test-Path $ExistingDistConfig){
  $PreservedConfig=[IO.File]::ReadAllText($ExistingDistConfig)
  try{$null=$PreservedConfig|ConvertFrom-Json}catch{throw "Existing dist\config.json is invalid; refusing to erase it during build: $($_.Exception.Message)"}
  Write-Host 'Preserving live dist\config.json across rebuild.' -ForegroundColor Cyan
}
if(Test-Path $ExistingUi){
  $PreservedUi=[IO.File]::ReadAllText($ExistingUi)
  try{$null=$PreservedUi|ConvertFrom-Json}catch{throw "Existing dist\ui.json is invalid; refusing to erase it during build: $($_.Exception.Message)"}
  Write-Host 'Preserving live dist\ui.json across rebuild.' -ForegroundColor Cyan
}

# Clean runtime distribution.
if(Test-Path $Dist){Remove-Item $Dist -Recurse -Force};New-Item -ItemType Directory -Force $Dist,(Join-Path $Dist 'engine'),(Join-Path $Dist 'logs'),(Join-Path $Dist 'benchmarks')|Out-Null
Copy-Item (Join-Path $DesktopOut '*') $Dist -Recurse -Force
Copy-Item (Join-Path $EngineBin '*') (Join-Path $Dist 'engine') -Recurse -Force
$VcpkgBin=Join-Path $Vcpkg 'installed\x64-windows\bin';if(Test-Path $VcpkgBin){Copy-Item (Join-Path $VcpkgBin '*.dll') (Join-Path $Dist 'engine') -Force -EA SilentlyContinue}
# Windows loads normally linked DLLs before main(). Put the selected ROCm runtime DLLs beside the engine EXE so startup does not depend on ambient PATH ordering.
$RocmBin=Join-Path $Rocm 'bin';$RocmDlls=@();if(Test-Path $RocmBin){$RocmDlls=@(Get-ChildItem $RocmBin -File -Filter '*.dll' -EA SilentlyContinue);if($RocmDlls.Count -gt 0){$RocmDlls|Copy-Item -Destination (Join-Path $Dist 'engine') -Force;Write-Host "Bundled $($RocmDlls.Count) ROCm runtime DLL(s) beside the engine." -ForegroundColor Cyan}}
if($RocmDlls.Count -eq 0){Write-Warning "No ROCm DLLs were found under $RocmBin. The runtime will fall back to PATH lookup."}
[IO.File]::WriteAllText((Join-Path $Dist 'project-root.txt'),$Root,[Text.UTF8Encoding]::new($false))
$DeviceLibPath='';if($bitcode){$DeviceLibPath=$bitcode.Directory.FullName};$RocblasLibPath='';if($rb){$RocblasLibPath=$rb.FullName};$HipblasltLibPath='';if($hb){$HipblasltLibPath=$hb.FullName};$runtime=[ordered]@{RocmPath=$Rocm;DeviceLibPath=$DeviceLibPath;RocblasTensileLibPath=$RocblasLibPath;HipblasltTensileLibPath=$HipblasltLibPath;VcpkgBin=$VcpkgBin};$runtimeJson=$runtime|ConvertTo-Json;[IO.File]::WriteAllText((Join-Path $Dist 'runtime.json'),$runtimeJson,[Text.UTF8Encoding]::new($false))

# Restore the exact live settings after rebuilding dist. dist\config.json is
# the one authoritative runtime config. On a first build, create defaults and
# let the desktop's normal model auto-discovery fill paths.
$DistConfig=Join-Path $Dist 'config.json'
if($null -ne $PreservedConfig){
  [IO.File]::WriteAllText($DistConfig,$PreservedConfig,[Text.UTF8Encoding]::new($false))
}else{
  $defaultConfig=@{model='';mtp='';mmproj='';host='127.0.0.1';port=8080;context=32768;draft_max=7;draft_confidence=0.75;mtp_proposal_mode='distribution';mtp_draft_vocabulary='latin';prefill_batch=4096;context_lookup=$true;context_lookup_min_ngram=5;context_lookup_max_ngram=7;context_lookup_window=32768;context_lookup_min_draft=6;context_lookup_max_draft=16;context_lookup_capacity=16;context_lookup_policy='sticky';memory_guard=$true;memory_guard_min_available_gib=5.0;sessions=1;default_max_tokens=4096;thinking=$false;preserve_thinking=$false;reasoning_effort='medium';sampling=@{temperature=.35;top_p=.90;top_k=20;min_p=0;repeat_penalty=1;frequency_penalty=0;presence_penalty=0;repeat_last_n=512;seed=-1}}|ConvertTo-Json -Depth 5
  [IO.File]::WriteAllText($DistConfig,$defaultConfig,[Text.UTF8Encoding]::new($false))
}
if($null -ne $PreservedUi){
  [IO.File]::WriteAllText((Join-Path $Dist 'ui.json'),$PreservedUi,[Text.UTF8Encoding]::new($false))
}

# Strict runtime validation when a model is configured.
if (-not $SkipBundledTests) {
  & (Join-Path $PSScriptRoot 'self-test.ps1') -Dist $Dist
  if($LASTEXITCODE -ne 0){throw 'Runtime self-test failed.'}
} else {
  Write-Host 'Bundled tests skipped; run the focused configuration validation separately.'
}

Write-Host ''
if ($SkipBundledTests) { Write-Host 'FLASHNEXTVELOCITY BUILD PASSED. Focused runtime validation must run separately.' -ForegroundColor Green }
else { Write-Host 'FLASHNEXTVELOCITY BUILD + RUNTIME VALIDATION PASSED.' -ForegroundColor Green }
Write-Host "Launch: $(Join-Path $Dist 'FlashNextVelocity.exe')" -ForegroundColor Green
Write-Host 'The tray/dashboard app is the normal entry point from now on.' -ForegroundColor Cyan
Stop-Transcript | Out-Null
