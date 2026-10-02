"""Verify explicit API prompts and live Studio prompts on an isolated candidate."""
import argparse
import json
import os
from pathlib import Path
import urllib.error
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--url', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    live = Path(__file__).resolve().parents[1] / 'dist/config.json'
    assert args.config.resolve() != live.resolve(), 'Use an isolated candidate configuration'
    before = args.config.read_bytes()
    config = json.loads(before)
    args.output.mkdir(parents=True, exist_ok=True)
    def health():
        with urllib.request.urlopen(args.url + '/health', timeout=5) as r:
            return json.load(r)
    def post(extra):
        body = dict(model='local', messages=[{'role':'user','content':'Reply with only CLIENT_ONLY.'}],
                    max_tokens=48, temperature=0, seed=17, stream=False,
                    chat_template_kwargs={'enable_thinking':False})
        body.update(extra)
        with urllib.request.urlopen(urllib.request.Request(args.url+'/v1/chat/completions',
                json.dumps(body).encode(), {'Content-Type':'application/json'}), timeout=60) as r:
            return json.load(r)
    def save(prompt):
        copy = dict(config, agent_prompt=prompt)
        temp = args.config.with_suffix('.prompt-test.tmp')
        temp.write_text(json.dumps(copy), encoding='utf-8'); os.replace(temp, args.config)
    initial = health()
    results = {}
    try:
        for marker in ('STUDIO_A','STUDIO_B'):
            prompt = 'Reply with only '+marker+'.'
            save(prompt)
            assert health()['studio_agent_prompt'] == prompt
            explicit = post({'agent_prompt':prompt})
            assert explicit['choices'][0]['message']['content'].strip() == marker, explicit['choices']
            results[marker] = explicit['usage']['prompt_tokens']
        neutral = [post(extra) for extra in ({}, {'agent_prompt':None}, {'agent_prompt':''})]
        assert all(r['choices'][0]['message']['content'].strip() == 'CLIENT_ONLY' for r in neutral)
        assert len({r['usage']['prompt_tokens'] for r in neutral}) == 1
        try:
            post({'agent_prompt':42})
            raise AssertionError('Invalid prompt type was accepted')
        except urllib.error.HTTPError as error:
            assert error.code == 400
        final = health()
        assert final['server_pid'] == initial['server_pid'] and final['engine_instance'] == initial['engine_instance']
        results.update(status='PASS', omission_null_empty_neutral=True, invalid_type_400=True, hot_edit_no_reload=True)
    finally:
        args.config.write_bytes(before)
    assert args.config.read_bytes() == before
    (args.output/'prompt-contract.json').write_text(json.dumps(results,indent=2))
    print(json.dumps(results),flush=True)


if __name__ == '__main__': main()
