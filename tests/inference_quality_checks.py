"""Small fixed coding/protocol checks; no client tools or generated commands."""
import ast
import json
from pathlib import Path
import re
import sys
import urllib.error
import urllib.request

BASE, OUTPUT = sys.argv[1], Path(sys.argv[2])
OUTPUT.mkdir(parents=True,exist_ok=True)

def post(messages, **extra):
    body=dict(model='local',messages=messages,max_tokens=768,temperature=.7,top_p=.8,top_k=20,
              min_p=0,repeat_penalty=1,presence_penalty=1.5,frequency_penalty=0,seed=12345,
              chat_template_kwargs={'enable_thinking':False},stream=False)
    body.update(extra)
    with urllib.request.urlopen(urllib.request.Request(BASE+'/v1/chat/completions',json.dumps(body).encode(),
            {'Content-Type':'application/json'}),timeout=120) as r: return json.load(r)

def function_test(name,prompt,cases):
    result=post([{'role':'system','content':'Return only complete Python code. No imports, tools or filesystem are available.'},
                 {'role':'user','content':prompt}])
    (OUTPUT/(name+'.json')).write_text(json.dumps(result,indent=2))
    assert result['choices'][0]['finish_reason']=='stop'
    content=result['choices'][0]['message']['content']
    fence=re.search(r'```(?:python)?\s*\n(.*?)```',content,re.S)
    code=fence.group(1) if fence else content
    tree=ast.parse(code)
    assert all(isinstance(n,ast.FunctionDef) or isinstance(n,ast.Expr) and isinstance(n.value,ast.Constant)
               for n in tree.body), 'Unexpected top-level generated code'
    assert not any(isinstance(n,(ast.Import,ast.ImportFrom)) for n in ast.walk(tree)), 'Unexpected imports'
    assert not any(isinstance(n,ast.Attribute) and n.attr.startswith('__') for n in ast.walk(tree))
    assert not any(isinstance(n,ast.Name) and n.id in {'open','eval','exec','compile','__import__'} for n in ast.walk(tree))
    namespace={'__builtins__':{'len':len,'range':range,'ValueError':ValueError,'isinstance':isinstance,'int':int,'set':set}}
    exec(compile(tree,'<quality-fixture>','exec'),namespace)
    for arguments, expected in cases: assert namespace[name](*arguments)==expected,(arguments,expected)
    print('PASS coding:',name,flush=True)

function_test('dedupe', 'Implement dedupe(items) returning a new list with duplicates removed while preserving order. Do not mutate input. Return complete Python code.',
              [(([],),[]),(([3,1,3,2,1],),[3,1,2]),(([None,None,1],),[None,1])])
function_test('binary_search', 'Correct this function to return the index of target or -1, supporting empty inputs and the first/last elements of a sorted list. Return the complete corrected Python function only:\n'
              'def binary_search(items, target):\n    lo, hi = 0, len(items)\n    while lo < hi:\n        mid = (lo+hi)//2\n        if items[mid] == target: return mid\n        if items[mid] < target: lo = mid\n        else: hi = mid\n    return -1',
              [(([],1),-1),(([1],1),0),(([1,3,5],1),0),(([1,3,5],5),2),(([1,3,5],2),-1)])

history=[{'role':'user','content':'What is 2+2?'},
         {'role':'assistant','content':'4','reasoning_content':'Adding two and two gives four.'},
         {'role':'user','content':'What was your previous answer? Reply briefly.'}]
result=post(history,temperature=1,top_p=.95,presence_penalty=0,
            chat_template_kwargs={'enable_thinking':True,'preserve_thinking':True,'reasoning_effort':'medium'})
assert '4' in result['choices'][0]['message']['content']
(OUTPUT/'reasoning-history.json').write_text(json.dumps(result,indent=2))
print('PASS: reasoning-history continuation',flush=True)
try:
    post([{'role':'user','content':'word '*40000}],max_tokens=16)
    raise AssertionError('Oversized context was accepted')
except urllib.error.HTTPError as error:
    assert error.code in (400,413),error.code
print('PASS: context-limit rejection',flush=True)
(OUTPUT/'quality-results.json').write_text('{"status":"PASS","coding_cases":2,"reasoning_history":true,"context_limit":true}')
