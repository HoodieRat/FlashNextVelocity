"""One warmed comparison of the existing short and tool fixtures. Never runs tools."""
import hashlib
import json
from pathlib import Path
import statistics
import sys
import time
import urllib.request
base, config_path, output = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
output.mkdir(parents=True, exist_ok=True)
cfg=json.loads(config_path.read_text(encoding='utf-8-sig'))
standalone='You are a helpful assistant in a standalone chat. No tools, filesystem, shell, or project workspace are available. Answer the user directly. For code and SVG requests, write the complete code in your response.'
sampling=cfg['sampling']
sampling=dict(sampling,seed=12345)
common=dict(model='local', **sampling, stream=False,
            context_lookup_policy=cfg['context_lookup_policy'],
            chat_template_kwargs={'enable_thinking':cfg['thinking'], 'preserve_thinking':cfg['preserve_thinking'], 'reasoning_effort':'off'})
# Same short fixture and request size as Desktop Studio.
short=dict(common, agent_prompt=cfg['agent_prompt'], max_tokens=8192,
           messages=[{'role':'system','content':standalone}, {'role':'user','content':'Explain speculative decoding, memory bandwidth, recurrent state, and long-context inference in technically precise, non-repetitive prose. '*48}])
# Reuse the existing representative raw tool-format SVG fixture exactly. Its
# intentional 2048-token cap can truncate an argument; this benchmark executes
# no tools. Complete/truncated API calls have separate acceptance checks.
tool=json.loads((Path(__file__).resolve().parents[1]/'benchmarks/lookup-policy-comparison/report.json').read_text())['request']
tool=dict(tool,stream=False,context_lookup_policy=cfg['context_lookup_policy'])
report={}
for name,endpoint,body in [('short','/v1/chat/completions',short),('tool','/v1/completions',tool)]:
    payload=json.dumps(dict(body,flashnext_profile=False)).encode()
    (output/(name+'-request.json')).write_bytes(payload)
    runs=[]
    for index in range(4):
        start=time.monotonic()
        with urllib.request.urlopen(urllib.request.Request(base+endpoint,payload,{'Content-Type':'application/json'}),timeout=180) as response: result=json.load(response)
        wall=time.monotonic()-start
        (output/(name+f'-{index}.json')).write_text(json.dumps(result,indent=2))
        choice=result['choices'][0]
        metrics=result['usage']['flashnext_velocity']
        normalized=json.dumps(choice.get('message',choice.get('text')),sort_keys=True)
        runs.append(dict(tps=metrics['completion_tokens_per_second'], decode_ms=metrics['decode_ms'], tokens=result['usage']['completion_tokens'], wall_seconds=wall, sha256=hashlib.sha256(normalized.encode()).hexdigest()))
        print(name,index,runs[-1],flush=True)
    assert len({r['sha256'] for r in runs})==1,'Seeded output changed within identical conditions'
    report[name]=dict(median_tps=statistics.median(r['tps'] for r in runs[1:]), median_decode_ms=statistics.median(r['decode_ms'] for r in runs[1:]), identical=True, runs=runs)
(output/'report.json').write_text(json.dumps(report,indent=2))
