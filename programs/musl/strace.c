#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aosabi.h"

// strace <prog> [args]: run <prog> as a child with syscall tracing enabled
// and print the collected traces of the child and every descendant that
// inherited the flag (strace -f semantics).
//
// Mechanics: we raise OUR trace flag before spawning, so the child inherits
// it atomically at spawn time (no early syscalls are missed) and passes it on
// to its own children together with the session marker (trace_root = our pid,
// kernel-side). After the spawn we drop the flag again. Once the child is a
// zombie we ask the kernel (AOS_TRACE_DUMP) to dump every undumped member of
// our session -- this works even for grandchildren whose parent already exited
// and was reparented to init -- then reap the child.
#define ARGS_MAX 256
#define POLL_MS  5
#define POLL_MAX 2000

static int file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

int main(int argc, char **argv) {
    if (argc < 2 || !argv[1][0]) {
        printf("\nusage: strace <prog> [args]");
        return 1;
    }

    // Resolve prog: raw path first, then the usual PATH dirs.
    const char *cands[3];
    char with_bin[64], with_abs[64];
    cands[0] = argv[1];
    snprintf(with_bin, sizeof(with_bin), "bin/%s", argv[1]);
    snprintf(with_abs, sizeof(with_abs), "/bin/%s", argv[1]);
    cands[1] = with_bin;
    cands[2] = with_abs;
    const char *prog = 0;
    for (int i = 0; i < 3; i++) {
        if (file_exists(cands[i])) { prog = cands[i]; break; }
    }
    if (!prog) prog = argv[1];      // let spawn report the failure

    // Rebuild the argument string (everything after argv[1]).
    char args[ARGS_MAX];
    unsigned int alen = 0;
    args[0] = '\0';
    for (int i = 2; i < argc && alen + 1 < sizeof(args); i++) {
        if (i > 2 && alen + 1 < sizeof(args)) args[alen++] = ' ';
        for (const char *p = argv[i]; *p && alen + 1 < sizeof(args); p++)
            args[alen++] = *p;
    }
    args[alen] = '\0';

    // Trace flag on self -> inherited by the child at spawn (race-free).
    aos_trace_set(1);
    int pid = aos_spawn(prog, args, 0);
    aos_trace_set(0);
    if (pid <= 0) {
        printf("\nFailed to load: %s", argv[1]);
        return 1;
    }

    // Wait for the child to become a zombie WITHOUT reaping it: a zombie
    // keeps its trace buffer until dumped, but waitpid would free the slot.
    char path[32];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    char stbuf[128];
    int exited = 0;
    for (int i = 0; i < POLL_MAX; i++) {
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            ssize_t rn = read(fd, stbuf, sizeof(stbuf) - 1);
            close(fd);
            if (rn > 0) {
                stbuf[rn] = '\0';
                if (strstr(stbuf, "zomb")) { exited = 1; break; }
            }
        } else {
            break;              // slot gone (reclaimed): nothing left to show
        }
        struct timespec ts = { 0, POLL_MS * 1000000L };
        nanosleep(&ts, 0);
    }

    // Dump every traced task of this session (ascending pid, kernel-side).
    aos_trace_dump((unsigned int)getpid());
    if (!exited)
        printf("\nstrace: %d still running", pid);

    // Reap the child so no zombie lingers.
    if (exited)
        aos_waitpid((unsigned int)pid);
    return 0;
}
