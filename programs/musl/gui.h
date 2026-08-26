#ifndef GUI_H
#define GUI_H

#include "aosabi.h"

#define FONT_W 8
#define FONT_H 16

struct gui_app {
    const char *title;
    int width, height;
    int timer_ms;
    void (*init)(void);
    void (*render)(void);
    void (*on_key)(int key);
    void (*on_scroll)(int delta);
    void (*on_poll)(void);
    void (*on_timer)(void);
};

extern unsigned int *gui_win;
extern int gui_w, gui_h;

void gui_init(struct gui_app *app);
void gui_run(void);
void gui_update(void);
void gui_fill(int x, int y, int w, int h, unsigned int rgb);
void gui_text(int x, int y, const char *s, unsigned int fg, unsigned int bg);
int gui_utf8_encode(char *out, unsigned int cp);

#endif
