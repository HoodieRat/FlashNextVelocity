"""Real-model regression for complete, truncated and disconnected tool streams.

Runs against an already-running engine; does not execute tools or change config.
"""
import json
from pathlib import Path
import sys
import time
import urllib.error
import urllib.request

BASE = sys.argv[1] if len(sys.argv) > 1 else 'http://127.0.0.1:8080'
OUT = Path(__file__).resolve().parents[1] / 'benchmarks/tool-stream-regression'
TOOLS = [{'type': 'function', 'function': {
    'name': 'write', 'description': 'Write a file.', 'parameters': {
        'type': 'object', 'properties': {
            'filePath': {'type': 'string'}, 'content': {'type': 'string'}},
        'required': ['filePath', 'content']}}}]
SVG = '<svg xmlns="http://www.w3.org/2000/svg"><circle cx="10" cy="10" r="8"/></svg>'
BODY = dict(model='local', tools=TOOLS, temperature=0, seed=12345,
    tool_choice={'type': 'function', 'function': {'name': 'write'}},
    messages=[{'role': 'system', 'content': 'Call write directly, without commentary.'},
              {'role': 'user', 'content': 'Use write once with filePath C:/test/probe.svg and content exactly: ' + SVG}],
    max_tokens=512, stream=True, stream_options={'include_usage': True})


def open_request(body):
    return urllib.request.urlopen(urllib.request.Request(
        BASE + '/v1/chat/completions', json.dumps(body).encode(),
        {'Content-Type': 'application/json'}), timeout=45)


def collect(body, name):
    chunks, wire, done = [], [], False
    with open_request(body) as response:
        for line in response:
            wire.append(line.decode())
            if not line.startswith(b'data:'):
                continue
            data = line[5:].strip()
            if data == b'[DONE]':
                done = True
                break
            chunk = json.loads(data)
            chunks.append(chunk)
            # Every published call must have usable arguments on its first event.
            for choice in chunk.get('choices', []):
                for call in choice.get('delta', {}).get('tool_calls', []):
                    args = json.loads(call['function']['arguments'])
                    assert isinstance(args['filePath'], str), args
                    assert isinstance(args['content'], str), args
    (OUT / (name + '.sse')).write_text(''.join(wire), encoding='utf-8')
    assert done, 'stream ended without DONE'
    return chunks


def calls(chunks):
    return [call for chunk in chunks for choice in chunk.get('choices', [])
            for call in choice.get('delta', {}).get('tool_calls', [])]


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    complete = collect(BODY, 'complete')
    published = calls(complete)
    assert len(published) == 1 and not any('error' in c for c in complete)
    assert json.loads(published[0]['function']['arguments']) == {
        'filePath': 'C:/test/probe.svg', 'content': SVG}
    assert any(c.get('choices', [{}])[0].get('finish_reason') == 'tool_calls'
               for c in complete if c.get('choices'))
    usage = next(c['usage'] for c in complete if 'usage' in c)
    cap = max(1, usage['completion_tokens'] // 2)
    limited = {**BODY, 'max_tokens': cap}
    truncated = collect(limited, 'truncated')
    errors = [c['error'] for c in truncated if 'error' in c]
    assert not calls(truncated) and len(errors) == 1, truncated
    assert 'max_tokens' in errors[0]['message'], errors
    print('PASS: complete write published atomically; truncated write emitted an error and zero calls.', flush=True)

    try:
        with open_request({**limited, 'stream': False}) as response:
            raise AssertionError('truncated non-stream response succeeded: ' + response.read().decode())
    except urllib.error.HTTPError as error:
        buffered = json.load(error)
        assert error.code == 400 and buffered['error']['message'] == errors[0]['message'], buffered
    print('PASS: streaming and JSON reject the same incomplete tool call.', flush=True)

    # Disconnect while a large tool-enabled request is in flight.
    long_body = {**BODY, 'max_tokens': 16000, 'messages': [
        {'role': 'system', 'content': 'Call write directly, without commentary.'},
        {'role': 'user', 'content': 'Write C:/test/large.txt with exactly 2000 lines, one integer per line from 1 through 2000. Use write.'}]}
    response = open_request(long_body)
    start = time.monotonic()
    seen_calls = 0
    for line in response:
        if line.startswith(b'data:') and line[5:].strip() != b'[DONE]':
            seen_calls += len(calls([json.loads(line[5:])]))
        if time.monotonic() - start >= 4:
            break
    response.close()
    assert seen_calls == 0, 'unfinished write was announced before disconnect'
    start = time.monotonic()
    with open_request(dict(model='local', messages=[{'role': 'user', 'content': 'Say ready.'}],
                           max_tokens=24, temperature=0, stream=False)) as response:
        ready = json.load(response)
    elapsed = time.monotonic() - start
    assert ready['choices'][0]['message']['content'] and elapsed < 15, (elapsed, ready)
    print('PASS: disconnected tool generation released the session.', flush=True)
    (OUT / 'report.json').write_text(json.dumps(dict(status='PASS',
        complete_tokens=usage['completion_tokens'], truncated_max_tokens=cap,
        truncated_error=errors[0]['message'], after_disconnect_seconds=elapsed), indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
