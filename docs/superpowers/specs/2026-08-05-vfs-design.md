# Design: hierarchical VFS (SFS2) with mount table, inode cache, fd API

Date: 2026-08-05
Status: approved (design review complete)

## Problem

AOS's SFS is a flat 64-entry table of files in a 1 MiB RAM image (`SFS_BASE`
0x300000). Names like `bin/help` and `sys/config.cfg` only *look* hierarchical —
there are no real directories, no per-directory listing, no CWD, no nested
structure. The Linux ABI emulates file descriptors on top of the path-based
API (`fd_name`/`fd_off`), which cannot express directories, CWD, or real fd
semantics. There is no way to mount another filesystem.

Goal: a real Virtual File System — hierarchical on-disk format, mount table,
inode cache with reference counting, open file table + per-task fd table, and a
single fd-based syscall API shared by the AOS and Linux ABIs.

## Decisions (from design review)

- **Real hierarchical on-disk format** (`SFS2`, magic `SFS2`) replacing the
  flat SFS: superblock + inode table + block bitmap, directories are files.
  Minimal feature set plus `mtime`. No permissions, hard links, or symlinks.
- **Direct disk access** (user's choice): data blocks go through a write-back
  sector cache over a backend abstraction; metadata (superblock, bitmap, inode
  table) lives in RAM and is flushed to disk.
- **RAM mirror mode**: the same FS code runs over a RAM backend when no
  virtio-blk disk is present. Embedded programs (`kernel/progs.c`) are seeded
  either way; on first boot with a disk (no `SFS2` magic) the disk is formatted
  and seeded too.
- **CWD + relative paths**: per-task current working directory (normalized path
  string), inherited by children. Absolute and relative resolution with `.`/`..`.
- **Full VFS** (user's choice): mount table (`/` = SFS2, `/proc` = procfs),
  inode cache with refcount, global open file table + per-task fd table, fd-based
  AOS syscalls.
- **Full Linux CWD + dirs**: Linux syscalls run on the same VFS fd table
  (chdir/getcwd, O_DIRECTORY, per-directory getdents64, mkdir/rmdir, writes to
  files).
- **Format features**: minimal + `mtime` (seconds from RTC).

## Architecture

New files:

- `kernel/block.c`, `kernel/block.h` — sector cache + backend abstraction
  (RAM buffer or `vblk`).
- `kernel/sfs2.c`, `kernel/sfs2.h` — SFS2 format: superblock, inode table,
  block bitmap, block allocator, dir/file operations. Implements `struct fs_ops`.
- `kernel/vfs.c`, `kernel/vfs.h` — mount table, inode cache, path resolution,
  CWD, open file table + per-task fd table, syscall glue.
- `kernel/procfs.c` — procfs instance (`/proc/uptime`, `/proc/version`,
  `/proc/meminfo`, `/proc/ticks`), validates the mount machinery.

Modified:

- `kernel/syscall.c` — replace `SYS_FS_*` with fd-based syscalls.
- `kernel/linux_syscall.c` — route through the VFS fd table; drop the local
  `fds[]/fd_name[]/fd_off[]` arrays.
- `kernel/task.c`/`task.h` — `fds[64]` + `cwd[PATH_MAX]` per task; spawn
  inherits cwd + fds 0/1/2; exit closes fds.
- `kernel/elf.c`, `kernel/progload.c`, `kernel/config.c`, `kernel/commands.c`,
  `kernel/terminal.c` — migrate to VFS calls; PATH default `/bin`; `cd`/`pwd`
  shell builtins.
- `kernel/kernel.c` — mount setup in the boot order.
- `programs/libaos.[ch]` + all programs — fd API + one-shot helpers.
- `Makefile` — new objects.
- Test scripts + `scripts/configtest.py` host-side disk builder.

### Layer overview

```
AOS syscalls ──┐
               ├──►  vfs.c     mount table, inode cache, path resolution, CWD, fd table
Linux syscalls─┘        │
                        ▼
                  sfs2.c      on-disk format, block allocator, dir/file ops (fs_ops)
                        │
                        ▼
                  block.c     sector cache (write-back) + backend
                        │
             RAM buffer  ── or ──  vblk (virtio-blk)
```

### SFS2 on-disk format (block = 512 B = one sector)

- **Superblock** (block 0): `magic[4]="SFS2"`, `block_size`, `total_blocks`,
  `inode_table_block`, `inode_count` (256), `bitmap_block`, `root_inode` (1),
  `mtime`.
- **Inode** (48 B, 256 entries → 12 KB): `type` (0=free, 1=file, 2=dir),
  `nlink`, `size`, `mtime`, `direct[8]`, `indirect` (u32). Max file size =
  8·512 + 128·512 = 68 KB (covers wm.elf at 28 KB).
- **Block bitmap**: 1 bit per block (1 KB at 4 MiB / 8192 blocks).
- **Directory = file** of 32-byte entries: `ino (u32) + name[28]` (16 entries
  per block). Entry type derived from the inode.
- Block allocation: first free bit in the bitmap. No defragmentation.

### Block cache and backends (`kernel/block.c`)

```c
struct block_backend {
    int  (*read)(unsigned int lba, void *buf);         // 512 B
    int  (*write)(unsigned int lba, const void *buf);
    unsigned int capacity_blocks;
};
```
- RAM backend (no disk): memcpy to/from a RAM buffer.
- Disk backend: `vblk_read`/`vblk_write`.
- Sector cache: 128 entries × 512 B = 64 KB, each `{ lba, valid, dirty,
  pin_count, data[512] }`. `bc_get(lba)` → pinned pointer (loads from backend,
  evicts LRU unpinned-clean on fill); `bc_put(lba)`; `bc_mark_dirty(lba)`.
- `bc_flush()` writes dirty sectors to the backend (periodic in the idle loop
  and on demand). Metadata is NOT cached here — it is held in RAM and flushed
  via `sfs2_flush()` with a dirty-sector bit pattern.
- Pin counts make it safe to hold a directory block while scanning entries.

### Inode cache and VFS core (`kernel/vfs.c`)

Each instance owns its inode storage (classic OO-in-C; `vfs.c` never allocates
inodes):

```c
struct vfs_inode {                    // shared header (vfs.h)
    unsigned int  ino;
    unsigned char type;               // 0=free, 1=file, 2=dir
    unsigned int  size;
    unsigned int  mtime;
    unsigned char dirty;
    int           refcount;           // open files + CWD + dir-streams
};
struct sfs2_inode { struct vfs_inode v; u32 direct[8]; u32 indirect; };
struct proc_inode { struct vfs_inode v; const char *name; const char *(*gen)(void); };
```
- sfs2: static array `sfs2_inode[256]` that is both the on-disk table mirror
  and the cache (no eviction; slots are reused only after delete). Unlink/rmdir
  of an inode with `refcount > 0` returns `EBUSY` (simplified semantics).
- procfs: a few static inodes.
- Filesystem driver interface:

```c
struct fs_ops {
    int (*lookup)(struct fs_instance *fs, struct vfs_inode *dir,
                  const char *name, struct vfs_inode **out);
    int (*create)(struct fs_instance *fs, struct vfs_inode *dir,
                  const char *name, int type, struct vfs_inode **out);
    int (*unlink)(struct fs_instance *fs, struct vfs_inode *dir, const char *name);
    int (*read)  (struct fs_instance *fs, struct vfs_inode *inode,
                  unsigned int off, void *buf, unsigned int size);
    int (*write) (struct fs_instance *fs, struct vfs_inode *inode,
                  unsigned int off, const void *buf, unsigned int size);
    int (*readdir)(struct fs_instance *fs, struct vfs_inode *dir,
                   unsigned int *cookie, char *name, struct vfs_inode **out);
    int (*flush) (struct fs_instance *fs);
};
struct fs_instance { const struct fs_ops *ops; void *data; struct vfs_inode *root; };
```
- **Path resolution** `vfs_resolve(cwd, path, ...)`: component split, `.`/`..`
  /empty components/trailing `/` handling, walk from root (absolute) or cwd
  (relative). At each component check for a mount point (longest-prefix match).
- **CWD**: normalized path string per task, inherited at spawn, updated by
  chdir. `getcwd` returns it. Task 0's CWD is the shell's — `cd` then running a
  program makes it see the same directory.
- **Dir-stream**: `{ dir_ino, cursor, inode ref }` used by both AOS `readdir`
  and Linux `getdents64`.

### Mount table + procfs

```c
struct mount { char path[PATH_MAX]; struct fs_instance *fs; };
struct mount mounts[8];
```
- Boot mounts: `/` → SFS2 (backend from `vblk_present()`), `/proc` → procfs.
- Single `/` mount keeps resolution trivial; more mounts scan the table.
- `umount` only when the mount root's refcount is 1.
- **procfs**: read-only; `uptime`, `version`, `meminfo`, `ticks`; content
  generated on read. Proves multi-instance resolution and `readdir`.

### Open file table + fd syscall API

```c
struct open_file {                    // global table, ~64 slots
    struct vfs_inode *inode;          // refcount++
    unsigned int offset;              // file / dir cursor
    unsigned int flags;               // O_RDONLY/WRONLY/RDWR/CREAT/TRUNC/DIRECTORY
    int dir;
};
struct open_file *task_fds[64];       // per task; 0/1/2 reserved for stdio
```
- `open()` returns the lowest free fd ≥ 3 (0/1/2 occupied by stdio for both
  ABIs). `dup`/`dup2` = two fds on one open_file (shared offset), cheap.
- New AOS syscalls: `open(path,flags)`, `close(fd)`, `read(fd,buf,n)`,
  `write(fd,buf,n)`, `lseek(fd,off,whence)`, `mkdir(path)`, `rmdir(path)`,
  `readdir(fd,name,size,is_dir)`, `chdir(path)`, `getcwd(buf,n)`,
  `stat(path,&st)`/`fstat(fd,&st)`, `unlink(path)`.
- Old path-based calls (`fs_delete`, `fs_get_size`, `fs_exists`,
  `fs_list_get`) are replaced; `libaos` keeps one-shot helpers
  `fs_read(path,...)`/`fs_write(path,...)` built on open/read/write/close so
  cat/echo/notepad/wm change little.
- **O_CREAT auto-creates missing parent directories** (mkdir -p), preserving
  today's `fs_write("sys/config.cfg")` and `bin/help` seeding behavior.
  Deliberate deviation from strict POSIX.
- **Errors: negative errno** (`-2` ENOENT, `-9` EBADF, `-17` EEXIST, `-20`
  ENOTDIR, `-21` EISDIR, `-22` EINVAL, `-28` ENOSPC, `-16` EBUSY, `-39`
  ENOTEMPTY). Invalid user pointer stays `-5` (`programs/test.c` expects it).

### Linux ABI integration

- `linux_ctx` drops its fd fields; the shared per-task fd table + VFS replace
  them. Remaining lctx fields: brk, mmap_cur, stack, TLS, win_lo/hi.
- Linux syscalls on VFS: `open`/`openat` (incl. `O_DIRECTORY`), `read`/`write`
  (write to files now works), `close`, `lseek`/`_llseek`, `unlink`, `stat64`/
  `fstat64`/`fstatat64`, `getdents64` (per-directory, `d_type`), `chdir`(12),
  `getcwd`(183), `mkdir`(39), `rmdir`(40), `access` → VFS stat. Fds 0/1/2 =
  stdio (`route_text`).
- musl `ls` (`opendir(".")`) = `open(".", O_DIRECTORY)` + `getdents64` — now
  real. `cat lin/test.txt`, `hello` unchanged.

### Kernel consumers + program migration

- `elf.c`: `vfs_pread(path, off, buf, size)` helper (open+read+close).
- `progload.c`, `config.c`: VFS stat/open(O_CREAT)/write; parents auto-created.
- `commands.c`: PATH default `/bin` (absolute, so `cd` cannot break command
  lookup); new `cd`/`pwd` builtins; `setpath` stays.
- `terminal.c`: tab completion via `readdir` on PATH dirs (not flat `sfs_get_entry`).
- `task.c`: `fds[64]` + `cwd[PATH_MAX]`; spawn inherits cwd + fds 0/1/2, others
  closed (CLOEXEC semantics); exit closes all fds.
- Shell prompt shows CWD.
- `libaos`: fd API + helpers; new `fs_mkdir`/`fs_rmdir`/`fs_listdir`/`chdir`/
  `getcwd`/`stat`.
- `ls` — per-directory listing with size + mtime + `/` suffix for dirs.
- `help` — names from `/bin`.
- `format` — recursive tree delete.
- `wm` — `refresh_files` lists `/` via readdir, hides `bin`/`lin`/`sys` dirs,
  folder kind from the is_dir flag; create-dialog makes a real `fs_mkdir`;
  dock launches `/bin/term`, `/bin/clock`.
- `rm` — file → unlink, dir → rmdir.
- `notepad`, `cat`, `echo`, `test` — via helpers, near-unchanged.
- `linrun` — `/lin/hello`.
- Seeding: `bin/*`, `lin/*`, `sys/config.cfg`, `demo.ico` — parents auto-created.

### Boot-time init order

```
... vblk_init -> sfs_set_disk ... (existing virtio init)
vfs_init()  (after fs_init call site is removed)
  -> block backend (RAM or vblk) + sector cache
  -> sfs2 mount "/" (format+seed if no SFS2 magic)
  -> procfs mount "/proc"
config_load -> load_embedded_programs -> load_embedded_data (via VFS O_CREAT)
```

## Data flow

```
mount:  vfs_init -> sfs2_mount(backend) -> read superblock -> load inode table
            -> bitmap in RAM -> root inode
open:   vfs_resolve(cwd, path) -> walk dirs via sfs2 lookup (block cache)
            -> open_file { inode, offset, flags } -> task_fds[fd]
read:   read(fd) -> fs_ops.read(inode, offset) -> bc_get(block) -> copy out
write:  write(fd) -> fs_ops.write -> bc_get + modify + bc_mark_dirty + bc_put
flush:  periodic bc_flush + sfs2_flush (dirty inode-table sectors)
delete: unlink -> remove dir entry -> free blocks (bitmap) -> free inode slot
```

## Error handling

- No virtio-blk: RAM backend; system fully functional (all legacy tests).
- Fresh/blank disk: format + seed embedded set. Corrupt magic: reformat.
- `ENOSPC` on write/`EEXIST` on mkdir/`ENOTEMPTY` on rmdir of non-empty
  dir/`EBUSY` on unlink of an open file — propagated as negative errno.
- Bad user pointers return `-5` (unchanged convention).
- Flush/backend errors logged, never panic.

## Testing

Updated:
- `virtiotest.py` — expects `SFS2` magic + `SFS2 mounted from disk.`/
  `SFS2 formatting new disk.`; persistence test becomes hierarchical
  (create `a/b/c.txt`, reboot, read back).
- `configtest.py` — host-side disk builder rewritten for SFS2 (superblock +
  inodes + bitmap + dirs; config in `/sys/config.cfg`).
- Unaffected: `blktest.py`, `rngtest.py`, `rtctest.py`, `sleeptest.py`,
  `netlooptest.py`.
- RAM-mode tests (`guitester`, `notepadtest`, `manytest`, `ipctest`,
  `linhello`, `lincat`) keep behavior via the wrapper API; touched only where
  they asserted flat listing.

New:
- `fstest.py` — hierarchy: mkdir/cd/ls, nested dirs, `.`/`..`, absolute/
  relative paths, rm file+dir, mtime in ls, per-dir readdir.
- `mounttest.py` — `cat /proc/uptime`/`version`/`meminfo`, `ls /proc`.
- `linuxdir.py` — musl `lin/ls /` and `lin/ls /bin` (opendir/getdents64),
  chdir/getcwd in a musl binary.
- `persisttest.py` — a directory tree on disk survives reboot.

Manual smoke: `ls /`, `mkdir a`, `cd a`, write `b.txt`, `cat b.txt`, `ls -l`.

`AGENTS.md` updated (filesystem model, PATH=/bin, cd/pwd, VFS layers).

## Risks

- **Largest change since SFS**: everything that touches files is touched.
  Mitigated by keeping the one-shot libaos helpers and seeding semantics.
- **Block-cache pointer invalidation**: pin counts make eviction of held
  sectors impossible; single-threaded kernel avoids concurrency races.
- **EBUSY on open-file unlink** differs from Linux (free-at-last-ref); our
  programs never unlink open files, acceptable.
- **1 MiB RAM image in RAM mode vs up to 4 MiB on disk**: block counts differ
  per boot (mode is fixed per boot); the format stores `total_blocks`, so each
  backend formats its own size.
- **procfs inode numbers**: encoded distinct from SFS2 inos; stat/getdents
  expose instance-local inos.
