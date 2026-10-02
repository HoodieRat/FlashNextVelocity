"""Bounded fixed6/sticky comparison with one owned engine and saved fixtures."""
from __future__ import annotations

import argparse
import ast
import copy
import csv
import hashlib
import json
import math
import time
from datetime import datetime, timezone
from pathlib import Path

import benchmark_report as bench

POLICIES = ('fixed6', 'sticky')


def fixtures():
    source_path = bench.ROOT / 'tests/benchmark_profile_transition.py'
    source = source_path.read_text(encoding='utf-8')
    old = 'parser.add_argument("--max-tokens", type=int, default=8192)'
    if source.count(old) != 1:
        raise ValueError('The file-edit fixture changed; review the expected edit before benchmarking.')
    expected = source.replace(old, old.replace('8192', '4096'))
    base = json.loads(bench.FIXTURE_PATH.read_text(encoding='utf-8'))
    # Keep the complete selected excerpt in the publishable fixture directory.
    logs_path = bench.ROOT / 'scripts/benchmark-fixtures/lookup-logs.txt'
    lines = logs_path.read_text(encoding='utf-8').splitlines()
    if len(lines) != 12:
        raise ValueError('Expected twelve complete log rows.')
    logs = '\n'.join(lines)
    cases = [
        {'id': 'file_edit', 'max_tokens': 2048, 'source_file': str(source_path.relative_to(bench.ROOT)),
         'original': source, 'expected': expected,
         'prompt': 'Return the complete updated Python file, without Markdown or commentary. '
                   'Change only the --max-tokens argument default from 8192 to 4096. '
                   'Include all unchanged code and preserve its formatting.\n\n' + source},
        {'id': 'log_quotation', 'max_tokens': 1536, 'expected': logs,
         'source_file': str(logs_path.relative_to(bench.ROOT)),
         'prompt': 'Quote all twelve log rows below exactly, preserving every character and line. '
                   'Begin with the first timestamp. No fences, explanation, omissions, or extra lines.\n\n' + logs},
        {'id': 'prose_control', 'max_tokens': 256, 'prompt': base['prose']}]
    for case in cases:
        case['prompt_sha256'] = bench.sha_text(case['prompt'])
    return {'system': base['system'], 'cases': cases}


def correctness(case, result):
    text = result.get('text', '')
    if result.get('reasoning'):
        return 'FAIL', 'Reasoning appeared despite the thinking-off request.'
    if case['id'] == 'file_edit':
        try:
            actual = ast.dump(ast.parse(text), include_attributes=False)
            expected = ast.dump(ast.parse(case['expected']), include_attributes=False)
        except SyntaxError as error:
            return 'FAIL', str(error)
        return ('PASS', 'Complete file AST matches the single requested edit.') if actual == expected else (
            'FAIL', 'The complete file AST differs from the requested edit.')
    if case['id'] == 'log_quotation':
        return ('PASS', 'All twelve log rows match exactly.') if text.strip('\r\n') == case['expected'] else (
            'FAIL', 'Log rows differ, are missing, or contain extra text.')
    return ('PASS', 'Nonempty prose; factual quality not scored.') if len(text.split()) >= 20 else (
        'FAIL', 'Empty or too-short prose.')


def summaries(data):
    output = []
    for case in data['inputs']['cases']:
        row = {'id': case['id']}
        for policy in POLICIES:
            runs = [r for r in data['runs'] if r['purpose'] == 'measured'
                    and r['case_id'] == case['id'] and r['policy'] == policy and r['status'] == 'ok']
            row[policy] = {'count': len(runs), 'checks': sum(r['check'] == 'PASS' for r in runs),
                'tps': bench.median([r['decode_tps'] for r in runs]),
                'total_ms': bench.median([r['client_total_ms'] for r in runs]),
                'ttft_ms': bench.median([r.get('client_ttft_ms') for r in runs]),
                'promotions': sum(r['metrics']['lookup_policy']['promoted'] for r in runs),
                'tokens': sorted({r['completion_tokens'] for r in runs}),
                'input_tokens': sorted({r['prompt_tokens'] for r in runs}),
                'lookup_acceptance': bench.median([r['lookup_acceptance'] for r in runs
                    if r['metrics']['lookup_draft_tokens']]),
                'lookup_per_output': bench.median([r['lookup_per_output'] for r in runs])}
        row['change_percent'] = (100 * (row['sticky']['tps'] / row['fixed6']['tps'] - 1)
            if row['sticky']['tps'] is not None and row['fixed6']['tps'] else None)
        pairs = []
        for rep in (1, 2, 3):
            pair = [next((r for r in data['runs'] if r['purpose'] == 'measured'
                and r['case_id'] == case['id'] and r['policy'] == p and r['repetition'] == rep
                and r['status'] == 'ok'), None) for p in POLICIES]
            if all(pair):
                pairs.append({'repetition': rep, 'same_output': pair[0]['output_sha256'] == pair[1]['output_sha256'],
                    'change_percent': 100 * (pair[1]['decode_tps'] / pair[0]['decode_tps'] - 1)})
        row['pairs'] = pairs
        output.append(row)
    return output


def decision(data):
    rows = summaries(data)
    complete = all(row[p]['count'] == 3 and row[p]['checks'] == 3 for row in rows for p in POLICIES)
    copy_rows = [r for r in rows if r['id'] != 'prose_control']
    hashes = all(len(r['pairs']) == 3 and all(p['same_output'] for p in r['pairs']) for r in copy_rows)
    gain = any(r['change_percent'] is not None and r['change_percent'] >= 5
               and all(p['change_percent'] > 0 for p in r['pairs']) for r in copy_rows)
    regressions = all(r['change_percent'] is not None and r['change_percent'] >= -3 for r in rows)
    promotions = any(r['sticky']['promotions'] == 3 for r in copy_rows)
    gates = {'all_18_measured_checks_pass': complete, 'copy_output_hashes_match': hashes,
             'at_least_5_percent_copy_gain_and_all_three_pairs_faster': gain,
             'no_workload_median_regression_over_3_percent': regressions,
             'copy_workload_promotes_in_all_three_sticky_runs': promotions}
    eligible = data['status'] == 'complete' and not data['errors']
    return {'result': 'PASS' if eligible and all(gates.values()) else 'NOT QUALIFIED', 'gates': gates}


def markdown(data, prefix=''):
    rows = summaries(data)
    lines = ['# Lookup Policy Qualification', '',
        f'> **{data["status"].upper()}** · {data["started_utc"]} · One engine load · Interleaved policies', '',
        '## Summary', '', f'**Decision: {decision(data)["result"]}.**', '',
        bench.table(['Workload', 'Fixed6 tps', 'Sticky tps', 'Change', 'Checks fixed6 / sticky', 'Sticky promotions'],
            [[r['id'], bench.fmt(r['fixed6']['tps']), bench.fmt(r['sticky']['tps']),
              bench.fmt(r['change_percent']) + '%' if r['change_percent'] is not None else 'N/A',
              f'{r["fixed6"]["checks"]}/3 · {r["sticky"]["checks"]}/3', f'{r["sticky"]["promotions"]}/3'] for r in rows]), '',
        'The decision gates were defined before measurement: all 18 checks pass; copied-output hashes match; '
        'at least one copy workload gains 5% with all three pairs faster; no workload median loses more than 3%; '
        'a copy workload promotes in all three sticky repetitions.', '',
        bench.table(['Gate', 'Result'], [[k.replace('_', ' '), 'PASS' if v else 'FAIL'] for k, v in decision(data)['gates'].items()]), '',
        '## Method', '',
        '- Three workloads × two policies × three repetitions = 18 measured requests, plus six full warmups.',
        '- Fixed6/sticky order alternates across pairs; the paired requests share the same prompt and seed.',
        '- Current production context, sampler, MTP policy, confidence, batch size, n-gram range, and search window. '
        'Benchmark copies use one session, text only, thinking off, and the published system prompt.',
        '- Common lookup capacity 16 and configured maximum 16. Only the request policy switches; '
        'fixed6 stays at six tokens, sticky can promote to sixteen. The model is not reloaded between policies.',
        '- Fresh prompts reset the recurrent session after the preceding generation. All outputs and actual token counts are saved.',
        '- File edit uses an actual repository Python module and checks the complete AST. Log quotation checks all twelve rows exactly. '
        'Prose uses a basic integrity check. Generated code is not executed.',
        '- Profiling is off. TPS is engine decode wall time; client total and first response have separate boundaries.', '',
        '## Latency and Lookup', '',
        bench.table(['Workload / policy', 'Input / output tokens', 'Client total s', 'First response ms', 'Lookup acceptance', 'Lookup accepted / output'],
            [[r['id'] + ' / ' + p, f'{r[p]["input_tokens"]} / {r[p]["tokens"]}',
              bench.fmt(r[p]['total_ms'] / 1000 if r[p]['total_ms'] is not None else None), bench.fmt(r[p]['ttft_ms']),
              bench.fmt(r[p]['lookup_acceptance'] * 100) + '%' if r[p]['lookup_acceptance'] is not None else 'N/A',
              bench.fmt(r[p]['lookup_per_output'], 3)] for r in rows for p in POLICIES]), '',
        '## Paired Results', '',
        bench.table(['Workload', 'Seed repetition', 'Same output hash', 'Sticky TPS change'],
            [[r['id'], p['repetition'], str(p['same_output']), bench.fmt(p['change_percent']) + '%'] for r in rows for p in r['pairs']]), '',
        'Sampling can produce different valid text across proposal policies. Copy-workload hashes are required for this qualification; '
        'prose is checked for basic integrity. No claim of universal seeded identity is made.', '',
        '## System and Reproducibility', '',
        bench.table(['Setting', 'Value'], [['CPU', data.get('machine', {}).get('cpu', 'Unavailable')],
            ['Context', data['config']['context']], ['Sampler', json.dumps(data['sampling'])],
            ['Engine revision', data.get('health', {}).get('runtime_revision', 'Not loaded')],
            ['Time limit', f'{data["budget_minutes"]} minutes'], ['Elapsed', f'{bench.fmt(data.get("elapsed_seconds", 0) / 60)} minutes'],
            ['Runner SHA256', data['runner_sha256']]]), '',
        f'- [Raw results, outputs, metrics, hashes, settings, and memory samples]({prefix}raw-results.json)',
        f'- [Exact prompts, original source, and expected answers]({prefix}inputs.json)',
        f'- [Request CSV]({prefix}requests.csv)', '',
        f'- [Runner source used for these measurements]({prefix}runner-source.py)',
        f'- [Shared measurement helper source]({prefix}helper-source.py)', '',
        '## Limitations and Run Notes', '',
        '- Three repetitions are a focused screening result. They do not establish tail latency, broad code quality, '
        'performance at other contexts, or memory peaks. No production settings are changed by this trial.',
        '- Both policies reserve the same lookup capacity. Memory is sampled at request boundaries; these observations '
        'cannot establish the cost of lookup versus lookup disabled.',
        '- Prose gains should not be inferred from workloads with no copied tokens. Search time and rejected-round cost '
        'are not separately instrumented in this unprofiled comparison.', '']
    for error in data['errors']:
        lines.append('- ' + bench.cell(error))
    lines += ['', '<details>', '<summary>Common benchmark configuration</summary>', '', '```json',
              json.dumps(data['config'], indent=2), '```', '', '</details>', '']
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--budget-minutes', type=float, default=20)
    parser.add_argument('--port', type=int, default=18081)
    args = parser.parse_args()
    if not math.isfinite(args.budget_minutes) or args.budget_minutes <= 0 or not 1024 <= args.port <= 65535:
        parser.error('Choose a positive finite budget and a valid private port.')
    budget = bench.Budget(args.budget_minutes)
    stamp = datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%SZ-lookup-policy')
    out = bench.ROOT / 'benchmark-reports' / stamp
    out.mkdir(parents=True, exist_ok=False)
    private = bench.ROOT / '.cache/benchmark-report' / stamp
    private.mkdir(parents=True, exist_ok=True)
    original_config = json.loads((bench.ROOT / 'dist/config.json').read_text(encoding='utf-8-sig'))
    config = copy.deepcopy(original_config)
    config.update(host='127.0.0.1', port=args.port, sessions=1, mmproj='', agent_prompt='',
        thinking=False, preserve_thinking=False, context_lookup=True, context_lookup_capacity=16,
        context_lookup_max_draft=16, context_lookup_policy='fixed6')
    sampling = bench.sampling_for(config, 'production')
    config['sampling'] = {**sampling, 'seed': bench.SEEDS[0]}
    runtime = json.loads((bench.ROOT / 'dist/runtime.json').read_text(encoding='utf-8-sig'))
    data = {'schema_version': 1, 'status': 'running', 'started_utc': datetime.now(timezone.utc).isoformat(),
        'budget_minutes': args.budget_minutes, 'config': bench.public_config(config),
        'source_config': bench.public_config(original_config), 'sampling': sampling,
        'machine': {}, 'runner_sha256': bench.sha_text(Path(__file__).read_text(encoding='utf-8')),
        'helper_sha256': bench.sha_text(Path(bench.__file__).read_text(encoding='utf-8')),
        'git': bench.git_info(bench.ROOT), 'inputs': fixtures(), 'runs': [], 'errors': []}
    exe = bench.ROOT / 'dist/engine/FlashNextVelocity.Engine.exe'
    (out / 'runner-source.py').write_text(Path(__file__).read_text(encoding='utf-8'), encoding='utf-8')
    (out / 'helper-source.py').write_text(Path(bench.__file__).read_text(encoding='utf-8'), encoding='utf-8')

    def save():
        data['elapsed_seconds'] = time.monotonic() - budget.started
        data['summaries'], data['decision'] = summaries(data), decision(data)
        bench.write_json(out / 'raw-results.json', data)
        bench.write_json(out / 'inputs.json', data['inputs'])

    def execute(engine, case, policy, rep, purpose):
        budget.remaining()
        body = {'model': 'local', 'agent_prompt': '', 'messages': [
            {'role': 'system', 'content': data['inputs']['system']}, {'role': 'user', 'content': case['prompt']}],
            **sampling, 'seed': bench.SEEDS[rep - 1], 'max_tokens': case['max_tokens'],
            'stream': True, 'stream_options': {'include_usage': True}, 'flashnext_profile': False,
            'context_lookup_policy': policy,
            'chat_template_kwargs': {'enable_thinking': False, 'preserve_thinking': False, 'reasoning_effort': 'off'}}
        record = {'case_id': case['id'], 'policy': policy, 'repetition': rep, 'purpose': purpose,
            'status': 'error', 'request_sha256': bench.sha_text(json.dumps(body, sort_keys=True))}
        print(f'{purpose:<8} {case["id"]:<15} {policy:<6} #{rep}: ', end='', flush=True)
        try:
            result = bench.collect_stream(engine.base + '/v1/chat/completions', body, budget, 180)
            record.update(result)
            metrics = bench.public_metrics(result['usage']['flashnext_velocity'])
            record['metrics'] = metrics
            count, rate = result['usage']['completion_tokens'], metrics['completion_tokens_per_second']
            if count <= 0 or not math.isfinite(rate) or rate <= 0:
                raise RuntimeError('Zero output or invalid decode rate.')
            actual = metrics['lookup_policy']
            if not metrics['context_lookup_active'] or actual['mode'] != policy or actual['hard_capacity'] != 16:
                raise RuntimeError('Requested lookup policy/capacity was not applied.')
            if actual['starting_width'] != 6 or (policy == 'fixed6' and (actual['final_width'] != 6 or actual['promoted'])):
                raise RuntimeError('Lookup policy starting/reset behavior is inconsistent.')
            effective = metrics['effective_request_settings']
            for key, expected in sampling.items():
                if key not in effective or not math.isclose(float(effective[key]), float(expected), rel_tol=1e-5, abs_tol=1e-6):
                    raise RuntimeError(f'Effective sampler differs: {key}')
            if effective['seed'] != body['seed'] or metrics['context_capacity'] != config['context']:
                raise RuntimeError('Effective seed/context changed.')
            check, note = correctness(case, result)
            record.update(status='ok', check=check, check_note=note, decode_tps=rate,
                prompt_tokens=result['usage']['prompt_tokens'], completion_tokens=count,
                output_sha256=bench.sha_text(result['text']), lookup_acceptance=metrics['lookup_acceptance'],
                lookup_per_output=metrics['lookup_draft_tokens_accepted'] / count,
                exact_text_match=(result['text'].strip('\r\n') == case['expected'].strip('\r\n')) if 'expected' in case else None)
            health = bench.request_json(engine.base + '/health', timeout=min(3, budget.remaining()))
            if health['server_pid'] != engine.process.pid:
                raise RuntimeError('Engine identity changed during the trial.')
            record['memory_available_bytes'] = health.get('memory_available_bytes')
            record['device_memory'] = health.get('device_memory')
            print(f'{rate:.1f} tps | {count} tokens | {check} | width {actual["final_width"]}', flush=True)
        except BaseException as error:
            record['status'], record['error'] = 'error', str(error)
            print('ERROR: ' + str(error), flush=True)
            raise
        finally:
            data['runs'].append(record)
            save()

    try:
        save()
        if bench.active_engines() or bench.port_open(args.port):
            raise RuntimeError('Another engine or service is running. The trial will not stop or attach to it.')
        if not config.get('mtp'):
            raise RuntimeError('An MTP sidecar must be configured.')
        data['machine'] = bench.machine_info()
        print(f'18 measured requests + 6 full warmups; {args.budget_minutes:g}-minute limit.\n{out}', flush=True)
        data['assets'] = bench.assets(config, exe, budget)
        if bench.active_engines():
            raise RuntimeError('Another engine started during fingerprinting; refusing a simultaneous model load.')
        print('Loading one engine...', flush=True)
        with bench.Engine(exe, config, runtime, private, 'mtp_lookup', budget, 180) as engine:
            data['health'] = bench.clean_health(engine.health)
            data['runtime_modules'] = bench.runtime_modules(engine.process.pid)
            data['engine_pid'] = engine.process.pid
            print(f'Loaded in {engine.health["load_seconds"]:.1f}s; context {config["context"]:,}', flush=True)
            for case in data['inputs']['cases']:
                for policy in POLICIES:
                    execute(engine, case, policy, 1, 'warmup')
            for index, case in enumerate(data['inputs']['cases']):
                for rep in (1, 2, 3):
                    order = POLICIES if (index + rep) % 2 else POLICIES[::-1]
                    for policy in order:
                        execute(engine, case, policy, rep, 'measured')
        if hashlib.sha256(exe.read_bytes()).hexdigest() != next(a['sha256'] for a in data['assets'] if a['role'] == 'engine'):
            raise RuntimeError('The engine binary changed during the trial.')
        data['status'] = 'complete' if all(r.get('check') == 'PASS' for r in data['runs']) else 'complete with check failures'
    except KeyboardInterrupt:
        data['status'] = 'interrupted'
        data['errors'].append('Stopped by the user; completed requests were retained.')
    except Exception as error:
        data['status'] = 'time limit' if isinstance(error, bench.BudgetExpired) else 'partial'
        data['errors'].append(str(error))
        print(str(error), flush=True)
    finally:
        save()
        (out / 'README.md').write_text(markdown(data), encoding='utf-8')
        prefix = f'benchmark-reports/{stamp}/'
        report = bench.ROOT / 'LOOKUP-POLICY-REPORT.md'
        report.write_text(markdown(data, prefix), encoding='utf-8')
        fields = ['case_id', 'policy', 'repetition', 'purpose', 'status', 'decode_tps', 'prompt_tokens',
                  'completion_tokens', 'client_ttft_ms', 'client_total_ms', 'lookup_acceptance',
                  'lookup_per_output', 'finish_reason', 'check', 'exact_text_match', 'output_sha256', 'error']
        with (out / 'requests.csv').open('w', encoding='utf-8', newline='') as stream:
            writer = csv.DictWriter(stream, fields, extrasaction='ignore')
            writer.writeheader()
            writer.writerows(data['runs'])
        print('\n' + bench.table(['Workload', 'Fixed6 tps', 'Sticky tps', 'Change'],
            [[r['id'], bench.fmt(r['fixed6']['tps']), bench.fmt(r['sticky']['tps']),
              bench.fmt(r['change_percent']) + '%' if r['change_percent'] is not None else 'N/A'] for r in summaries(data)]), flush=True)
        print(f'\nStatus: {data["status"]}; decision: {decision(data)["result"]}; report: {report}', flush=True)
    return 0 if data['status'] == 'complete' else 2


if __name__ == '__main__':
    raise SystemExit(main())
