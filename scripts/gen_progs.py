#!/usr/bin/env python3
import sys, os, re

def cname(name, prefix):
    return prefix + re.sub(r'\W', '_', name)

progs = []
data = []
i = 0
args = sys.argv[1:]
while i < len(args):
    a = args[i]
    if a == '--data':
        name, path = args[i + 1].split('=', 1)
        with open(path, 'rb') as fp:
            blob = fp.read()
        data.append((name, blob))
        i += 2
        continue
    name = os.path.basename(a).replace('.elf', '')
    with open(a, 'rb') as fp:
        blob = fp.read()
    progs.append((name, blob))
    i += 1

print('#include "elf.h"')
print()

for name, blob in progs:
    sym = cname(name, 'prog_')
    print(f'static const unsigned char {sym}_elf[] = {{')
    for i in range(0, len(blob), 12):
        chunk = blob[i:i+12]
        print('    ' + ', '.join(f'0x{b:02x}' for b in chunk) + ',')
    print('};')
    print()

print('const struct embedded_prog {')
print('    const char *name;')
print('    const unsigned char *data;')
print('    unsigned int size;')
print('} embedded_progs[] = {')
for name, blob in progs:
    sym = cname(name, 'prog_')
    print(f'    {{ "bin/{name}", {sym}_elf, sizeof({sym}_elf) }},')
print('    { 0, 0, 0 }')
print('};')
print()

for name, blob in data:
    sym = cname(name, 'data_')
    print(f'static const unsigned char {sym}[] = {{')
    for i in range(0, len(blob), 12):
        chunk = blob[i:i+12]
        print('    ' + ', '.join(f'0x{b:02x}' for b in chunk) + ',')
    print('};')
    print()

print('const struct embedded_file {')
print('    const char *name;')
print('    const unsigned char *data;')
print('    unsigned int size;')
print('} embedded_data[] = {')
for name, blob in data:
    sym = cname(name, 'data_')
    print(f'    {{ "{name}", {sym}, sizeof({sym}) }},')
print('    { 0, 0, 0 }')
print('};')
