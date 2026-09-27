"""Focused Prompt 11 integration: three 8-token config checks, one 64-token stream.

Run after StudioConfigTruth and scripts/build.ps1. Uses the production config
without changing it. Model load is shared by all four requests; no benchmarks.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
DIST = ROOT / 'dist'
OUT = ROOT / 'benchmarks/prompt11-tests'


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def write(name, value):
    (OUT / (name + '.json')).write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def main():
    config_bytes = (DIST / 'config.json').read_bytes()
    config = json.loads(config_bytes)
    runtime = read(DIST / 'runtime.json')
    env = os.environ.copy()
    for key, field in [('ROCM_PATH', 'RocmPath'), ('HIP_PATH', 'RocmPath'),
                       ('HIP_DEVICE_LIB_PATH', 'DeviceLibPath'), ('DEVICE_LIB_PATH', 'DeviceLibPath'),
                       ('ROCBLAS_TENSILE_LIBPATH', 'RocblasTensileLibPath'),
                       ('HIPBLASLT_TENSILE_LIBPATH', 'HipblasltTensileLibPath')]:
        env[key] = runtime[field]
    env['PATH'] = ';'.join((str(DIST / 'engine'), str(Path(runtime['RocmPath']) / 'bin'), runtime['VcpkgBin'], env['PATH']))
    env['GUFO_PLATFORM_TUNING'] = 'none,+flag_waits'
    base = f"http://127.0.0.1:{config['port']}"
    identity_keys = ['server_pid', 'engine_instance', 'target_model_load_count', 'mtp_model_load_count', 'target_shards']

    def health():
        with urllib.request.urlopen(base + '/health', timeout=5) as response:
            return json.load(response)

    report = {'states': [], 'benchmark': False, 'production_config_sha256': hashlib.sha256(config_bytes).hexdigest()}
    with (OUT / 'runtime.out.log').open('wb') as stdout, (OUT / 'runtime.err.log').open('wb') as stderr:
        proc = subprocess.Popen([str(DIST / 'engine/FlashNextVelocity.Engine.exe'), '--config', str(DIST / 'config.json')],
                                cwd=DIST, env=env, stdout=stdout, stderr=stderr, creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            deadline = time.monotonic() + 180
            initial = None
            while time.monotonic() < deadline:
                assert proc.poll() is None, 'Engine exited during startup'
                try:
                    initial = health()
                    if initial['status'] == 'ok':
                        break
                except Exception:
                    time.sleep(1)
            assert initial and initial['status'] == 'ok'
            assert initial['server_pid'] == proc.pid
            identity = {k: initial[k] for k in identity_keys}
            assert identity['target_model_load_count'] == identity['mtp_model_load_count'] == 1
            assert identity['target_shards'] == 4
            assert initial['gpu_arch'].startswith('gfx1151')
            assert initial['mtp'] and initial['config_mtp'] == config['mtp'] and 'Q8_0' in config['mtp']
            assert initial['mtp_draft_vocabulary'] == 'latin' and initial['mtp_draft_vocab_size'] == 130202
            assert initial['memory_guard'] and initial['memory_guard_min_available_gib'] == 5
            assert initial['context'] == 131117 and initial['draft_max'] == 6
            assert initial['context_lookup'] and initial['context_lookup_active']
            assert initial['context_lookup_policy'] == 'sticky'
            assert initial['context_lookup_start_width'] == 6 and initial['context_lookup_promotion_width'] == initial['context_lookup_capacity'] == 16
            assert initial['tuning']['flag_waits'] and sum(initial['tuning'].values()) == 1
            assert initial['effective_reasoning_effort'] == 'OFF' and initial['effective_draft_confidence'] is None
            write('health', initial)
            report['initial_health'] = initial
            print('READY: gfx1151, four target shards, Q8_0 Latin, sticky 6 -> 16, MTP 6.', flush=True)
            for state in ['B-off', 'B-on', 'C-numeric', 'D-final']:
                request = read(OUT / (state + '-request.json'))
                before = health()
                assert {k: before[k] for k in identity_keys} == identity
                req = urllib.request.Request(base + '/v1/chat/completions', json.dumps(request).encode('utf-8'),
                                             {'Content-Type': 'application/json'})
                with urllib.request.urlopen(req, timeout=300) as response:
                    if request['stream']:
                        assert response.headers['Content-Type'].startswith('text/event-stream')
                        wire, fragments, usage, done = [], [], None, False
                        for line in response:
                            decoded = line.decode('utf-8'); wire.append(decoded)
                            if not decoded.startswith('data:'):
                                continue
                            data = decoded[5:].strip()
                            if data == '[DONE]':
                                done = True; break
                            chunk = json.loads(data)
                            for choice in chunk.get('choices', []):
                                fragments.append(choice.get('delta', {}).get('content', ''))
                            if chunk.get('usage'):
                                usage = chunk['usage']
                        assert done and ''.join(fragments) and usage
                        (OUT / 'final-stream.sse').write_text(''.join(wire), encoding='utf-8')
                        result = {'usage': usage, 'content': ''.join(fragments), 'done': done}
                    else:
                        result = json.load(response)
                metrics = result['usage']['flashnext_velocity']
                effective = metrics['effective_settings']
                for key in ['temperature', 'top_p', 'top_k', 'min_p', 'repeat_penalty', 'frequency_penalty', 'presence_penalty', 'repeat_last_n']:
                    assert abs(effective[key] - request[key]) < 1e-6, (state, key, effective[key], request[key])
                kwargs = request['chat_template_kwargs']
                assert effective['thinking'] == kwargs['enable_thinking']
                assert effective['reasoning_effort'].lower() == kwargs['reasoning_effort']
                assert effective['preserve_thinking'] == kwargs['preserve_thinking']
                assert effective['context'] == 131117 and effective['draft_max'] == 6
                assert effective['mtp_proposal_mode'] == 'halo_greedy'
                assert effective['context_lookup_policy'] == 'sticky' and effective['context_lookup_start_width'] == 6
                assert effective['context_lookup_promotion_width'] == effective['context_lookup_capacity'] == 16
                assert effective['context_lookup_resets_each_request'] and effective['tuning'] == initial['tuning']
                assert metrics['lookup_policy']['starting_width'] == 6
                after = health()
                assert {k: after[k] for k in identity_keys} == identity
                write(state + '-response', result)
                report['states'].append({'state': state, 'status': 'PASS', 'effective': effective, 'lookup_policy': metrics['lookup_policy'],
                                         'identity_before': {k: before[k] for k in identity_keys}, 'identity_after': {k: after[k] for k in identity_keys}})
                print(f"PASS {state}: effort={effective['reasoning_effort']}, lookup={metrics['lookup_policy']['starting_width']} -> {metrics['lookup_policy']['final_width']}, identity unchanged", flush=True)
            report['final_health'] = health()
            assert (DIST / 'config.json').read_bytes() == config_bytes, 'Live production configuration changed'
            report['status'] = 'PASS'
        finally:
            if proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=30)
            report['engine_stopped_after_validation'] = True
            write('runtime-results', report)


if __name__ == '__main__':
    main()
