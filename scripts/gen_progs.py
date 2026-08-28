#!/usr/bin/env python3
import subprocess, sys, os, re, tempfile

def cname(name, prefix):
    return prefix + re.sub(r'\W', '_', name)

# newc (SVR4) cpio archive layout, byte-for-byte what Linux'
# usr/gen_init_cpio.c writes (the authoritative initramfs generator):
#   - 110-byte ASCII header, 13 %08X fields after the "070701" magic
#   - name (namesize bytes incl. trailing NUL), then padding so the file
#     data starts at a 4-byte boundary *relative to the archive start*
#   - file data, then padding so the next header is 4-aligned
#   - trailing "TRAILER!!!" entry padded to a 512-byte boundary
# S_IFREG = 0o0100000, S_IFDIR = 0o0040000
S_IFREG = 0o0100000
S_IFDIR = 0o0040000
CPIO_TRAILER = b"TRAILER!!!\0"                 # 11 bytes: name + NUL
CPIO_HDR_LEN = 110
CPIO_DATE = 0                                   # deterministic builds

def cpio_write(out_path, progs, data):
    # Directory entries first (parents of every file), bare names (a trailing
    # "/" is the CPIO convention but SFS paths use bare names).
    dirs = set()
    for name, _blob in progs:
        dirs.add("bin")
    for name, _blob in data:
        i = name.rfind("/")
        if i > 0:
            dirs.add(name[:i])
    entries = [("d", sorted(dirs))]
    files = [("bin/" + n, b) for n, b in progs]
    files += [(n, b) for n, b in data]      # data entries keep their names
    entries += [("f", files)]

    out = bytearray()
    off = 0
    ino = 721

    def push(b):
        nonlocal off, out
        out += b
        off += len(b)

    def pad_to(align):
        nonlocal off, out
        p = ((align - (off & (align - 1))) % align) if align else 0
        if p:
            out += b"\0" * p
            off += p

    def header(mode, filesize, namesize, nlink):
        nonlocal ino
        ino += 1
        h = b"070701"
        for v in (ino, mode, 0, 0, nlink, CPIO_DATE, filesize,
                  3, 1, 0, 0, namesize, 0):
            h += ("%08X" % (v & 0xFFFFFFFF)).encode("ascii")
        return h

    for kind, lst in entries:
        for item in lst:
            if kind == "d":
                name, blob = item, None
                mode, nlink = S_IFDIR | 0o755, 2
                size = 0
            else:
                name, blob = item
                mode, nlink = S_IFREG | 0o755 if name.startswith("bin/") else S_IFREG | 0o644, 1
                size = len(blob)
            nb = name.encode("utf-8") + b"\0"
            push(header(mode, size, len(nb), nlink))
            push(nb)
            pad_to(4)
            if blob:
                push(blob)
                pad_to(4)

    # Trailer, padded to the mandatory 512-byte archive boundary.
    push(header(0, 0, len(CPIO_TRAILER), 1))
    push(CPIO_TRAILER)
    pad_to(4)
    pad_to(512)

    with open(out_path, "wb") as fp:
        fp.write(bytes(out))

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
cpio_out = None
i = 0
args = sys.argv[1:]
while i < len(args):
    a = args[i]
    if a == '--cpio':
        cpio_out = args[i + 1]
        i += 2
        continue
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

# Initramfs mode: write a newc cpio archive on the ISO instead of C arrays
# baked into the kernel image (see Makefile build/initramfs.cpio).
if cpio_out:
    cpio_write(cpio_out, progs, data)
    sys.exit(0)

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
