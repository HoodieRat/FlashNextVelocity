$ErrorActionPreference='Stop'
$Root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if(Test-Path "$Root\gufo-overlay\src\models\qwen38_flash_next\distribution_q8_c1_costs.hpp"){
  throw 'This bounded profile experiment is closed; its accepted curve and decision are already retained.'
}
$header=Join-Path $Root 'gufo-overlay\src\models\qwen38_flash_next\mtp_costs.hpp'
$audit=Get-Content "$Root\.cache\mtp-cost-audit-complete.log" -Raw
$rows=[regex]::Matches($audit,'MTP_COST depth=(\d+) C=1 width=(\d+).*?total_ms=([\d.]+)')
if($rows.Count -ne 24){throw 'Complete three-anchor C1 cost audit required.'}
$cfg=Get-Content "$Root\dist\config.json" -Raw | ConvertFrom-Json
if($cfg.mtp_proposal_mode -ne 'distribution' -or $cfg.mtp_draft_vocabulary -ne 'latin' -or $cfg.mtp -notmatch 'Q8_0' -or $cfg.sessions -ne 1 -or $cfg.draft_max -ne 7 -or $cfg.context -ne 32768 -or $cfg.prefill_batch -ne 4096){throw 'Cost profile trial configuration changed.'}
$original=[IO.File]::ReadAllBytes($header)
$text=[Text.Encoding]::UTF8.GetString($original)
$directory="$Root\.cache\inference-profile"
New-Item -ItemType Directory -Force $directory | Out-Null
[IO.File]::WriteAllBytes("$directory\incumbent.hpp",$original)
$costs=@()
foreach($depth in @(0,4096,32672)){
  $values=@($rows | Where-Object {$_.Groups[1].Value -eq [string]$depth} | Sort-Object {[int]$_.Groups[2].Value} | ForEach-Object {$_.Groups[3].Value+'F'})
  $costs+=('{'+($values -join ', ')+'},  // C1')
}
$matches=[regex]::Matches($text,'\{[^{}\r\n]+\},  // C1')
if($matches.Count -ne 3){throw 'Unexpected incumbent cost table.'}
for($i=2;$i -ge 0;$i--){$text=$text.Remove($matches[$i].Index,$matches[$i].Length).Insert($matches[$i].Index,$costs[$i])}
@{proposal_mode='distribution';sidecar=$cfg.mtp;format='Q8_0';draft_vocabulary='latin';serving_capacity=1;verification_capacity=17;context=32768;prefill_batch=4096;draft_max=7;confidence=.75;cost_rows=@($rows | ForEach-Object {$_.Value});production_profile='incumbent until acceptance gates pass'} | ConvertTo-Json -Depth 5 | Set-Content "$directory\candidate-metadata.json"
try {
  [IO.File]::WriteAllText($header,$text,[Text.UTF8Encoding]::new($false))
  & "$PSScriptRoot\build-inference-candidate.ps1" -Stage '.cache\inference-profile-candidate' *> "$directory\build.log"
  & "$PSScriptRoot\start-inference-candidate.ps1" -Stage '.cache\inference-profile-candidate'
  $pidPath="$Root\.cache\inference-profile-candidate\acceptance-pid.txt"
  $engineId=[int](Get-Content $pidPath)
  $ready=$false
  for($i=0;$i -lt 90;$i++){
    try{$h=Invoke-RestMethod 'http://127.0.0.1:8081/health' -TimeoutSec 2; if($h.server_pid -ne $engineId){throw 'Wrong engine answered health'}; $ready=$true; break}catch{Start-Sleep -Seconds 1}
  }
  if(-not $ready){throw 'Candidate profile failed startup.'}
  & 'C:\Users\Ian\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' "$Root\tests\inference_bounded_benchmark.py" 'http://127.0.0.1:8081' "$Root\.cache\inference-profile-candidate\acceptance-config.json" "$Root\.cache\inference-acceptance\profile-candidate" *> "$directory\benchmark.log"
  if($LASTEXITCODE -ne 0){throw 'Candidate profile benchmark failed.'}
  $inc=Get-Content "$Root\.cache\inference-acceptance\incumbent\report.json" -Raw | ConvertFrom-Json
  $candidate=Get-Content "$Root\.cache\inference-acceptance\profile-candidate\report.json" -Raw | ConvertFrom-Json
  $gain=$candidate.short.median_tps/$inc.short.median_tps-1
  $toolChange=$candidate.tool.median_tps/$inc.tool.median_tps-1
  @{short_gain=$gain;tool_change=$toolChange;eligible=($gain -ge .05 -and $toolChange -ge -.05);incumbent_short=$inc.short.median_tps;candidate_short=$candidate.short.median_tps;incumbent_tool=$inc.tool.median_tps;candidate_tool=$candidate.tool.median_tps} | ConvertTo-Json | Set-Content "$directory\decision.json"
  Get-Content "$directory\decision.json"
} finally {
  if($engineId){$p=Get-Process -Id $engineId -ErrorAction SilentlyContinue; if($p -and $p.Path -eq "$Root\.cache\inference-profile-candidate\engine\FlashNextVelocity.Engine.exe"){Stop-Process -Id $engineId}}
  [IO.File]::WriteAllBytes($header,$original)
  # Rebuild the installable incumbent even if the isolated experiment fails.
  & "$PSScriptRoot\build-inference-candidate.ps1" *> "$directory\restore-build.log"
}
