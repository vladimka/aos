#include "interrupts.h"
#include "linux_syscall.h"
#include "task.h"
#include "user.h"
#include "fs.h"
#include "sfs.h"
#include "paging.h"
#include "gdt.h"
#include "string.h"
#include "syscall.h"

static char lin_str[1024];

static struct linux_ctx *cur_lctx(void) {
    return task_current_lctx();
}

static int in_luser(const void *p, unsigned int n) {
    unsigned int a = (unsigned int)p;
    struct linux_ctx *lc = cur_lctx();
    return a >= lc->win_lo && n <= lc->win_hi - a;
}

static int copy_lin_str(const void *usr, char *dst, unsigned int max) {
    unsigned int a = (unsigned int)usr;
    struct linux_ctx *lc = cur_lctx();
    if (a < lc->win_lo) return -1;
    unsigned int i = 0;
    while (i < max - 1) {
        if (a + i >= lc->win_hi) break;
        char c = *(const char *)(a + i);
        dst[i++] = c;
        if (c == '\0') return i;
    }
    dst[i] = '\0';
    return i;
}

static int lc_alloc_fd(struct linux_ctx *lc, const char *name) {
    if (!fs_exists(name)) return -1;
    for (int i = 3; i < LINUX_FDS; i++) {
        if (lc->fds[i] < 0) {
            strncpy(lc->fd_name[i], name, 31);
            lc->fd_name[i][31] = '\0';
            lc->fds[i] = 1;
            lc->fd_off[i] = 0;
            return i;
        }
    }
    return -24;   // EMFILE
}

static void linux_exit(void) {
    if (task_current_pid() == 0) {
        task_set_abi_current(ABI_AOS);
        linux_ctx_init(task_current_lctx());
        user_program_exit();
    }
    task_exit_current();
}

void linux_ctx_init(struct linux_ctx *lc) {
    memset(lc, 0, sizeof(struct linux_ctx));
    for (int i = 0; i < LINUX_FDS; i++)
        lc->fds[i] = i <= 2 ? i : -1;   // 0,1,2 = std; >=3 free
}

// i386 musl struct stat (see toolchain bits/stat.h): 108 bytes.
static void fill_stat64(struct linux_ctx *lc, unsigned int size, unsigned char *st) {
    (void)lc;
    memset(st, 0, 108);
    *(unsigned int *)(st + 8)  = 1;                        // __st_ino_truncated
    *(unsigned int *)(st + 12) = 0x81A4;                   // st_mode S_IFREG|0644
    *(unsigned int *)(st + 16) = 1;                        // st_nlink
    *(unsigned int *)(st + 20) = 0;                        // st_uid
    *(unsigned int *)(st + 24) = 0;                        // st_gid
    *(unsigned long long *)(st + 36) = size;               // st_size
    *(unsigned int *)(st + 44) = 4096;                     // st_blksize
    *(unsigned long long *)(st + 48) = (unsigned long long)(size + 511) / 512; // st_blocks
    *(unsigned int *)(st + 80) = 1;                        // st_ino
}

// linux_dirent64: u64 d_ino, i64 d_off, u16 d_reclen, u8 d_type, char d_name[]
static void put_dirent64(unsigned char *dst, unsigned long long ino,
                         unsigned long long off, unsigned char type,
                         const char *name) {
    unsigned int len = (unsigned int)strlen(name);
    unsigned short reclen = (unsigned short)(19 + len);
    *(unsigned long long *)(dst + 0) = ino;
    *(unsigned long long *)(dst + 8) = off;
    *(unsigned short *)(dst + 16) = reclen;
    dst[18] = type;
    for (unsigned int i = 0; i < len; i++)
        dst[19 + i] = (unsigned char)name[i];
}

void linux_syscall_handler(struct registers *r) {
    unsigned int n = r->eax;
    struct linux_ctx *lc = cur_lctx();

    switch (n) {
    case 1:    // exit
    case 252:  // exit_group
        linux_exit();
        break;

    case 4: {  // write(fd, buf, count)
        int fd = r->ebx;
        const char *buf = (const char *)r->ecx;
        unsigned int count = r->edx;
        if (!in_luser(buf, count)) { r->eax = -14; break; }   // -EFAULT
        if (fd <= 2) {
            route_text(buf, count);
            r->eax = count;
        } else {
            r->eax = -9;                                      // -EBADF
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

    case 3: {  // read(fd, buf, count)
        int fd = r->ebx;
        char *buf = (char *)r->ecx;
        unsigned int count = r->edx;
        if (fd == 0) { r->eax = -11; break; }                 // -EAGAIN
        if (fd < 0 || fd >= LINUX_FDS || lc->fds[fd] < 0 || fd <= 2) { r->eax = -9; break; }
        if (!in_luser(buf, count)) { r->eax = -14; break; }
        int n = fs_read_at(lc->fd_name[fd], buf, count, lc->fd_off[fd]);
        if (n < 0) { r->eax = -2; break; }                    // -ENOENT
        lc->fd_off[fd] += (unsigned int)n;
        r->eax = n;
        break;
    }

    case 5:   // open(path, flags, mode)
    case 295: { // openat(dirfd, path, flags, mode)
        const void *pp = (n == 295) ? (const void *)r->ecx : (const void *)r->ebx;
        if (copy_lin_str(pp, lin_str, sizeof(lin_str)) < 0) { r->eax = -14; break; }
        int fd = lc_alloc_fd(lc, lin_str);
        r->eax = (fd < 0) ? (fd == -24 ? -24 : -2) : fd;
        break;
    }

    case 6: {  // close(fd)
        int fd = r->ebx;
        if (fd < 0 || fd >= LINUX_FDS || lc->fds[fd] < 0) { r->eax = -9; break; }
        lc->fds[fd] = -1;
        r->eax = 0;
        break;
    }

    case 10:   // unlink(path)
        if (copy_lin_str((const void *)r->ebx, lin_str, sizeof(lin_str)) < 0) {
            r->eax = -14;
        } else {
            r->eax = fs_delete(lin_str) == 0 ? 0 : -2;
        }
        break;

    case 19:   // lseek(fd, offset, whence)
    case 140: { // _llseek(fd, off_hi, off_lo, res, whence)
        int fd = r->ebx;
        if (fd < 0 || fd >= LINUX_FDS || lc->fds[fd] < 0 || fd <= 2) { r->eax = -9; break; }
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
        int sz = fs_get_size(lc->fd_name[fd]);
        unsigned int cur = lc->fd_off[fd];
        unsigned int newoff = cur;
        if (whence == 0) newoff = off;
        else if (whence == 1) newoff = cur + off;
        else if (whence == 2) newoff = (sz < 0 ? 0 : (unsigned int)sz) + off;
        if (newoff > 0x7FFFFFFF) newoff = 0x7FFFFFFF;
        lc->fd_off[fd] = newoff;
        if (n == 140)
            *res = newoff;
        r->eax = (n == 140) ? 0 : newoff;
        break;
    }

    case 33:   // access(path, mode)
        if (copy_lin_str((const void *)r->ebx, lin_str, sizeof(lin_str)) < 0) {
            r->eax = -14;
        } else {
            r->eax = fs_exists(lin_str) ? 0 : -2;
        }
        break;

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

    case 54:   // ioctl — no ttys
        r->eax = -25;                                       // -ENOTTY
        break;

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

    case 162: {  // nanosleep(req, rem) — spin on the PIT tick
        extern volatile unsigned int tick;
        const unsigned char *req = (const unsigned char *)r->ebx;
        if (!in_luser(req, 8)) { r->eax = -14; break; }
        unsigned int sec, nsec;
        memcpy(&sec, req, 4);
        memcpy(&nsec, req + 4, 4);
        unsigned int ms = sec * 1000 + nsec / 1000000u;
        unsigned int start = tick;
        while (tick - start < ms);
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
        const void *pp = (n == 300) ? (const void *)r->ecx : (const void *)r->ebx;
        unsigned char *st = (unsigned char *)r->edx;
        if (copy_lin_str(pp, lin_str, sizeof(lin_str)) < 0) { r->eax = -14; break; }
        if (!in_luser(st, 108)) { r->eax = -14; break; }
        int size = fs_get_size(lin_str);
        if (size < 0) { r->eax = -2; break; }
        fill_stat64(lc, (unsigned int)size, st);
        r->eax = 0;
        break;
    }

    case 197: {  // fstat64(fd, st)
        int fd = r->ebx;
        unsigned char *st = (unsigned char *)r->ecx;
        if (fd < 0 || fd >= LINUX_FDS || lc->fds[fd] < 0 || fd <= 2) { r->eax = -9; break; }
        if (!in_luser(st, 108)) { r->eax = -14; break; }
        int size = fs_get_size(lc->fd_name[fd]);
        if (size < 0) size = 0;
        fill_stat64(lc, (unsigned int)size, st);
        r->eax = 0;
        break;
    }

    case 220: {  // getdents64(fd, buf, count)
        int fd = r->ebx;
        unsigned char *buf = (unsigned char *)r->ecx;
        unsigned int count = r->edx;
        if (fd < 0 || fd >= LINUX_FDS || lc->fds[fd] < 0 || fd <= 2) { r->eax = -9; break; }
        if (!in_luser(buf, count)) { r->eax = -14; break; }
        unsigned int idx = lc->fd_off[fd];   // reused as the SFS entry cursor
        unsigned int written = 0;
        for (; idx < SFS_MAX_FILES; idx++) {
            char name[32];
            unsigned int size;
            if (sfs_get_entry(idx, name, &size) != 0) continue;
            unsigned int len = (unsigned int)strlen(name);
            unsigned int reclen = 19 + len;
            if (written + reclen > count) break;
            unsigned char type = (name[len - 1] == '/') ? 4 : 8;
            put_dirent64(buf + written, (unsigned long long)idx + 1,
                         (unsigned long long)(written + reclen), type, name);
            written += reclen;
            lc->fd_off[fd] = idx + 1;
        }
        r->eax = written;
        break;
    }

    default:
        r->eax = -38;   // -ENOSYS
        break;
    }
}
