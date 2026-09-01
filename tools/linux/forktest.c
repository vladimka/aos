/* forktest: Stage-1 regression for AOS Linux fork(2) (syscall 2) and dup2(2)
 * (syscall 63).
 *
 *  1. fork() returns 0 in the child and the child's pid in the parent.
 *  2. The child and parent each run independently (both print a marker and
 *     report their own getpid(), which must differ).
 *  3. dup2 duplicate a pipe write end onto a distinct fd and echo through it.
 *
 * Prints "FORKTEST OK" only if every sub-check passes. Prints FAIL otherwise.
 */
#include <stdio.h>
#include <unistd.h>
#include <string.h>

static int fail;

static void chk(int ok, const char *what) {
    if (!ok) {
        printf("FAIL: %s\n", what);
        fail = 1;
    }
}

int main(void) {
    /* --- dup2 with a pipe --- */
    int p[2];
    if (pipe(p) != 0) {
        printf("FAIL: pipe\n");
        return 1;
    }
    if (dup2(p[1], 7) != 7) {
        printf("FAIL: dup2\n");
        return 1;
    }
    write(7, "Z", 1);      /* write via the new fd 7 */
    close(p[1]);           /* keep only fd 7 for the write end */
    char c = 0;
    if (read(p[0], &c, 1) != 1 || c != 'Z') {
        printf("FAIL: dup2 echo\n");
        return 1;
    }
    close(p[0]);
    close(7);
    printf("dup2: OK\n");

    /* --- fork --- */
    pid_t pid = fork();
    if (pid < 0) {
        printf("FAIL: fork returned %d\n", (int)pid);
        return 1;
    }

    if (pid == 0) {
        /* child */
        printf("CHILD pid=%d parent=%d\n", (int)getpid(), (int)getppid());
        return 0; /* exit code 0; validated by the harness via CHILD line */
    }

    /* parent */
    printf("PARENT pid=%d child=%d\n", (int)getpid(), (int)pid);
    chk(pid > 0, "parent got positive child pid");
    chk(pid != (int)getpid(), "child pid differs from parent pid");

    printf("%s\n", fail ? "FORKTEST FAIL" : "FORKTEST OK");
    return fail ? 1 : 0;
}
