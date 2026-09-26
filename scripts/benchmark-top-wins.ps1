param(
  [string]$BaseUrl = 'http://127.0.0.1:8080',
  [string]$Label = 'run',
  [ValidateRange(1,20)][int]$Repetitions = 3,
  [ValidateRange(32,4096)][int]$MaxTokens = 256,
  [int]$SeedBase = 424242,
  [switch]$IncludeContextLadder,
  [string]$OutputDir = '',
  [string]$CompareTo = ''
)

$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$Root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if([string]::IsNullOrWhiteSpace($OutputDir)){$OutputDir=Join-Path $Root 'benchmarks\top-wins'}
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$BaseUrl=$BaseUrl.TrimEnd('/')

function Sha256Text([string]$Text){
  $bytes=[Text.Encoding]::UTF8.GetBytes($Text)
  $sha=[Security.Cryptography.SHA256]::Create()
  try {$hash=$sha.ComputeHash($bytes)} finally {$sha.Dispose()}
  return ([BitConverter]::ToString($hash)).Replace('-','').ToLowerInvariant()
}
function Median([double[]]$Values){
  if($null -eq $Values -or $Values.Count -eq 0){return 0.0}
  $x=@($Values|Sort-Object);$m=[int]($x.Count/2)
  if(($x.Count % 2)-eq 1){return [double]$x[$m]}
  return ([double]$x[$m-1]+[double]$x[$m])/2.0
}
function Invoke-JsonPost([string]$Uri,$Body,[int]$TimeoutSec=1200){
  $json=$Body|ConvertTo-Json -Depth 20 -Compress
  return Invoke-RestMethod -Method Post -Uri $Uri -ContentType 'application/json' -Body $json -TimeoutSec $TimeoutSec
}
function EffectiveRequestSettings($Health){
  $s=$Health.sampling
  $effort=if([bool]$Health.thinking){[string]$Health.reasoning_effort}else{'off'}
  return [ordered]@{
    temperature=[double]$s.temperature
    top_p=[double]$s.top_p
    top_k=[int]$s.top_k
    min_p=[double]$s.min_p
    repeat_penalty=[double]$s.repeat_penalty
    repeat_last_n=[int]$s.repeat_last_n
    chat_template_kwargs=[ordered]@{
      enable_thinking=[bool]$Health.thinking
      preserve_thinking=([bool]$Health.thinking -and [bool]$Health.preserve_thinking)
      reasoning_effort=$effort
    }
  }
}
function EffectiveRuntimeFingerprint($Health){
  $s=$Health.sampling
  $thinking=[bool]$Health.thinking
  $effectiveEffort=if($thinking){[string]$Health.reasoning_effort}else{'OFF'}
  return [ordered]@{
    model=[string]$Health.model
    mtp=[bool]$Health.mtp
    context=[int64]$Health.context
    draft_max=[int]$Health.draft_max
    draft_confidence=[double]$Health.draft_confidence
    mtp_proposal_mode=[string]$Health.mtp_proposal_mode
    prefill_batch=[int]$Health.prefill_batch
    context_lookup=[bool]$Health.context_lookup
    context_lookup_active=[bool]$Health.context_lookup_active
    context_lookup_min_ngram=[int]$Health.context_lookup_min_ngram
    context_lookup_max_ngram=[int]$Health.context_lookup_max_ngram
    context_lookup_window=[int]$Health.context_lookup_window
    context_lookup_min_draft=[int]$Health.context_lookup_min_draft
    sessions=[int]$Health.sessions
    thinking=$thinking
    preserve_thinking=($thinking -and [bool]$Health.preserve_thinking)
    reasoning_effort=$effectiveEffort
    sampling=[ordered]@{
      temperature=[double]$s.temperature
      top_p=[double]$s.top_p
      top_k=[int]$s.top_k
      min_p=[double]$s.min_p
      repeat_penalty=[double]$s.repeat_penalty
      repeat_last_n=[int]$s.repeat_last_n
    }
  }
}
function RequestBody([string]$Prompt,[int]$Max,[int]$Seed,$Settings,[bool]$Profile=$true){
  $body=[ordered]@{
    model='local'
    messages=@([ordered]@{role='user';content=$Prompt})
    max_tokens=$Max
    stream=$false
    seed=$Seed
    flashnext_profile=$Profile
    temperature=$Settings.temperature
    top_p=$Settings.top_p
    top_k=$Settings.top_k
    min_p=$Settings.min_p
    repeat_penalty=$Settings.repeat_penalty
    repeat_last_n=$Settings.repeat_last_n
    chat_template_kwargs=$Settings.chat_template_kwargs
  }
  return $body
}
function RunCase([string]$Name,[string]$Prompt,[int]$CaseMax,$Settings,$Health,[int]$Reps){
  $promptHash=Sha256Text $Prompt
  Write-Host "[$Name] warmup" -ForegroundColor DarkCyan
  $null=Invoke-JsonPost "$BaseUrl/v1/chat/completions" (RequestBody $Prompt ([Math]::Min(32,$CaseMax)) 17 $Settings $false)
  $runs=@()
  for($i=0;$i -lt $Reps;$i++){
    $seed=$SeedBase+$i
    Write-Host "[$Name] repetition $($i+1)/$Reps seed=$seed" -ForegroundColor Cyan
    $r=Invoke-JsonPost "$BaseUrl/v1/chat/completions" (RequestBody $Prompt $CaseMax $seed $Settings $true)
    $m=$r.usage.flashnext_velocity
    if($null -eq $m){throw "[$Name] response did not contain usage.flashnext_velocity"}
    $text=[string]$r.choices[0].message.content
    $acceptance=if($null -ne $m.mtp_acceptance){[double]$m.mtp_acceptance}else{[double]$m.draft_acceptance}
    $verification=if($null -ne $m.mtp -and $null -ne $m.mtp.verification){$m.mtp.verification}else{$null}
    $fullRows=if($null -ne $verification){[int64]$verification.full_rows_d2h}else{0}
    $compactRows=if($null -ne $verification){[int64]$verification.compact_rows_d2h}else{0}
    $d2hBytes=if($null -ne $verification){[int64]$verification.d2h_bytes}else{0}
    $certOk=if($null -ne $verification){[int64]$verification.certificate_successes}else{0}
    $certFallback=if($null -ne $verification){[int64]$verification.certificate_fallbacks}else{0}
    $runs += [ordered]@{
      repetition=$i+1
      seed=$seed
      output_sha256=Sha256Text $text
      output=$text
      prompt_tokens=[int64]$m.prompt_tokens
      completion_tokens=[int64]$m.completion_tokens
      prefill_tps=[double]$m.prefill_tokens_per_second
      decode_tps=[double]$m.completion_tokens_per_second
      ttft_ms=[double]$m.ttft_ms
      mtp_acceptance=$acceptance
      avg_draft_depth=[double]$m.avg_draft_depth
      verification=$verification
      full_rows_d2h=$fullRows
      compact_rows_d2h=$compactRows
      d2h_bytes=$d2hBytes
      certificate_successes=$certOk
      certificate_fallbacks=$certFallback
      profile_text=[string]$m.profile_text
      metrics=$m
    }
  }
  return [ordered]@{
    name=$Name
    prompt_sha256=$promptHash
    max_tokens=$CaseMax
    prompt=$Prompt
    runs=$runs
    median_decode_tps=Median @($runs|ForEach-Object{[double]$_.decode_tps})
    median_prefill_tps=Median @($runs|ForEach-Object{[double]$_.prefill_tps})
    median_acceptance=Median @($runs|ForEach-Object{[double]$_.mtp_acceptance})
  }
}

$health=Invoke-RestMethod "$BaseUrl/health" -TimeoutSec 15
if(-not $health.mtp){Write-Warning 'MTP is not active. The suite will still run, but it will not measure the intended MTP path.'}
$settings=EffectiveRequestSettings $health
$runtimeSettings=EffectiveRuntimeFingerprint $health
$settingsJson=$runtimeSettings|ConvertTo-Json -Depth 10 -Compress
$settingsHash=Sha256Text $settingsJson

$cases=@(
  [ordered]@{name='normal-chat';max=$MaxTokens;prompt='Explain why a program can begin inference near 48 tokens per second and then settle near 30 tokens per second. Focus on GPU work, speculative verification, memory bandwidth, and synchronization. Be technically precise and non-repetitive.'},
  [ordered]@{name='coding';max=$MaxTokens;prompt='Write a production-quality C# method that atomically saves a JSON configuration file on Windows, validates a round trip, replaces the live file, and preserves the original on failure. Explain the important failure cases after the code.'},
  [ordered]@{name='structured-json';max=[Math]::Min($MaxTokens,220);prompt='Return valid JSON only. Build an object with keys summary, bottlenecks, experiments, and acceptance_criteria. experiments must contain five objects with id, hypothesis, metric, and pass_condition for optimizing a speculative-decoding inference runtime.'},
  [ordered]@{name='continuation-prose';max=$MaxTokens;prompt='Continue this technical paragraph in a coherent, non-repetitive style for several paragraphs: A fast inference runtime is not defined by one peak token-rate sample. Sustained throughput depends on the interaction between kernel occupancy, memory traffic, speculative acceptance, and the cost of verifying each proposed token.'},
  [ordered]@{name='predictable-text';max=[Math]::Min($MaxTokens,192);prompt='Continue the sequence exactly, one item per line, through item 120: item 1 = alpha beta gamma; item 2 = alpha beta gamma; item 3 = alpha beta gamma; item 4 = alpha beta gamma;'}
)

$results=@()
foreach($c in $cases){$results += RunCase $c.name $c.prompt $c.max $settings $health $Repetitions}

if($IncludeContextLadder){
  # The repeated token-like unit is deliberately simple. The report records the
  # actual prompt-token count returned by the runtime; no claimed depth is based
  # on the character count.
  $targets=@(4096,32768,65536,126000)
  foreach($target in $targets){
    $unit=' x'
    $filler=($unit * $target) -join ''
    $prompt="Context ladder target approximately $target tokens. Read the filler, then answer with exactly three concise sentences describing why long-context QSA/indexer cost matters. FILLER:$filler"
    try{$results += RunCase "context-$target" $prompt ([Math]::Min(96,$MaxTokens)) $settings $health 1}
    catch{Write-Warning "context-$target skipped/failed: $($_.Exception.Message)"}
  }
}

$record=[ordered]@{
  schema=1
  label=$Label
  timestamp_utc=[DateTime]::UtcNow.ToString('o')
  base_url=$BaseUrl
  runtime_revision=[string]$health.runtime_revision
  version=[string]$health.version
  model=[string]$health.model
  context=[int64]$health.context
  request_settings=$settings
  effective_settings=$runtimeSettings
  settings_sha256=$settingsHash
  repetitions=$Repetitions
  seed_base=$SeedBase
  cases=$results
}
$stamp=Get-Date -Format 'yyyyMMdd-HHmmss'
$out=Join-Path $OutputDir ("{0}-{1}.json" -f ($Label -replace '[^A-Za-z0-9_.-]','_'),$stamp)
[IO.File]::WriteAllText($out,($record|ConvertTo-Json -Depth 100),[Text.UTF8Encoding]::new($false))

$csvRows=foreach($c in $results){
  [pscustomobject]@{label=$Label;case=$c.name;prompt_sha256=$c.prompt_sha256;median_decode_tps=$c.median_decode_tps;median_prefill_tps=$c.median_prefill_tps;median_acceptance=$c.median_acceptance;settings_sha256=$settingsHash;runtime_revision=$health.runtime_revision}
}
$csv=Join-Path $OutputDir ("{0}-{1}.csv" -f ($Label -replace '[^A-Za-z0-9_.-]','_'),$stamp)
$csvRows|Export-Csv -NoTypeInformation -Encoding UTF8 $csv

Write-Host "Saved: $out" -ForegroundColor Green
Write-Host "Saved: $csv" -ForegroundColor Green
Write-Host ''
$csvRows|Format-Table -AutoSize

if(-not [string]::IsNullOrWhiteSpace($CompareTo)){
  $baseline=Get-Content $CompareTo -Raw|ConvertFrom-Json
  if([string]$baseline.settings_sha256 -ne $settingsHash){throw 'Comparison refused: effective settings fingerprints differ.'}
  $baseBy=@{};foreach($c in $baseline.cases){$baseBy[[string]$c.name]=$c}
  $comparison=@()
  foreach($c in $results){
    if(-not $baseBy.ContainsKey($c.name)){continue}
    $b=$baseBy[$c.name]
    if([string]$b.prompt_sha256 -ne [string]$c.prompt_sha256){throw "Comparison refused for $($c.name): prompt hashes differ."}
    $bt=[double]$b.median_decode_tps;$pt=[double]$c.median_decode_tps
    $deltaPct=if($bt -gt 0){100.0*($pt-$bt)/$bt}else{0.0}
    $comparison += [pscustomobject]@{
      case=$c.name
      baseline_tps=$bt
      current_tps=$pt
      delta_tps=$pt-$bt
      delta_percent=$deltaPct
      baseline_acceptance=[double]$b.median_acceptance
      current_acceptance=[double]$c.median_acceptance
    }
  }
  Write-Host 'A/B comparison:' -ForegroundColor Yellow
  $comparison|Format-Table -AutoSize
}
