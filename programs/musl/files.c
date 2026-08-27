#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "gui.h"
#include "theme.h"

#define WIN_W    640
#define WIN_H    416
#define HEADER_H 24
#define ROW_H    20
#define MAX_ENT  128
#define PATH_MAX 128

enum { E_DIR = 0, E_FILE = 1 };

static struct entry {
    char name[28];
    int kind;
    unsigned int size;
} ents[MAX_ENT];
static int nents;
static int sel;
static int scroll;
static int view_h;

static char cwd[PATH_MAX];

static unsigned int col_hdr_bg;
static unsigned int col_hdr_fg;
static unsigned int col_list_bg;
static unsigned int col_list_fg;
static unsigned int col_sel_bg;
static unsigned int col_sel_fg;
static unsigned int col_dir_fg;
static unsigned int col_size_fg;

static void load_dir(const char *path) {
    DIR *d = opendir(path);
    nents = 0;
    sel = 0;
    scroll = 0;
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && nents < MAX_ENT) {
        const char *nm = de->d_name;
        if (nm[0] == '.' && nm[1] == 0) continue;
        if (nm[0] == '.' && nm[1] == '.' && nm[2] == 0) continue;
        int len = 0;
        while (nm[len] && len < 27) { ents[nents].name[len] = nm[len]; len++; }
        ents[nents].name[len] = 0;
        char full[PATH_MAX];
        int p = 0;
        while (path[p] && p < PATH_MAX - 32) { full[p] = path[p]; p++; }
        if (p > 1) full[p++] = '/';
        int q = 0;
        while (nm[q] && p < PATH_MAX - 1) { full[p++] = nm[q++]; }
        full[p] = 0;
        struct stat st;
        if (stat(full, &st) == 0) {
            ents[nents].kind = (st.st_mode & 0170000) == 0040000 ? E_DIR : E_FILE;
            ents[nents].size = (unsigned int)st.st_size;
        } else {
            ents[nents].kind = E_FILE;
            ents[nents].size = 0;
        }
        nents++;
    }
    closedir(d);
    /* sort: dirs first, then alphabetical */
    for (int i = 0; i < nents - 1; i++)
        for (int j = i + 1; j < nents; j++) {
            int swap = 0;
            if (ents[j].kind < ents[i].kind) swap = 1;
            else if (ents[j].kind == ents[i].kind) {
                const char *a = ents[i].name;
                const char *b = ents[j].name;
                while (*a && *a == *b) { a++; b++; }
                if ((unsigned char)*a > (unsigned char)*b) swap = 1;
            }
            if (swap) {
                struct entry tmp = ents[i];
                ents[i] = ents[j];
                ents[j] = tmp;
            }
        }
}

static void enter_dir(const char *name) {
    char next[PATH_MAX];
    int p = 0;
    while (cwd[p] && p < PATH_MAX - 32) { next[p] = cwd[p]; p++; }
    if (p > 1) next[p++] = '/';
    int q = 0;
    while (name[q] && p < PATH_MAX - 1) { next[p++] = name[q++]; }
    next[p] = 0;
    struct stat st;
    if (stat(next, &st) == 0 && (st.st_mode & 0170000) == 0040000) {
        int i = 0;
        while (next[i] && i < PATH_MAX - 1) { cwd[i] = next[i]; i++; }
        cwd[i] = 0;
        load_dir(cwd);
    }
}

static void go_parent(void) {
    int len = 0;
    while (cwd[len]) len++;
    if (len <= 1) return;
    /* strip trailing slash */
    if (cwd[len - 1] == '/') len--;
    /* find last slash */
    int last = -1;
    for (int i = 0; i < len; i++)
        if (cwd[i] == '/') last = i;
    if (last < 0) { cwd[0] = '/'; cwd[1] = 0; }
    else if (last == 0) { cwd[1] = 0; }
    else { cwd[last] = 0; }
    load_dir(cwd);
}

static void ensure_visible(void) {
    if (sel < 0) sel = 0;
    if (sel >= nents) sel = nents > 0 ? nents - 1 : 0;
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + view_h) scroll = sel - view_h + 1;
    int max_s = nents - view_h;
    if (max_s < 0) max_s = 0;
    if (scroll > max_s) scroll = max_s;
    if (scroll < 0) scroll = 0;
}

static void draw_header(void) {
    gui_fill(0, 0, WIN_W, HEADER_H, col_hdr_bg);
    /* folder icon: simplified */
    gui_text(4, 4, ">", col_dir_fg, col_hdr_bg);
    /* path */
    gui_text(20, 4, cwd, col_hdr_fg, col_hdr_bg);
    /* item count */
    static char cnt[32];
    int n = 0;
    cnt[n++] = '[';
    if (nents < 100) { cnt[n++] = '0' + nents / 10; cnt[n++] = '0' + nents % 10; }
    else { cnt[n++] = '0' + (nents / 100) % 10; cnt[n++] = '0' + (nents / 10) % 10; cnt[n++] = '0' + nents % 10; }
    cnt[n++] = ']';
    cnt[n] = 0;
    gui_text(WIN_W - n * FONT_W - 8, 4, cnt, col_size_fg, col_hdr_bg);
    /* accent line under header */
    gui_fill(0, HEADER_H - 1, WIN_W, 1, col_dir_fg);
}

static void draw_list(void) {
    gui_fill(0, HEADER_H, WIN_W, WIN_H - HEADER_H, col_list_bg);
    ensure_visible();
    for (int i = 0; i < view_h; i++) {
        int idx = scroll + i;
        int y = HEADER_H + i * ROW_H;
        if (idx < nents) {
            int bg = (idx == sel) ? col_sel_bg : col_list_bg;
            int fg = (idx == sel) ? col_sel_fg : col_list_fg;
            if (idx != sel && ents[idx].kind == E_DIR) fg = col_dir_fg;
            if (idx == sel && ents[idx].kind == E_DIR) fg = col_dir_fg;
            /* selection highlight */
            if (idx == sel)
                gui_fill(0, y, WIN_W, ROW_H, col_sel_bg);
            /* icon */
            const char *icon = ents[idx].kind == E_DIR ? ">" : "-";
            gui_text(8, y + 2, icon, col_dir_fg, bg);
            /* name */
            gui_text(24, y + 2, ents[idx].name, fg, bg);
            /* size (files only) */
            if (ents[idx].kind == E_FILE) {
                static char sz[16];
                unsigned int s = ents[idx].size;
                int p = 15;
                sz[p] = 0;
                if (s == 0) { sz[--p] = '0'; }
                else {
                    while (s > 0 && p > 0) { sz[--p] = '0' + s % 10; s /= 10; }
                }
                gui_text(WIN_W - (15 - p) * FONT_W - 8, y + 2, sz + p, col_size_fg, bg);
            } else {
                /* dir marker */
                gui_text(WIN_W - 3 * FONT_W - 8, y + 2, "DIR", col_dir_fg, bg);
            }
        }
    }
    /* scrollbar */
    if (nents > view_h) {
        int sb_x = WIN_W - 6;
        int sb_h = WIN_H - HEADER_H;
        gui_fill(sb_x, HEADER_H, 6, sb_h, col_list_bg);
        int thumb_h = (view_h * sb_h) / nents;
        if (thumb_h < 8) thumb_h = 8;
        int thumb_y = HEADER_H + (scroll * (sb_h - thumb_h)) / (nents - view_h + 1);
        gui_fill(sb_x + 1, thumb_y, 4, thumb_h, col_dir_fg);
    }
}

static void do_render(void) {
    view_h = (WIN_H - HEADER_H) / ROW_H;
    draw_header();
    draw_list();
    gui_update();
}

static void activate(void) {
    if (sel < 0 || sel >= nents) return;
    if (ents[sel].kind == E_DIR) {
        enter_dir(ents[sel].name);
    } else {
        /* open file in notepad */
        char args[80];
        int n = 0;
        const char *p = cwd;
        while (*p && n < 70) args[n++] = *p++;
        if (n > 1 && args[n - 1] != '/') args[n++] = '/';
        p = ents[sel].name;
        while (*p && n < 78) args[n++] = *p++;
        args[n] = 0;
        aos_spawn("bin/notepad", args, (unsigned int)aos_get_event_pid());
    }
}

static void on_key(int k) {
    switch (k) {
    case 0x0101: /* up */
        if (sel > 0) sel--;
        break;
    case 0x0102: /* down */
        if (sel < nents - 1) sel++;
        break;
    case 0x0105: /* home */
        sel = 0;
        break;
    case 0x0106: /* end */
        sel = nents > 0 ? nents - 1 : 0;
        break;
    case 0x0108: /* pgup */
        sel -= view_h;
        if (sel < 0) sel = 0;
        break;
    case 0x0109: /* pgdn */
        sel += view_h;
        if (sel >= nents) sel = nents > 0 ? nents - 1 : 0;
        break;
    case '\r': /* enter */
        activate();
        break;
    case '\b': /* backspace = parent dir */
        go_parent();
        break;
    }
}

static void on_scroll(int delta) {
    sel -= delta;
    if (sel < 0) sel = 0;
    if (sel >= nents) sel = nents > 0 ? nents - 1 : 0;
}

static void on_init(void) {
    col_hdr_bg = theme_color("theme_dock_bg", 0x232C40);
    col_hdr_fg = theme_color("theme_text_fg", 0xD8D8D8);
    col_list_bg = theme_color("theme_text_bg", 0x101010);
    col_list_fg = theme_color("theme_text_fg", 0xD8D8D8);
    col_sel_bg = theme_color("theme_accent", 0x5B93D8);
    col_sel_fg = 0xFFFFFF;
    col_dir_fg = theme_color("theme_title_focus", 0x4E86C7);
    col_size_fg = 0x888888;
    cwd[0] = '/'; cwd[1] = 0;
    load_dir(cwd);
}

void main(void) {
    struct gui_app app = {
        .title = "Files",
        .width = WIN_W, .height = WIN_H,
        .init = on_init,
        .render = do_render,
        .on_key = on_key,
        .on_scroll = on_scroll,
    };
    gui_init(&app);
    gui_run();
}
