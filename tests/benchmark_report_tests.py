"""Focused tests of benchmark measurement/reporting; no real model is loaded."""
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import subprocess
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('benchmark_report', ROOT / 'scripts/benchmark_report.py')
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)
FIXTURES = json.loads(bench.FIXTURE_PATH.read_text())


class Stream:
    def __init__(self, chunks, done=True):
        self.lines = [b': keep-alive\n'] + [b'data: ' + json.dumps(c).encode() + b'\n' for c in chunks]
        if done:
            self.lines.append(b'data: [DONE]\n')

    def __enter__(self):
        return iter(self.lines)

    def __exit__(self, *_):
        pass


class BenchmarkTests(unittest.TestCase):
    def test_empty_powershell_pipeline_has_no_json(self):
        empty = subprocess.CompletedProcess([], 0, stdout='\ufeff\r\n', stderr='')
        with patch.object(bench.subprocess, 'run', return_value=empty):
            self.assertIsNone(bench.powershell_json('@() | ConvertTo-Json'))

    def test_case_and_request_counts(self):
        cases = bench.build_cases(FIXTURES, 3)
        self.assertEqual(len(cases), 16)
        self.assertEqual(sum(c['repetitions'] for c in cases), 46)
        self.assertEqual(sum(c['repetitions'] for c in cases) + 3, 49)

    def test_stream_ignores_role_and_usage_and_preserves_tool_fragments(self):
        chunks = [
            {'choices': [{'delta': {'role': 'assistant'}}]},
            {'choices': [{'delta': {'content': 'Hello'}}]},
            {'choices': [{'delta': {'tool_calls': [{'index': 0, 'function': {'name': 'benchmark_lookup', 'arguments': '{"record_id":'}}]}}]},
            {'choices': [{'delta': {'tool_calls': [{'index': 0, 'function': {'arguments': '17}'}}]}}]},
            {'choices': [{'delta': {}, 'finish_reason': 'tool_calls'}]},
            {'choices': [], 'usage': {'completion_tokens': 9}}]
        with patch.object(bench.urllib.request, 'urlopen', return_value=Stream(chunks)):
            result = bench.collect_stream('http://localhost/example', {}, bench.Budget(1), 10)
        self.assertEqual(result['payload_event_count'], 3)
        self.assertEqual(result['tool_calls'][0]['arguments'], '{"record_id":17}')
        self.assertEqual(result['text'], 'Hello')
        self.assertIsNotNone(result['client_ttft_ms'])

    def test_stream_error_is_not_success(self):
        with patch.object(bench.urllib.request, 'urlopen', return_value=Stream([{'error': {'message': 'truncated tool'}}])):
            with self.assertRaisesRegex(RuntimeError, 'truncated tool'):
                bench.collect_stream('http://localhost/example', {}, bench.Budget(1), 10)

    def test_stream_requires_usage_and_done(self):
        with patch.object(bench.urllib.request, 'urlopen', return_value=Stream([{'choices': []}], False)):
            with self.assertRaisesRegex(RuntimeError, 'Incomplete SSE'):
                bench.collect_stream('http://localhost/example', {}, bench.Budget(1), 10)

    def test_deadline_is_total_not_per_chunk(self):
        budget = bench.Budget(1)
        budget.deadline = 0
        with patch.object(bench.urllib.request, 'urlopen', return_value=Stream([{'choices': []}])):
            with self.assertRaises(bench.BudgetExpired):
                bench.collect_stream('http://localhost/example', {}, budget, 10)

    def test_correctness_checks_do_not_accept_just_parseable_json(self):
        self.assertEqual(bench.check_output('structured_json', {'text': '{}'}, FIXTURES)[0], 'FAIL')
        right = {'status': 'ready', 'count': 3, 'items': ['alpha', 'beta', 'gamma']}
        self.assertEqual(bench.check_output('structured_json', {'text': json.dumps(right)}, FIXTURES)[0], 'PASS')
        self.assertEqual(bench.check_output('code_edit', {'text': FIXTURES['code_edit_expected']}, FIXTURES)[0], 'PASS')
        self.assertEqual(bench.check_output('code_edit', {'text': "def total_units(items): return 3"}, FIXTURES)[0], 'FAIL')

    def test_continuation_summary_excludes_fresh_control(self):
        case = {'id': 'conversation', 'kind': 'conversation', 'mode': 'mtp_lookup', 'repetitions': 1}
        base = {'case_id': 'conversation', 'purpose': 'measured', 'status': 'ok',
                'prompt_tokens': 100, 'completion_tokens': 50, 'check': 'PASS'}
        rows = [{**base, 'cache': 'continuation-attempt', 'decode_tps': 30},
                {**base, 'cache': 'paired-fresh', 'decode_tps': 300}]
        summary = bench.summarize_case(case, rows)
        self.assertEqual(summary['completed'], 1)
        self.assertEqual(summary['decode_tps'], 30)

    def test_edit_accepts_integer_sum_with_optional_conversion_and_preserves_other_code(self):
        direct = FIXTURES['code_edit_expected']
        converted = direct.replace("sum(row['quantity'] for row in items)",
                                   "sum(int(row['quantity']) for row in items)")
        self.assertEqual(bench.check_output('code_edit', {'text': direct}, FIXTURES)[0], 'PASS')
        self.assertEqual(bench.check_output('code_edit', {'text': converted}, FIXTURES)[0], 'PASS')
        altered = direct.replace("VERSION = '1.0'", "VERSION = '2.0'")
        self.assertEqual(bench.check_output('code_edit', {'text': altered}, FIXTURES)[0], 'FAIL')
        snippet = "def total_units(items):\n    return sum(row['quantity'] for row in items)"
        self.assertEqual(bench.check_output('code_edit', {'text': snippet}, FIXTURES)[0], 'FAIL')

    def test_offline_recheck_preserves_original_measurements_and_records_check_history(self):
        row = {'case_id': 'code_edit-off', 'repetition': 1, 'purpose': 'measured', 'status': 'ok',
               'check': 'FAIL', 'check_note': 'AST differs from requested edit',
               'text': FIXTURES['code_edit_expected'], 'decode_tps': 27.3, 'completion_tokens': 74}
        data = {'status': 'partial with check failures', 'planned_measured_requests': 49,
                'elapsed_seconds': 1031.9477046999382, 'runs': [row],
                'cases': [{'id': 'code_edit-off', 'kind': 'code_edit', 'prompt': FIXTURES['code_edit']}]}
        result = bench.recheck_edits(data, FIXTURES)
        self.assertEqual(data['runs'][0]['check'], 'FAIL')
        self.assertEqual(result['runs'][0]['check'], 'PASS')
        self.assertEqual(result['runs'][0]['decode_tps'], row['decode_tps'])
        self.assertEqual(result['elapsed_seconds'], data['elapsed_seconds'])
        self.assertEqual(result['status'], 'partial')
        self.assertEqual(result['correctness_recheck']['changed'], 1)
        self.assertEqual(result['correctness_recheck']['changes'][0]['before']['check'], 'FAIL')
        data['cases'][0]['prompt'] = 'A different task requiring integer conversion.'
        with self.assertRaisesRegex(ValueError, 'prompt differs'):
            bench.recheck_edits(data, FIXTURES)

    def test_chart_and_markdown_escape_labels(self):
        self.assertIn('a\\|b', bench.table(['name'], [['a|b']]))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'chart.svg'
            bench.make_chart(path, 'A < B', ['x & y'], [10])
            import xml.etree.ElementTree as ET
            ET.parse(path)
            self.assertIn('x &amp; y', path.read_text())

    def test_matching_comparison_refuses_fixture_or_config_drift(self):
        case = {'id': 'prose-off', 'mode': 'off', 'prompt_sha256': 'p', 'max_tokens': 256}
        data = {'metadata': {'preset': 'production', 'assets': [{'role': 'target', 'sha256': 'm'}]},
                'cases': [case], 'modes': [{'mode': 'off', 'config': {'context': 67584}}],
                'summaries': [{'id': 'prose-off', 'status': 'complete', 'decode_tps': 40}]}
        previous = json.loads(json.dumps(data))
        previous['summaries'][0]['decode_tps'] = 20
        self.assertEqual(bench.compare_reports(data, previous)[0]['delta_percent'], 100)
        previous['cases'][0]['prompt_sha256'] = 'different'
        self.assertIsNone(bench.compare_reports(data, previous)[0]['delta_percent'])

    def test_public_metrics_preserve_values_and_remove_local_asset_paths(self):
        result = bench.public_metrics({'effective_settings': {'model': str(ROOT / 'private/model.gguf'), 'top_k': 20},
                                       'active_config': str(ROOT / '.cache/config.json')})
        self.assertEqual(result['effective_settings']['model'], 'model.gguf')
        self.assertEqual(result['effective_settings']['top_k'], 20)
        self.assertEqual(result['active_config'], 'config.json')

    @unittest.skipUnless(os.name == 'nt', 'Windows process ownership test')
    def test_windows_job_stops_only_the_owned_process(self):
        process = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'],
            creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            job = bench.OwnedProcessJob(process)
            job.close()
            process.wait(timeout=5)
            self.assertIsNotNone(process.returncode)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()


if __name__ == '__main__':
    unittest.main()
