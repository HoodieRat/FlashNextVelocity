"""Bounded, dependency-free Windows benchmark and GitHub Markdown report.

Owns three sequential engine launches; never edits the Studio configuration or
terminates an existing engine. See docs/BENCHMARKING.md for the measurement contract.
"""
from __future__ import annotations

import argparse
import ast
import copy
import csv
import ctypes
from ctypes import wintypes
import hashlib
import html
import json
import math
import os
from pathlib import Path
import platform
import re
import socket
import statistics
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[1]
FIXTURE_PATH = ROOT / 'scripts/benchmark-fixtures/cases.json'
MODES = ('off', 'mtp', 'mtp_lookup')
MODE_NAMES = {'off': 'Speculation off', 'mtp': 'MTP only', 'mtp_lookup': 'MTP + lookup'}
SEEDS = (12345, 23456, 34567)
GIB = 1024 ** 3


class BudgetExpired(RuntimeError):
    pass


class Budget:
    def __init__(self, minutes):
        self.started = time.monotonic()
        self.deadline = self.started + minutes * 60

    def remaining(self):
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise BudgetExpired('The benchmark time limit was reached.')
        return remaining


def sha_text(text):
    return hashlib.sha256(text.encode('utf-8')).hexdigest()


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False,
                                    allow_nan=False) + '\n', encoding='utf-8')
    temporary.replace(path)


def hash_file(path, budget):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        while chunk := stream.read(8 * 1024 * 1024):
            budget.remaining()
            digest.update(chunk)
    return digest.hexdigest()


def median(values):
    values = [v for v in values if isinstance(v, (int, float)) and math.isfinite(v)]
    return statistics.median(values) if values else None


def fmt(value, digits=1):
    return f'{value:,.{digits}f}' if value is not None else 'N/A'


def cell(value):
    return str(value).replace('|', '\\|').replace('\r', '').replace('\n', '<br>')


def table(headers, rows):
    return '\n'.join(['| ' + ' | '.join(map(cell, headers)) + ' |',
                      '| ' + ' | '.join('---' for _ in headers) + ' |'] +
                     ['| ' + ' | '.join(map(cell, row)) + ' |' for row in rows])


def powershell_json(script):
    command = ['powershell.exe', '-NoLogo', '-NoProfile', '-Command',
               "[Console]::OutputEncoding=[Text.UTF8Encoding]::new(); " + script]
    result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8',
                            timeout=20, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    if result.returncode:
        raise RuntimeError(result.stderr.strip())
    output = result.stdout.lstrip('\ufeff').strip()
    # Windows PowerShell emits no JSON for an empty pipeline/array.
    return json.loads(output) if output else None


def machine_info():
    if os.name != 'nt':
        return {'os': platform.platform(), 'cpu': platform.processor(),
                'note': 'Windows telemetry unavailable'}
    try:
        return powershell_json("""
        $cpuInfo=Get-CimInstance Win32_Processor | Select-Object -First 1;
        $osInfo=Get-CimInstance Win32_OperatingSystem;
        $ramInfo=@(Get-CimInstance Win32_PhysicalMemory);
        $gpuInfo=@(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion);
        $powerInfo=(powercfg /getactivescheme | Out-String).Trim();
        [ordered]@{cpu=$cpuInfo.Name;cores=$cpuInfo.NumberOfCores;threads=$cpuInfo.NumberOfLogicalProcessors;
          os=$osInfo.Caption;os_version=$osInfo.Version;os_build=$osInfo.BuildNumber;
          ram_bytes=($ramInfo | Measure-Object Capacity -Sum).Sum;
          ram_mt_s=@($ramInfo | Select-Object -ExpandProperty ConfiguredClockSpeed -Unique);
          gpu=$gpuInfo;power_plan=$powerInfo} | ConvertTo-Json -Depth 5
        """)
    except Exception as error:
        return {'os': platform.platform(), 'telemetry_error': str(error)}


def active_engines():
    if os.name != 'nt':
        return []
    data = powershell_json("@(Get-CimInstance Win32_Process | Where-Object { "
                           "$_.Name -eq 'FlashNextVelocity.Engine.exe' } | "
                           "Select-Object ProcessId,Name) | ConvertTo-Json -Compress")
    return data if isinstance(data, list) else [data] if data else []


def runtime_modules(pid):
    try:
        data = powershell_json(f"@( (Get-Process -Id {int(pid)}).Modules | Where-Object {{ "
            "$_.ModuleName -match '^(amdhip|hipblas|rocblas)' } | ForEach-Object { "
            "[ordered]@{module=$_.ModuleName;file_version=$_.FileVersionInfo.FileVersion;"
            "product_version=$_.FileVersionInfo.ProductVersion} }) | ConvertTo-Json -Depth 3")
        return data if isinstance(data, list) else [data] if data else []
    except Exception:
        return []


def port_open(port):
    with socket.socket() as probe:
        probe.settimeout(0.5)
        return probe.connect_ex(('127.0.0.1', port)) == 0


def request_json(url, body=None, timeout=10):
    request = urllib.request.Request(url, data=None if body is None else
        json.dumps(body).encode('utf-8'), headers={'Content-Type': 'application/json'})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return json.load(response)
    except urllib.error.HTTPError as error:
        raise RuntimeError(f'HTTP {error.code}: {error.read().decode("utf-8", errors="replace")}') from error


def collect_stream(url, body, budget, timeout):
    """Time meaningful SSE payloads; role/heartbeat/usage events are not tokens."""
    start = time.monotonic()
    deadline = min(budget.deadline, start + timeout)
    request = urllib.request.Request(url, json.dumps(body).encode('utf-8'),
                                     {'Content-Type': 'application/json'})
    text, reasoning, calls, events = [], [], {}, []
    usage, finish, first, last, done = None, None, None, None, False
    with urllib.request.urlopen(request, timeout=max(0.1, deadline - start)) as response:
        for wire_line in response:
            now = time.monotonic()
            if now >= deadline:
                raise BudgetExpired('Time limit reached during generation.') if now >= budget.deadline else TimeoutError('Request time limit reached.')
            if not wire_line.startswith(b'data:'):
                continue
            payload = wire_line[5:].strip()
            if payload == b'[DONE]':
                done = True
                break
            chunk = json.loads(payload)
            if chunk.get('error'):
                raise RuntimeError(chunk['error'].get('message', str(chunk['error'])))
            if chunk.get('usage'):
                usage = chunk['usage']
            meaningful = False
            for choice in chunk.get('choices', []):
                delta = choice.get('delta', {})
                if delta.get('content'):
                    text.append(delta['content'])
                    meaningful = True
                if delta.get('reasoning_content'):
                    reasoning.append(delta['reasoning_content'])
                    meaningful = True
                for call in delta.get('tool_calls', []):
                    current = calls.setdefault(call.get('index', 0),
                                               {'name': '', 'arguments': ''})
                    function = call.get('function', {})
                    current['name'] += function.get('name', '')
                    current['arguments'] += function.get('arguments', '')
                    meaningful = True
                if choice.get('finish_reason'):
                    finish = choice['finish_reason']
            if meaningful:
                first = now if first is None else first
                last = now
                events.append((now - start) * 1000)
    if not done or usage is None:
        raise RuntimeError('Incomplete SSE response: missing DONE or final usage.')
    gaps = [b - a for a, b in zip(events, events[1:])]
    return {'text': ''.join(text), 'reasoning': ''.join(reasoning),
            'tool_calls': list(calls.values()), 'usage': usage, 'finish_reason': finish,
            'client_ttft_ms': None if first is None else (first - start) * 1000,
            'client_last_payload_ms': None if last is None else (last - start) * 1000,
            'client_total_ms': (time.monotonic() - start) * 1000,
            'payload_event_times_ms': events, 'payload_event_count': len(events),
            'median_payload_gap_ms': median(gaps), 'max_payload_gap_ms': max(gaps) if gaps else None}


def extract_code(text):
    match = re.search(r'```(?:python|py)?\s*\n(.*?)```', text, re.S | re.I)
    return match.group(1).strip() if match else text.strip()


def check_output(kind, result, fixtures):
    text = result.get('text', '')
    try:
        if kind == 'structured_json':
            value = json.loads(text)
            return ('PASS', 'Exact JSON schema and values') if value == {
                'status': 'ready', 'count': 3, 'items': ['alpha', 'beta', 'gamma']} else ('FAIL', 'JSON values differ')
        if kind == 'tool_call':
            calls = result.get('tool_calls', [])
            good = len(calls) == 1 and calls[0]['name'] == 'benchmark_lookup' and json.loads(
                calls[0]['arguments']) == {'record_id': 17, 'category': 'inventory'}
            return ('PASS', 'Expected simulated call; nothing executed') if good else ('FAIL', 'Tool name/arguments differ')
        if kind == 'code_edit':
            output = ast.dump(ast.parse(extract_code(text)), include_attributes=False)
            source = fixtures['code_edit_expected']
            # The prompt specifies integer quantities; coercion is optional.
            # Both accepted forms still require all unrelated ASTs to match.
            alternatives = (source, source.replace("sum(row['quantity'] for row in items)",
                "sum(int(row['quantity']) for row in items)"))
            expected = {ast.dump(ast.parse(code), include_attributes=False) for code in alternatives}
            return ('PASS', 'AST matches accepted integer-quantity edit and preserved code') if output in expected else ('FAIL', 'AST differs from requested edit')
        if kind == 'code_generation':
            code = extract_code(text)
            tree = ast.parse(code)
            found = any(isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and
                        node.name == 'chunked' for node in ast.walk(tree))
            return ('PASS', 'Python parses and chunked is present; behavior not executed') if found else ('FAIL', 'chunked function missing')
        if not text.strip() or len(text.split()) < 20:
            return 'FAIL', 'Empty or too-short response'
        if result.get('reasoning'):
            return 'FAIL', 'Reasoning appeared despite thinking-off preset'
        return 'PASS', 'Nonempty response; factual quality not scored'
    except (ValueError, SyntaxError, KeyError, TypeError) as error:
        return 'FAIL', str(error)


def recheck_edits(data, fixtures):
    """Re-evaluate saved edits only when their original prompt still matches."""
    result = copy.deepcopy(data)
    cases = {case['id']: case for case in result['cases']}
    edits = [row for row in result['runs'] if row.get('purpose') == 'measured'
             and row.get('status') == 'ok' and cases.get(row['case_id'], {}).get('kind') == 'code_edit']
    if not edits:
        raise ValueError('No successful saved code-edit responses to recheck.')
    for row in edits:
        if cases[row['case_id']].get('prompt') != fixtures['code_edit']:
            raise ValueError('Saved code-edit prompt differs; offline rechecking is not valid.')
    changes = []
    for row in edits:
        before = {'check': row.get('check'), 'check_note': row.get('check_note')}
        check, note = check_output('code_edit', row, fixtures)
        changes.append({'case_id': row['case_id'], 'repetition': row['repetition'],
                        'before': before, 'after': {'check': check, 'check_note': note}})
        row['check'], row['check_note'] = check, note
    measured = [row for row in result['runs'] if row.get('purpose') == 'measured']
    if result['status'] in ('complete', 'complete with check failures', 'partial', 'partial with check failures'):
        successful = sum(row.get('status') == 'ok' for row in measured)
        result['status'] = 'complete' if successful == result['planned_measured_requests'] else 'partial'
        if any(row.get('check') == 'FAIL' for row in measured):
            result['status'] += ' with check failures'
    result.pop('comparison', None)
    result['correctness_recheck'] = {
        'checked_utc': datetime.now(timezone.utc).isoformat(),
        'checker_revision': fixtures.get('code_edit_check_revision', 1),
        'checker_sha256': sha_text(Path(__file__).read_text(encoding='utf-8')),
        'fixture_sha256': sha_text(FIXTURE_PATH.read_text(encoding='utf-8')),
        'expected_source': fixtures['code_edit_expected'],
        'rule': 'Integer quantities may be summed directly or with int(); unrelated code must retain its AST.',
        'evaluated': len(edits),
        'changed': sum(change['before']['check'] != change['after']['check'] for change in changes),
        'changes': changes}
    return result


def build_cases(fixtures, repetitions):
    cases = []
    for mode in MODES:
        for kind in ('prose', 'code_generation', 'code_edit'):
            cases.append({'id': f'{kind}-{mode}', 'kind': kind, 'mode': mode,
                          'prompt': fixtures[kind], 'max_tokens': 256, 'repetitions': repetitions,
                          'cache': 'fresh'})
    for depth in (8192, 32768, 65536):
        cases.append({'id': f'context-{depth}', 'kind': 'context', 'mode': 'mtp_lookup',
                      'target_context': depth, 'max_tokens': 256,
                      'repetitions': repetitions, 'cache': 'fresh'})
    for kind in ('structured_json', 'tool_call'):
        cases.append({'id': kind, 'kind': kind, 'mode': 'mtp_lookup',
                      'prompt': fixtures[kind], 'max_tokens': 128,
                      'repetitions': repetitions, 'cache': 'fresh'})
    cases.append({'id': 'conversation', 'kind': 'conversation', 'mode': 'mtp_lookup',
                  'max_tokens': 256, 'repetitions': repetitions, 'cache': 'paired continuation/fresh'})
    cases.append({'id': 'sustained', 'kind': 'sustained', 'mode': 'mtp_lookup',
                  'prompt': fixtures['sustained'], 'max_tokens': 1024,
                  'repetitions': 1, 'cache': 'fresh'})
    return cases


def context_corpus(root, fixtures):
    sections, sources = [], []
    for relative in fixtures['context_sources']:
        path = root / relative
        if not path.is_file():
            continue
        content = path.read_text(encoding='utf-8-sig')
        sections.append(f'\nREFERENCE FILE: {relative}\n{content}\n')
        sources.append({'file': relative, 'sha256': sha_text(content)})
    corpus = ''.join(sections)
    if len(corpus) < 250000:
        raise RuntimeError('Context corpus is too short. Restore the documented source files.')
    return corpus, sources


def sampling_for(config, preset):
    defaults = {'temperature': 0.35, 'top_p': 0.9, 'top_k': 40, 'min_p': 0.05,
                'repeat_penalty': 1.05, 'repeat_last_n': 512,
                'frequency_penalty': 0, 'presence_penalty': 0}
    if preset == 'greedy':
        defaults.update(temperature=0, top_p=1, top_k=0, min_p=0, repeat_penalty=1)
    else:
        defaults.update({key: config.get('sampling', {}).get(key, value)
                         for key, value in defaults.items()})
    return defaults


def public_config(config):
    result = copy.deepcopy(config)
    for key in ('model', 'mtp', 'mmproj'):
        result[key] = Path(result[key]).name if result.get(key) else ''
    result.pop('agent_prompt', None)
    return result


def public_metrics(value):
    if isinstance(value, dict):
        return {key: (Path(item).name if isinstance(item, str) and item else item)
                if key in ('model', 'mtp', 'mmproj', 'active_config') else public_metrics(item)
                for key, item in value.items()}
    if isinstance(value, list):
        return [public_metrics(item) for item in value]
    if isinstance(value, str):
        return value.replace(str(ROOT), '[project]').replace(str(Path.home()), '[user]')
    return value


class OwnedProcessJob:
    """Windows closes this non-inherited handle if the runner is terminated.

    JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE prevents a hidden model process surviving
    a closed terminal. The job contains only the process launched by this runner.
    """
    def __init__(self, process):
        self.handle = None
        if os.name != 'nt':
            return

        class BasicLimits(ctypes.Structure):
            _fields_ = [('PerProcessUserTimeLimit', ctypes.c_longlong),
                        ('PerJobUserTimeLimit', ctypes.c_longlong), ('LimitFlags', wintypes.DWORD),
                        ('MinimumWorkingSetSize', ctypes.c_size_t), ('MaximumWorkingSetSize', ctypes.c_size_t),
                        ('ActiveProcessLimit', wintypes.DWORD), ('Affinity', ctypes.c_size_t),
                        ('PriorityClass', wintypes.DWORD), ('SchedulingClass', wintypes.DWORD)]

        class IoCounters(ctypes.Structure):
            _fields_ = [(name, ctypes.c_ulonglong) for name in
                        ('ReadOperationCount', 'WriteOperationCount', 'OtherOperationCount',
                         'ReadTransferCount', 'WriteTransferCount', 'OtherTransferCount')]

        class ExtendedLimits(ctypes.Structure):
            _fields_ = [('BasicLimitInformation', BasicLimits), ('IoInfo', IoCounters),
                        ('ProcessMemoryLimit', ctypes.c_size_t), ('JobMemoryLimit', ctypes.c_size_t),
                        ('PeakProcessMemoryUsed', ctypes.c_size_t), ('PeakJobMemoryUsed', ctypes.c_size_t)]

        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.CreateJobObjectW.argtypes = (ctypes.c_void_p, wintypes.LPCWSTR)
        kernel.CreateJobObjectW.restype = wintypes.HANDLE
        kernel.SetInformationJobObject.argtypes = (wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD)
        kernel.AssignProcessToJobObject.argtypes = (wintypes.HANDLE, wintypes.HANDLE)
        kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
        self.kernel = kernel
        handle = kernel.CreateJobObjectW(None, None)
        if not handle:
            raise ctypes.WinError(ctypes.get_last_error())
        info = ExtendedLimits()
        info.BasicLimitInformation.LimitFlags = 0x2000
        if (not kernel.SetInformationJobObject(handle, 9, ctypes.byref(info), ctypes.sizeof(info)) or
                not kernel.AssignProcessToJobObject(handle, wintypes.HANDLE(int(process._handle)))):
            error = ctypes.get_last_error()
            kernel.CloseHandle(handle)
            raise ctypes.WinError(error)
        self.handle = handle

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


class Engine:
    def __init__(self, exe, config, runtime, private_dir, mode, budget, startup_timeout):
        self.exe, self.config, self.runtime = exe, config, runtime
        self.private_dir, self.mode, self.budget = private_dir, mode, budget
        self.startup_timeout, self.process, self.log = startup_timeout, None, None
        self.base = f'http://127.0.0.1:{config["port"]}'
        self.health = None
        self.job, self.timer = None, None

    def __enter__(self):
        if port_open(self.config['port']):
            raise RuntimeError('Benchmark port is in use; refusing to attach to another engine.')
        config_path = self.private_dir / f'config-{self.mode}.json'
        write_json(config_path, self.config)
        env = os.environ.copy()
        for key, field in [('ROCM_PATH', 'RocmPath'), ('HIP_PATH', 'RocmPath'),
                           ('HIP_DEVICE_LIB_PATH', 'DeviceLibPath'), ('DEVICE_LIB_PATH', 'DeviceLibPath'),
                           ('ROCBLAS_TENSILE_LIBPATH', 'RocblasTensileLibPath'),
                           ('HIPBLASLT_TENSILE_LIBPATH', 'HipblasltTensileLibPath')]:
            if self.runtime.get(field):
                env[key] = self.runtime[field]
        env['PATH'] = os.pathsep.join([str(self.exe.parent),
            str(Path(self.runtime['RocmPath']) / 'bin'), self.runtime.get('VcpkgBin', ''),
            env.get('PATH', '')])
        self.log = (self.private_dir / f'engine-{self.mode}.log').open('wb')
        try:
            self.process = subprocess.Popen([str(self.exe), '--config', str(config_path)],
                cwd=self.exe.parent, env=env, stdout=self.log, stderr=subprocess.STDOUT,
                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            self.job = OwnedProcessJob(self.process)
            self.timer = threading.Timer(self.budget.remaining(), self.expire)
            self.timer.daemon = True
            self.timer.start()
            deadline = min(self.budget.deadline, time.monotonic() + self.startup_timeout)
            last_error = ''
            while time.monotonic() < deadline:
                self.budget.remaining()
                if self.process.poll() is not None:
                    raise RuntimeError(f'Engine exited during startup ({self.process.returncode}); see local engine log.')
                try:
                    health = request_json(self.base + '/health', timeout=min(2, self.budget.remaining()))
                    if health.get('server_pid') != self.process.pid:
                        raise RuntimeError('Health belongs to another process.')
                    expected_mtp = self.mode != 'off'
                    expected_lookup = self.mode == 'mtp_lookup'
                    if (health.get('mtp') != expected_mtp or
                            health.get('context_lookup_active') != expected_lookup):
                        raise RuntimeError('Engine acceleration mode does not match its launch configuration.')
                    for key in ('context', 'draft_max', 'prefill_batch', 'mtp_proposal_mode'):
                        if health.get(key) != self.config[key]:
                            raise RuntimeError(f'Engine launch setting mismatch: {key}')
                    self.health = health
                    return self
                except (OSError, urllib.error.URLError) as error:
                    last_error = str(error)
                time.sleep(0.25)
            raise RuntimeError(f'Engine startup timed out: {last_error}')
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.timer:
            self.timer.cancel()
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        if self.job:
            self.job.close()
        if self.log:
            self.log.close()

    def expire(self):
        if self.process is not None and self.process.poll() is None:
            try:
                self.process.terminate()
            except OSError:
                pass

    def __exit__(self, *_):
        self.close()


def assets(config, exe, budget):
    paths = []
    first = Path(config['model'])
    match = re.search(r'(.*)-00001-of-(\d+)\.gguf$', first.name)
    shards = [first.with_name(f'{match[1]}-{number:05d}-of-{int(match[2]):05d}.gguf')
              for number in range(1, int(match[2]) + 1)] if match else [first]
    paths.extend(('target', path) for path in shards)
    if config.get('mtp'):
        paths.append(('mtp', Path(config['mtp'])))
    paths.append(('engine', exe))
    for name in ('amdhip64_7.dll', 'hipblaslt.dll', 'rocblas.dll'):
        if (exe.parent / name).exists():
            paths.append(('runtime', exe.parent / name))
    result = []
    for role, path in paths:
        print(f'  Fingerprint: {path.name}', flush=True)
        result.append({'role': role, 'file': path.name, 'bytes': path.stat().st_size,
                       'sha256': hash_file(path, budget)})
    return result


def git_info(root):
    def run(*args):
        p = subprocess.run(['git', '-C', str(root), *args], capture_output=True, timeout=10)
        return p.stdout if p.returncode == 0 else b''
    try:
        return {'commit': run('rev-parse', 'HEAD').decode().strip(),
                'dirty': bool(run('status', '--porcelain')),
                'tracked_diff_sha256': hashlib.sha256(run('diff', 'HEAD', '--binary')).hexdigest()}
    except (OSError, subprocess.TimeoutExpired):
        return {'commit': 'unavailable'}


def clean_health(health):
    return {key: value for key, value in health.items() if key not in
            ('config_model', 'config_mtp', 'config_mmproj', 'studio_agent_prompt')}


def summarize_case(case, runs):
    records = [r for r in runs if r['case_id'] == case['id'] and r.get('purpose') == 'measured']
    good = [r for r in records if r.get('status') == 'ok' and r.get('cache') != 'paired-fresh']
    expected = case['repetitions']
    rates = [r['decode_tps'] for r in good if r.get('decode_tps') is not None]
    return {'id': case['id'], 'kind': case['kind'], 'mode': case['mode'],
            'planned': expected, 'completed': len(good),
            'status': 'complete' if len(good) == expected else 'partial' if good else 'not measured',
            'decode_tps': median(rates), 'min_tps': min(rates) if rates else None,
            'max_tps': max(rates) if rates else None,
            'prompt_tokens': sorted({r['prompt_tokens'] for r in good}),
            'output_tokens': sorted({r['completion_tokens'] for r in good}),
            'client_ttft_ms': median([r.get('client_ttft_ms') for r in good]),
            'engine_ttft_ms': median([r.get('engine_ttft_ms') for r in good]),
            'total_ms': median([r.get('client_total_ms') for r in good]),
            'prefill_ms': median([r.get('prefill_ms') for r in good]),
            'prefill_tps': median([r.get('prefill_tps') for r in good if r['cache'] == 'fresh']),
            'mtp_acceptance': median([r.get('mtp_acceptance') for r in good]),
            'mtp_per_output': median([r.get('mtp_per_output') for r in good]),
            'lookup_per_output': median([r.get('lookup_per_output') for r in good]),
            'payload_gap_ms': median([r.get('max_payload_gap_ms') for r in good]),
            'check_passes': sum(r.get('check') == 'PASS' for r in good),
            'ended_early': sum(r.get('finish_reason') != 'length' for r in good)}


def make_chart(path, title, labels, values, colors=None):
    valid = [(label, value, (colors or ['#2563eb'] * len(labels))[i])
             for i, (label, value) in enumerate(zip(labels, values)) if value is not None]
    width, height = 920, 100 + 44 * max(1, len(valid))
    elements = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" role="img">',
                f'<title>{html.escape(title)}</title>',
                '<rect width="100%" height="100%" rx="12" fill="#f8fafc"/>',
                f'<text x="24" y="34" font-family="sans-serif" font-size="20" fill="#0f172a">{html.escape(title)}</text>']
    scale = max([value for _, value, _ in valid], default=1) or 1
    for i, (label, value, color) in enumerate(valid):
        y, length = 65 + i * 44, 470 * value / scale
        elements.extend([f'<text x="24" y="{y + 19}" font-family="sans-serif" font-size="13" fill="#334155">{html.escape(label)}</text>',
                         f'<rect x="330" y="{y}" width="{length:.1f}" height="28" rx="4" fill="{color}"/>',
                         f'<text x="{340 + length:.1f}" y="{y + 19}" font-family="sans-serif" font-size="13" fill="#0f172a">{value:.1f} tps</text>'])
    if not valid:
        elements.append('<text x="24" y="85" font-family="sans-serif" fill="#64748b">No measured data available.</text>')
    elements.append('</svg>')
    path.write_text('\n'.join(elements), encoding='utf-8')


def markdown_report(data, prefix=''):
    summaries, runs = data.get('summaries', []), data.get('runs', [])
    meta = data['metadata']
    measured = [r for r in runs if r.get('purpose') == 'measured']
    successful = [r for r in measured if r.get('status') == 'ok']
    failures = [r for r in measured if r.get('status') != 'ok' or r.get('check') == 'FAIL']
    case_count = sum(s['status'] == 'complete' for s in summaries)
    link = lambda name: prefix + name
    lines = ['# FlashNextVelocity Benchmark Report', '',
             f'> **{data["status"].upper()}** · {data["started_utc"]} · Suite v1 · {meta["preset"]} sampling', '',
             '## 1. Executive Summary', '',
             table(['Measure', 'Result'], [
                 ['Completed conditions', f'{case_count}/{len(data["cases"])}'],
                 ['Measured requests', f'{len(successful)}/{data["planned_measured_requests"]} successful'],
                 ['Warmup / setup requests', f'{sum(r["purpose"] == "warmup" for r in runs)} / {sum(r["purpose"] == "setup" for r in runs)}'],
                 ['Elapsed time', f'{fmt(data.get("elapsed_seconds", 0) / 60)} minutes'],
                 ['Time limit', f'{meta["budget_minutes"]} minutes'],
                 ['Basic correctness checks', f'{sum(r.get("check") == "PASS" for r in successful)}/{len(successful)} passed'],
                 ['Profiling', 'Disabled for throughput measurements']]), '',
             '**Rates apply to the listed workloads and conditions. No single rate represents every task.**', '']
    if data.get('errors'):
        lines += ['**Run notes:**', ''] + [f'- {cell(e)}' for e in data['errors']] + ['']
    recheck = data.get('correctness_recheck')
    if recheck:
        lines += [f'**Correctness rechecked offline:** {recheck["evaluated"]} saved code-edit responses were evaluated '
                  f'with checker revision {recheck["checker_revision"]}; {recheck["changed"]} check results changed. '
                  'No inference was rerun. Original outputs, timings, settings, and benchmark duration are retained.', '',
                  'The prompt specifies integer quantities, so both direct summation and optional `int()` conversion '
                  'are accepted. Unrelated code must retain its AST. The earlier checker required conversion that the prompt did not request.', '',
                  f'[Original report]({link(recheck["source_report"])}). Check history and source hashes are retained in the raw results.', '']
    core = [s for s in summaries if s['kind'] in ('prose', 'code_generation', 'code_edit')]
    by_id = {s['id']: s for s in core}
    overview = []
    for kind in ('prose', 'code_generation', 'code_edit'):
        baseline = by_id.get(kind + '-off', {}).get('decode_tps')
        mtp = by_id.get(kind + '-mtp', {}).get('decode_tps')
        full = by_id.get(kind + '-mtp_lookup', {}).get('decode_tps')
        ratio = f'{full / baseline:.2f}x' if full is not None and baseline else 'N/A'
        overview.append([kind.replace('_', ' '), fmt(baseline), fmt(mtp), fmt(full), ratio])
    lines += [table(['Workload', 'Off tps', 'MTP tps', 'MTP + lookup tps', 'Full / off'], overview), '',
              'Ratios describe observed throughput in this grouped run. Consult correctness results before treating them as completed-task gains.', '']
    lines += [f'![Decode throughput by workload and acceleration]({link("workloads.svg")})', '',
              '## 2. System and Software Configuration', '']
    machine = meta.get('machine', {})
    hardware_rows = [['CPU', machine.get('cpu', 'Unavailable')], ['OS', machine.get('os', 'Unavailable')],
                     ['OS build', machine.get('os_build', 'Unavailable')],
                     ['Installed RAM', f'{fmt(machine.get("ram_bytes", 0) / GIB)} GiB' if machine.get('ram_bytes') else 'Unavailable'],
                     ['RAM configured speed', (', '.join(map(str, machine.get('ram_mt_s', []))) + ' MT/s') if machine.get('ram_mt_s') else 'Unavailable'],
                     ['GPU / driver', '; '.join(f'{g.get("Name")} / {g.get("DriverVersion")}' for g in machine.get('gpu', [])) or 'Unavailable'],
                     ['Power plan', machine.get('power_plan', 'Unavailable')],
                     ['Backend', 'Native Gufo / ROCm HIP / gfx1151'],
                     ['Loaded HIP runtime file version', ', '.join(sorted({module.get('file_version') or 'Unavailable'
                         for mode in data.get('modes', []) for module in mode.get('runtime_modules', [])
                         if module.get('module', '').lower().startswith('amdhip')})) or 'Unavailable'],
                     ['Engine runtime revision', ', '.join(sorted({m['health'].get('runtime_revision', 'unknown') for m in data.get('modes', [])})) or 'Not loaded'],
                     ['Git commit', meta.get('git', {}).get('commit', 'Unavailable')],
                     ['Working tree', 'Contains local changes' if meta.get('git', {}).get('dirty') else 'Clean or unavailable'],
                     ['Context capacity', meta.get('context_capacity', 'Unavailable')],
                     ['Preset', meta['preset']],
                     ['Sampling', ', '.join(f'{key}={value}' for key, value in meta.get('sampling', {}).items())],
                     ['Target model', meta.get('source_config', {}).get('model', 'Unavailable')],
                     ['MTP sidecar', meta.get('source_config', {}).get('mtp', 'Unavailable')]]
    lines += [table(['Setting', 'Value'], hardware_rows), '',
              'Full asset hashes, per-mode settings, fixture hashes, and request settings are in '
              f'[raw-results.json]({link("raw-results.json")}).', '',
              '## 3. Benchmark Methodology', '',
              '- One client; three sequential engine loads: speculation off, MTP only, MTP plus context lookup.',
              f'- {meta["repetitions"]} repetitions per condition; one longer generation. Seed schedule: ' + ', '.join(map(str, meta['seeds'])) + '.',
              '- Most outputs are capped at 256 tokens; compact JSON/tool tasks at 128; longer generation at 1,024. Natural early stops are retained and labeled.',
              '- Six planned warmups: one per mode and one per context depth. Calibration and continuation priming are setup requests, excluded from performance medians.',
              '- Fresh chat requests diverge from the preceding generated session. This native engine resets recurrent state on divergence. Conversation continuation uses the separate raw-completion API and has an explicitly matched fresh control.',
              '- Context inputs are snapshots of actual repository documents and code, not repeated single-token filler. The requested depths are approximate; actual token counts are reported.',
              '- Thinking, vision, and the private Studio agent prompt are disabled for this text suite. Context capacity is identical across modes and raised to at least 67,584 tokens for the 64k test.',
              '- Client first response timing starts before sending the request and ignores role, heartbeat, and usage events. Tool responses may be buffered until a complete call exists.',
              '- Engine decode tps = generated tokens / engine decode seconds. Client total time includes the HTTP request and streaming delivery. These are different measurement boundaries.',
              '- Streaming gaps are intervals between meaningful payload events, not exact per-token GPU latency. Draft IDs and hidden tokens are not counted as accepted output.',
              '- Reported prefill tps is used only for fresh prompts. Continuation reports prefill milliseconds because the engine does not expose the actual number of newly processed tokens.',
              '- Engine loading uses the existing OS file cache; these are not cold-disk load tests.', '',
              f'Exact input snapshots: [inputs.json]({link("inputs.json")}). No generated tool or code is executed.', '',
              '## 4. Performance Results', '', '### 4.1 Workloads and Acceleration', '']
    def rate(s):
        return f'{fmt(s["decode_tps"])} [{fmt(s["min_tps"])}–{fmt(s["max_tps"])}]' if s['decode_tps'] is not None else 'Not measured'
    def counts(s):
        return ', '.join(map(str, s['prompt_tokens'])) + ' / ' + ', '.join(map(str, s['output_tokens'])) if s['completed'] else '—'
    lines += [table(['Workload', 'Acceleration', 'Input / output tokens', 'Decode tps: median [range]', 'Client first response ms', 'Total s', 'Runs'],
                    [[s['kind'].replace('_', ' '), MODE_NAMES[s['mode']], counts(s), rate(s), fmt(s['client_ttft_ms']),
                      fmt(s['total_ms'] / 1000 if s['total_ms'] is not None else None), f'{s["completed"]}/{s["planned"]}'] for s in core]), '',
              '### 4.2 Context Depth', '', f'![Decode throughput versus actual occupied context]({link("context.svg")})', '',
              table(['Condition', 'Actual input / output tokens', 'Decode tps: median [range]', 'Fresh prefill tps', 'Client first response ms', 'Runs'],
                    [[s['id'], counts(s), rate(s), fmt(s['prefill_tps']), fmt(s['client_ttft_ms']),
                      f'{s["completed"]}/{s["planned"]}'] for s in summaries if s['kind'] == 'context']), '',
              '### 4.3 Structured Output and Sustained Generation', '',
              table(['Condition', 'Input / output tokens', 'Decode tps: median [range]', 'First response ms', 'Checks', 'Early stops'],
                    [[s['id'], counts(s), rate(s), fmt(s['client_ttft_ms']), f'{s["check_passes"]}/{s["completed"]}', s['ended_early']]
                     for s in summaries if s['kind'] in ('structured_json', 'tool_call', 'sustained')]), '',
              'Longer-generation 64-token window rates are retained in the raw results. An output shorter than 1,024 tokens is not evidence of sustained speed over 1,024 tokens.', '',
              '### 4.4 Conversation Continuation', '']
    continuation = [r for r in successful if r.get('case_id') == 'conversation']
    lines += [table(['Request', 'Actual input / output tokens', 'Engine first token ms', 'Prefill ms', 'Total s', 'Notes'],
                    [[f'{r["cache"]} #{r["repetition"]}', f'{r["prompt_tokens"]} / {r["completion_tokens"]}', fmt(r.get('engine_ttft_ms')),
                      fmt(r.get('prefill_ms')), fmt(r['client_total_ms'] / 1000), r.get('continuation_note', '')] for r in continuation]), '',
              'Continuation is an attempt to extend the exact raw prefix. Re-tokenization or EOS can prevent reuse; the API does not expose cached-token counts, so reuse is not asserted as proven.', '',
              '### 4.5 Speculation and Streaming', '',
              table(['Condition', 'MTP acceptance', 'MTP accepted / output', 'Lookup accepted / output', 'Median longest payload gap ms'],
                    [[s['id'], fmt(s['mtp_acceptance'] * 100) + '%' if s['mtp_acceptance'] is not None else 'N/A',
                      fmt(s['mtp_per_output'], 3), fmt(s['lookup_per_output'], 3), fmt(s['payload_gap_ms'])]
                     for s in summaries if s['completed']]), '',
              'Acceptance is N/A when no proposals occurred. High acceptance alone does not establish a throughput improvement.', '',
              '### 4.6 Memory and Loading', '']
    lines += [table(['Mode', 'Engine load s', 'Sampled minimum free RAM GiB', 'Sampled maximum GPU local/shared GiB'],
                    [[MODE_NAMES[m['mode']], fmt(m['health'].get('load_seconds')), fmt(m.get('min_available_bytes', 0) / GIB),
                      f'{fmt(m.get("max_local_bytes", 0) / GIB)} / {fmt(m.get("max_shared_bytes", 0) / GIB)}'] for m in data.get('modes', [])]), '',
              'Memory is sampled at request boundaries, not a guaranteed peak/minimum. GPU allocations are not added to host RAM usage on this unified-memory machine. Temperatures, clock traces, board power, and physical NVMe throughput are unavailable in this suite.', '',
              '## 5. Correctness and Reliability', '',
              'JSON checks exact values; tool checks validate the simulated function and arguments; code editing compares Python ASTs; code generation checks syntax and function presence. Prose checks only a nonempty response. These are basic integrity checks, not a comprehensive quality evaluation.', '']
    lines += [table(['Condition / repetition', 'Result', 'Details'],
                    [[f'{r["case_id"]} #{r["repetition"]}', r.get('status') if r.get('status') != 'ok' else r.get('check'),
                      r.get('error') or r.get('check_note', '')] for r in failures]) if failures else 'No measured request failed the basic integrity checks.', '',
              '## 6. Analysis and Limitations', '',
              '- Compare matching workload rows, occupied context, token caps, model hashes, sampling, and cache conditions. Model/backend changes require an explicitly labeled comparison.',
              '- Three repetitions support medians and ranges, not reliable tail-latency percentiles or small-gain claims. No p95/p99 request latency is inferred.',
              '- Capped outputs can truncate code or tasks. The speed remains measured, but a failed task check is not a useful completed answer.',
              '- This API benchmark includes sampling and serving. It is not a llama-bench pp/tg microbenchmark, and its rates should not be ranked directly against those numbers.',
              '- Modes are grouped to avoid repeated model loads. This is not an interleaved A/B experiment; temperature and clock drift can influence mode comparisons.',
              '- Long context uses one repository corpus and is not a broad retrieval-quality benchmark. No multi-client serving, vision, reasoning-on, or overnight stability test is included.',
              '- Pure GPU graph timing is not reported; completion waits cannot be assumed to be removable overhead.', '']
    if data.get('comparison'):
        lines += ['### 6.1 Previous Report Comparison', '',
                  table(['Condition', 'Compatibility', 'Previous tps', 'Current tps', 'Change'],
                        [[r['id'], r['compatibility'], fmt(r.get('previous')), fmt(r.get('current')),
                          fmt(r.get('delta_percent')) + '%' if r.get('delta_percent') is not None else 'N/A'] for r in data['comparison']]), '']
    lines += ['## 7. Reproducibility Appendix', '',
              f'- [Raw results and effective settings]({link("raw-results.json")})',
              f'- [Per-request CSV]({link("requests.csv")})',
              f'- [Exact fixtures and context snapshots]({link("inputs.json")})',
              '- Benchmark runner SHA-256: `' + meta.get('runner_sha256', 'unavailable') + '`', '',
              '<details>', '<summary>Model, sidecar, binary, and runtime fingerprints</summary>', '',
              table(['Role', 'File', 'GiB', 'SHA-256'], [[a['role'], a['file'], fmt(a['bytes'] / GIB, 3), '`' + a['sha256'] + '`']
                    for a in meta.get('assets', [])]), '', '</details>', '',
              '<details>', '<summary>Effective benchmark configuration by acceleration mode</summary>', '']
    for mode in data.get('modes', []):
        lines += [f'**{MODE_NAMES[mode["mode"]]}**', '', '```json', json.dumps(mode['config'], indent=2), '```', '']
    lines += ['</details>', '', 'Generated locally by `BENCHMARK-REPORT.bat`. No upload or Git publication is performed.', '']
    return '\n'.join(lines)


def compare_reports(data, previous):
    assets_key = lambda d: [(a['role'], a['sha256']) for a in d['metadata'].get('assets', []) if a['role'] in ('target', 'mtp')]
    old_summaries = {s['id']: s for s in previous.get('summaries', [])}
    old_cases = {c['id']: c for c in previous.get('cases', [])}
    old_modes = {m['mode']: m['config'] for m in previous.get('modes', [])}
    new_modes = {m['mode']: m['config'] for m in data.get('modes', [])}
    output = []
    for summary in data['summaries']:
        case = next(c for c in data['cases'] if c['id'] == summary['id'])
        old = old_summaries.get(summary['id'])
        reason = None
        if old is None:
            reason = 'No matching condition'
        elif not assets_key(data) or assets_key(data) != assets_key(previous):
            reason = 'Model/sidecar fingerprints differ or unavailable'
        elif data['metadata']['preset'] != previous['metadata'].get('preset'):
            reason = 'Sampling preset differs'
        elif case.get('prompt_sha256') is None or case.get('prompt_sha256') != old_cases.get(case['id'], {}).get('prompt_sha256'):
            reason = 'Input differs or continuation is not a fixed fixture'
        elif case.get('max_tokens') != old_cases[case['id']].get('max_tokens'):
            reason = 'Output cap differs'
        elif new_modes.get(case['mode']) != old_modes.get(case['mode']):
            reason = 'Effective mode configuration differs'
        elif summary['status'] != 'complete' or old.get('status') != 'complete':
            reason = 'Incomplete measurements'
        row = {'id': summary['id'], 'compatibility': reason or 'Matching conditions',
               'previous': old.get('decode_tps') if old else None, 'current': summary['decode_tps'], 'delta_percent': None}
        if reason is None and row['previous'] and row['current']:
            row['delta_percent'] = 100 * (row['current'] / row['previous'] - 1)
        output.append(row)
    return output


class Suite:
    def __init__(self, args, config, runtime, fixtures):
        self.args, self.source_config, self.runtime, self.fixtures = args, config, runtime, fixtures
        self.budget = Budget(args.budget_minutes)
        stamp = datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%SZ')
        self.out = args.output / stamp
        self.out.mkdir(parents=True, exist_ok=False)
        self.private = ROOT / '.cache' / 'benchmark-report' / stamp
        self.private.mkdir(parents=True, exist_ok=True)
        self.exe = ROOT / 'dist/engine/FlashNextVelocity.Engine.exe'
        self.sampling = sampling_for(config, args.preset)
        self.corpus, sources = context_corpus(ROOT, fixtures)
        self.data = {'schema_version': 1, 'status': 'planned' if args.plan else 'running',
                     'started_utc': datetime.now(timezone.utc).isoformat(), 'errors': [], 'runs': [], 'modes': [],
                     'cases': build_cases(fixtures, args.repetitions),
                     'metadata': {'preset': args.preset, 'budget_minutes': args.budget_minutes,
                                  'repetitions': args.repetitions, 'seeds': list(SEEDS[:args.repetitions]),
                                  'context_capacity': max(config.get('context', 32768), 67584),
                                  'sampling': self.sampling, 'fixture_file_sha256': sha_text(FIXTURE_PATH.read_text(encoding='utf-8')),
                                  'code_edit_check_revision': fixtures.get('code_edit_check_revision', 1),
                                  'runner_sha256': sha_text(Path(__file__).read_text(encoding='utf-8')),
                                  'source_config': public_config(config), 'git': git_info(ROOT),
                                  'context_sources': sources}}
        # The continuation condition includes a matched fresh control per repetition.
        self.data['planned_measured_requests'] = sum(c['repetitions'] for c in self.data['cases']) + args.repetitions
        self.inputs = {'suite_version': 1, 'system': fixtures['system'], 'cases': {},
                       'source_files': sources, 'conversation_requests': []}
        for case in self.data['cases']:
            if 'prompt' in case:
                case['prompt_sha256'] = sha_text(case['prompt'])
                self.inputs['cases'][case['id']] = {'prompt': case['prompt'], 'sha256': case['prompt_sha256']}

    def base_body(self, prompt, cap, seed):
        return {'model': 'local', 'agent_prompt': '', 'messages': [
            {'role': 'system', 'content': self.fixtures['system']}, {'role': 'user', 'content': prompt}],
            **self.sampling, 'max_tokens': cap, 'seed': seed, 'stream': True,
            'stream_options': {'include_usage': True}, 'flashnext_profile': False,
            'chat_template_kwargs': {'enable_thinking': False, 'preserve_thinking': False, 'reasoning_effort': 'off'}}

    def execute(self, engine, case, repetition, purpose='measured', body=None, cache=None, completion=False):
        self.budget.remaining()
        record = {'case_id': case['id'], 'mode': case['mode'], 'repetition': repetition,
                  'purpose': purpose, 'cache': cache or case['cache'], 'status': 'error'}
        body = body or self.base_body(case['prompt'], case['max_tokens'], SEEDS[(repetition - 1) % len(SEEDS)])
        record['request_sha256'] = sha_text(json.dumps(body, sort_keys=True))
        if purpose == 'measured':
            print(f'  {case["id"]:<27} {record["cache"]:<16} {repetition}/{case["repetitions"]}', end=' ', flush=True)
        start = time.monotonic()
        try:
            if completion:
                response = request_json(engine.base + '/v1/completions', body,
                    timeout=min(self.args.request_timeout, self.budget.remaining()))
                result = {'text': response['choices'][0]['text'], 'usage': response['usage'],
                          'finish_reason': response['choices'][0]['finish_reason'],
                          'client_total_ms': (time.monotonic() - start) * 1000, 'tool_calls': []}
            else:
                result = collect_stream(engine.base + '/v1/chat/completions', body, self.budget, self.args.request_timeout)
            usage = result.pop('usage')
            metrics = public_metrics(usage['flashnext_velocity'])
            if not math.isfinite(metrics['completion_tokens_per_second']) or metrics['completion_tokens_per_second'] <= 0:
                raise RuntimeError('Engine returned an invalid decode rate.')
            effective = metrics.get('effective_request_settings', {})
            for key, expected in self.sampling.items():
                if key in effective and not math.isclose(float(effective[key]), float(expected), rel_tol=1e-5, abs_tol=1e-6):
                    raise RuntimeError(f'Effective sampling changed: {key}')
            if effective.get('seed', body['seed']) != body['seed']:
                raise RuntimeError('Effective seed differs from requested seed.')
            if metrics.get('context_lookup_active') != (engine.mode == 'mtp_lookup'):
                raise RuntimeError('Response lookup mode differs from the launch mode.')
            count = usage['completion_tokens']
            mtp_count, lookup_count = metrics.get('mtp_draft_tokens', 0), metrics.get('lookup_draft_tokens', 0)
            check, note = check_output(case['kind'], result, self.fixtures)
            record.update(result, status='ok', check=check, check_note=note,
                prompt_tokens=usage['prompt_tokens'], completion_tokens=count,
                decode_tps=metrics['completion_tokens_per_second'], decode_ms=metrics['decode_ms'],
                engine_ttft_ms=metrics.get('ttft_ms'), prefill_ms=metrics.get('prefill_ms'),
                prefill_tps=metrics.get('prefill_tokens_per_second') if record['cache'] == 'fresh' else None,
                mtp_acceptance=metrics.get('mtp_acceptance') if mtp_count else None,
                mtp_per_output=metrics.get('mtp_draft_tokens_accepted', 0) / count if count else None,
                lookup_per_output=metrics.get('lookup_draft_tokens_accepted', 0) / count if count else None,
                output_sha256=sha_text(result['text']), metrics=metrics)
            if purpose == 'measured':
                print(f'{record["decode_tps"]:6.1f} tps | {count:4} tokens | {check}', flush=True)
        except Exception as error:
            record['error'] = str(error)
            if purpose == 'measured':
                print('ERROR: ' + str(error), flush=True)
            if isinstance(error, BudgetExpired):
                raise
            if purpose != 'measured':
                raise
        finally:
            self.data['runs'].append(record)
            self.save()
        if record['status'] == 'ok':
            self.sample_memory(engine)
        return record

    def sample_memory(self, engine):
        try:
            health = request_json(engine.base + '/health', timeout=min(3, self.budget.remaining()))
        except BudgetExpired:
            raise
        except Exception as error:
            note = f'Memory boundary sampling unavailable: {error}'
            if note not in self.data['errors']:
                self.data['errors'].append(note)
            return
        mode = next(m for m in self.data['modes'] if m['mode'] == engine.mode)
        mode['min_available_bytes'] = min(mode['min_available_bytes'], health.get('memory_available_bytes', 0))
        device = health.get('device_memory', {})
        mode['max_local_bytes'] = max(mode['max_local_bytes'], device.get('local_usage_bytes', 0))
        mode['max_shared_bytes'] = max(mode['max_shared_bytes'], device.get('shared_usage_bytes', 0))

    def calibrate_context(self, engine, case):
        target, chars = case['target_context'], min(len(self.corpus), int(case['target_context'] * 3.7))
        for attempt in range(3):
            prompt = 'REFERENCE MATERIAL\n' + self.corpus[:chars] + '\nEND REFERENCE\n' + self.fixtures['context_question']
            probe = {**case, 'prompt': prompt, 'max_tokens': 1}
            result = self.execute(engine, probe, attempt + 1, 'setup')
            count = result['prompt_tokens']
            if abs(count - target) <= target * 0.02:
                break
            new_chars = min(len(self.corpus), max(100, round(chars * (target - 100) / max(1, count - 100))))
            if new_chars == chars:
                break
            chars = new_chars
        case['prompt'], case['prompt_sha256'], case['calibrated_prompt_tokens'] = prompt, sha_text(prompt), count
        self.inputs['cases'][case['id']] = {'prompt': prompt, 'sha256': case['prompt_sha256'],
                                          'target_tokens': target, 'actual_calibration_tokens': count}
        print(f'  Context calibration: target {target:,}, actual {count:,} tokens', flush=True)
        warmup = {**case, 'max_tokens': 64}
        self.execute(engine, warmup, 1, 'warmup')

    def continuation(self, engine, case, repetition):
        # Raw generated text is the only available API representation of the
        # session prefix. EOS and BPE boundary changes remain explicit limitations.
        prefix = ('Reference conversation about a local inference runtime:\n' + self.corpus[:16000] +
                  '\nContinue with a technical explanation of the runtime: ')
        prime_body = {'model': 'local', 'prompt': prefix, **self.sampling,
                      'max_tokens': 32, 'seed': SEEDS[repetition - 1], 'flashnext_profile': False}
        primed = self.execute(engine, case, repetition, 'setup', prime_body, 'fresh', True)
        prompt = prefix + primed['text']
        body = {**prime_body, 'prompt': prompt, 'max_tokens': 256}
        self.inputs['conversation_requests'].append({'repetition': repetition, 'prime': prime_body, 'measured': body})
        warm = self.execute(engine, case, repetition, body=body, cache='continuation-attempt', completion=True)
        warm['continuation_note'] = 'EOS/BPE can prevent prefix reuse' if primed.get('finish_reason') != 'length' else 'Raw prefix extended; cached-token count unavailable'
        # After generation, replaying this shorter prompt diverges from the
        # session and resets it, giving a matched fresh-input control.
        self.execute(engine, case, repetition, body=body, cache='paired-fresh', completion=True)

    def save(self):
        self.data['elapsed_seconds'] = getattr(self, 'preserved_elapsed_seconds',
                                               time.monotonic() - self.budget.started)
        self.data['summaries'] = [summarize_case(c, self.data['runs']) for c in self.data['cases']]
        write_json(self.out / 'raw-results.json', self.data)
        write_json(self.out / 'inputs.json', self.inputs)

    def publish(self):
        self.save()
        if self.args.compare:
            self.data['comparison'] = compare_reports(self.data, json.loads(self.args.compare.read_text(encoding='utf-8-sig')))
            self.save()
        summaries = self.data['summaries']
        core = [s for s in summaries if s['kind'] in ('prose', 'code_generation', 'code_edit')]
        make_chart(self.out / 'workloads.svg', 'Decode throughput by workload and acceleration',
                   [s['kind'].replace('_', ' ') + ' / ' + MODE_NAMES[s['mode']] for s in core],
                   [s['decode_tps'] for s in core], [{'off': '#64748b', 'mtp': '#2563eb', 'mtp_lookup': '#059669'}[s['mode']] for s in core])
        context = [s for s in summaries if s['kind'] == 'context']
        make_chart(self.out / 'context.svg', 'Decode throughput at actual occupied context',
                   [f'{s["prompt_tokens"][0]:,} input tokens' if s['prompt_tokens'] else s['id'] for s in context],
                   [s['decode_tps'] for s in context])
        fields = ['case_id', 'mode', 'repetition', 'purpose', 'cache', 'status', 'prompt_tokens', 'completion_tokens',
                  'decode_tps', 'client_ttft_ms', 'engine_ttft_ms', 'prefill_ms', 'client_total_ms',
                  'mtp_acceptance', 'mtp_per_output', 'lookup_per_output', 'finish_reason', 'check', 'check_note', 'error']
        with (self.out / 'requests.csv').open('w', encoding='utf-8', newline='') as stream:
            writer = csv.DictWriter(stream, fields, extrasaction='ignore')
            writer.writeheader()
            writer.writerows(self.data['runs'])
        (self.out / 'README.md').write_text(markdown_report(self.data), encoding='utf-8')
        prefix = os.path.relpath(self.out, ROOT).replace('\\', '/') + '/'
        (ROOT / 'BENCHMARK-REPORT.md').write_text(markdown_report(self.data, prefix), encoding='utf-8')
        print('\n' + '=' * 86 + '\nBENCHMARK SUMMARY\n' + '=' * 86, flush=True)
        print(f'{"Condition":29} {"Median tps":>12} {"Range":>17} {"Runs":>7} {"Checks":>8}')
        for summary in summaries:
            spread = f'{fmt(summary["min_tps"])} - {fmt(summary["max_tps"])}' if summary['completed'] else 'not measured'
            print(f'{summary["id"]:29} {fmt(summary["decode_tps"]):>12} {spread:>17} '
                  f'{summary["completed"]:>3}/{summary["planned"]:<3} {summary["check_passes"]:>3}/{summary["completed"]:<3}')
        print(f'\nStatus: {self.data["status"]}; elapsed {self.data["elapsed_seconds"] / 60:.1f} min')
        print(f'GitHub report: {ROOT / "BENCHMARK-REPORT.md"}\nRun folder: {self.out}', flush=True)

    def run_mode(self, mode):
        config = copy.deepcopy(self.source_config)
        config.update(host='127.0.0.1', port=self.args.port,
            context=self.data['metadata']['context_capacity'], sessions=1,
            mmproj='', agent_prompt='', thinking=False, preserve_thinking=False,
            context_lookup=mode == 'mtp_lookup', sampling={**self.sampling, 'seed': 12345})
        if mode == 'off':
            config['mtp'] = ''
        print(f'\nLoading {MODE_NAMES[mode]}...', flush=True)
        with Engine(self.exe, config, self.runtime, self.private, mode,
                    self.budget, self.args.startup_timeout) as engine:
            health = engine.health
            device = health.get('device_memory', {})
            self.data['modes'].append({'mode': mode, 'config': public_config(config),
                'runtime_modules': runtime_modules(engine.process.pid),
                'health': clean_health(health), 'min_available_bytes': health.get('memory_available_bytes', 0),
                'max_local_bytes': device.get('local_usage_bytes', 0), 'max_shared_bytes': device.get('shared_usage_bytes', 0)})
            self.save()
            print(f'  Loaded in {health["load_seconds"]:.1f}s; context {health["context"]:,}; proposal {health["mtp_proposal_mode"]}', flush=True)
            warm_case = {'id': f'warmup-{mode}', 'kind': 'prose', 'mode': mode, 'cache': 'fresh',
                         'prompt': self.fixtures['prose'], 'max_tokens': 64, 'repetitions': 1}
            self.execute(engine, warm_case, 1, 'warmup')
            for case in [c for c in self.data['cases'] if c['mode'] == mode]:
                try:
                    if case['kind'] == 'context':
                        self.calibrate_context(engine, case)
                    for repetition in range(1, case['repetitions'] + 1):
                        if case['kind'] == 'conversation':
                            self.continuation(engine, case, repetition)
                            continue
                        body = self.base_body(case['prompt'], case['max_tokens'], SEEDS[repetition - 1])
                        if case['kind'] == 'tool_call':
                            body['tools'] = [{'type': 'function', 'function': {
                                'name': 'benchmark_lookup', 'description': 'Simulated benchmark lookup. Never executed.',
                                'parameters': {'type': 'object', 'properties': {
                                    'record_id': {'type': 'integer'}, 'category': {'type': 'string'}},
                                    'required': ['record_id', 'category']}}}]
                            body['tool_choice'] = {'type': 'function', 'function': {'name': 'benchmark_lookup'}}
                        self.execute(engine, case, repetition, body=body)
                except BudgetExpired:
                    raise
                except Exception as error:
                    note = f'{case["id"]} could not finish: {error}'
                    self.data['errors'].append(note)
                    print('  ' + note, flush=True)
                    self.save()
                    if engine.process.poll() is not None:
                        raise RuntimeError('The owned engine exited; remaining conditions in this mode are unmeasured.') from error

    def run(self):
        print(f'FlashNextVelocity compact benchmark: 16 conditions / {self.data["planned_measured_requests"]} measured requests')
        print(f'Budget: {self.args.budget_minutes} min. Output: {self.out}', flush=True)
        if self.args.plan:
            self.publish()
            return 0
        try:
            if active_engines():
                raise RuntimeError('An engine is already running. Stop it through its owner (usually Studio), then rerun. The suite needs the GPU memory and will not terminate another process.')
            if port_open(self.args.port):
                raise RuntimeError('The benchmark port is occupied. Choose --port or stop the service using it.')
            self.data['metadata']['machine'] = machine_info()
            self.data['metadata']['assets'] = assets(self.source_config, self.exe, self.budget)
            for mode in MODES:
                self.budget.remaining()
                if active_engines():
                    raise RuntimeError('Another engine started during the suite; stopping the benchmark to avoid simultaneous model loads.')
                try:
                    self.run_mode(mode)
                except BudgetExpired:
                    raise
                except Exception as error:
                    self.budget.remaining()
                    self.data['errors'].append(f'{MODE_NAMES[mode]} could not finish: {error}')
                    self.save()
            successful = sum(r.get('status') == 'ok' for r in self.data['runs'] if r['purpose'] == 'measured')
            self.data['status'] = 'complete' if successful == self.data['planned_measured_requests'] else 'partial'
            if any(r.get('check') == 'FAIL' for r in self.data['runs'] if r['purpose'] == 'measured'):
                self.data['status'] = 'complete with check failures' if self.data['status'] == 'complete' else 'partial with check failures'
        except KeyboardInterrupt:
            self.data['status'] = 'interrupted'
            self.data['errors'].append('Stopped by the user. Completed measurements were retained.')
        except Exception as error:
            self.data['status'] = 'time limit' if isinstance(error, BudgetExpired) or time.monotonic() >= self.budget.deadline else 'partial'
            self.data['errors'].append(str(error))
            print('\n' + str(error), flush=True)
        finally:
            self.publish()
        return 0 if self.data['status'] == 'complete' else 2


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--budget-minutes', type=float, default=45, help='Total cap including hashing and loading (default: 45)')
    parser.add_argument('--repetitions', type=int, choices=(1, 2, 3), default=3)
    parser.add_argument('--preset', choices=('production', 'greedy'), default='production')
    parser.add_argument('--port', type=int, default=18080)
    parser.add_argument('--request-timeout', type=float, default=180, help='Per-request cap in seconds')
    parser.add_argument('--startup-timeout', type=float, default=180)
    parser.add_argument('--output', type=Path, default=ROOT / 'benchmark-reports')
    parser.add_argument('--compare', type=Path, help='Previous raw-results.json; incompatible rows are marked')
    parser.add_argument('--plan', action='store_true', help='Write a clearly labeled report plan without loading the model')
    parser.add_argument('--render', type=Path, help='Rebuild Markdown/charts from a saved raw-results.json without inference')
    parser.add_argument('--recheck-edits', type=Path,
                        help='Recheck saved edit responses and write a separate annotated report without inference')
    args = parser.parse_args(argv)
    if args.render and args.recheck_edits:
        parser.error('Choose --render or --recheck-edits, not both.')
    if args.budget_minutes <= 0 or args.request_timeout <= 0 or args.startup_timeout <= 0 or not 1024 <= args.port <= 65535:
        parser.error('Time limits must be positive; port must be between 1024 and 65535.')
    return args


def main(argv=None):
    args = parse_args(argv)
    try:
        if args.render or args.recheck_edits:
            suite = Suite.__new__(Suite)
            source_path = (args.render or args.recheck_edits).resolve()
            suite.args, suite.out = args, source_path.parent
            suite.data = json.loads(source_path.read_text(encoding='utf-8-sig'))
            suite.inputs = json.loads((source_path.parent / 'inputs.json').read_text(encoding='utf-8-sig'))
            if args.recheck_edits:
                fixtures = json.loads(FIXTURE_PATH.read_text(encoding='utf-8'))
                suite.data = recheck_edits(suite.data, fixtures)
                stamp = datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%SZ-rechecked')
                suite.out = args.output / stamp
                suite.out.mkdir(parents=True, exist_ok=False)
                suite.data['correctness_recheck'].update(
                    source_report=os.path.relpath(source_path.parent / 'README.md', suite.out).replace('\\', '/'),
                    source_results_sha256=hashlib.sha256(source_path.read_bytes()).hexdigest())
            suite.budget = Budget(1)
            suite.preserved_elapsed_seconds = suite.data.get('elapsed_seconds', 0)
            suite.budget.started -= suite.data.get('elapsed_seconds', 0)
            if suite.data['status'] == 'running':
                suite.data['status'] = 'interrupted'
                suite.data['errors'].append('Recovered incremental results from an unfinished run.')
            suite.publish()
            return 0
        config = json.loads((ROOT / 'dist/config.json').read_text(encoding='utf-8-sig'))
        runtime = json.loads((ROOT / 'dist/runtime.json').read_text(encoding='utf-8-sig'))
        fixtures = json.loads(FIXTURE_PATH.read_text(encoding='utf-8'))
        if not args.plan and (not config.get('mtp') or not (ROOT / 'dist/engine/FlashNextVelocity.Engine.exe').is_file()):
            raise RuntimeError('Build the native engine and configure the target and MTP sidecar in Studio first.')
        return Suite(args, config, runtime, fixtures).run()
    except Exception as error:
        print(f'Benchmark could not start: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
