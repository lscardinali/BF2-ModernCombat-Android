#!/usr/bin/env python3
"""Prints a SPIR-V module's names with their set, binding, location and builtin decorations.
   tools/spv_bindings.py <file.spv>"""
import struct
import sys

d = open(sys.argv[1], 'rb').read()
w = struct.unpack(f'<{len(d) // 4}I', d)
names, deco, storage = {}, {}, {}
i = 5
while i < len(w):
    op, n = w[i] & 0xFFFF, w[i] >> 16
    a = w[i + 1:i + n]
    if op == 5:  # OpName
        names[a[0]] = struct.pack(f'<{len(a) - 1}I', *a[1:]).split(b'\0')[0].decode()
    elif op == 71:  # OpDecorate
        deco.setdefault(a[0], []).append((a[1], a[2] if len(a) > 2 else None))
    elif op == 59:  # OpVariable
        storage[a[1]] = a[2]
    elif op == 15:  # OpEntryPoint
        print('entry', a[0], struct.pack(f'<{len(a) - 2}I', *a[2:]).split(b'\0')[0].decode())
    i += n
kinds = {33: 'binding', 34: 'set', 30: 'location', 11: 'builtin'}
classes = {0: 'UniformConstant', 1: 'Input', 2: 'Uniform', 3: 'Output', 7: 'Function', 12: 'StorageBuffer', 9: 'PushConstant', 6: 'Private'}
for v, c in storage.items():
    if c in (7, 6):
        continue
    ds = [f'{kinds[k]}={x}' for k, x in deco.get(v, []) if k in kinds]
    print(f'{classes.get(c, c):16} {names.get(v, "?"):24} {" ".join(ds)}')
