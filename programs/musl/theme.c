#include <fcntl.h>
#include <unistd.h>
#include "theme.h"

// Defaults must match the generated sys/config.cfg (kernel/config.c).
static unsigned int v_title = 0x263C5E;
static unsigned int v_title_focus = 0x4E86C7;
static unsigned int v_border = 0x12161F;
static unsigned int v_border_focus = 0x6B9BD2;
static unsigned int v_dock_bg = 0x232C40;
static unsigned int v_accent = 0x5B93D8;
static unsigned int v_menu_bg = 0x20283A;
static unsigned int v_menu_fg = 0xFFFFFF;
static unsigned int v_text_fg = 0xD8D8D8;
static unsigned int v_text_bg = 0x101010;
static unsigned int v_top = 0x1A2030;
static unsigned int v_bot = 0x0E1620;

struct th_entry {
    const char *key;
    unsigned int *val;
};

static struct th_entry entries[] = {
    { "theme_title", &v_title },
    { "theme_title_focus", &v_title_focus },
    { "theme_border", &v_border },
    { "theme_border_focus", &v_border_focus },
    { "theme_dock_bg", &v_dock_bg },
    { "theme_accent", &v_accent },
    { "theme_menu_bg", &v_menu_bg },
    { "theme_menu_fg", &v_menu_fg },
    { "theme_text_fg", &v_text_fg },
    { "theme_text_bg", &v_text_bg },
    { "wallpaper_top", &v_top },
    { "wallpaper_bot", &v_bot },
};
#define NENT (int)(sizeof(entries) / sizeof(entries[0]))

static int strequal(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

// Parse a hex color. Returns 0 and stores *out on success; -1 (leaving the
// caller's default intact) when the value has no hex digits or trailing
// garbage.
static int parse_hex(const char *s, unsigned int *out) {
    unsigned int v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    int n = 0;
    for (;;) {
        char c = *s;            // peek; never consume the terminator
        unsigned int d;
        if (c >= '0' && c <= '9') d = (unsigned int)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned int)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned int)(c - 'A' + 10);
        else break;
        s++;
        v = v * 16 + d;
        n++;
    }
    if (n == 0 || *s != 0) return -1;
    *out = v;
    return 0;
}

void theme_load(void) {
    static char seen[NENT];
    char buf[512];
    int fd = open("sys/config.cfg", O_RDONLY);
    if (fd < 0) return;
    int sz = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (sz <= 0) return;
    buf[sz] = 0;
    char *p = buf;
    while (p && *p) {
        char *eol = p;
        while (*eol && *eol != '\n') eol++;
        if (eol > p && eol[-1] == '\r') eol[-1] = 0;
        char saved = *eol;
        *eol = 0;
        char *k = p;
        while (*k == ' ' || *k == '\t') k++;
        if (*k != '#' && *k != 0) {
            char *eq = k;
            while (*eq && *eq != '=') eq++;
            if (*eq == '=') {
                *eq = 0;
                const char *val = eq + 1;
                while (*val == ' ' || *val == '\t') val++;
                for (int i = 0; i < NENT; i++)
                    if (!seen[i] && strequal(entries[i].key, k)) {
                        unsigned int hv;
                        if (parse_hex(val, &hv) == 0) {
                            *entries[i].val = hv;
                            seen[i] = 1;
                        }
                        break;
                    }
            }
        }
        *eol = saved;
        if (saved == '\n') p = eol + 1;
        else break;
    }
}

unsigned int theme_color(const char *key, unsigned int fallback) {
    for (int i = 0; i < NENT; i++)
        if (strequal(entries[i].key, key)) return *entries[i].val;
    return fallback;
}
