#!/usr/bin/env python3
"""Lists what a recompiled function calls, with guest addresses, to read the game's control flow from the generated C++.
   tools/gen_calls.py sub_8239C4B8 [--full]"""
import glob
import re
import sys

name = sys.argv[1]
full = '--full' in sys.argv
call = re.compile(r'^\s*(__imp__\w+|sub_[0-9A-F]+|__\w+_\d+|REX_CALL_INDIRECT_FUNC)\(')
for path in glob.glob('out/recomp/generated/default/bf2mc_recomp.*.cpp'):
    with open(path, encoding='utf-8') as f:
        lines = f.read().split('\n')
    start = next((i for i, l in enumerate(lines) if l.startswith(f'DEFINE_REX_FUNC({name})')), None)
    if start is None:
        continue
    end = next(i for i in range(start + 1, len(lines)) if lines[i].startswith('DEFINE_REX_FUNC('))
    print(f'{path}:{start + 1}, {end - start} lines')
    if full:
        print('\n'.join(lines[start:end]))
        break
    for i in range(start, end):
        m = call.match(lines[i])
        if m:
            print(f'  {i - start:6} {lines[i - 1].strip():28} {lines[i].strip()}')
    break
else:
    print('not found')
