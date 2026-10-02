"""Real-model chat/tool lifecycle regression, against an already-running engine.

Usage: python tests/chat_integration.py [base-url]
Does not modify runtime config, execute model-suggested commands, or benchmark.
"""
import json
from pathlib import Path
import sys
import time
import urllib.request
import xml.etree.ElementTree as ET

BASE = sys.argv[1] if len(sys.argv) > 1 else 'http://127.0.0.1:8080'
OUT = Path(__file__).resolve().parents[1] / 'benchmarks/chat-integration'
SYSTEM = ('You are a helpful assistant in a standalone chat. No tools, filesystem, '
          'shell, or project workspace are available. Answer the user directly. '
          'For code and SVG requests, write the complete code in your response.')
PELICAN = ('Create a detailed night-time SVG of a mischievous pelican sneaking '
           'through a sleepy seaside town on a bicycle, wearing a tiny hoodie, '
           'with moonlight, long shadows, and an expression that says it is '
           'absolutely up to no good.')
REPORT = {}


def open_request(body):
    return urllib.request.urlopen(urllib.request.Request(
        BASE + '/v1/chat/completions', json.dumps(body).encode(),
        {'Content-Type': 'application/json'}), timeout=45)


def request(messages, **extra):
    return dict(model='local', messages=messages, max_tokens=256,
                temperature=0, seed=12345, **extra)


def collect(body, name):
    start = time.monotonic()
    text, reasoning, calls, finish, usage = '', '', {}, None, None
    with open_request(body) as response:
        headers_seconds = time.monotonic() - start
        if not body.get('stream'):
            result = json.load(response)
            (OUT / (name + '.json')).write_text(json.dumps(result, indent=2), encoding='utf-8')
            return result
        wire = []
        done = False
        for line in response:
            decoded = line.decode('utf-8'); wire.append(decoded)
            if not decoded.startswith('data:'):
                continue
            data = decoded[5:].strip()
            if data == '[DONE]':
                done = True
                break
            chunk = json.loads(data)
            assert 'error' not in chunk, chunk
            assert 'created' in chunk
            for choice in chunk.get('choices', []):
                delta = choice.get('delta', {})
                text += delta.get('content', '')
                reasoning += delta.get('reasoning_content', '')
                for call in delta.get('tool_calls', []):
                    assembled = calls.setdefault(call['index'], {'function': {'arguments': ''}})
                    for key in ('id', 'type'):
                        if key in call:
                            assembled[key] = call[key]
                    fn = call.get('function', {})
                    if 'name' in fn:
                        assembled['function']['name'] = fn['name']
                    assembled['function']['arguments'] += fn.get('arguments', '')
                if choice.get('finish_reason'):
                    assert not delta, 'finish must be a separate event'
                    finish = choice['finish_reason']
            usage = chunk.get('usage') or usage
        assert done and finish and usage
    (OUT / (name + '.sse')).write_text(''.join(wire), encoding='utf-8')
    result = dict(content=text, reasoning=reasoning, tool_calls=list(calls.values()),
                  finish_reason=finish, usage=usage, headers_seconds=headers_seconds,
                  total_seconds=time.monotonic() - start)
    (OUT / (name + '.json')).write_text(json.dumps(result, indent=2), encoding='utf-8')
    return result


def main(svg_test=True):
    OUT.mkdir(parents=True, exist_ok=True)
    for attempt in range(180):
        try:
            with urllib.request.urlopen(BASE + '/health', timeout=2) as response:
                REPORT['health'] = json.load(response)
            break
        except OSError:
            if attempt == 179:
                raise
            time.sleep(1)
    if svg_test:
        # The user's actual standalone prompt must produce a complete SVG.
        body = request([{'role': 'system', 'content': SYSTEM}, {'role': 'user', 'content': PELICAN}],
                       stream=True, stream_options={'include_usage': True})
        body['max_tokens'] = 6000
        svg = collect(body, 'pelican')
        assert svg['finish_reason'] == 'stop', svg['finish_reason']
        assert '<tool_call>' not in svg['content']
        begin, end = svg['content'].index('<svg'), svg['content'].index('</svg>') + 6
        code = svg['content'][begin:end]
        ET.fromstring(code)
        (OUT / 'pelican.svg').write_text(code, encoding='utf-8')
        REPORT['svg'] = {'status': 'PASS', 'completion_tokens': svg['usage']['completion_tokens']}
        print('PASS: actual pelican prompt returned complete, parseable SVG.', flush=True)

    # OpenCode-shaped round trip: function call -> tool result -> continuation.
    tools = [{'type': 'function', 'function': {'name': 'read_file', 'description': 'Read a file.',
              'parameters': {'type': 'object', 'properties': {'path': {'type': 'string'}}, 'required': ['path']}}}]
    messages = [{'role': 'system', 'content': 'Use read_file to read the requested file, then report its contents.'},
                {'role': 'user', 'content': 'Read the file named 123. After receiving its contents, reply with the exact file text.'}]
    body = request(messages, tools=tools, tool_choice={'type': 'function', 'function': {'name': 'read_file'}},
                   stream=True, stream_options={'include_usage': True})
    first = collect(body, 'tool-first')
    assert first['finish_reason'] == 'tool_calls' and len(first['tool_calls']) == 1
    call = first['tool_calls'][0]
    assert json.loads(call['function']['arguments'])['path'] == '123'
    messages += [{'role': 'assistant', 'content': first['content'], 'tool_calls': first['tool_calls']},
                 {'role': 'tool', 'tool_call_id': call['id'], 'content': 'The pelican has escaped on a bicycle.'}]
    body = request(messages, tools=tools, stream=True, stream_options={'include_usage': True})
    second = collect(body, 'tool-continuation')
    assert second['finish_reason'] == 'stop' and 'pelican has escaped' in second['content']
    assert second['headers_seconds'] < 2
    REPORT['tool_loop'] = {'status': 'PASS', 'continuation_seconds': second['total_seconds']}
    print('PASS: typed tool arguments and tool-result continuation.', flush=True)

    # Truncation must be distinguishable from EOS, with identical wire content.
    body = request([{'role': 'user', 'content': 'Count from 1 to 1000, one number per line.'}],
                   stream=True, stream_options={'include_usage': True})
    body['max_tokens'] = 8
    streamed = collect(body, 'limited-stream')
    body['stream'] = False
    buffered = collect(body, 'limited-json')
    assert streamed['finish_reason'] == buffered['choices'][0]['finish_reason'] == 'length'
    assert streamed['content'] == buffered['choices'][0]['message']['content']
    REPORT['limit_and_parity'] = 'PASS'
    print('PASS: length finish reason and stream/non-stream parity.', flush=True)

    thinking = collect(request([{'role': 'user', 'content': 'What is 2 + 2? Answer briefly.'}],
        stream=True, stream_options={'include_usage': True},
        chat_template_kwargs={'enable_thinking': True, 'reasoning_effort': 'low'}), 'reasoning')
    assert thinking['reasoning'] and '4' in thinking['content']
    assert '<think>' not in thinking['reasoning'] and '</think>' not in thinking['content']
    REPORT['reasoning_stream'] = 'PASS'
    print('PASS: reasoning streams separately from the answer.', flush=True)

    # Close a long-running tool-enabled stream, then immediately issue a request.
    body = request([{'role': 'user', 'content': 'Count from 1 to 10000, one number per line. Do not use tools.'}],
                   tools=tools, stream=True)
    body['max_tokens'] = 30000
    response = open_request(body)
    for line in response:
        if b'"content"' in line:
            break
    response.close()
    start = time.monotonic()
    result = collect(request([{'role': 'user', 'content': 'Say ready.'}], stream=False), 'after-cancel')
    elapsed = time.monotonic() - start
    assert result['choices'][0]['message']['content'] and elapsed < 15, elapsed
    REPORT['disconnect_cancellation'] = {'status': 'PASS', 'next_request_seconds': elapsed}
    print('PASS: disconnected generation released the session.', flush=True)
    REPORT['status'] = 'PASS'
    (OUT / ('report.json' if svg_test else 'protocol-report.json')).write_text(json.dumps(REPORT, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main(svg_test='--protocol-only' not in sys.argv)
