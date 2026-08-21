#!/usr/bin/env python3
import subprocess, sys, os, re, tempfile

def cname(name, prefix):
    return prefix + re.sub(r'\W', '_', name)

# The kernel image is size-capped (_end <= RAMDISK_BASE, asserted in
# linker.ld), so embed stripped copies: the raw build ELFs carry ~16 KB of
# symtab/strtab each that elf_load() never reads. The build/prog/*.elf
# artifacts themselves stay unstripped for nm-level debugging.
STRIP = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                     'tools', 'musl-i686', 'bin', 'i686-linux-musl-strip')

def read_stripped(path):
    with tempfile.NamedTemporaryFile(suffix='.elf', delete=False) as tf:
        tmp = tf.name
    try:
        subprocess.run([STRIP, '-o', tmp, path], check=True)
        with open(tmp, 'rb') as fp:
            return fp.read()
    finally:
        os.unlink(tmp)

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
    progs.append((name, read_stripped(a)))
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
