#!/usr/bin/env python3
"""Config / boot-logo / gradient-wallpaper regression for AOS.

Boot A (no disk, default ramdisk):
  1. serial log has the ASCII AOS logo and `config: created sys/config.cfg`,
  2. desktop gradient top pixel (700,0) is still (26,32,48) = 0x1A2030,
  3. no `sys/` icon is shown on the desktop (grid slot 1 is empty),
  4. a term spawned from the dock renders `cat sys/config.cfg`.

Boot B (disk image with a host-built SFS containing timezone=+180):
  5. serial log has `config: loaded sys/config.cfg` and `config: timezone +180`.
"""
import os
import socket
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
MON = "/tmp/aos-config.sock"
SER = "/tmp/aos-config.log"
BEFORE = "/tmp/aos-config-before.ppm"
PPM = "/tmp/aos-config.ppm"
MOUSE_STATE = "/tmp/aos-config.state"
MOUSE_BOOT = (511, 383)
IMG = "/tmp/aos-config-disk.img"

DESKTOP = (26, 32, 48)
LOGO_LINE = "AAA    OOO    SSS"
CFG_CREATED = "config: created sys/config.cfg"
CFG_LOADED = "config: loaded sys/config.cfg"
CFG_TZ = "config: timezone +180"

TXT_X0, TXT_X1 = 21, 660          # term text band
TXT_Y0, TXT_Y1 = 39, 39 + 26 * 16
TXT_THRESHOLD = 300


def wait_for(path, seconds=15):
    end = time.time() + seconds
    while time.time() < end:
        if os.path.exists(path):
            return
        time.sleep(0.05)
    raise RuntimeError("timeout waiting for " + path)


def hmp(command):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(3)
        s.connect(MON)
        data = b""
        while b"(qemu)" not in data:
            data += s.recv(4096)
        s.sendall(command.encode() + b"\n")
        data = b""
        while b"(qemu)" not in data:
            data += s.recv(4096)
        return data.decode(errors="replace")


def read_state():
    try:
        with open(MOUSE_STATE) as f:
            x, y = f.read().split()
            return int(x), int(y)
    except Exception:
        return MOUSE_BOOT


def write_state(x, y):
    with open(MOUSE_STATE, "w") as f:
        f.write("%d %d\n" % (x, y))


def move_abs(x, y):
    cx, cy = read_state()
    dx, dy = x - cx, y - cy
    step = 100
    while dx or dy:
        sx = max(-step, min(step, dx))
        sy = max(-step, min(step, dy))
        hmp("mouse_move %d %d" % (sx, sy))
        time.sleep(0.02)
        dx -= sx
        dy -= sy
    write_state(x, y)


def click(x, y):
    move_abs(x, y)
    time.sleep(0.3)
    hmp("mouse_button 1")
    time.sleep(0.4)
    hmp("mouse_button 0")


def sendkey(key):
    hmp("sendkey " + key)
    time.sleep(0.04)


def send_text(text):
    keys = {"\n": "ret", " ": "spc"}
    for ch in text:
        sendkey(keys.get(ch, ch))


def snap(name):
    hmp("screendump " + name)
    wait_for(name)


def ppm_data(path):
    with open(path, "rb") as f:
        if f.readline().strip() != b"P6":
            raise AssertionError("not a P6 PPM: " + path)
        dim = f.readline().split()
        f.readline()                       # maxval
        return int(dim[0]), int(dim[1]), f.read()


def pixel(path, x, y):
    w, _, data = ppm_data(path)
    off = (y * w + x) * 3
    return tuple(data[off:off + 3])


def count_bright(path, x0, y0, x1, y1):
    w, _, data = ppm_data(path)
    n = 0
    for y in range(y0, y1 + 1):
        row = data[(y * w + x0) * 3:(y * w + x1 + 1) * 3]
        for i in range(0, len(row), 3):
            if row[i] >= 0xC0 and row[i + 1] >= 0xC0 and row[i + 2] >= 0xC0:
                n += 1
    return n


def serial_text():
    try:
        with open(SER, "r", errors="replace") as f:
            return f.read()
    except FileNotFoundError:
        return ""


def wait_for_serial(text, seconds=20):
    end = time.time() + seconds
    while time.time() < end:
        if text in serial_text():
            return True
        time.sleep(0.2)
    return False


def assert_pixel(path, x, y, want, what):
    got = pixel(path, x, y)
    if got != want:
        raise AssertionError(
            "%s: pixel(%d,%d)=%s want %s (%s)" % (path, x, y, got, want, what))
    print("  ok: %s at (%d,%d)=%s" % (what, x, y, got))


def build_sfs(entries, block_count=8192):
    """Build an SFS2 disk image matching kernel/sfs2.{h,c} layout.

    Block 0: super (magic "SFS2", layout offsets). Blocks 1..: inode table
    (256 x 48 B), then the block bitmap (bit=1 free, 0 used), then data.
    ``entries`` is a list of (path, bytes); parent dirs are created as needed.
    """
    BS = 512
    INODES = 256
    INO_SIZE = 48
    DIRENT = 32
    DIRENTS_PER_BLOCK = BS // DIRENT          # 16
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

    def put_inode(ino, itype, size, first_block):
        o = bitmap_start * 0 + inode_start * BS + ino * INO_SIZE
        struct.pack_into("<BBHII", out, o, itype, 0, 1, size, 0)
        struct.pack_into("<I", out, o + 12, first_block)

    def add_dirent(dir_ino, child_ino, name):
        o = inode_start * BS + dir_ino * INO_SIZE
        size = struct.unpack_from("<I", out, o + 4)[0]
        idx = size // DIRENT
        blk_i = idx // DIRENTS_PER_BLOCK
        blk = struct.unpack_from("<I", out, o + 12 + 4 * blk_i)[0]
        if blk == 0:
            blk = alloc_block()
            struct.pack_into("<I", out, o + 12 + 4 * blk_i, blk)
        off = blk * BS + (idx % DIRENTS_PER_BLOCK) * DIRENT
        struct.pack_into("<I", out, off, child_ino)
        struct.pack_into("<28s", out, off + 4, name.encode())
        struct.pack_into("<I", out, o + 4, size + DIRENT)

    dirs = {"": 1}
    put_inode(1, 2, 0, alloc_block())        # root dir

    def ensure_dir(path):
        if path in dirs:
            return dirs[path]
        parent = ensure_dir(path.rsplit("/", 1)[0]) if "/" in path else 1
        name = path.rsplit("/", 1)[1] if "/" in path else path
        nonlocal next_ino
        ino = next_ino
        next_ino += 1
        put_inode(ino, 2, 0, alloc_block())
        add_dirent(parent, ino, name)
        dirs[path] = ino
        return ino

    for path, content in entries:
        name = path.rsplit("/", 1)[1]
        parent = ensure_dir(path.rsplit("/", 1)[0]) if "/" in path else 1
        ino = next_ino
        next_ino += 1
        put_inode(ino, 1, len(content), alloc_block())
        blk = struct.unpack_from("<I", out, inode_start * BS + ino * INO_SIZE + 12)[0]
        out[blk * BS:blk * BS + len(content)] = content
        add_dirent(parent, ino, name)

    # bitmap: set all bits free (1), clear the used ones
    bm_base = bitmap_start * BS
    for b in range(0, block_count, 8):
        out[bm_base + b // 8] = 0xFF
    for b in used:
        out[bm_base + b // 8] &= ~(1 << (b & 7))
    return out


def terminate(qemu):
    qemu.terminate()
    try:
        qemu.wait(timeout=5)
    except subprocess.TimeoutExpired:
        qemu.kill()


def boot_qemu(disk):
    cmd = [
        "qemu-system-i386", "-m", "256", "-cdrom", ISO, "-display", "none",
        "-serial", "file:" + SER, "-monitor", "unix:" + MON + ",server,nowait",
    ]
    if disk:
        cmd += [
            "-drive", "file=" + IMG + ",format=raw,if=none,id=d0",
            "-device", "virtio-blk-pci,disable-modern=on,drive=d0",
        ]
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)


def main():
    # ---- Boot A: default ramdisk ----
    for p in (MON, SER, BEFORE, PPM, MOUSE_STATE):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass
    write_state(*MOUSE_BOOT)
    qemu = boot_qemu(disk=False)
    try:
        wait_for(MON)
        time.sleep(6)
        log = serial_text()
        if "KERNEL PANIC" in log:
            raise AssertionError("kernel panic during boot")
        if LOGO_LINE not in log:
            raise AssertionError("boot logo missing from serial log; tail:\n"
                                 + log[-300:])
        if not wait_for_serial(CFG_CREATED):
            raise AssertionError("config file was not created on first boot")
        print("  ok: boot logo + config: created")

        # Grid slot 1 would hold sys/config.cfg if not hidden; slot 0 is demo.ico.
        snap(BEFORE)
        if count_bright(BEFORE, 68, 24, 99, 55) != 0:
            raise AssertionError("sys/config.cfg shown as a desktop icon")
        assert_pixel(BEFORE, 700, 0, DESKTOP, "gradient top == 0x1A2030")
        print("  ok: no sys/ icon; gradient top color")

        click(472, 724)                    # dock term launcher
        time.sleep(1)
        hmp("screendump " + BEFORE)
        wait_for(BEFORE)
        before = count_bright(BEFORE, TXT_X0, TXT_Y0, TXT_X1, TXT_Y1)
        send_text("cat sys/config.cfg\n")
        time.sleep(1)
        snap(PPM)
        after = count_bright(PPM, TXT_X0, TXT_Y0, TXT_X1, TXT_Y1)
        if after - before <= TXT_THRESHOLD:
            raise AssertionError(
                "cat sys/config.cfg did not render (band grew %d)"
                % (after - before))
        print("  ok: cat sys/config.cfg rendered")
    finally:
        terminate(qemu)

    # ---- Boot B: disk-seeded timezone +180 ----
    try:
        os.unlink(SER)
    except FileNotFoundError:
        pass
    with open(IMG, "wb") as f:
        f.write(build_sfs([("sys/config.cfg", b"timezone=+180\n")]))
    subprocess.run(["truncate", "-s", "4M", IMG], check=True)
    qemu = boot_qemu(disk=True)
    try:
        wait_for(MON)
        if not wait_for_serial(CFG_LOADED):
            raise AssertionError("config: loaded banner missing; log:\n"
                                 + serial_text()[-300:])
        if not wait_for_serial(CFG_TZ):
            raise AssertionError("config: timezone +180 not applied; log:\n"
                                 + serial_text()[-300:])
        if "KERNEL PANIC" in serial_text():
            raise AssertionError("kernel panic during config boot")
        print("  ok: disk-seeded timezone +180 applied")
    finally:
        terminate(qemu)

    print("PASS: boot logo, config, gradient wallpaper")
    return 0


if __name__ == "__main__":
    sys.exit(main())
