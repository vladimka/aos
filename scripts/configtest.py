#!/usr/bin/env python3
"""Config / boot-logo / gradient-wallpaper regression for AOS.

Boot A (no disk, default ramdisk):
  1. serial log has the ASCII AOS logo and `config: created sys/config.cfg`,
  2. the WM top panel (0x232C40) covers (700,0) and the desktop gradient one
     row below it is still the default wp_top 0x1A2030,
  3. no `sys/` icon is shown on the desktop (grid slot 1 is empty),
  4. a term spawned from the dock renders `cat sys/config.cfg`.

Boot B (disk image with a host-built SFS containing timezone=+180):
  5. serial log has `config: loaded sys/config.cfg` and `config: timezone +180`.
"""
import os
import struct
import subprocess
import sys
import time

from qtest import QTest, count_bright

IMG = "/tmp/aos-config-disk.img"
# The WM draws a top panel (PANEL_H=26, col_dock_bg 0x232C40) over the
# gradient's top rows, so the wallpaper assertions move one row below it.
PANEL_BG = (35, 44, 64)                 # 0x232C40
DESKTOP_TOP = (25, 31, 47)              # gradient at y=26, default wp_top
WALL_TOP = (15, 31, 47)                 # gradient at y=26, wp_top=0x102030
LOGO_LINE = "AAA    OOO    SSS"
CFG_CREATED = "config: created sys/config.cfg"
CFG_LOADED = "config: loaded sys/config.cfg"
CFG_TZ = "config: timezone +180"

TXT_X0, TXT_X1 = 21, 660
TXT_Y0, TXT_Y1 = 39, 39 + 26 * 16
TXT_THRESHOLD = 300

ACCENT = (255, 0, 255)


def build_sfs(entries, block_count=8192):
    """Build an SFS2 disk image matching kernel/sfs2.{h,c} layout.

    Inode records are 64 B (type/uid/gid/pad, nlink, mode, size, mtime,
    direct[8], indirect) since the multi-user commit; SFS2_DIRENT 32 B.
    """
    BS = 512
    INODES = 256
    INO_SIZE = 64
    DIRENT = 32
    DIRENTS_PER_BLOCK = BS // DIRENT
    inode_blocks = (INODES * INO_SIZE + BS - 1) // BS
    bitmap_blocks = (block_count - (1 + inode_blocks)) // (BS * 8) + 1
    inode_start = 1
    bitmap_start = inode_start + inode_blocks
    data_start = bitmap_start + bitmap_blocks
    out = bytearray(block_count * BS)
    struct.pack_into("<4sIIIIIII", out, 0, b"SFS2", 1, block_count, INODES,
                     1, bitmap_start, data_start, 1)

    used = {0}
    used.update(range(1, inode_blocks + 1))
    used.update(range(bitmap_start, bitmap_start + bitmap_blocks))
    next_block = data_start
    next_ino = 2

    def alloc_block():
        nonlocal next_block
        b = next_block
        next_block += 1
        used.add(b)
        return b

    def put_inode(ino, itype, size, first_block, mode):
        o = inode_start * BS + ino * INO_SIZE
        struct.pack_into("<BBBxHHII", out, o, itype, 0, 0, 1, mode, size, 0)
        struct.pack_into("<I", out, o + 16, first_block)

    def add_dirent(dir_ino, child_ino, name):
        o = inode_start * BS + dir_ino * INO_SIZE
        size = struct.unpack_from("<I", out, o + 8)[0]
        idx = size // DIRENT
        blk_i = idx // DIRENTS_PER_BLOCK
        blk = struct.unpack_from("<I", out, o + 16 + 4 * blk_i)[0]
        if blk == 0:
            blk = alloc_block()
            struct.pack_into("<I", out, o + 16 + 4 * blk_i, blk)
        off = blk * BS + (idx % DIRENTS_PER_BLOCK) * DIRENT
        struct.pack_into("<I", out, off, child_ino)
        struct.pack_into("<28s", out, off + 4, name.encode())
        struct.pack_into("<I", out, o + 8, size + DIRENT)

    dirs = {"": 1}
    put_inode(1, 2, 0, alloc_block(), 0o755)

    def ensure_dir(path):
        if path in dirs:
            return dirs[path]
        parent = ensure_dir(path.rsplit("/", 1)[0]) if "/" in path else 1
        name = path.rsplit("/", 1)[1] if "/" in path else path
        nonlocal next_ino
        ino = next_ino
        next_ino += 1
        put_inode(ino, 2, 0, alloc_block(), 0o755)
        add_dirent(parent, ino, name)
        dirs[path] = ino
        return ino

    for path, content in entries:
        name = path.rsplit("/", 1)[1]
        parent = ensure_dir(path.rsplit("/", 1)[0]) if "/" in path else 1
        ino = next_ino
        next_ino += 1
        put_inode(ino, 1, len(content), alloc_block(), 0o644)
        blk = struct.unpack_from("<I", out, inode_start * BS + ino * INO_SIZE + 16)[0]
        out[blk * BS:blk * BS + len(content)] = content
        add_dirent(parent, ino, name)

    bm_base = bitmap_start * BS
    for b in range(0, block_count, 8):
        out[bm_base + b // 8] = 0xFF
    for b in used:
        out[bm_base + b // 8] &= ~(1 << (b & 7))
    return out


GPU_ARGS = ["-vga", "none", "-device", "virtio-vga,disable-modern=on"]


def disk_extra():
    return GPU_ARGS + [
        "-drive", "file=" + IMG + ",format=raw,if=none,id=d0",
        "-device", "virtio-blk-pci,disable-modern=on,drive=d0",
    ]


def main():
    # ---- Boot A: default ramdisk ----
    with QTest("config", extra_args=GPU_ARGS, boot_wait=6) as q:
        q.boot_and_ready()
        log = q.serial_read()
        if LOGO_LINE not in log:
            raise AssertionError("boot logo missing from serial log; tail:\n"
                                 + log[-300:])
        if not q.serial_wait(CFG_CREATED):
            raise AssertionError("config file was not created on first boot")
        print("  ok: boot logo + config: created")

        q.screenshot("/tmp/aos-config-before.ppm")
        if count_bright("/tmp/aos-config-before.ppm", 68, 34, 99, 65) != 0:
            raise AssertionError("sys/config.cfg shown as a desktop icon")
        q.assert_pixel(700, 0, PANEL_BG, "top panel == 0x232C40")
        q.assert_pixel(700, 26, DESKTOP_TOP, "gradient below panel == wp_top")
        print("  ok: no sys/ icon; top panel + gradient below")

        q.dock_spawn_term()
        before = q.screenshot("/tmp/aos-config-before.ppm")
        before_txt = count_bright(before, TXT_X0, TXT_Y0, TXT_X1, TXT_Y1)
        q.type_text("cat sys/config.cfg\n")
        # TCG is slow under the virtio-gpu scanout path; poll until the term
        # text band grows past the threshold instead of a fixed sleep.
        ppm = None
        after_txt = before_txt
        for _ in range(40):
            time.sleep(0.25)
            ppm = q.screenshot("/tmp/aos-config.ppm")
            after_txt = count_bright(ppm, TXT_X0, TXT_Y0, TXT_X1, TXT_Y1)
            if after_txt - before_txt > TXT_THRESHOLD:
                break
        if after_txt - before_txt <= TXT_THRESHOLD:
            raise AssertionError(
                "cat sys/config.cfg did not render (band grew %d)"
                % (after_txt - before_txt))
        print("  ok: cat sys/config.cfg rendered")

    # ---- Boot B: disk-seeded timezone +180 ----
    with open(IMG, "wb") as f:
        f.write(build_sfs([("sys/config.cfg",
                            b"timezone=+180\nwallpaper_top=0x102030\n"
                            b"theme_accent=0xFF00FF\n")]))
    subprocess.run(["truncate", "-s", "4M", IMG], check=True)
    with QTest("config", extra_args=disk_extra(), boot_wait=6) as q:
        q.boot_and_ready()
        if not q.serial_wait(CFG_LOADED):
            raise AssertionError("config: loaded banner missing; log:\n"
                                 + q.serial_read()[-300:])
        if not q.serial_wait(CFG_TZ):
            raise AssertionError("config: timezone +180 not applied; log:\n"
                                 + q.serial_read()[-300:])
        if "KERNEL PANIC" in q.serial_read():
            raise AssertionError("kernel panic during config boot")
        print("  ok: disk-seeded timezone +180 applied")
        time.sleep(2)
        bb = q.screenshot("/tmp/aos-config-bootb.ppm")
        q.assert_pixel(700, 0, PANEL_BG, "panel unaffected by wallpaper_top", path=bb)
        q.assert_pixel(700, 26, WALL_TOP, "gradient below panel == disk wp_top", path=bb)
        q.assert_pixel(480, 708, ACCENT, "dock accent line == theme_accent", path=bb)
        print("  ok: disk-seeded wallpaper_top + accent applied")

    print("PASS: boot logo, config, gradient wallpaper")
    return 0


if __name__ == "__main__":
    sys.exit(main())
