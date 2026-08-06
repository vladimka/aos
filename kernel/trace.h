#ifndef TRACE_H
#define TRACE_H

// Syscall tracing (strace). Per-task state lives in struct task (trace_on,
// trace_buf, ...); the three syscall dispatch handlers wrap themselves with
// trace_record()/trace_finish(). Rendering (name tables, hex args) and the
// shell/`/proc/<pid>/trace` dump live here.

struct registers;
struct task;

#define TRACE_MAX 512

struct trace_rec {
    unsigned int num;
    unsigned int a0, a1, a2, a3, a4;
    unsigned int ret;
};

// Entry/exit record points for the three ABI handlers.
void trace_record(struct registers *r);
void trace_finish(struct registers *r);

// Print (to the kernel terminal) the trace of every traced, undumped task in
// pid order and mark them dumped. Freed here only for dead tasks.
void trace_session_dump(void);

// Render task `pid`'s ring starting at virtual offset `off` into `dst` (up to
// `cap` bytes); returns bytes copied (0 at/past the end or untraced). When
// `total_out` is non-NULL it receives the full virtual size.
unsigned int trace_render_at(unsigned int pid, unsigned int off, void *dst,
                             unsigned int cap, unsigned int *total_out);

#endif
