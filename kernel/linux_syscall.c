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
#include "elf.h"

#define STACK_MARGIN 0x10000   // pages mapped ahead of stack building

static struct linux_ctx *cur_lctx(void) {
    return task_current_lctx();
}

static int in_luser(const void *p, unsigned int n) {
    unsigned int a = (unsigned int)p;
    struct linux_ctx *lc = cur_lctx();
    return a >= lc->win_lo && a < lc->win_hi && n <= lc->win_hi - a;
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

// i386 musl struct stat (toolchain bits/stat.h): 144 bytes, st_mode at +16.
// uid/gid/mode come from the vfs inode via aos_stat.
static void fill_stat64(struct linux_ctx *lc, const struct aos_stat *s,
                        unsigned int ino, unsigned char *st) {
    (void)lc;
    memset(st, 0, 144);
    *(unsigned int *)(st + 8)  = ino;                        // st_ino
    *(unsigned int *)(st + 12) = 1;                          // __st_ino_truncated
    // st_mode: map aos mode bits to Linux S_IFMT|S_IRWXO format
    unsigned int lmode = 0;
    if (s->type == 2)
        lmode = 0x4000;                                      // S_IFDIR
    else
        lmode = 0x8000;                                      // S_IFREG
    lmode |= (s->mode & 0777);                               // permission bits
    if (s->mode & 04000) lmode |= 0x800;                    // S_ISUID
    if (s->mode & 02000) lmode |= 0x400;                    // S_ISGID
    if (s->mode & 01000) lmode |= 0x200;                    // S_ISVTX
    *(unsigned int *)(st + 16) = lmode;                      // st_mode
    *(unsigned int *)(st + 20) = s->nlink;                   // st_nlink
    *(unsigned int *)(st + 24) = s->uid;                     // st_uid
    *(unsigned int *)(st + 28) = s->gid;                     // st_gid
    *(unsigned long long *)(st + 44) = s->size;              // st_size
    *(unsigned int *)(st + 52) = 512;                        // st_blksize
    *(unsigned long long *)(st + 56) = (unsigned long long)(s->size + 511) / 512; // st_blocks
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

    case 24:    // getuid (i386 16-bit)
    case 199: { // getuid32 (musl i386)
        r->eax = get_current_task()->uid;
        break;
    }
    case 47:    // getgid (i386 16-bit)
    case 200: { // getgid32 (musl i386)
        r->eax = get_current_task()->gid;
        break;
    }
    case 49:    // geteuid (i386 16-bit)
    case 201: { // geteuid32 (musl i386)
        r->eax = get_current_task()->euid;
        break;
    }
    case 50:    // getegid (i386 16-bit)
    case 202: { // getegid32 (musl i386)
        r->eax = get_current_task()->egid;
        break;
    }

    case 23:    // setuid (i386 16-bit)
    case 213: { // setuid32 (musl i386) — set real and effective uid
        unsigned int uid = r->ebx;
        struct task *t = get_current_task();
        if (t->euid != 0) { r->eax = -1; break; }  // -EPERM
        t->uid = uid;
        t->euid = uid;
        r->eax = 0;
        break;
    }
    case 46:    // setgid (i386 16-bit)
    case 214: { // setgid32 (musl i386) — set real and effective gid
        unsigned int gid = r->ebx;
        struct task *t = get_current_task();
        if (t->euid != 0) { r->eax = -1; break; }  // -EPERM
        t->gid = gid;
        t->egid = gid;
        r->eax = 0;
        break;
    }
    case 70:    // setreuid (i386 16-bit)
    case 203: { // setreuid32 (musl i386)
        unsigned int ruid = r->ebx;
        unsigned int euid = r->ecx;
        struct task *t = get_current_task();
        if (t->euid != 0 && ruid != t->uid && euid != t->uid) {
            r->eax = -1; break;                      // -EPERM
        }
        if (ruid != (unsigned int)-1) t->uid = ruid;
        if (euid != (unsigned int)-1) t->euid = euid;
        r->eax = 0;
        break;
    }
    case 71:    // setregid (i386 16-bit)
    case 204: { // setregid32 (musl i386)
        unsigned int rgid = r->ebx;
        unsigned int egid_val = r->ecx;
        struct task *t = get_current_task();
        if (t->euid != 0 && rgid != t->gid && egid_val != t->gid) {
            r->eax = -1; break;
        }
        if (rgid != (unsigned int)-1) t->gid = rgid;
        if (egid_val != (unsigned int)-1) t->egid = egid_val;
        r->eax = 0;
        break;
    }
    case 164:   // setresuid (i386 16-bit)
    case 208: { // setresuid32 (musl i386)
        unsigned int ruid = r->ebx;
        unsigned int euid_val = r->ecx;
        struct task *t = get_current_task();
        if (t->euid != 0) { r->eax = -1; break; }
        if (ruid != (unsigned int)-1) t->uid = ruid;
        if (euid_val != (unsigned int)-1) t->euid = euid_val;
        r->eax = 0;
        break;
    }
    case 170:   // setresgid (i386 16-bit)
    case 210: { // setresgid32 (musl i386)
        unsigned int rgid = r->ebx;
        unsigned int egid_val = r->ecx;
        struct task *t = get_current_task();
        if (t->euid != 0) { r->eax = -1; break; }
        if (rgid != (unsigned int)-1) t->gid = rgid;
        if (egid_val != (unsigned int)-1) t->egid = egid_val;
        r->eax = 0;
        break;
    }
    case 60: {  // umask(mask)
        struct task *t = get_current_task();
        unsigned int old = t->umask;
        t->umask = r->ebx & 0777;
        r->eax = old;
        break;
    }
    case 80:    // getgroups (i386 16-bit)
    case 205: { // getgroups32 (musl i386)
        unsigned int ngids = r->ebx;
        unsigned int *list = (unsigned int *)r->ecx;
        struct task *t = get_current_task();
        if (ngids == 0) { r->eax = 1; break; }      // just return count
        if (!in_luser(list, ngids * 4)) { r->eax = -14; break; }
        list[0] = t->gid;
        r->eax = 1;
        break;
    }
    case 81:    // setgroups (i386 16-bit)
    case 206: { // setgroups32 (musl i386) — stub (return 0)
        r->eax = 0;
        break;
    }

    case 15: {  // chmod(path, mode)
        char *p = copy_lin_str((const void *)r->ebx);
        if (!p) { r->eax = -14; break; }
        unsigned int mode = r->ecx & 07777;
        struct vfs_inode *cwd = current_task_cwd();
        struct task *t = get_current_task();
        // check: owner or root
        struct aos_stat st;
        int rc = vfs_stat(cwd, p, &st);
        if (rc == 0 && t->euid != 0 && t->euid != st.uid) {
            vfs_put(cwd); kfree(p); r->eax = -1; break;
        }
        rc = vfs_chmod(cwd, p, mode);
        vfs_put(cwd);
        kfree(p);
        r->eax = rc;
        break;
    }
    case 94: {  // fchmod(fd, mode)
        int fd = (int)r->ebx;
        unsigned int mode = r->ecx & 07777;
        if (!lin_fd_valid(fd)) { r->eax = -9; break; }
        struct task *t = get_current_task();
        struct open_file *of = vfs_ofile_ptr(fd);
        if (of && t->euid != 0 && t->euid != of->inode->uid) {
            r->eax = -1; break;
        }
        r->eax = vfs_fchmod(fd, mode);
        break;
    }
    case 182: {  // chown(path, owner, group)
        char *p = copy_lin_str((const void *)r->ebx);
        if (!p) { r->eax = -14; break; }
        unsigned int owner = r->ecx;
        unsigned int group = r->edx;
        struct vfs_inode *cwd = current_task_cwd();
        struct task *t = get_current_task();
        struct aos_stat st;
        int rc = vfs_stat(cwd, p, &st);
        if (rc == 0 && t->euid != 0) {
            vfs_put(cwd); kfree(p); r->eax = -1; break;
        }
        // -1 means "don't change"
        if (owner == (unsigned int)-1) owner = st.uid;
        if (group == (unsigned int)-1) group = st.gid;
        rc = vfs_chown(cwd, p, owner, group);
        vfs_put(cwd);
        kfree(p);
        r->eax = rc;
        break;
    }
    case 95: {  // fchown(fd, owner, group)
        int fd = (int)r->ebx;
        unsigned int owner = r->ecx;
        unsigned int group = r->edx;
        if (!lin_fd_valid(fd)) { r->eax = -9; break; }
        struct task *t = get_current_task();
        struct open_file *of = vfs_ofile_ptr(fd);
        if (of && t->euid != 0 && t->euid != of->inode->uid) {
            r->eax = -1; break;
        }
        if (of) {
            if (owner == (unsigned int)-1) owner = of->inode->uid;
            if (group == (unsigned int)-1) group = of->inode->gid;
        }
        r->eax = vfs_fchown(fd, owner, group);
        break;
    }

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
        fill_stat64(lc, &s, 0, st);
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
        fill_stat64(lc, &s, 0, st);
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

    case 11: {  // execve(filename, argv, envp)
        const char *path = (const char *)r->ebx;
        unsigned int *user_argv = (unsigned int *)r->ecx;
        unsigned int *user_envp = (unsigned int *)r->edx;
        struct task *t = get_current_task();

        // Copy filename to kernel buffer before user memory is overwritten
        char kpath[256];
        if (!in_luser(path, 1)) { r->eax = -14; break; }
        {
            unsigned int len = 0;
            while (len < 255 && path[len]) { kpath[len] = path[len]; len++; }
            kpath[len] = 0;
            if (len == 0) { r->eax = -2; break; }  // -ENOENT
        }

        // Copy argv strings to kernel buffers
        char kargv_buf[2048];
        unsigned int kargv_count = 0;
        {
            unsigned int kpos = 0;
            if (user_argv && in_luser(user_argv, 4)) {
                unsigned int a_ptr;
                while (kargv_count < 63) {
                    if (!in_luser(user_argv + kargv_count, 4)) break;
                    a_ptr = user_argv[kargv_count];
                    if (a_ptr == 0) break;
                    if (!in_luser((const char *)a_ptr, 1)) break;
                    unsigned int slen = 0;
                    while (slen < 255 && ((const char *)a_ptr)[slen]) slen++;
                    if (kpos + slen + 1 > sizeof(kargv_buf)) break;
                    for (unsigned int i = 0; i <= slen; i++)
                        kargv_buf[kpos++] = ((const char *)a_ptr)[i];
                    kargv_count++;
                }
            }
        }

        // Copy envp strings to kernel buffers (strip __AOS_UID/__AOS_GID)
        char kenv_buf[2048];
        unsigned int kenv_len = 0;
        {
            unsigned int kpos = 0;
            if (user_envp && in_luser(user_envp, 4)) {
                unsigned int e_ptr;
                for (unsigned int ei = 0; ei < 64; ei++) {
                    if (!in_luser(user_envp + ei, 4)) break;
                    e_ptr = user_envp[ei];
                    if (e_ptr == 0) break;
                    if (!in_luser((const char *)e_ptr, 1)) break;
                    unsigned int slen = 0;
                    while (slen < 511 && ((const char *)e_ptr)[slen]) slen++;

                    // Skip __AOS_UID and __AOS_GID (handled by kernel)
                    if (slen >= 10 && strncmp((const char *)e_ptr, "__AOS_UID=", 10) == 0) {
                        // Extract uid
                        const char *v = (const char *)e_ptr + 10;
                        unsigned int uid = 0;
                        while (*v >= '0' && *v <= '9') { uid = uid * 10 + (*v - '0'); v++; }
                        t->uid = uid;
                        t->euid = uid;
                        continue;
                    }
                    if (slen >= 10 && strncmp((const char *)e_ptr, "__AOS_GID=", 10) == 0) {
                        const char *v = (const char *)e_ptr + 10;
                        unsigned int gid = 0;
                        while (*v >= '0' && *v <= '9') { gid = gid * 10 + (*v - '0'); v++; }
                        t->gid = gid;
                        t->egid = gid;
                        continue;
                    }

                    if (kpos + slen + 1 > sizeof(kenv_buf)) break;
                    for (unsigned int i = 0; i <= slen; i++)
                        kenv_buf[kpos++] = ((const char *)e_ptr)[i];
                    kenv_len = kpos;
                }
            }
            // Double-NUL terminate
            if (kpos + 1 <= sizeof(kenv_buf)) { kenv_buf[kpos] = 0; kenv_len = kpos + 1; }
        }

        // Reset linux context for fresh address space
        struct linux_ctx *lc = t->lctx;
        lc->brk_base = 0;
        lc->brk_cur = 0;
        lc->mmap_cur = 0x10000000 - STACK_MARGIN;
        lc->tls_base = 0;
        lc->tls_limit = 0;
        lc->tls_seg32 = 0;
        lc->tls_ro = 0;
        lc->tls_gran_pages = 0;

        // Build the argv string (space-delimited) for elf_load_linux
        char kargs_str[1024];
        {
            unsigned int pos = 0;
            unsigned int off = 0;
            for (unsigned int i = 0; i < kargv_count; i++) {
                unsigned int slen = 0;
                while (slen < 255 && kargv_buf[off + slen]) slen++;
                if (i > 0 && pos < sizeof(kargs_str) - 1)
                    kargs_str[pos++] = ' ';
                for (unsigned int j = 0; j < slen && pos < sizeof(kargs_str) - 1; j++)
                    kargs_str[pos++] = kargv_buf[off + j];
                off += slen + 1;
            }
            kargs_str[pos] = 0;
        }

        // Build the env block (double-NUL terminated) for elf_load_linux
        char kenv_str[2048];
        if (kenv_len > 0) {
            unsigned int copylen = kenv_len < sizeof(kenv_str) ? kenv_len : sizeof(kenv_str);
            memcpy(kenv_str, kenv_buf, copylen);
        }

        // Load the new ELF — switch to the task's CR3 so segments are written
        // into the correct address space.
        unsigned int *kpd = paging_kernel_pd();
        void *entry;
        __asm__ volatile("cli");
        paging_set_cr3(t->cr3);
        entry = elf_load_linux(kpath, kargs_str, lc,
                               kenv_len > 0 ? kenv_str : 0);
        paging_set_cr3((unsigned int)kpd);
        __asm__ volatile("sti");

        if (!entry) {
            r->eax = -2;   // -ENOENT
            break;
        }

        // Close fds >= 3 (CLOEXEC) — only after successful ELF load so the
        // original program's fds survive a failed exec.
        for (int i = 3; i < TASK_MAX_FDS; i++) {
            if (t->fds[i]) { vfs_close_fd(i); t->fds[i] = 0; }
        }

        // Update the interrupt frame so iret returns to the new program
        r->eip = (unsigned int)entry;
        r->user_esp = lc->stack_sp;
        r->eax = 0;   // execve returns 0 on success (never actually returns)
        break;
    }

    default:
        r->eax = -38;   // -ENOSYS
        break;
    }
    trace_finish(r);
}
