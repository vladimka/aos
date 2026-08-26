#include "trace.h"
#include "task.h"
#include "terminal.h"
#include "syscall.h"
#include "kmm.h"
#include "interrupts.h"
#include "serial.h"

struct trace_name {
    const char *name;
    unsigned char nargs;
};

// ---- AOS ABI (int 0x80, syscall_handler switch): dense 0..48 ----
static const struct trace_name aos_names[49] = {
    [0]  = { "print", 1 },
    [1]  = { "print_hex", 1 },
    [2]  = { "print_dec", 1 },
    [3]  = { "putchar", 1 },
    [10] = { "tick", 0 },
    [11] = { "clear", 0 },
    [12] = { "reboot", 0 },
    [13] = { "panic", 0 },
    [14] = { "shutdown", 0 },
    [15] = { "get_args", 2 },
    [16] = { "exit", 1 },
    [17] = { "read_key", 0 },
    [18] = { "yield", 0 },
    [19] = { "getpid", 0 },
    [20] = { "send", 5 },
    [21] = { "recv", 1 },
    [22] = { "event", 0 },
    [23] = { "mouse", 4 },
    [24] = { "fb_info", 5 },
    [25] = { "text", 1 },
    [26] = { "fill", 1 },
    [27] = { "setout", 1 },
    [28] = { "spawn", 3 },
    [29] = { "getevent", 0 },
    [30] = { "sleep", 1 },
    [31] = { "waitpid", 1 },
    [32] = { "get_children", 2 },
    [33] = { "random", 2 },
    [34] = { "rtc", 1 },
    [35] = { "uptime", 0 },
    [36] = { "open", 2 },
    [37] = { "close", 1 },
    [38] = { "read", 3 },
    [39] = { "write", 3 },
    [40] = { "lseek", 3 },
    [41] = { "mkdir", 1 },
    [42] = { "rmdir", 1 },
    [43] = { "readdir", 3 },
    [44] = { "chdir", 1 },
    [45] = { "getcwd", 2 },
    [46] = { "stat", 2 },
    [47] = { "fstat", 2 },
    [48] = { "unlink", 1 },
};

// ---- AOS_EXT (500-527, aos_gui_handler) ----
static const struct trace_name aos_ext_names[28] = {
    [0]  = { "fb_info", 5 },
    [1]  = { "text", 1 },
    [2]  = { "fill", 1 },
    [3]  = { "clear", 0 },
    [4]  = { "mouse", 4 },
    [5]  = { "read_key", 0 },
    [6]  = { "key_poll", 0 },
    [7]  = { "reg_events", 0 },
    [8]  = { "get_event_pid", 0 },
    [9]  = { "send", 2 },
    [10] = { "recv", 1 },
    [11] = { "setout", 1 },
    [12] = { "spawn", 3 },
    [13] = { "waitpid", 1 },
    [14] = { "get_children", 2 },
    [15] = { "get_args", 2 },
    [16] = { "get_rtc", 1 },
    [17] = { "uptime", 0 },
    [18] = { "get_tick", 0 },
    [19] = { "panic", 0 },
    [25] = { "kill", 1 },
    [26] = { "trace_set", 1 },
    [27] = { "trace_dump", 1 },
};

// ---- Linux ABI (linux_syscall_handler): sparse, ascending ----
static const struct trace_name linux_names[] = {
    { "exit", 1 },          { "read", 3 },
    { "write", 3 },         { "open", 3 },
    { "close", 1 },         { "unlink", 1 },
    { "chdir", 1 },         { "time", 1 },
    { "lseek", 3 },         { "getpid", 0 },
    { "getuid", 0 },        { "access", 2 },
    { "mkdir", 2 },         { "rmdir", 1 },
    { "brk", 1 },           { "getgid", 0 },
    { "geteuid", 0 },       { "getegid", 0 },
    { "ioctl", 3 },         { "gettimeofday", 2 },
    { "reboot", 3 },        { "munmap", 2 },
    { "modify_ldt", 3 },    { "mprotect", 3 },
    { "_llseek", 5 },       { "writev", 3 },
    { "nanosleep", 2 },     { "getcwd", 2 },
    { "mmap2", 6 },         { "stat64", 2 },
    { "fstat64", 2 },       { "getdents64", 3 },
    { "set_thread_area", 1 }, { "exit_group", 1 },
    { "set_tid_address", 1 }, { "clock_gettime", 2 },
    { "openat", 4 },        { "fstatat64", 4 },
    { "getrandom", 3 },
};
static const unsigned int linux_nums[] = {
    1, 3, 4, 5, 6, 10, 12, 13, 19, 20, 24, 33, 39, 40, 45, 47, 49, 50,
    54, 78, 88, 91, 123, 125, 140, 146, 162, 183, 192, 195, 197, 220,
    243, 252, 258, 265, 295, 300, 355,
};
#define NLIN (sizeof(linux_nums) / sizeof(linux_nums[0]))

static const struct trace_name *linux_name(unsigned int num) {
    for (unsigned int i = 0; i < NLIN; i++)
        if (linux_nums[i] == num) return &linux_names[i];
    return 0;
}

#define AOS_EXT_BASE 500

// AOS_EXT range wins over the task ABI (musl GUI programs are ABI_LINUX but
// issue 500-519 GUI syscalls).
static const struct trace_name *trace_name_for(const struct task *t,
                                               unsigned int num) {
    if (num >= AOS_EXT_BASE && num < AOS_EXT_BASE + 100)
        return num - AOS_EXT_BASE < 28 ? &aos_ext_names[num - AOS_EXT_BASE] : 0;
    if (t->abi == ABI_LINUX)
        return linux_name(num);
    if (num < 49 && aos_names[num].name) return &aos_names[num];
    return 0;
}

static unsigned int hex_str(unsigned int v, char *dst, unsigned int cap) {
    const char *hex = "0123456789abcdef";
    char tmp[16];
    unsigned int n = 0;
    unsigned int started = 0;
    for (int s = 28; s >= 0; s -= 4) {
        unsigned int d = (v >> s) & 0xF;
        if (d || started || s == 0) { tmp[n++] = hex[d]; started = 1; }
    }
    if (n > cap) n = cap;
    for (unsigned int i = 0; i < n; i++) dst[i] = tmp[i];
    return n;
}

static unsigned int dec_str(unsigned int v, char *dst, unsigned int cap) {
    char tmp[16];
    unsigned int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    unsigned int out = 0;
    for (unsigned int i = n; i > 0 && out < cap;) {
        i--;
        dst[out++] = tmp[i];
    }
    return out;
}

// Render one record as "name(a0, a1, ...) = 0xret\n" into dst (dcap bytes).
// Exactly nargs args are printed. Returns characters written (NUL-terminated).
// Lines fit comfortably in 96 bytes (longest ~90), so no truncation occurs.
static unsigned int emit_rec(const struct task *t, const struct trace_rec *rec,
                             char *dst, unsigned int dcap) {
    const struct trace_name *tn = trace_name_for(t, rec->num);
    unsigned int i = 0;
    if (tn) {
        const char *nm = tn->name;
        while (*nm && i + 1 < dcap) dst[i++] = *nm++;
    } else {
        const char *p = "syscall_0x";
        while (*p && i + 1 < dcap) dst[i++] = *p++;
        i += hex_str(rec->num, dst + i, dcap - i);
    }
    if (i + 1 < dcap) dst[i++] = '(';
    unsigned int nargs = tn ? tn->nargs : 5;
    unsigned int args[5] = { rec->a0, rec->a1, rec->a2, rec->a3, rec->a4 };
    for (unsigned int k = 0; k < nargs; k++) {
        if (k) {
            if (i + 1 < dcap) dst[i++] = ',';
            if (i + 1 < dcap) dst[i++] = ' ';
        }
        if (i + 2 < dcap) dst[i++] = '0';
        if (i + 2 < dcap) dst[i++] = 'x';
        i += hex_str(args[k], dst + i, dcap - i);
    }
    if (i + 1 < dcap) dst[i++] = ')';
    const char *eq = " = 0x";
    while (*eq && i + 1 < dcap) dst[i++] = *eq++;
    i += hex_str(rec->ret, dst + i, dcap - i);
    if (i + 1 < dcap) dst[i++] = '\n';
    if (i < dcap) dst[i] = '\0';
    return i;
}

static unsigned int emit_header(unsigned int pid, char *line, unsigned int cap) {
    const char *h = "== pid ";
    unsigned int n = 0;
    while (*h && n + 1 < cap) line[n++] = *h++;
    n += dec_str(pid, line + n, cap - n - 1);
    if (n + 3 < cap) { line[n++] = ' '; line[n++] = '='; line[n++] = '='; line[n++] = '\n'; }
    return n;
}

static unsigned int emit_overwritten(unsigned int lost, char *line,
                                     unsigned int cap) {
    const char *a = "... ";
    const char *b = " records overwritten\n";
    unsigned int n = 0;
    while (*a && n + 1 < cap) line[n++] = *a++;
    n += dec_str(lost, line + n, cap - n - 1);
    while (*b && n + 1 < cap) line[n++] = *b++;
    return n;
}

// Record / finish: the ring is written only by the owning task's own syscall
// context (single CPU, IF=0), so no locking is needed.
void trace_record(struct registers *r) {
    struct task *t = get_current_task();
    if (!t->trace_on) return;
    if (!t->trace_buf) {
        t->trace_buf = kmalloc(TRACE_MAX * sizeof(struct trace_rec));
        if (!t->trace_buf) { t->trace_on = 0; return; }
        t->trace_head = 0;
        t->trace_count = 0;
        t->trace_wrapped = 0;
    }
    struct trace_rec *ring = (struct trace_rec *)t->trace_buf;
    struct trace_rec *rec = &ring[t->trace_head];
    rec->num = r->eax;
    rec->a0 = r->ebx;
    rec->a1 = r->ecx;
    rec->a2 = r->edx;
    rec->a3 = r->esi;
    rec->a4 = r->edi;
    rec->ret = 0;
    t->trace_head = (t->trace_head + 1) % TRACE_MAX;
    if (t->trace_count < TRACE_MAX)
        t->trace_count++;
    else
        t->trace_wrapped = 1;
}

void trace_finish(struct registers *r) {
    struct task *t = get_current_task();
    if (!t->trace_on || !t->trace_buf) return;
    unsigned int slot = (t->trace_head + TRACE_MAX - 1) % TRACE_MAX;
    ((struct trace_rec *)t->trace_buf)[slot].ret = r->eax;
}

// Line emitter: kernel-side dumps (old shell path) write straight to the
// console; userland strace dumps go through the CALLER's stdout route so the
// trace shows up in the GUI terminal (setout sink), a redirect or a pipe.
typedef void (*trace_emit_fn)(const char *buf, unsigned int len);

static void emit_route(const char *buf, unsigned int len) {
    route_text(buf, len);
}

// Session dump: uses the given emitter, NOT printf, so the dump does not feed
// back into the klog ring.
static void dump_one(struct task *t, trace_emit_fn emit) {
    if (!t->trace_on || !t->trace_buf || t->trace_dumped) return;
    char line[96];
    unsigned int n = emit_header(t->pid, line, sizeof(line));
    emit(line, n);
    unsigned int total = t->trace_wrapped ? TRACE_MAX : t->trace_count;
    unsigned int start = t->trace_wrapped ? t->trace_head : 0;
    for (unsigned int i = 0; i < total; i++) {
        const struct trace_rec *rec =
            (const struct trace_rec *)t->trace_buf + (start + i) % TRACE_MAX;
        n = emit_rec(t, rec, line, sizeof(line));
        emit(line, n);
    }
    if (t->trace_wrapped) {
        n = emit_overwritten(t->trace_count - TRACE_MAX, line, sizeof(line));
        emit(line, n);
    }
    t->trace_dumped = 1;
    // Only dead tasks lose their buffer here; a live child keeps tracing so
    // /proc/<pid>/trace keeps working (its buffer is freed on task exit).
    if (t->state == TASK_ZOMBIE || t->state == TASK_FREE) {
        kfree(t->trace_buf);
        t->trace_buf = 0;
    }
}

void trace_session_dump(void) {
    serial_print("TDMP:start\n");
    for (unsigned int i = 0; i < MAX_TASKS; i++) {
        struct task *t = task_slot(i);
        if (t) dump_one(t, terminal_write);
    }
    serial_print("TDMP:done\n");
}

// Userland strace(1) path (AOS_TRACE_DUMP): dump every traced-undumped task
// of the session owned by `root` (the caller's pid), ascending pid. Lines go
// through route_text: the caller's redirect/pipe when stdout_fd is set, its
// GUI terminal sink otherwise, console as the last resort. Slots the kernel
// force-freed at a parent's exit keep their trace fields until reuse, so
// reparented grandchildren are still collected here.
unsigned int trace_session_dump_root(unsigned int root) {
    unsigned int dumped = 0;
    for (unsigned int i = 0; i < MAX_TASKS; i++) {
        struct task *t = task_slot(i);
        if (!t || !t->trace_on || !t->trace_buf || t->trace_dumped) continue;
        if (t->trace_root != root && t->pid != root) continue;
        // Never dump the tracer itself: its flag was already cleared and it
        // only holds a few of its own setup records.
        if (t->pid == root) continue;
        dump_one(t, emit_route);
        dumped++;
    }
    return dumped;
}

// Live render (procfs path): runs in the reader's syscall context, IF=0, so
// the copy is atomic on this single CPU.
struct render_ctx {
    char *dst;
    unsigned int cap;
    unsigned int off;
    unsigned int total;
    unsigned int copied;
};

static void rpush(struct render_ctx *c, const char *s, unsigned int n) {
    unsigned int start = c->total;
    c->total += n;
    if (c->total <= c->off) return;
    unsigned int skip = c->off > start ? c->off - start : 0;
    unsigned int take = n - skip;
    if (take > c->cap - c->copied) take = c->cap - c->copied;
    for (unsigned int i = 0; i < take; i++)
        c->dst[c->copied + i] = s[skip + i];
    c->copied += take;
}

unsigned int trace_render_at(unsigned int pid, unsigned int off, void *dst,
                             unsigned int cap, unsigned int *total_out) {
    struct task *t = pid < MAX_TASKS ? task_slot(pid) : 0;
    if (!t || !t->trace_on || !t->trace_buf) {
        if (total_out) *total_out = 0;
        return 0;
    }
    struct render_ctx c = { (char *)dst, cap, off, 0, 0 };
    char line[96];
    unsigned int n = emit_header(t->pid, line, sizeof(line));
    rpush(&c, line, n);
    unsigned int total = t->trace_wrapped ? TRACE_MAX : t->trace_count;
    unsigned int start = t->trace_wrapped ? t->trace_head : 0;
    for (unsigned int i = 0; i < total; i++) {
        const struct trace_rec *rec =
            (const struct trace_rec *)t->trace_buf + (start + i) % TRACE_MAX;
        n = emit_rec(t, rec, line, sizeof(line));
        rpush(&c, line, n);
    }
    if (t->trace_wrapped) {
        n = emit_overwritten(t->trace_count - TRACE_MAX, line, sizeof(line));
        rpush(&c, line, n);
    }
    if (total_out) *total_out = c.total;
    return c.copied;
}
