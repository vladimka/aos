#!/usr/bin/env python3
import json, os, glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CFLAGS = [
    "-ffreestanding", "-Wall", "-Wextra", "-O2", "-std=c11", "-nostdlib",
    "-fno-builtin", "-fno-stack-protector", "-fno-pie", "-fno-pic",
    "-m32", "-mno-sse", "-mno-mmx", "-mno-80387",
    "-Ikernel", "-Idrivers", "-Iarch/i386", "-Iboot", "-Iprograms",
]

sources = []
for pattern in ("kernel/*.c", "drivers/*.c", "arch/i386/*.c", "boot/*.c", "programs/*.c"):
    sources += sorted(glob.glob(os.path.join(ROOT, pattern)))

entries = []
for src in sources:
    rel = os.path.relpath(src, ROOT)
    entries.append({
        "directory": ROOT,
        "file": rel,
        "arguments": ["clang", *CFLAGS, "-c", rel],
    })

with open(os.path.join(ROOT, "compile_commands.json"), "w") as fp:
    json.dump(entries, fp, indent=2)
    fp.write("\n")
