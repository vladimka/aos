#include <sched.h>
#include <unistd.h>
#include "gui.h"
#include "theme.h"

unsigned int *gui_win;
int gui_w, gui_h;

static struct gui_app app;
static unsigned int winid;

void gui_init(struct gui_app *a) {
    app = *a;
}

void gui_run(void) {
    unsigned int my = (unsigned int)getpid();
    struct aos_msg m = {MSG_CREATE,
                        (unsigned int)app.width,
                        (unsigned int)app.height,
                        my, (unsigned int)app.title};
    aos_send((unsigned int)aos_get_event_pid(), &m);

    for (;;) {
        if (aos_recv(&m) == 0 && m.type == MSG_WININFO) {
            winid = m.a;
            gui_win = (unsigned int *)(AOS_SLAB_BASE + m.b * AOS_SLAB_SIZE);
            break;
        }
        sched_yield();
    }
    gui_w = app.width;
    gui_h = app.height;

    theme_load();

    if (app.init) app.init();
    if (app.render) app.render();

    unsigned int last_tick = 0;
    for (;;) {
        int dirty = 0;
        while (aos_recv(&m) == 0) {
            dirty = 1;
            if (m.type == MSG_CLOSE) return;
            if (m.type == MSG_KEY && app.on_key) app.on_key((int)m.a);
            if (m.type == MSG_WHEEL && app.on_scroll)
                app.on_scroll((int)m.a);
        }
        if (app.on_poll) app.on_poll();
        if (app.timer_ms > 0 && app.on_timer) {
            unsigned int now = aos_get_tick();
            if (now - last_tick >= (unsigned int)app.timer_ms) {
                last_tick = now;
                app.on_timer();
                dirty = 1;
            }
        }
        if (dirty && app.render) app.render();
        sched_yield();
    }
}

void gui_update(void) {
    struct aos_msg m = {MSG_UPDATE, winid, 0, 0, 0};
    aos_send((unsigned int)aos_get_event_pid(), &m);
}

void gui_fill(int x, int y, int w, int h, unsigned int rgb) {
    aos_fill(gui_win, (unsigned int)gui_w * 4, x, y, w, h, rgb);
}

void gui_text(int x, int y, const char *s, unsigned int fg, unsigned int bg) {
    aos_render_text(gui_win, (unsigned int)gui_w * 4, x, y, s, fg, bg);
}

int gui_utf8_encode(char *out, unsigned int cp) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}
