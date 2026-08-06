#include "config.h"
#include "vfs.h"
#include "rtc.h"
#include "printf.h"
#include "string.h"
#include "kmm.h"

#define CONFIG_PATH "sys/config.cfg"

#define DEFAULT_TZ   0
#define DEFAULT_TOP  0x1A2030
#define DEFAULT_BOT  0x0E1620

static int tz_min;
static unsigned int wp_top;
static unsigned int wp_bot;

static int parse_int(const char *s) {
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    int v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

static unsigned int parse_hex(const char *s) {
    unsigned int v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (;;) {
        char c = *s++;
        unsigned int d;
        if (c >= '0' && c <= '9') d = (unsigned int)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned int)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned int)(c - 'A' + 10);
        else break;
        v = v * 16 + d;
    }
    return v;
}

static void apply_line(const char *line) {
    while (*line == ' ' || *line == '\t') line++;
    if (*line == 0 || *line == '#') return;
    const char *eq = line;
    while (*eq && *eq != '=') eq++;
    if (*eq != '=') return;
    int klen = 0;
    while (line[klen] != '=') klen++;
    while (klen > 0 && (line[klen - 1] == ' ' || line[klen - 1] == '\t')) klen--;
    const char *val = eq + 1;
    while (*val == ' ' || *val == '\t') val++;
    if (klen == 8 && strncmp(line, "timezone", 8) == 0)
        tz_min = parse_int(val);
    else if (klen == 13 && strncmp(line, "wallpaper_top", 13) == 0)
        wp_top = parse_hex(val);
    else if (klen == 13 && strncmp(line, "wallpaper_bot", 13) == 0)
        wp_bot = parse_hex(val);
}

void config_load(void) {
    tz_min = DEFAULT_TZ;
    wp_top = DEFAULT_TOP;
    wp_bot = DEFAULT_BOT;

    struct aos_stat st;
    if (vfs_kernel_stat(CONFIG_PATH, &st) < 0) {
        static const char def[] =
            "# AOS system config\n"
            "timezone=0\n"
            "wallpaper_top=0x1A2030\n"
            "wallpaper_bot=0x0E1620\n"
            "theme_title=0x263C5E\n"
            "theme_title_focus=0x4E86C7\n"
            "theme_border=0x12161F\n"
            "theme_border_focus=0x6B9BD2\n"
            "theme_dock_bg=0x232C40\n"
            "theme_accent=0x5B93D8\n"
            "theme_menu_bg=0x20283A\n"
            "theme_menu_fg=0xFFFFFF\n"
            "theme_text_fg=0xD8D8D8\n"
            "theme_text_bg=0x101010\n";
        if (vfs_kernel_write(CONFIG_PATH, def, sizeof(def) - 1, 0) >= 0)
            printf("config: created %s\n", CONFIG_PATH);
        else
            printf("config: create %s failed\n", CONFIG_PATH);
    } else {
        printf("config: loaded %s\n", CONFIG_PATH);
    }

    char *buf = kmalloc(512);
    if (buf) {
        int sz = vfs_kernel_read(CONFIG_PATH, buf, 511, 0);
        if (sz > 0) {
            buf[sz] = 0;
            char *p = buf;
            while (p && *p) {
                char *eol = p;
                while (*eol && *eol != '\n') eol++;
                if (eol > p && eol[-1] == '\r') eol[-1] = 0;
                char saved = *eol;
                *eol = 0;
                apply_line(p);
                *eol = saved;
                if (saved == '\n') p = eol + 1;
                else break;
            }
        }
        kfree(buf);
    }

    rtc_set_tz(tz_min);
    if (tz_min != 0)
        printf("config: timezone %s%d\n", tz_min > 0 ? "+" : "", tz_min);
}

int config_tz_min(void) { return tz_min; }
unsigned int config_wallpaper_top(void) { return wp_top; }
unsigned int config_wallpaper_bot(void) { return wp_bot; }
