"""Verify the installed profile and exact output against the accepted candidate."""
import hashlib
import json
from pathlib import Path
import urllib.request
root=Path(__file__).resolve().parents[1]
base='http://127.0.0.1:8080'
with urllib.request.urlopen(base+'/health',timeout=5) as response: health=json.load(response)
cfg=json.loads((root/'dist/config.json').read_text())
assert health['runtime_revision']=='fnv-mtp-cache-replay-v13'
assert health['mtp_cost_profile']=='Windows distribution Q8 C1 calibrated'
assert health['studio_agent_prompt']==cfg['agent_prompt']
request=(root/'.cache/inference-acceptance/profile-candidate/short-request.json').read_bytes()
with urllib.request.urlopen(urllib.request.Request(base+'/v1/chat/completions',request,{'Content-Type':'application/json'}),timeout=120) as response: result=json.load(response)
actual_hash=hashlib.sha256(json.dumps(result['choices'][0]['message'],sort_keys=True).encode()).hexdigest()
candidate=json.loads((root/'.cache/inference-acceptance/profile-candidate/report.json').read_text())
assert actual_hash==candidate['short']['runs'][0]['sha256'],'Installed seeded output differs from validated candidate'
metrics=result['usage']['flashnext_velocity']
assert metrics['mtp_cost_profile']=='Windows distribution Q8 C1 calibrated'
out=root/'.cache/inference-acceptance/installed';out.mkdir(exist_ok=True)
(out/'response.json').write_text(json.dumps(result,indent=2))
(out/'verification.json').write_text(json.dumps(dict(status='PASS',runtime_revision=health['runtime_revision'],cost_profile=metrics['mtp_cost_profile'],candidate_output_identical=True,server_pid=health['server_pid']),indent=2))
print('PASS: installed runtime, profile selection, durable prompt, and exact candidate output',flush=True)
