#!/usr/bin/env python3
import sys, os

progs = []
for f in sys.argv[1:]:
    name = os.path.basename(f).replace('.elf', '')
    with open(f, 'rb') as fp:
        data = fp.read()
    progs.append((name, data))

print('#include "elf.h"')
print()

for name, data in progs:
    cname = f'prog_{name}_elf'
    print(f'static const unsigned char {cname}[] = {{')
    for i in range(0, len(data), 12):
        chunk = data[i:i+12]
        print('    ' + ', '.join(f'0x{b:02x}' for b in chunk) + ',')
    print('};')
    print()

print('const struct embedded_prog {')
print('    const char *name;')
print('    const unsigned char *data;')
print('    unsigned int size;')
print('} embedded_progs[] = {')
for name, data in progs:
    print(f'    {{ "bin/{name}", prog_{name}_elf, sizeof(prog_{name}_elf) }},')
print('    { 0, 0, 0 }')
print('};')
