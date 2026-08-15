#include "interrupts.h"
#include "linux_syscall.h"
#include "task.h"
#include "user.h"
#include "vfs.h"
#include "paging.h"
#include "gdt.h"
#include "string.h"
#include "syscall.h"
#include "terminal.h"
#include "commands.h"
#include "kmm.h"
#include "ports.h"
#include "vrng.h"
#include "trace.h"
#include "serial.h"

static struct linux_ctx *cur_lctx(void) {
    return task_current_lctx();
}

static int in_luser(const void *p, unsigned int n) {
    unsigned int a = (unsigned int)p;
    struct linux_ctx *lc = cur_lctx();
    return a >= lc->win_lo && n <= lc->win_hi - a;
}

// Copy a Linux-window user string into a fresh kmalloc'd buffer (no 1024 cap).
// Returns 0 on bad pointer or kmalloc failure.
static char *copy_lin_str(const void *usr) {
    unsigned int a = (unsigned int)usr;
    struct linux_ctx *lc = cur_lctx();
    if (a < lc->win_lo) return 0;
    unsigned int len = 0;
    while (a + len < lc->win_hi) {
        if (((const char *)a)[len] == '\0') break;
        len++;
    }
    char *dst = kmalloc(len + 1);
    if (!dst) return 0;
    for (unsigned int i = 0; i <= len; i++)
        dst[i] = ((const char *)a)[i];   // copies the NUL too
    return dst;
}

static int lin_fd_valid(int fd) {
    struct task *t = get_current_task();
    return fd >= 3 && fd < TASK_MAX_FDS && t->fds[fd] != 0;
}

static void linux_exit(unsigned int code) {
    serial_print("LX:pid=");
    serial_print_dec(task_current_pid());
    serial_print(" code=");
    serial_print_dec(code);
    serial_print("\n");
    if (task_current_pid() == 0) {
        shell_set_status((int)code);
        task_set_abi_current(ABI_AOS);
        linux_ctx_init(task_current_lctx());
        user_program_exit();
    }
    task_exit_current(code);
}

void linux_ctx_init(struct linux_ctx *lc) {
    memset(lc, 0, sizeof(struct linux_ctx));
}

// i386 musl struct stat (see toolchain bits/stat.h): 108 bytes.
static void fill_stat64(struct linux_ctx *lc, unsigned int type,
                        unsigned int size, unsigned char *st) {
    (void)lc;
    memset(st, 0, 108);
    *(unsigned int *)(st + 8)  = 1;                        // __st_ino_truncated
    if (type == 2)
        *(unsigned int *)(st + 12) = 0x41ED;               // st_mode S_IFDIR|0777
    else
        *(unsigned int *)(st + 12) = 0x81ED;               // st_mode S_IFREG|0777
    *(unsigned int *)(st + 16) = 1;                        // st_nlink
    *(unsigned int *)(st + 20) = 0;                        // st_uid
    *(unsigned int *)(st + 24) = 0;                        // st_gid
    *(unsigned long long *)(st + 36) = size;               // st_size
    *(unsigned int *)(st + 44) = 512;                      // st_blksize
    *(unsigned long long *)(st + 48) = (unsigned long long)(size + 511) / 512; // st_blocks
    *(unsigned int *)(st + 80) = 1;                        // st_ino
}

// linux_dirent64: u64 d_ino, i64 d_off, u16 d_reclen, u8 d_type, char d_name[]
static void put_dirent64(unsigned char *dst, unsigned long long ino,
                         unsigned long long off, unsigned char type,
                         const char *name) {
    unsigned int len = (unsigned int)strlen(name);
    unsigned short reclen = (unsigned short)(20 + len);   // NUL-terminated
    *(unsigned long long *)(dst + 0) = ino;
    *(unsigned long long *)(dst + 8) = off;
    *(unsigned short *)(dst + 16) = reclen;
    dst[18] = type;
    for (unsigned int i = 0; i < len; i++)
        dst[19 + i] = (unsigned char)name[i];
    dst[19 + len] = 0;
}

void linux_syscall_handler(struct registers *r) {
    unsigned int n = r->eax;
    struct linux_ctx *lc = cur_lctx();
    trace_record(r);

    switch (n) {
    case 1:    // exit
    case 252:  // exit_group
        linux_exit(r->ebx);
        break;

    case 4: {  // write(fd, buf, count)
        int fd = r->ebx;
        const char *buf = (const char *)r->ecx;
        unsigned int count = r->edx;
        if (!in_luser(buf, count)) { r->eax = -14; break; }   // -EFAULT
        if (fd <= 2) {
            int rc = route_text(buf, count);
            r->eax = (rc < 0) ? rc : (int)count;
        } else {
            if (!lin_fd_valid(fd)) { r->eax = -9; break; }    // -EBADF
            r->eax = vfs_write_fd(fd, buf, count);
        }
        break;
    }

    case 146: {  // writev(fd, iov, count)
        int fd = r->ebx;
        unsigned int count = r->edx;
        if (fd > 2) { r->eax = -9; break; }                   // -EBADF
        struct { unsigned int base; unsigned int len; } iov;
        unsigned int total = 0;
        int err = 0;
        for (unsigned int i = 0; i < count && i < 1024; i++) {
            if (!in_luser((const void *)(r->ecx + i * 8), 8)) { err = -14; break; }
            memcpy(&iov, (const void *)(r->ecx + i * 8), 8);
            if (!in_luser((const void *)iov.base, iov.len)) { err = -14; break; }
            route_text((const char *)iov.base, iov.len);
            total += iov.len;
        }
        r->eax = err ? (unsigned int)err : total;
        break;
    }

    case 45: {  // brk(addr)
        unsigned int addr = r->ebx;
        if (addr == 0) { r->eax = lc->brk_cur; break; }
        if (addr < lc->brk_base || addr > lc->win_hi) { r->eax = lc->brk_cur; break; }
        for (unsigned int a = (lc->brk_cur + 0xFFF) & ~0xFFFu; a < addr; a += 0x1000)
            if (paging_map_user_page(a) < 0) { r->eax = lc->brk_cur; break; }
        lc->brk_cur = addr;
        r->eax = lc->brk_cur;
        break;
    }

    case 192: {  // mmap2(addr, len, prot, flags, fd, off_pages)
        unsigned int addr = r->ebx;
        unsigned int len = r->ecx;
        unsigned int flags = r->edx;
        if (len == 0) { r->eax = -22; break; }                // -EINVAL
        len = (len + 0xFFF) & ~0xFFFu;
        unsigned int base;
        if (addr && (flags & 0x10)) {                          // MAP_FIXED
            base = addr & ~0xFFFu;
        } else {
            if (lc->mmap_cur < lc->win_lo + len) { r->eax = -12; break; } // -ENOMEM
            lc->mmap_cur -= len;
            base = lc->mmap_cur;
        }
        for (unsigned int a = base; a < base + len; a += 0x1000)
            if (paging_map_user_page(a) < 0) { r->eax = -12; break; }
        r->eax = base;
        break;
    }

    case 91:     // munmap — pages stay mapped (leak) for step 1
    case 125:    // mprotect
        r->eax = 0;
        break;

    case 243: {  // set_thread_area(user_desc*)
        struct {
            unsigned int entry_number;
            unsigned int base_addr;
            unsigned int limit;
            unsigned int flags;
        } ud;
        if (!in_luser((const void *)r->ebx, sizeof(ud))) { r->eax = -14; break; }
        memcpy(&ud, (const void *)r->ebx, sizeof(ud));
        unsigned int bitfield = ud.flags;
        lc->tls_base = ud.base_addr;
        lc->tls_limit = ud.limit;
        lc->tls_seg32 = (bitfield >> 0) & 1;
        lc->tls_ro = (bitfield >> 3) & 1;
        lc->tls_gran_pages = (bitfield >> 4) & 1;
        ldt_set_tls(lc->tls_base, lc->tls_limit,
                    lc->tls_seg32, lc->tls_ro, lc->tls_gran_pages);
        *(unsigned int *)r->ebx = TLS_ENTRY;   // entry_number -> GDT TLS slot 0x33
        tls_reload_gs();
        r->eax = 0;
        break;
    }

    case 123: {  // modify_ldt(func, ptr, bytecount) — func 1 == write
        unsigned int func = r->ebx;
        if (func == 1) {
            struct {
                unsigned int entry_number;
                unsigned int base_addr;
                unsigned int limit;
                unsigned int flags;
            } ld;
            if (!in_luser((const void *)r->ecx, sizeof(ld))) { r->eax = -14; break; }
            memcpy(&ld, (const void *)r->ecx, sizeof(ld));
            lc->tls_base = ld.base_addr;
            lc->tls_limit = ld.limit;
            lc->tls_seg32 = (ld.flags >> 0) & 1;
            lc->tls_ro = (ld.flags >> 3) & 1;
            lc->tls_gran_pages = (ld.flags >> 4) & 1;
            ldt_set_tls(lc->tls_base, lc->tls_limit,
                        lc->tls_seg32, lc->tls_ro, lc->tls_gran_pages);
            *(unsigned int *)r->ecx = TLS_ENTRY;
            tls_reload_gs();
        }
        r->eax = 0;
        break;
    }

    case 258:    // set_tid_address(ptr) — no kernel pid stored; tid is 0
        r->eax = 0;
        break;

    case 36: {   // sync() — flush the write-back block cache to the device
        r->eax = vfs_sync();
        break;
    }

    case 118: {  // fsync(fd) — validate the fd, then flush dirty blocks
        int fd = (int)r->ebx;
        if (!lin_fd_valid(fd)) {
            r->eax = -9;                 // -EBADF
            break;
        }
        r->eax = vfs_sync();
        break;
    }

    case 88: {  // reboot(magic1, magic2, cmd, arg)
        unsigned int cmd = r->edx;
        if (cmd == 0x1234567) {          // LINUX_REBOOT_CMD_RESTART
            vfs_sync();
            outb(0x64, 0xFE);
            for (;;);
        }
        if (cmd == 0x4321fedc) {         // LINUX_REBOOT_CMD_POWER_OFF
            vfs_sync();
            outw(0x604, 0x2000);
            for (;;);
        }
        r->eax = -22;                    // -EINVAL
        break;
    }

    case 355: {  // getrandom(buf, buflen, flags)
        void *buf = (void *)r->ebx;
        unsigned int len = r->edx;
        if (!in_luser(buf, len)) { r->eax = -14; break; }   // -EFAULT
        if (len > 512) len = 512;
        r->eax = vrng_bytes(buf, len);
        break;
    }

    case 3: {  // read(fd, buf, count)
        int fd = r->ebx;
        char *buf = (char *)r->ecx;
        unsigned int count = r->edx;
        if (!in_luser(buf, count)) { r->eax = -14; break; }
        if (fd == 0) {
            if (count == 0) { r->eax = 0; break; }
            if (get_current_task()->stdin_fd >= 0) {
                r->eax = vfs_read_fd(get_current_task()->stdin_fd, buf, count);
                break;
            }
            int k = terminal_read_key();
            r->eax = (k < 0) ? -11 : 1;                     // -EAGAIN when empty
            if (k >= 0) buf[0] = (char)k;
            break;
        }
        if (!lin_fd_valid(fd)) { r->eax = -9; break; }
        r->eax = vfs_read_fd(fd, buf, count);
        break;
    }

    case 42: {  // pipe(int fds[2])
        unsigned int *fds = (unsigned int *)r->ebx;
        if (!in_luser(fds, 8)) { r->eax = -14; break; }    // -EFAULT
        int rd, wr;
        int rc = vfs_pipe(&rd, &wr);
        if (rc < 0) { r->eax = rc; break; }
        struct task *t = get_current_task();
        t->fds[rd] = vfs_ofile_ptr(rd);
        t->fds[wr] = vfs_ofile_ptr(wr);
        fds[0] = (unsigned int)rd;
        fds[1] = (unsigned int)wr;
        r->eax = 0;
        break;
    }

    case 5:   // open(path, flags, mode)
    case 295: { // openat(dirfd, path, flags, mode)
        int dirfd = (n == 295) ? (int)r->ebx : -100;        // -100 == AT_FDCWD
        const void *pp = (n == 295) ? (const void *)r->ecx : (const void *)r->ebx;
        char *p = copy_lin_str(pp);
        if (!p) { r->eax = -14; break; }
        int flags = (n == 295) ? (int)r->edx : (int)r->ecx;
        flags &= ~0x80000;                                  // mask O_CLOEXEC
        struct vfs_inode *base = current_task_cwd();
        if (n == 295 && dirfd != -100) {
            struct open_file *of = (dirfd >= 3 && dirfd < TASK_MAX_FDS)
                                       ? vfs_ofile_ptr(dirfd) : 0;
            if (base) vfs_put(base);
            if (!of || !of->inode) { kfree(p); r->eax = -9; break; }
            base = vfs_get(of->inode->fs, of->inode->ino);  // own ref
        }
        if (!base) { kfree(p); r->eax = -9; break; }
        int fd = vfs_open_fd(base, p, flags);
        vfs_put(base);
        kfree(p);
        if (fd >= 0 && fd < TASK_MAX_FDS)
            get_current_task()->fds[fd] = vfs_ofile_ptr(fd);
        r->eax = fd;                                        // VFS errno (<0) directly
        break;
    }

    case 6: {  // close(fd)
        int fd = r->ebx;
        struct task *t = get_current_task();
        if (fd >= 0 && fd < 3) {
            if (fd < TASK_MAX_FDS && t->fds[fd] == &console_open_file)
                t->fds[fd] = 0;
            r->eax = 0;
            break;
        }
        int rc = vfs_close_fd(fd);
        if (rc == 0 && fd >= 0 && fd < TASK_MAX_FDS)
            t->fds[fd] = 0;
        r->eax = rc;
        break;
    }

    case 10: {  // unlink(path)
        char *p = copy_lin_str((const void *)r->ebx);
        if (!p) { r->eax = -14; break; }
        struct vfs_inode *cwd = current_task_cwd();
        r->eax = vfs_unlink(cwd, p);
        vfs_put(cwd);
        kfree(p);
        break;
    }

    case 19:   // lseek(fd, offset, whence)
    case 140: { // _llseek(fd, off_hi, off_lo, res, whence)
        int fd = r->ebx;
        if (!lin_fd_valid(fd)) { r->eax = -9; break; }
        unsigned int off, whence;
        unsigned int *res = 0;
        if (n == 140) {
            off = r->edx;              // off_lo
            res = (unsigned int *)r->esi;
            whence = r->edi;
            if (!in_luser(res, 4)) { r->eax = -14; break; }
        } else {
            off = r->ecx;
            whence = r->edx;
        }
        int rc = vfs_lseek_fd(fd, off, whence);
        if (n == 140) {
            if (rc < 0) { r->eax = rc; break; }
            *res = (unsigned int)rc;
            r->eax = 0;
        } else {
            r->eax = rc;
        }
        break;
    }

    case 33: {  // access(path, mode)
        char *p = copy_lin_str((const void *)r->ebx);
        if (!p) { r->eax = -14; break; }
        struct vfs_inode *cwd = current_task_cwd();
        struct aos_stat st;
        int rc = vfs_stat(cwd, p, &st);
        vfs_put(cwd);
        kfree(p);
        r->eax = (rc == 0) ? 0 : -2;
        break;
    }

    case 12: {  // chdir(path)
        char *p = copy_lin_str((const void *)r->ebx);
        if (!p) { r->eax = -14; break; }
        struct task *t = get_current_task();
        char nb[PATH_MAX];
        int rc = path_norm(t->cwd, p, nb, sizeof(nb));
        if (rc != 0) { kfree(p); r->eax = -22; break; }      // -EINVAL
        struct aos_stat st;
        if (vfs_kernel_stat(nb, &st) != 0) { kfree(p); r->eax = -2; break; }
        if (st.type != 2) { kfree(p); r->eax = -20; break; } // -ENOTDIR
        strncpy(t->cwd, nb, PATH_MAX);
        t->cwd[PATH_MAX - 1] = '\0';
        kfree(p);
        r->eax = 0;
        break;
    }

    case 39: {  // mkdir(path, mode)
        char *p = copy_lin_str((const void *)r->ebx);
        if (!p) { r->eax = -14; break; }
        struct vfs_inode *cwd = current_task_cwd();
        r->eax = vfs_mkdir(cwd, p);
        vfs_put(cwd);
        kfree(p);
        break;
    }

    case 40: {  // rmdir(path)
        char *p = copy_lin_str((const void *)r->ebx);
        if (!p) { r->eax = -14; break; }
        struct vfs_inode *cwd = current_task_cwd();
        r->eax = vfs_rmdir(cwd, p);
        vfs_put(cwd);
        kfree(p);
        break;
    }

    case 183: {  // getcwd(buf, size)
        char *buf = (char *)r->ebx;
        unsigned int size = r->ecx;
        if (!in_luser(buf, size)) { r->eax = -14; break; }
        const char *c = get_current_task()->cwd;
        unsigned int need = 0;
        while (c[need]) need++;
        need++;
        if (need > size) { r->eax = -34; break; }            // -ERANGE
        for (unsigned int i = 0; i < need; i++) buf[i] = c[i];
        r->eax = need;
        break;
    }

    case 13:   // time(t) — return 0
    case 20:   // getpid
        if (n == 20) { r->eax = task_current_pid(); break; }
        r->eax = 0;
        break;

    case 24:   // getuid
    case 47:   // getgid
    case 49:   // geteuid
    case 50:   // getegid
        r->eax = 0;
        break;

    case 54: {  // ioctl(fd, req, arg) — FIONBIO only (0x5421)
        int fd = (int)r->ebx;
        unsigned int req = r->ecx;
        struct open_file *of = vfs_ofile_ptr(fd);
        if (!of) { r->eax = -9; break; }                       // EBADF
        if (req == 0x5421) {                                    // FIONBIO
            const int *arg = (const int *)r->edx;
            if (!in_luser(arg, 4)) { r->eax = -14; break; }    // EFAULT
            if (*arg) of->flags |= VFS_O_NONBLOCK;
            else of->flags &= ~VFS_O_NONBLOCK;
            r->eax = 0;
        } else {
            r->eax = -25;                                      // ENOTTY
        }
        break;
    }

    case 78: {  // gettimeofday(tv, tz)
        void *tv = (void *)r->ebx;
        if (tv) {
            if (!in_luser(tv, 8)) { r->eax = -14; break; }
            memset(tv, 0, 8);
        }
        r->eax = 0;
        break;
    }

    case 122: {  // uname(struct utsname*)
        unsigned char *u = (unsigned char *)r->ebx;
        if (!in_luser(u, 390)) { r->eax = -14; break; }
        memset(u, 0, 390);
        strncpy((char *)(u + 0),   "Linux", 65);
        strncpy((char *)(u + 65),  "aos", 65);
        strncpy((char *)(u + 130), "5.0.0", 65);
        strncpy((char *)(u + 195), "#1", 65);
        strncpy((char *)(u + 260), "i686", 65);
        strncpy((char *)(u + 325), "aos", 65);
        r->eax = 0;
        break;
    }

    case 162: {  // nanosleep(req, rem) — block on the PIT tick
        const unsigned char *req = (const unsigned char *)r->ebx;
        if (!in_luser(req, 8)) { r->eax = -14; break; }
        unsigned int sec, nsec;
        memcpy(&sec, req, 4);
        memcpy(&nsec, req + 4, 4);
        unsigned int ms = sec * 1000 + nsec / 1000000u;
        task_sleep(ms);          // sti;hlt;cli: lets the timer IRQ advance tick
        r->eax = 0;
        break;
    }

    case 265: {  // clock_gettime(clockid, timespec*)
        void *ts = (void *)r->ecx;
        if (!in_luser(ts, 8)) { r->eax = -14; break; }
        memset(ts, 0, 8);
        r->eax = 0;
        break;
    }

    case 195:   // stat64(path, st)
    case 300: { // fstatat64(dirfd, path, st, flags)
        int dirfd = (n == 300) ? (int)r->ebx : -100;        // -100 == AT_FDCWD
        const void *pp = (n == 300) ? (const void *)r->ecx : (const void *)r->ebx;
        unsigned char *st = (n == 300) ? (unsigned char *)r->edx : (unsigned char *)r->ecx;
        if (!in_luser(st, 108)) { r->eax = -14; break; }
        char *p = copy_lin_str(pp);
        if (!p) { r->eax = -14; break; }
        struct vfs_inode *base = current_task_cwd();
        if (n == 300 && dirfd != -100) {
            struct open_file *of = (dirfd >= 3 && dirfd < TASK_MAX_FDS)
                                       ? vfs_ofile_ptr(dirfd) : 0;
            if (base) vfs_put(base);
            if (!of || !of->inode) { kfree(p); r->eax = -9; break; }
            base = vfs_get(of->inode->fs, of->inode->ino);  // own ref
        }
        struct aos_stat s;
        int rc = vfs_stat(base, p, &s);
        vfs_put(base);
        kfree(p);
        if (rc < 0) { r->eax = -2; break; }
        fill_stat64(lc, s.type, s.size, st);
        r->eax = 0;
        break;
    }

    case 197: {  // fstat64(fd, st)
        int fd = r->ebx;
        unsigned char *st = (unsigned char *)r->ecx;
        if (!lin_fd_valid(fd)) { r->eax = -9; break; }
        if (!in_luser(st, 108)) { r->eax = -14; break; }
        struct aos_stat s;
        int rc = vfs_fstat_fd(fd, &s);
        if (rc < 0) { r->eax = rc; break; }
        fill_stat64(lc, s.type, s.size, st);
        r->eax = 0;
        break;
    }

    case 220: {  // getdents64(fd, buf, count)
        int fd = r->ebx;
        unsigned char *buf = (unsigned char *)r->ecx;
        unsigned int count = r->edx;
        if (!lin_fd_valid(fd)) { r->eax = -9; break; }
        if (!in_luser(buf, count)) { r->eax = -14; break; }
        struct open_file *of = vfs_ofile_ptr(fd);
        if (!of || !of->inode || of->inode->type != 2) { r->eax = -20; break; } // -ENOTDIR
        unsigned int written = 0;
        unsigned int entry_ino = 1;
        for (;;) {
            unsigned int save_pos = of->pos;
            char name[VFS_NAME_MAX + 1];
            if (vfs_readdir_fd(fd, name, sizeof(name)) != 1) break;
            unsigned int len = (unsigned int)strlen(name);
            unsigned int reclen = 20 + len;               // NUL-terminated d_name
            if (written + reclen > count) { of->pos = save_pos; break; }
            unsigned char type = 0;
            struct aos_stat s;
            if (vfs_stat(of->inode, name, &s) == 0)
                type = (s.type == 2) ? 4 : 8;               // DT_DIR / DT_REG
            put_dirent64(buf + written, (unsigned long long)entry_ino,
                         (unsigned long long)(written + reclen), type, name);
            written += reclen;
            entry_ino++;
        }
        r->eax = written;
        break;
    }

    default:
        r->eax = -38;   // -ENOSYS
        break;
    }
    trace_finish(r);
}
