/* sigtest: Stage-3 regression for AOS Linux signal syscalls:
 *   rt_sigaction (174)          - register a handler for SIGUSR1
 *   tkill (238) / raise         - self-signal delivery
 *   handler frame + rt_sigreturn(173)
 *   sigprocmask (175)           - block SIGUSR2, verify it stays pending
 * Prints SIGTEST OK only if every check passes.
 */
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static volatile int sigcnt = 0;
static volatile int got_sig = 0;

static void handler(int s) {
    got_sig = s;
    sigcnt++;
    printf("  in handler sig=%d cnt=%d\n", s, sigcnt);
}

int main(void) {
    printf("sigtest start pid=%d\n", (int)getpid());

    printf("push SIGUSR1 handler...\n");
    if (signal(SIGUSR1, handler) == SIG_ERR) {
        printf("FAIL signal(SIGUSR1)\n");
        printf("SIGTEST FAIL\n");
        return 1;
    }

    printf("raise(SIGUSR1)...\n");
    raise(SIGUSR1);
    printf("after raise sigcnt=%d got=%d\n", sigcnt, got_sig);
    if (sigcnt != 1 || got_sig != SIGUSR1) {
        printf("SIGTEST FAIL (usrcount)\n");
        return 1;
    }

    /* block SIGUSR2, raise it, then unblock and expect the handler to fire
     * exactly once (pending delivered after sigprocmask). */
    printf("block SIGUSR2...\n");
    sigset_t set, old;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    if (sigprocmask(SIG_BLOCK, &set, &old) != 0) {
        printf("SIGTEST FAIL (sigprocmask block)\n");
        return 1;
    }

    printf("raise(SIGUSR2) while blocked...\n");
    raise(SIGUSR2);                    /* must stay pending */
    if (sigcnt != 1) {
        printf("SIGTEST FAIL (delivered while blocked)\n");
        return 1;
    }

    printf("unblock SIGUSR2...\n");
    if (sigprocmask(SIG_SETMASK, &old, 0) != 0) {
        printf("SIGTEST FAIL (sigprocmask unblock)\n");
        return 1;
    }
    printf("after unblock sigcnt=%d got=%d\n", sigcnt, got_sig);
    if (sigcnt != 2 || got_sig != SIGUSR2) {
        printf("SIGTEST FAIL (pending not delivered)\n");
        return 1;
    }

    printf("SIGTEST OK\n");
    return 0;
}
