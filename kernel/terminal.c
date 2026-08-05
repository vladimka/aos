#include "terminal.h"
#include "vga.h"
#include "serial.h"
#include "commands.h"
#include "vfs.h"
#include "string.h"
#include "user.h"

static char line_buf[LINE_BUF_SIZE];
static unsigned int line_pos = 0;
static unsigned int cursor_pos = 0;

static int shift_pressed = 0;
static int ctrl_pressed = 0;
static int caps_lock = 0;
static int ru_layout = 0;
static int e0_prefix = 0;

#define PROMPT     "AOS> "
#define PROMPT_LEN 5

// ---- Input queue (for user programs) ----
#define KEY_QUEUE_SIZE 64
static int key_queue[KEY_QUEUE_SIZE];
static unsigned int key_queue_head = 0;
static unsigned int key_queue_tail = 0;

static void key_queue_push(int cp) {
    unsigned int next = (key_queue_tail + 1) % KEY_QUEUE_SIZE;
    if (next == key_queue_head) return;
    key_queue[key_queue_tail] = cp;
    key_queue_tail = next;
}

int terminal_read_key(void) {
    if (key_queue_head == key_queue_tail) return -1;
    int cp = key_queue[key_queue_head];
    key_queue_head = (key_queue_head + 1) % KEY_QUEUE_SIZE;
    return cp;
}

void terminal_reset_keys(void) {
    key_queue_head = 0;
    key_queue_tail = 0;
}

// ---- Command history ----
#define HIST_SIZE 16
static char history[HIST_SIZE][LINE_BUF_SIZE];
static unsigned int hist_count = 0;
static unsigned int hist_next = 0;
static int hist_cur = -1;

static void hist_push(void) {
    if (line_pos == 0) return;
    char *dst = history[hist_next];
    unsigned int i;
    for (i = 0; i < line_pos && i < LINE_BUF_SIZE - 1; i++)
        dst[i] = line_buf[i];
    dst[i] = '\0';
    hist_next = (hist_next + 1) % HIST_SIZE;
    if (hist_count < HIST_SIZE) hist_count++;
}

// ---- US layout ----
static const char us_upper[] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0,
    0, 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0,
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*',
    0, ' ', 0
};

static const char us_lower[] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0,
    0, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0,
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*',
    0, ' ', 0
};

// ---- Russian layout ----
static unsigned short ru_lower(unsigned char sc) {
    static const unsigned short map[] = {
        0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0, 0,
        0x0439, 0x0446, 0x0443, 0x043A, 0x0435, 0x043D, 0x0433, 0x0448,
        0x0449, 0x0437, 0x0445, 0x044A, 0, 0,
        0x0444, 0x044B, 0x0432, 0x0430, 0x043F, 0x0440, 0x043E, 0x043B,
        0x0434, 0x0436, 0x044D, 0x0451, 0, 0x005C,
        0x044F, 0x0447, 0x0441, 0x043C, 0x0438, 0x0442, 0x044C, 0x0431,
        0x044E, '.',
        0, 0, 0, ' ',
    };
    if (sc >= 0x3A) return 0;
    unsigned short cp = map[sc];
    if (cp == 0 && sc >= 0x02 && sc <= 0x0D) cp = us_lower[sc];
    return cp;
}

static unsigned short ru_upper(unsigned char sc) {
    static const unsigned short map[] = {
        0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0, 0,
        0x0419, 0x0426, 0x0423, 0x041A, 0x0415, 0x041D, 0x0413, 0x0428,
        0x0429, 0x0417, 0x0425, 0x042A, 0, 0,
        0x0424, 0x042B, 0x0412, 0x0410, 0x041F, 0x0420, 0x041E, 0x041B,
        0x0414, 0x0416, 0x042D, 0x0401, 0, '/',
        0x042F, 0x0427, 0x0421, 0x041C, 0x0418, 0x0422, 0x042C, 0x0411,
        0x042E, ',',
        0, 0, 0, ' ',
    };
    if (sc >= 0x3A) return 0;
    unsigned short cp = map[sc];
    if (cp == 0 && sc >= 0x02 && sc <= 0x0D) cp = us_upper[sc];
    return cp;
}

// ---- UTF-8 helpers ----
static int cp_to_utf8(unsigned short cp, unsigned char *out) {
    if (cp < 0x80) {
        out[0] = cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = 0xC0 | (cp >> 6);
        out[1] = 0x80 | (cp & 0x3F);
        return 2;
    } else {
        out[0] = 0xE0 | (cp >> 12);
        out[1] = 0x80 | ((cp >> 6) & 0x3F);
        out[2] = 0x80 | (cp & 0x3F);
        return 3;
    }
}

static int utf8_char_len_at(const char *s, unsigned int i) {
    unsigned char c = s[i];
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static int utf8_char_len_rev(const char *s, unsigned int i) {
    if (i == 0) return 0;
    int n = 1;
    while (n < 4 && (s[i - n] & 0xC0) == 0x80) n++;
    return n;
}

static unsigned int utf8_vis_len(const char *s, unsigned int n) {
    unsigned int vis = 0, i = 0;
    while (i < n) {
        int cl = utf8_char_len_at(s, i);
        if (cl <= 0) break;
        i += cl;
        vis++;
    }
    return vis;
}

// ---- scancode mapping ----
static unsigned short map_scancode(unsigned char sc) {
    if (sc >= 0x3A) return 0;
    int use_upper = shift_pressed;
    if (ru_layout)
        return use_upper ? ru_upper(sc) : ru_lower(sc);
    if (caps_lock) {
        char c = us_lower[sc];
        if (c >= 'a' && c <= 'z') use_upper = !use_upper;
    }
    unsigned short cp = use_upper ? (unsigned char)us_upper[sc] : (unsigned char)us_lower[sc];
    return cp;
}

// Translate one PS/2 scancode to a key event. Maintains the shared keyboard
// state (shift/ctrl/caps/ru_layout/E0 prefix). Returns -1 for modifiers,
// releases and unmapped keys, otherwise an ASCII codepoint (or a GUI_KEY_*
// special code for navigation keys).
int terminal_scan_event(unsigned char scancode) {
    if (scancode == 0xE0) {
        e0_prefix = 1;
        return -1;
    }
    int ext = e0_prefix;
    e0_prefix = 0;

    // Shift keys
    if (scancode == 0x2A || scancode == 0x36) {
        shift_pressed = 1;
        if (ctrl_pressed) ru_layout = !ru_layout;
        return -1;
    }
    if (scancode == 0xAA || scancode == 0xB6) {
        shift_pressed = 0;
        return -1;
    }

    // Ctrl keys
    if (!ext && scancode == 0x1D) {
        ctrl_pressed = 1;
        if (shift_pressed) ru_layout = !ru_layout;
        return -1;
    }
    if (!ext && scancode == 0x9D) {
        ctrl_pressed = 0;
        return -1;
    }

    if (scancode & 0x80) return -1;

    if (!ext && scancode == 0x3A) {
        caps_lock = !caps_lock;
        return -1;
    }

    if (ext) {
        switch (scancode) {
        case 0x48: return GUI_KEY_UP;
        case 0x50: return GUI_KEY_DOWN;
        case 0x4B: return GUI_KEY_LEFT;
        case 0x4D: return GUI_KEY_RIGHT;
        case 0x47: return GUI_KEY_HOME;
        case 0x4F: return GUI_KEY_END;
        case 0x53: return GUI_KEY_DEL;
        default: return -1;
        }
    }

    if (scancode == 0x0F) return '\t';
    if (scancode == 0x0E) return '\b';
    if (scancode == 0x1C) return '\r';
    if (scancode == 0x01) return 27;    // Escape

    unsigned short cp = map_scancode(scancode);
    if (!cp) return -1;
    // Ctrl+letter emits the classic ASCII control code (Ctrl+S -> 0x13).
    // Applies to Latin letters only; GUI apps use these (notepad: Ctrl+S).
    if (ctrl_pressed && ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')))
        return (int)(cp & 0x1F);
    return (int)cp;
}

// ---- Line redraw ----
static void line_redraw_from(unsigned int from_byte) {
    vga_cursor_off();
    // position at prompt + visual offset of from_byte
    int from_col = utf8_vis_len(line_buf, from_byte);
    vga_set_cursor(PROMPT_LEN + from_col, vga_get_cursor_y());
    vga_clear_eol();
    unsigned int i = from_byte;
    while (i < line_pos) {
        int cl = utf8_char_len_at(line_buf, i);
        if (cl <= 0) break;
        for (int j = 0; j < cl; j++)
            vga_putchar(line_buf[i + j]);
        i += cl;
    }
    // position cursor
    int cur_col = utf8_vis_len(line_buf, cursor_pos);
    vga_set_cursor(PROMPT_LEN + cur_col, vga_get_cursor_y());
    vga_cursor_on();
}

static void line_full_redraw(void) {
    vga_cursor_off();
    vga_set_cursor(PROMPT_LEN, vga_get_cursor_y());
    vga_clear_eol();
    for (unsigned int i = 0; i < line_pos; ) {
        int cl = utf8_char_len_at(line_buf, i);
        if (cl <= 0) break;
        for (int j = 0; j < cl; j++)
            vga_putchar(line_buf[i + j]);
        i += cl;
    }
    int cur_col = utf8_vis_len(line_buf, cursor_pos);
    vga_set_cursor(PROMPT_LEN + cur_col, vga_get_cursor_y());
    vga_cursor_on();
}

// ---- Tab completion with cycling ----
static int tab_cycle_idx = -1;
static int tab_match_count = 0;
static char tab_saved_word[64];
static char tab_matches[40][28];

static int tab_match(const char *prefix, const char *name) {
    while (*prefix && *name && *prefix == *name) {
        prefix++;
        name++;
    }
    return *prefix == '\0';
}

static int common_prefix_len(const char *a, const char *b) {
    int i = 0;
    while (a[i] && b[i] && a[i] == b[i]) i++;
    return i;
}

static void tab_reset(void) {
    tab_cycle_idx = -1;
    tab_match_count = 0;
}

static void tab_complete(void) {
    unsigned int word_start = cursor_pos;
    while (word_start > 0 && line_buf[word_start - 1] != ' ')
        word_start--;

    if (word_start == cursor_pos) { tab_reset(); return; }

    unsigned int word_len = cursor_pos - word_start;
    char word[64];
    if (word_len > 60) word_len = 60;
    for (unsigned int i = 0; i < word_len; i++)
        word[i] = line_buf[word_start + i];
    word[word_len] = '\0';

    // Check if we're continuing a cycle from the same word
    int cycling = (tab_match_count > 0 && tab_cycle_idx >= 0 &&
                   strncmp(word, tab_saved_word, 63) == 0);

    if (!cycling) {
        // Fresh search
        tab_match_count = 0;
        int i;
        for (i = 0; i < 63; i++) tab_saved_word[i] = word[i];
        tab_saved_word[i] = '\0';

        if (tab_match(word, "format"))
            strncpy(tab_matches[tab_match_count++], "format", 27);

        if (tab_match(word, "setpath"))
            strncpy(tab_matches[tab_match_count++], "setpath", 27);

        if (tab_match(word, "cd"))
            strncpy(tab_matches[tab_match_count++], "cd", 27);

        if (tab_match(word, "pwd"))
            strncpy(tab_matches[tab_match_count++], "pwd", 27);

        // Search all PATH directories
        char path_copy[PATH_MAX];
        extern char command_path[PATH_MAX];
        strncpy(path_copy, command_path, PATH_MAX - 1);
        path_copy[PATH_MAX - 1] = '\0';

        struct vfs_inode *root = vfs_get_root();
        char *dir = path_copy;
        while (*dir && tab_match_count < 40) {
            char *next = dir;
            while (*next && *next != ':') next++;
            int dir_len = next - dir;
            int has_sep = (*next == ':');
            *next = '\0';

            if (dir_len > 0) {
                struct vfs_inode *d = vfs_resolve(root, dir, 0);
                if (d && d->type == 2) {
                    char nbuf[VFS_NAME_MAX + 1];
                    unsigned int pos = 0;
                    while (tab_match_count < 40) {
                        unsigned int ino;
                        int r = d->fs->readdir(d->fs, d->ino, pos, nbuf, &ino);
                        if (r <= 0) break;
                        pos++;
                        if (ino == 0) continue;
                        if (tab_match(word, nbuf))
                            strncpy(tab_matches[tab_match_count++], nbuf, 27);
                    }
                }
                if (d) vfs_put(d);
            }

            if (!has_sep) break;
            dir = next + 1;
        }
        if (root) vfs_put(root);

        if (tab_match_count == 0) return;

        if (tab_match_count == 1) {
            // Single match: auto-complete
            const char *completion = tab_matches[0];
            unsigned int complen = 0;
            while (completion[complen]) complen++;
            if (word_start + complen >= LINE_BUF_SIZE - 1) { tab_reset(); return; }
            unsigned int rest_len = line_pos - cursor_pos;
            for (unsigned int i = 0; i < rest_len; i++)
                line_buf[word_start + complen + rest_len - 1 - i] = line_buf[cursor_pos + rest_len - 1 - i];
            for (unsigned int i = 0; i < complen; i++)
                line_buf[word_start + i] = completion[i];
            line_pos += complen - word_len;
            cursor_pos = word_start + complen;
            line_full_redraw();
            tab_reset();
            return;
        }

        // Multiple matches: find common prefix
        int pref_len = 63;
        for (int i = 1; i < tab_match_count; i++) {
            int l = common_prefix_len(tab_matches[0], tab_matches[i]);
            if (l < pref_len) pref_len = l;
        }

        if (pref_len > (int)word_len) {
            // Auto-complete common prefix
            for (unsigned int i = 0; i < (unsigned int)pref_len; i++)
                word[i] = tab_matches[0][i];
            word[pref_len] = '\0';
            unsigned int complen = pref_len;
            if (word_start + complen >= LINE_BUF_SIZE - 1) { tab_reset(); return; }
            unsigned int rest_len = line_pos - cursor_pos;
            for (unsigned int i = 0; i < rest_len; i++)
                line_buf[word_start + complen + rest_len - 1 - i] = line_buf[cursor_pos + rest_len - 1 - i];
            for (unsigned int i = 0; i < complen; i++)
                line_buf[word_start + i] = word[i];
            line_pos += complen - word_len;
            cursor_pos = word_start + complen;
            line_full_redraw();
            // Set up for next tab to show list
            tab_cycle_idx = -1;
            return;
        }

        // No common prefix beyond what user typed: show matches
        vga_print("\n");
        for (int i = 0; i < tab_match_count; i++) {
            vga_print(tab_matches[i]);
            vga_putchar(' ');
        }
        vga_print("\n");
        vga_print(PROMPT);
        line_full_redraw();
        tab_cycle_idx = -1;
        return;
    }

    // Cycling: advance to next match
    tab_cycle_idx = (tab_cycle_idx + 1) % tab_match_count;
    const char *completion = tab_matches[tab_cycle_idx];
    unsigned int complen = 0;
    while (completion[complen]) complen++;

    if (word_start + complen >= LINE_BUF_SIZE - 1) { tab_reset(); return; }
    unsigned int rest_len = line_pos - cursor_pos;
    for (unsigned int i = 0; i < rest_len; i++)
        line_buf[word_start + complen + rest_len - 1 - i] = line_buf[cursor_pos + rest_len - 1 - i];
    for (unsigned int i = 0; i < complen; i++)
        line_buf[word_start + i] = completion[i];
    line_pos += complen - word_len;
    cursor_pos = word_start + complen;
    line_full_redraw();
}

// ---- Terminal I/O ----
void terminal_putchar(char c) {
    vga_putchar(c);
    serial_putchar(c);
}

void terminal_print(const char *str) {
    vga_print(str);
    serial_print(str);
}

void terminal_print_hex(unsigned int n) {
    vga_print_hex(n);
    serial_print_hex(n);
}

void terminal_print_dec(unsigned int n) {
    vga_print_dec(n);
    serial_print_dec(n);
}

void terminal_write(const char *buf, unsigned int len) {
    for (unsigned int i = 0; i < len; i++)
        terminal_putchar(buf[i]);
}

void terminal_set_prompt(void) {
    vga_reset_scroll();
    terminal_print("\n");
    vga_set_cursor(0, vga_get_cursor_y());
    terminal_print(PROMPT);
}

static void process_line(void) {
    line_buf[line_pos] = '\0';
    hist_push();
    commands_execute(line_buf);
    line_pos = 0;
    cursor_pos = 0;
    hist_cur = -1;
}

static void hist_load(int dir) {
    if (hist_count == 0) return;

    int oldest = (hist_next - hist_count + HIST_SIZE) % HIST_SIZE;
    int newest = (hist_next - 1 + HIST_SIZE) % HIST_SIZE;

    if (dir > 0) { // Up = older
        if (hist_cur == -1)
            hist_cur = newest;
        else if (hist_cur == oldest)
            return;
        else
            hist_cur = (hist_cur - 1 + HIST_SIZE) % HIST_SIZE;
    } else { // Down = newer
        if (hist_cur == -1) return;
        if (hist_cur == newest) {
            hist_cur = -1;
            line_pos = 0;
            cursor_pos = 0;
            line_full_redraw();
            return;
        }
        hist_cur = (hist_cur + 1) % HIST_SIZE;
    }

    const char *src = history[hist_cur];
    unsigned int i;
    for (i = 0; i < LINE_BUF_SIZE - 1 && src[i]; i++)
        line_buf[i] = src[i];
    line_pos = i;
    cursor_pos = i;
    line_buf[line_pos] = '\0';
    line_full_redraw();
}

static void insert_codepoint(unsigned short cp) {
    unsigned char utf8[4];
    int len = cp_to_utf8(cp, utf8);
    if (line_pos + len >= LINE_BUF_SIZE - 1) return;

    // Shift rest right
    for (unsigned int i = 0; i < line_pos - cursor_pos; i++)
        line_buf[line_pos + len - 1 - i] = line_buf[line_pos - 1 - i];

    for (int i = 0; i < len; i++)
        line_buf[cursor_pos + i] = utf8[i];

    line_pos += len;
    cursor_pos += len;
    line_redraw_from(cursor_pos - len);
}

static void handle_backspace(void) {
    if (cursor_pos == 0) return;
    int clen = utf8_char_len_rev(line_buf, cursor_pos);
    for (unsigned int i = cursor_pos - clen; i + clen <= line_pos; i++)
        line_buf[i] = line_buf[i + clen];
    line_pos -= clen;
    cursor_pos -= clen;
    line_redraw_from(cursor_pos);
}

static void handle_delete(void) {
    if (cursor_pos >= line_pos) return;
    int clen = utf8_char_len_at(line_buf, cursor_pos);
    for (unsigned int i = cursor_pos; i + clen <= line_pos; i++)
        line_buf[i] = line_buf[i + clen];
    line_pos -= clen;
    line_redraw_from(cursor_pos);
}

// Serial console input: feed a byte as if it came from the keyboard
void terminal_serial_byte(unsigned char c) {
    if (user_program_active()) {
        key_queue_push(c);
        return;
    }
    switch (c) {
    case '\r':
    case '\n':
        tab_reset();
        vga_reset_scroll();
        process_line();
        break;
    case '\b':
    case 0x7F:
        tab_reset();
        vga_reset_scroll();
        handle_backspace();
        break;
    case '\t':
        vga_reset_scroll();
        tab_complete();
        break;
    default:
        if (c >= 0x20 && c < 0x7F) {
            tab_reset();
            vga_reset_scroll();
            insert_codepoint(c);
        }
        break;
    }
}

void terminal_keyboard_handler(unsigned char scancode) {
    int k = terminal_scan_event(scancode);
    if (k < 0) return;

    vga_reset_scroll();
    tab_reset();

    switch (k) {
    case GUI_KEY_UP:
        hist_load(1);
        return;
    case GUI_KEY_DOWN:
        hist_load(-1);
        return;
    case GUI_KEY_LEFT:
        if (cursor_pos > 0) {
            int clen = utf8_char_len_rev(line_buf, cursor_pos);
            cursor_pos -= clen;
            line_full_redraw();
        }
        return;
    case GUI_KEY_RIGHT:
        if (cursor_pos < line_pos) {
            int clen = utf8_char_len_at(line_buf, cursor_pos);
            cursor_pos += clen;
            line_full_redraw();
        }
        return;
    case GUI_KEY_HOME:
        cursor_pos = 0;
        line_full_redraw();
        return;
    case GUI_KEY_END:
        cursor_pos = line_pos;
        line_full_redraw();
        return;
    case GUI_KEY_DEL:
        handle_delete();
        return;
    }

    if (k == '\t') {
        tab_complete();
        return;
    }
    if (k == '\b') {
        handle_backspace();
        return;
    }
    if (k == '\r') {
        process_line();
        return;
    }
    if (k < 0x20) return;   // escape and other controls are ignored

    if (user_program_active()) {
        key_queue_push(k);
        return;
    }

    insert_codepoint((unsigned short)k);
}

void terminal_init(void) {
    line_pos = 0;
    cursor_pos = 0;
    shift_pressed = 0;
    ctrl_pressed = 0;
    caps_lock = 0;
    ru_layout = 0;
    e0_prefix = 0;
    hist_count = 0;
    hist_next = 0;
    hist_cur = -1;
    terminal_print("Terminal ready.\n");
    terminal_set_prompt();
}
