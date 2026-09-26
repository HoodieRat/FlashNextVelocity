param([Parameter(Mandatory=$true)][string]$Dist)
$ErrorActionPreference='Stop'
$cfgPath=Join-Path $Dist 'config.json'
$cfg=Get-Content $cfgPath -Raw|ConvertFrom-Json
if(-not $cfg.model -or -not(Test-Path $cfg.model)){
  Write-Host 'Runtime self-test skipped because no model is configured yet. The desktop app will auto-discover C:\FlashNextModels on first launch.' -ForegroundColor Yellow
  exit 0
}

$runtime=Get-Content (Join-Path $Dist 'runtime.json') -Raw|ConvertFrom-Json
$env:ROCM_PATH=$runtime.RocmPath
$env:HIP_PATH=$runtime.RocmPath
$env:HIP_DEVICE_LIB_PATH=$runtime.DeviceLibPath
$env:DEVICE_LIB_PATH=$runtime.DeviceLibPath
$env:ROCBLAS_TENSILE_LIBPATH=$runtime.RocblasTensileLibPath
$env:HIPBLASLT_TENSILE_LIBPATH=$runtime.HipblasltTensileLibPath
$engineDir=Join-Path $Dist 'engine'
$rocmBin=Join-Path $runtime.RocmPath 'bin'
$env:PATH="$engineDir;$rocmBin;$($runtime.VcpkgBin);$env:PATH"

Get-Process 'FlashNextVelocity.Engine' -EA SilentlyContinue|Stop-Process -Force -EA SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $Dist 'logs') | Out-Null
$exe=Join-Path $engineDir 'FlashNextVelocity.Engine.exe'
$stdout=Join-Path $Dist 'logs\self-test.out.log'
$stderr=Join-Path $Dist 'logs\self-test.err.log'
Remove-Item $stdout,$stderr -Force -EA SilentlyContinue

function Exit-Hex([int]$Code){
  $bytes=[BitConverter]::GetBytes($Code)
  $u=[BitConverter]::ToUInt32($bytes,0)
  return ('0x{0:X8}' -f $u)
}
function Explain-Exit([int]$Code){
  switch(Exit-Hex $Code){
    '0xC0000135' { return 'STATUS_DLL_NOT_FOUND: Windows could not locate a required DLL before main() started.' }
    '0xC000007B' { return 'STATUS_INVALID_IMAGE_FORMAT: a DLL/EXE architecture or binary format is incompatible.' }
    '0xC000001D' { return 'STATUS_ILLEGAL_INSTRUCTION: the process executed an unsupported CPU instruction.' }
    '0xC0000005' { return 'STATUS_ACCESS_VIOLATION: the process crashed on an invalid memory access.' }
    default { return '' }
  }
}
function Save-Logs([string]$Out,[string]$Err){
  [IO.File]::WriteAllText($stdout,$Out,[Text.UTF8Encoding]::new($false))
  [IO.File]::WriteAllText($stderr,$Err,[Text.UTF8Encoding]::new($false))
}
function Read-HttpErrorBody($ErrorRecord){
  try {
    $response=$ErrorRecord.Exception.Response
    if($null -eq $response){return $ErrorRecord.Exception.Message}
    $stream=$response.GetResponseStream()
    if($null -eq $stream){return $ErrorRecord.Exception.Message}
    $reader=New-Object IO.StreamReader($stream)
    try{return $reader.ReadToEnd()}finally{$reader.Dispose();$stream.Dispose()}
  } catch { return $ErrorRecord.Exception.Message }
}
function Invoke-JsonPost([string]$Uri,[string]$Body,[int]$TimeoutSec){
  try {
    return Invoke-RestMethod $Uri -Method Post -ContentType 'application/json' -Body $Body -TimeoutSec $TimeoutSec
  } catch {
    $detail=Read-HttpErrorBody $_
    throw "POST $Uri failed: $detail"
  }
}

$psi=New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName=$exe
$psi.Arguments='--config "'+$cfgPath+'"'
$psi.WorkingDirectory=$Dist
$psi.UseShellExecute=$false
$psi.RedirectStandardOutput=$true
$psi.RedirectStandardError=$true
$psi.CreateNoWindow=$true
$p=New-Object System.Diagnostics.Process
$p.StartInfo=$psi
if(-not $p.Start()){throw 'Windows failed to create the native engine process.'}
$outTask=$p.StandardOutput.ReadToEndAsync()
$errTask=$p.StandardError.ReadToEndAsync()

try{
  $probeHost=$cfg.host
  if($probeHost -eq '0.0.0.0'){$probeHost='127.0.0.1'}
  $base="http://$probeHost`:$($cfg.port)"
  $health=$null
  for($i=0;$i -lt 180;$i++){
    Start-Sleep -Seconds 1
    if($p.HasExited){
      $p.WaitForExit()
      $outText=$outTask.GetAwaiter().GetResult()
      $errText=$errTask.GetAwaiter().GetResult()
      Save-Logs $outText $errText
      $code=[int]$p.ExitCode
      $hex=Exit-Hex $code
      Write-Host '--- stdout ---'
      if($outText){Write-Host $outText}
      Write-Host '--- stderr ---'
      if($errText){Write-Host $errText}
      $explain=Explain-Exit $code
      if($explain){Write-Host $explain -ForegroundColor Red}
      throw "Engine exited during self-test with code $code ($hex). Logs: $stdout / $stderr"
    }
    try{
      $health=Invoke-RestMethod "$base/health" -TimeoutSec 2
      if($health.status -eq 'ok'){break}
    }catch{}
  }
  if(-not $health){throw 'Engine did not become healthy during self-test.'}
  Write-Host "Health OK: model=$($health.model) MTP=$($health.mtp) vision=$($health.vision)" -ForegroundColor Green

  # Cheap surface checks first: these prove the API discovery routes and browser dashboard are actually present.
  $apiInfo=Invoke-RestMethod "$base/v1" -TimeoutSec 10
  if($apiInfo.name -ne 'FlashNextVelocity'){throw 'GET /v1 returned unexpected API information.'}
  $models=Invoke-RestMethod "$base/v1/models" -TimeoutSec 10
  if(-not $models.data -or $models.data.Count -lt 1){throw 'GET /v1/models returned no model.'}
  $dashboard=Invoke-WebRequest "$base/" -UseBasicParsing -TimeoutSec 10
  if($dashboard.StatusCode -ne 200 -or $dashboard.Content -notmatch 'FlashNextVelocity'){throw 'Browser dashboard self-test failed.'}
  Write-Host 'API discovery + browser dashboard OK.' -ForegroundColor Green

  $textBody=@{model='local';messages=@(@{role='user';content='Write a valid Python function named add that returns the sum of two numbers. Output only the code.'});max_tokens=96;temperature=0;stream=$false;chat_template_kwargs=@{enable_thinking=$false}}|ConvertTo-Json -Depth 10
  $text=Invoke-JsonPost "$base/v1/chat/completions" $textBody 600
  if(-not $text.choices[0].message.content){throw 'Text completion self-test returned no content.'}
  Write-Host 'Text/code completion OK.' -ForegroundColor Green

  if($cfg.mmproj -and (Test-Path $cfg.mmproj)){
    # Use a real on-disk fixture generated by the project, rather than a hand-copied base64 string.
    # This fixture is a strict 64x64 RGB PNG with valid IHDR/IDAT/IEND CRCs.
    $projectRoot=(Get-Content (Join-Path $Dist 'project-root.txt') -Raw).Trim()
    $pngPath=Join-Path $projectRoot 'assets\self-test-red.png'
    if(-not(Test-Path $pngPath)){throw "Vision self-test PNG missing: $pngPath"}
    $pngBytes=[IO.File]::ReadAllBytes($pngPath)
    if($pngBytes.Length -lt 32){throw 'Vision self-test PNG is unexpectedly short.'}
    $signature=[byte[]](137,80,78,71,13,10,26,10)
    for($i=0;$i -lt $signature.Length;$i++){if($pngBytes[$i] -ne $signature[$i]){throw 'Vision self-test fixture does not have a valid PNG signature.'}}
    $png=[Convert]::ToBase64String($pngBytes)
    $visionBody=@{model='local';messages=@(@{role='user';content=@(@{type='text';text='What is the dominant color of this image? Answer with one color word.'},@{type='image_url';image_url=@{url="data:image/png;base64,$png"}})});max_tokens=32;temperature=0;stream=$false;chat_template_kwargs=@{enable_thinking=$false}}|ConvertTo-Json -Depth 20
    $vr=Invoke-JsonPost "$base/v1/chat/completions" $visionBody 900
    if(-not $vr.choices[0].message.content){throw 'Vision self-test returned no content.'}
    Write-Host ("Vision request OK: {0}" -f $vr.choices[0].message.content.Trim()) -ForegroundColor Green
  }

  $benchPrompt=('Explain speculative decoding and memory bandwidth precisely. '*24)
  $benchBody=@{model='local';messages=@(@{role='user';content=$benchPrompt});max_tokens=64;temperature=.35;top_p=.9;top_k=40;min_p=.05;stream=$false;chat_template_kwargs=@{enable_thinking=$false}}|ConvertTo-Json -Depth 10
  $br=Invoke-JsonPost "$base/v1/chat/completions" $benchBody 900
  $m=$br.usage.flashnext_velocity
  if($null -eq $m){throw 'flashnext_velocity metrics object was missing from benchmark self-test.'}
  if($null -eq $m.prefill_tokens_per_second -or $null -eq $m.completion_tokens_per_second){throw 'Prefill/decode metrics were missing from benchmark self-test.'}
  if([double]$m.prefill_tokens_per_second -le 0 -or [double]$m.completion_tokens_per_second -le 0){throw 'Benchmark metrics were not positive.'}
  if($health.mtp -and $null -eq $m.draft_acceptance){throw 'MTP is enabled but draft acceptance metrics were missing.'}
  Write-Host ("Metrics OK: prefill {0:N2} tok/s, decode {1:N2} tok/s, MTP {2:N1}%" -f $m.prefill_tokens_per_second,$m.completion_tokens_per_second,(100*[double]$m.draft_acceptance)) -ForegroundColor Green
  if($m.speed_windows_64_tokens){Write-Host ("64-token windows: {0}" -f (($m.speed_windows_64_tokens|ForEach-Object {[string]::Format('{0:N2}',[double]$_)}) -join ', ')) -ForegroundColor Cyan}

  $metrics=Invoke-RestMethod "$base/metrics" -TimeoutSec 10
  if($null -eq $metrics.completion_tokens_per_second){throw 'GET /metrics did not expose the last inference metrics.'}
  Write-Host 'Full runtime acceptance gate PASSED: dashboard + API + text + vision + benchmark/metrics.' -ForegroundColor Green
}
finally{
  if($p -and -not $p.HasExited){$p.Kill();$p.WaitForExit()}
  if($p){
    try{
      $outText=$outTask.GetAwaiter().GetResult()
      $errText=$errTask.GetAwaiter().GetResult()
      Save-Logs $outText $errText
    }catch{}
    $p.Dispose()
  }
}
exit 0
