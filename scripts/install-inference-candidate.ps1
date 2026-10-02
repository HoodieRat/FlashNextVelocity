param([string]$Stage='.cache\inference-candidate')
$ErrorActionPreference='Stop'
$Root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Dist=Join-Path $Root 'dist'
$Stage=[IO.Path]::GetFullPath((Join-Path $Root $Stage))
if (-not $Stage.StartsWith($Root+'\',[StringComparison]::OrdinalIgnoreCase) -or $Stage -eq $Dist) {throw 'Use an isolated candidate directory.'}
$running=Get-Process FlashNextVelocity* -ErrorAction SilentlyContinue | Where-Object {$_.Path -and $_.Path.StartsWith($Dist+'\',[StringComparison]::OrdinalIgnoreCase)}
if($running){throw 'Close the installed Studio and its engine before installing.'}
$files=@('FlashNextVelocity.exe','FlashNextVelocity.dll','FlashNextVelocity.deps.json','FlashNextVelocity.runtimeconfig.json','FlashNextVelocity.pdb','engine\FlashNextVelocity.Engine.exe')
foreach($file in $files){if(-not(Test-Path (Join-Path $Stage $file))){throw "Candidate file missing: $file"}}
$engineText=[Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes("$Stage\engine\FlashNextVelocity.Engine.exe"))
if(-not $engineText.Contains('fnv-mtp-cache-replay-v13')){throw 'Stale candidate engine.'}
$before=@{}; foreach($file in @('config.json','ui.json','runtime.json')){$before[$file]=(Get-FileHash "$Dist\$file").Hash}
$backup=Join-Path $Root ('build\inference-install-backup-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $backup,"$backup\engine" | Out-Null
foreach($file in ($files+@('config.json','ui.json','runtime.json'))){if(Test-Path "$Dist\$file"){Copy-Item -LiteralPath "$Dist\$file" -Destination "$backup\$file" -Force}}
$manifest=@()
try {
  foreach($file in $files){
    Copy-Item -LiteralPath "$Stage\$file" -Destination "$Dist\$file" -Force
    $expected=(Get-FileHash "$Stage\$file").Hash; $actual=(Get-FileHash "$Dist\$file").Hash
    if($expected -ne $actual){throw "Installed hash mismatch: $file"}
    $manifest+=@{file=$file;sha256=$actual}
  }
  foreach($file in $before.Keys){if((Get-FileHash "$Dist\$file").Hash -ne $before[$file]){throw "Preserved settings changed: $file"}}
} catch {
  foreach($file in $files){if(Test-Path "$backup\$file"){Copy-Item -LiteralPath "$backup\$file" -Destination "$Dist\$file" -Force}}
  throw
}
@{installed=(Get-Date).ToString('o');backup=$backup;files=$manifest;preserved_settings=$before} | ConvertTo-Json -Depth 10 | Set-Content "$backup\installation.json" -Encoding utf8NoBOM
if (Test-Path -LiteralPath (Join-Path $Stage 'third-party')) {
  Copy-Item -LiteralPath (Join-Path $Stage 'third-party') -Destination $Dist -Recurse -Force
} else {
  & (Join-Path $PSScriptRoot 'copy-third-party-notices.ps1') -DestinationRoot $Dist
}
Write-Output "Installed and hash-verified. Rollback binaries: $backup"
