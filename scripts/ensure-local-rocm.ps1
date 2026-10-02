param(
  [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
  [switch]$AllowMissing
)
$ErrorActionPreference='Stop'
# Normalize caller-supplied roots. A quoted batch argument ending in \ can
# otherwise leave a literal quote in the path on Windows.
$Root = [Environment]::ExpandEnvironmentVariables([string]$Root).Trim().Trim('"')
$Root = [IO.Path]::GetFullPath($Root)
$ProjectRocm=Join-Path $Root '.deps\rocm'
$Dist=Join-Path $Root 'dist'
$RuntimeFile=Join-Path $Dist 'runtime.json'

function Find-Clang([string]$p){
  if(-not $p -or -not(Test-Path $p)){return $null}
  return Get-ChildItem $p -Recurse -Filter clang++.exe -ErrorAction SilentlyContinue |
    Where-Object {$_.FullName -match 'llvm\\bin\\clang\+\+\.exe$'} | Select-Object -First 1
}
function Find-DeviceLib([string]$p){
  return Get-ChildItem $p -Recurse -Filter oclc_isa_version_1151.bc -ErrorAction SilentlyContinue | Select-Object -First 1
}
function Find-HipBlasLtGfx1151([string]$p){
  $dir=Get-ChildItem $p -Recurse -Directory -ErrorAction SilentlyContinue |
    Where-Object {$_.FullName -match 'hipblaslt\\library\\gfx1151$'} | Select-Object -First 1
  if($dir){return $dir}
  $db=Get-ChildItem $p -Recurse -File -Filter 'TensileLibrary_lazy_gfx1151.*' -ErrorAction SilentlyContinue |
    Where-Object {$_.FullName -match 'hipblaslt'} | Select-Object -First 1
  if($db){return $db.Directory}
  return $null
}
function Find-RocBlasLibrary([string]$p){
  $gfx=Get-ChildItem $p -Recurse -Directory -ErrorAction SilentlyContinue |
    Where-Object {$_.FullName -match 'rocblas\\library\\gfx1151$'} | Select-Object -First 1
  if($gfx){return $gfx}
  $lib=Get-ChildItem $p -Recurse -Directory -ErrorAction SilentlyContinue |
    Where-Object {$_.FullName -match 'rocblas\\library$'} | Select-Object -First 1
  return $lib
}
function Test-Rocm([string]$p){
  if(-not(Test-Path $p)){return $false}
  return [bool]((Find-Clang $p) -and (Find-DeviceLib $p))
}

if(-not(Test-Rocm $ProjectRocm)){
  $sources=@()
  if(Test-Path $RuntimeFile){
    try {$r=Get-Content $RuntimeFile -Raw|ConvertFrom-Json;if($r.RocmPath){$sources += [string]$r.RocmPath}} catch {}
  }
  $marker=Join-Path $Root '.rocm-path.txt'
  if(Test-Path $marker){$sources += (Get-Content $marker -Raw).Trim()}
  $sources += @($env:ROCM_PATH,$env:HIP_PATH,'C:\Program Files\AMD\ROCm\10.0','C:\Program Files\AMD\ROCm\10.1')
  $source=$sources | Where-Object {$_ -and ($_ -ne $ProjectRocm) -and (Test-Rocm $_)} | Select-Object -First 1
  if(-not $source){
    if($AllowMissing){Write-Host 'No reusable ROCm installation found; BUILD will install the project-local SDK.' -ForegroundColor Yellow;exit 0}
    throw "No reusable Windows ROCm gfx1151 SDK was found. Run BUILD.bat so the pinned project-local .deps\\rocm SDK can be installed."
  }
  $source=(Resolve-Path $source).Path
  Write-Host "Seeding project-local ROCm from an existing Windows SDK:" -ForegroundColor Cyan
  Write-Host "  FROM: $source"
  Write-Host "  TO:   $ProjectRocm"
  New-Item -ItemType Directory -Force $ProjectRocm | Out-Null
  & robocopy.exe $source $ProjectRocm /E /COPY:DAT /DCOPY:DAT /R:2 /W:1 /MT:16 /NP
  $rc=$LASTEXITCODE
  if($rc -ge 8){throw "ROCm SDK copy failed; robocopy exit code $rc"}
}

if(-not(Test-Rocm $ProjectRocm)){throw "Project-local ROCm validation failed: $ProjectRocm"}
$ProjectRocm=(Resolve-Path $ProjectRocm).Path
$clang=Find-Clang $ProjectRocm
$device=Find-DeviceLib $ProjectRocm
$hiplt=Find-HipBlasLtGfx1151 $ProjectRocm
$rocblas=Find-RocBlasLibrary $ProjectRocm
if(-not $hiplt){throw "gfx1151 hipBLASLt assets are missing under $ProjectRocm\\bin\\hipblaslt\\library. Expected a gfx1151 directory/TensileLibrary_lazy_gfx1151.* file."}
$db=Get-ChildItem $hiplt.FullName -File -Filter 'TensileLibrary_lazy_gfx1151.*' -ErrorAction SilentlyContinue | Select-Object -First 1
$kernel=Get-ChildItem $hiplt.FullName -Recurse -File -ErrorAction SilentlyContinue |
  Where-Object {$_.Extension -in '.hsaco','.co' -or $_.Name -match 'gfx1151'} | Select-Object -First 1
if(-not $db){throw "hipBLASLt gfx1151 database is missing in $($hiplt.FullName)"}
if(-not $kernel){Write-Warning "No obvious gfx1151 .hsaco/.co file was found under $($hiplt.FullName); continuing because some ROCm packages use packed code objects."}

[IO.File]::WriteAllText((Join-Path $Root '.rocm-path.txt'),$ProjectRocm,[Text.UTF8Encoding]::new($false))

function Stop-FlashNextVelocityProcesses {
  $names=@('FlashNextVelocity','FlashNextVelocity.Engine')
  $running=@(Get-Process -Name $names -ErrorAction SilentlyContinue)
  if($running.Count -gt 0){
    Write-Host 'Stopping existing FlashNextVelocity processes so runtime DLLs can be updated...' -ForegroundColor Yellow
    $running | Stop-Process -Force -ErrorAction SilentlyContinue
    $deadline=(Get-Date).AddSeconds(10)
    do {
      Start-Sleep -Milliseconds 150
      $running=@(Get-Process -Name $names -ErrorAction SilentlyContinue)
    } while($running.Count -gt 0 -and (Get-Date) -lt $deadline)
    if($running.Count -gt 0){
      throw 'FlashNextVelocity is still running and holding runtime DLLs. Exit the tray app, then retry RUN.bat.'
    }
  }
}

if(Test-Path $Dist){
  $engineDir=Join-Path $Dist 'engine'
  New-Item -ItemType Directory -Force $engineDir | Out-Null
  $rocmBin=Join-Path $ProjectRocm 'bin'
  if(Test-Path $rocmBin){
    Stop-FlashNextVelocityProcesses
    $dlls=@(Get-ChildItem $rocmBin -File -Filter '*.dll' -ErrorAction SilentlyContinue)
    foreach($dll in $dlls){
      $dest=Join-Path $engineDir $dll.Name
      $copied=$false
      for($attempt=1;$attempt -le 5 -and -not $copied;$attempt++){
        try {
          Copy-Item $dll.FullName -Destination $dest -Force -ErrorAction Stop
          $copied=$true
        } catch [System.IO.IOException] {
          if($attempt -eq 5){throw}
          Start-Sleep -Milliseconds (250*$attempt)
        }
      }
    }
    Write-Host "Runtime DLLs synchronized: $($dlls.Count)" -ForegroundColor Green
  }
  $vcpkgBin=Join-Path $Root '.deps\vcpkg\installed\x64-windows\bin'
  if(Test-Path $RuntimeFile){
    try {$old=Get-Content $RuntimeFile -Raw|ConvertFrom-Json;if($old.VcpkgBin){$vcpkgBin=[string]$old.VcpkgBin}} catch {}
  }
  $rocblasPath='';if($rocblas){$rocblasPath=$rocblas.FullName}
  $runtime=[ordered]@{
    RocmPath=$ProjectRocm
    DeviceLibPath=$device.Directory.FullName
    RocblasTensileLibPath=$rocblasPath
    HipblasltTensileLibPath=$hiplt.FullName
    VcpkgBin=$vcpkgBin
  }
  [IO.File]::WriteAllText($RuntimeFile,($runtime|ConvertTo-Json),[Text.UTF8Encoding]::new($false))
  Write-Host "Updated runtime.json to project-local ROCm." -ForegroundColor Green
}
Write-Host "ROCm:        $ProjectRocm" -ForegroundColor Green
Write-Host "hipBLASLt:   $($hiplt.FullName)" -ForegroundColor Green
Write-Host "Tensile DB:  $($db.Name)" -ForegroundColor Green
# robocopy uses 0-7 for success. Do not leak that code to BUILD.bat.
exit 0
