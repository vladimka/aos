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

static char lin_str[1024] __attribute__((unused));

static struct linux_ctx *cur_lctx(void) {
    return task_current_lctx();
}

static int in_luser(const void *p, unsigned int n) {
    unsigned int a = (unsigned int)p;
    struct linux_ctx *lc = cur_lctx();
    return a >= lc->win_lo && n <= lc->win_hi - a;
}

static int copy_lin_str(const void *usr, char *dst, unsigned int max) __attribute__((unused));
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

static int lc_alloc_fd(struct linux_ctx *lc, const char *name) __attribute__((unused));
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
        *(unsigned int *)r->ebx = 0;   // entry_number = 0 -> selector 0x03
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
            *(unsigned int *)r->ecx = 0;
        }
        r->eax = 0;
        break;
    }

    case 258:    // set_tid_address(ptr) — no kernel pid stored; tid is 0
        r->eax = 0;
        break;

    default:
        r->eax = -38;   // -ENOSYS
        break;
    }
}
