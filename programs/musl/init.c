#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "aosabi.h"

// /bin/init - the first user process. Reads /etc/init.conf, starts every
// service whose mode matches AOS_MODE (gui|text), then babysits them: each
// child death arrives as MSG_EXIT in this task's mailbox; respawn-able
// services are restarted after a short backoff, with a crash-loop guard for
// processes that die instantly again and again.

#define MAX_SVC           8
#define CONF_MAX          1024
#define BACKOFF_TICKS     100    // 1 s pause before a respawn
#define FAST_DEATH_TICKS  100    // life shorter than this counts as a fast death
#define RESET_LIFE_TICKS  6000   // living longer than this resets the counter
#define MAX_FAST_DEATHS   5      // fast deaths in a row -> give up

struct svc {
    char path[64];
    char args[64];
    int respawn;
    int gui_only;
    int active;
    int failed;
    int pending;              // waiting for the backoff to expire
    unsigned int due_tick;
    int pid;
    unsigned int start_tick;
    int fast_deaths;
};

static struct svc svcs[MAX_SVC];
static int nsvc;              // highest svc index seen in the config

static void say(const char *s) { write(1, s, strlen(s)); }

static void spawn_svc(struct svc *s) {
    s->pending = 0;
    int pid = aos_spawn(s->path, s->args, 0);
    char b[128];
    if (pid <= 0) {
        int n = snprintf(b, sizeof b, "init: spawn failed: %s\r\n", s->path);
        write(1, b, (size_t)n);
        return;
    }
    s->active = 1;
    s->pid = pid;
    s->start_tick = aos_get_tick();
    int n = snprintf(b, sizeof b, "init: started %s (pid %d)\r\n", s->path, pid);
    write(1, b, (size_t)n);
}

static void load_conf(void) {
    int fd = open("/etc/init.conf", O_RDONLY, 0);
    if (fd < 0) { say("init: cannot open /etc/init.conf\r\n"); return; }
    static char buf[CONF_MAX];
    int n = (int)read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    struct svc *cur = 0;
    for (char *line = strtok(buf, "\n"); line; line = strtok(0, "\n")) {
        char *cr = strchr(line, '\r');
        if (cr) *cr = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *key = line, *val = eq + 1;
        if (strncmp(key, "svc", 3) != 0) continue;
        char *dot = strchr(key + 3, '.');
        if (!dot) continue;
        int idx = atoi(key + 3);
        if (idx < 1 || idx > MAX_SVC) continue;
        cur = &svcs[idx - 1];
        dot++;
        if (strcmp(dot, "path") == 0) {
            strncpy(cur->path, val, sizeof cur->path - 1);
            cur->path[sizeof cur->path - 1] = 0;
            if (idx > nsvc) nsvc = idx;
        } else if (strcmp(dot, "args") == 0) {
            strncpy(cur->args, val, sizeof cur->args - 1);
            cur->args[sizeof cur->args - 1] = 0;
        } else if (strcmp(dot, "respawn") == 0) {
            cur->respawn = atoi(val);
        } else if (strcmp(dot, "mode") == 0) {
            cur->gui_only = (strcmp(val, "gui") == 0);
        }
    }
}

int main(void) {
    load_conf();
    const char *mode = getenv("AOS_MODE");
    int gui = mode && strcmp(mode, "gui") == 0;
    char b[80];
    snprintf(b, sizeof b, "init: started (pid %d, mode %s)\r\n",
             (int)getpid(), gui ? "gui" : "text");
    say(b);
    for (int i = 0; i < nsvc; i++) {
        struct svc *s = &svcs[i];
        if (!s->path[0]) continue;
        if (s->gui_only && !gui) continue;
        spawn_svc(s);
    }
    struct aos_msg m;
    for (;;) {
        if (aos_recv(&m) == 0 && m.type == MSG_EXIT) {
            for (int i = 0; i < nsvc; i++) {
                struct svc *s = &svcs[i];
                if (!s->active || s->pid != (int)m.a) continue;
                int code = aos_waitpid((unsigned int)m.a);
                s->active = 0;
                s->pid = 0;
                unsigned int now = aos_get_tick();
                unsigned int life = now - s->start_tick;
                char lb[128];
                int ln = snprintf(lb, sizeof lb,
                                  "init: exited %s (code %d, life %u ticks)\r\n",
                                  s->path, code, life);
                write(1, lb, (size_t)ln);
                if (life > RESET_LIFE_TICKS) s->fast_deaths = 0;
                else if (life < FAST_DEATH_TICKS) s->fast_deaths++;
                if (s->respawn && !s->failed) {
                    if (s->fast_deaths >= MAX_FAST_DEATHS) {
                        s->failed = 1;
                        ln = snprintf(lb, sizeof lb,
                                      "init: %s crashed %dx fast, giving up\r\n",
                                      s->path, s->fast_deaths);
                        write(1, lb, (size_t)ln);
                    } else {
                        s->pending = 1;
                        s->due_tick = now + BACKOFF_TICKS;
                        ln = snprintf(lb, sizeof lb,
                                      "init: respawn %s scheduled\r\n", s->path);
                        write(1, lb, (size_t)ln);
                    }
                }
            }
            continue;
        }
        unsigned int now = aos_get_tick();
        for (int i = 0; i < nsvc; i++) {
            struct svc *s = &svcs[i];
            if (s->pending && !s->failed && (int)(now - s->due_tick) >= 0)
                spawn_svc(s);
        }
        usleep(10000);
    }
    return 0;
}
