# Window Close Button + Drag-Flicker Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an "X" close button to every window title bar (cooperative `MSG_CLOSE` → app `exit()`) and eliminate the screen flicker during window dragging via incremental dirty-rect redraws.

**Architecture:** Pure user-space (ring 3) changes. The WM (`programs/wm.c`) draws a 16×16 X in the title bar, hit-tests it on left-press, and sends the new `MSG_CLOSE` IPC message; term/clock handle it by calling `exit()`. The WM also starts spawning window apps with itself as the exit-sink so it learns of their death via the existing `MSG_EXIT` path. Flicker is fixed by giving the WM's drawing primitives a clip rectangle and adding `composite_rect(x0,y0,x1,y1)`; dragging repaints only the union of the window's old and new rects instead of the whole 3 MB framebuffer. `MSG_UPDATE` is routed through `composite_rect` too, which also fixes a pre-existing z-order bug (a lower-z window's content update covered a higher-z window's title bar).

**Tech Stack:** C11 freestanding (`-ffreestanding -nostdlib -m32`), no libc, GRUB2 multiboot2 ISO, QEMU TCG testing (headless, monitor on unix socket, screendump analysis). Build: `make`. No unit-test framework — verification is build + boot + pixel analysis.

## Global Constraints

- No kernel changes. All edits are in `programs/`.
- No comments in code unless the surrounding code already has them.
- `mcpy()` `n` is a **word count**, never bytes.
- Framebuffer: 1024×768 32bpp, `fb_pitch = 4096` (pixels/row = `fb_pitch >> 2`). Colors: desktop `COL_DESKTOP 0x1A2030`, title focused `COL_TITLE_FOCUS 0x4A7AB5`, unfocused `COL_TITLE 0x2E4E7B`, title text/cursor `COL_TITLE_TEXT/COL_CURSOR 0xFFFFFF`.
- Window rect (whole window incl. frame): `(x, y, cw + 2*BORDER, ch + TITLE_H + 2*BORDER)` with `BORDER=1`, `TITLE_H=18`.
- Term window starts at (20,20) 640×416 content; clock at (44,48) 260×100 content. Clock has higher z-order (drawn on top).
- Mouse: positive dx = right, positive dy = down (`mouse_y -= packet[2]`).
- Commits after every task with a message following repo style (`feat: ...`, `fix: ...`).

## Verification Harness (used by every task)

Save `/tmp/opencode/qmon.py`:

```python
import socket, time, sys
s = socket.socket(socket.AF_UNIX); s.settimeout(8); s.connect('/tmp/aos-gui.sock')
time.sleep(0.2); s.recv(4096)
for c in sys.argv[1:]:
    s.sendall((c+'\n').encode()); time.sleep(0.4); s.recv(1<<20)
s.close()
```

Boot sequence (from repo root; replace any running instance first):

```bash
make
kill $(cat /tmp/qemu.pid) 2>/dev/null; sleep 0.5
qemu-system-i386 -m 256 -cdrom aos.iso -vga std -display none \
  -serial file:/tmp/aos-gui.log -monitor unix:/tmp/aos-gui.sock,server,nowait -daemonize
echo $! > /tmp/qemu.pid; sleep 2
grep -q "Window manager spawned" /tmp/aos-gui.log && echo BOOTED
```

Screendump: `python3 /tmp/opencode/qmon.py "screendump /tmp/shot.ppm"`.

Helper to locate a window's title bar (focused color `(74,122,181)`), usable in verification:

```python
from PIL import Image
def title(path, col=(74,122,181)):
    im=Image.open(path).convert('RGB'); px=im.load()
    ys=[y for y in range(768) if sum(1 for x in range(1024) if px[x,y]==col)>5]
    if not ys: return None
    a=min(ys); xs=[x for x in range(1024) if px[x,a]==col]
    return (min(xs), a, max(xs), max(ys))
```

---

### Task 1: `MSG_CLOSE` IPC message + app handling

**Files:**
- Modify: `programs/aosipc.h:10`
- Modify: `programs/term.c:209-214`
- Modify: `programs/clock.c:24`

**Interfaces:**
- Consumes: existing `exit()` (never returns: `syscall(SYS_EXIT,...); for(;;);`).
- Produces: `MSG_CLOSE = 7` — WM sends it to an app pid to request termination. Task 3 sends it; later tasks do not depend on it.

- [ ] **Step 1: Add the message constant**

In `programs/aosipc.h`, after the `MSG_EXIT` line:

```c
#define MSG_EXIT    6   // kernel -> sink: "task exited" a = pid
```
add:
```c
#define MSG_CLOSE   7   // wm -> app: "please exit"    a = unused
```

- [ ] **Step 2: term.c handles MSG_CLOSE**

In `programs/term.c`, in the main loop's `switch (m.type)` (the block containing `case MSG_EXIT:`), add a new case after `MSG_EXIT`:

```c
            case MSG_EXIT:
                child_active = 0;
                put_char('\n');
                prompt();
                render();
                break;
            case MSG_CLOSE:
                exit();
                break;
```

- [ ] **Step 3: clock.c handles MSG_CLOSE**

In `programs/clock.c`, at the top of the `for (;;)` main loop (before `unsigned int t = get_tick();`), add:

```c
        if (recv_msg(&m) == 0 && m.type == MSG_CLOSE)
            exit();
```

(`m` is the `struct aos_msg` already declared in `main()`; the clock's main loop otherwise never drains its mailbox.)

- [ ] **Step 4: Build and boot regression check**

Run the boot sequence from the Verification Harness. Expected: BOOTED, no new warnings in the `make` output, `wm:` framebuffer line in `/tmp/aos-gui.log`. (End-to-end close is exercised in Task 3; this task only must not regress boot or rendering.)

- [ ] **Step 5: Commit**

```bash
git add programs/aosipc.h programs/term.c programs/clock.c
git commit -m "feat: add MSG_CLOSE so window apps can be requested to exit"
```

---

### Task 2: Clip rectangle + `composite_rect` (partial-redraw refactor + `MSG_UPDATE` z-order fix)

**Files:**
- Modify: `programs/wm.c` (globals near line 30), `programs/wm.c:53-59` (`fb_fill`), `programs/wm.c:70-75` (title text blit), `programs/wm.c:78-87` (`blit_content`), `programs/wm.c:89-107` (`composite` → `composite_rect` + thin `composite`), `programs/wm.c:241-257` (`MSG_UPDATE` case)

**Interfaces:**
- Consumes: existing `fb_fill`, `blit_content`, `draw_title`, `mcpy`, `cursor_overlaps`, `update_cursor`.
- Produces: `composite_rect(int x0,int y0,int x1,int y1)` — repaints only `[x0,y0,x1,y1)` (exclusive), z-order correct; the global clip rect `clip_x0..clip_y1`; `composite()` now just calls `composite_rect(0,0,fb_w,fb_h)`. Tasks 3 and 4 consume `composite_rect`. The `MSG_UPDATE` case now calls `composite_rect` over the affected window's rect instead of the blit-only path — this fixes a pre-existing bug where a lower-z window's content re-blit covered a higher-z window's title bar (e.g. the term's updates hid the clock's title, verified via screendump before this task).

- [ ] **Step 1: Add the clip globals + defaults**

After the `static unsigned int snap[...]` declaration, add:

```c
static int clip_x0, clip_y0, clip_x1, clip_y1;
```

In `main()`, right after `get_fb_info(...)`, add:

```c
    clip_x1 = (int)fb_w;
    clip_y1 = (int)fb_h;
```

- [ ] **Step 2: Clip `fb_fill`**

Replace the body of `fb_fill` with:

```c
static void fb_fill(int x, int y, int w, int h, unsigned int rgb) {
    unsigned int *fb = (unsigned int *)fb_addr;
    unsigned int pitch = fb_pitch >> 2;
    int x0 = x > clip_x0 ? x : clip_x0;
    int y0 = y > clip_y0 ? y : clip_y0;
    int x1 = (x + w < clip_x1) ? x + w : clip_x1;
    int y1 = (y + h < clip_y1) ? y + h : clip_y1;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++)
            fb[(unsigned)yy * pitch + (unsigned)xx] = rgb;
}
```

- [ ] **Step 3: Clip the title-text blit**

In `draw_title`, replace the `dst`/`mcpy` tail (from `unsigned int *dst = ...` through the `for (int r = 0; r < 16; r++)` loop) with:

```c
    int tx = wn->x + BORDER + 6;
    int ty = wn->y + BORDER + (TITLE_H - 16) / 2;
    int x0 = tx > clip_x0 ? tx : clip_x0;
    int y0 = ty > clip_y0 ? ty : clip_y0;
    int x1 = (tx + tw < clip_x1) ? tx + tw : clip_x1;
    int y1 = (ty + 16 < clip_y1) ? ty + 16 : clip_y1;
    unsigned int *fb = (unsigned int *)fb_addr;
    unsigned int pitch = fb_pitch >> 2;
    for (int r = y0; r < y1; r++)
        mcpy(fb + (unsigned)r * pitch + (unsigned)x0,
             sc + (unsigned)(r - ty) * 1024 + (unsigned)(x0 - tx),
             (unsigned)(x1 - x0));
```

- [ ] **Step 4: Clip `blit_content`**

Replace the body of `blit_content` with:

```c
static void blit_content(const struct win *wn) {
    const unsigned int *src =
        (const unsigned int *)(AOS_SLAB_BASE + wn->slab * AOS_SLAB_SIZE);
    unsigned int *dst = (unsigned int *)fb_addr;
    unsigned int pitch = fb_pitch >> 2;
    int cx = wn->x + BORDER;
    int cy = wn->y + BORDER + TITLE_H;
    int x0 = cx > clip_x0 ? cx : clip_x0;
    int y0 = cy > clip_y0 ? cy : clip_y0;
    int x1 = (cx + wn->cw < clip_x1) ? cx + wn->cw : clip_x1;
    int y1 = (cy + wn->ch < clip_y1) ? cy + wn->ch : clip_y1;
    for (int r = y0; r < y1; r++)
        mcpy(dst + (unsigned)r * pitch + (unsigned)x0,
             src + (unsigned)(r - cy) * wn->cw + (unsigned)(x0 - cx),
             (unsigned)(x1 - x0));
}
```

- [ ] **Step 5: Add `composite_rect` and slim `composite`**

Replace the whole `composite()` function with:

```c
static void composite_rect(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)fb_w) x1 = (int)fb_w;
    if (y1 > (int)fb_h) y1 = (int)fb_h;
    if (x0 >= x1 || y0 >= y1) return;
    clip_x0 = x0; clip_y0 = y0; clip_x1 = x1; clip_y1 = y1;
    fb_fill(x0, y0, x1 - x0, y1 - y0, COL_DESKTOP);
    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct win *wn = &wins[i];
        if (!wn->used) continue;
        if (wn->x < x1 && wn->x + wn->cw + 2 * BORDER > x0 &&
            wn->y < y1 && wn->y + wn->ch + TITLE_H + 2 * BORDER > y0) {
            fb_fill(wn->x, wn->y, wn->cw + 2 * BORDER, BORDER, COL_BORDER);
            fb_fill(wn->x, wn->y + wn->ch + TITLE_H + BORDER,
                    wn->cw + 2 * BORDER, BORDER, COL_BORDER);
            fb_fill(wn->x, wn->y, BORDER, wn->ch + TITLE_H + 2 * BORDER,
                    COL_BORDER);
            fb_fill(wn->x + wn->cw + BORDER, wn->y, BORDER,
                    wn->ch + TITLE_H + 2 * BORDER, COL_BORDER);
            draw_title(wn);
            blit_content(wn);
        }
    }
    clip_x0 = 0; clip_y0 = 0;
    clip_x1 = (int)fb_w; clip_y1 = (int)fb_h;
}

static void composite(void) {
    composite_rect(0, 0, (int)fb_w, (int)fb_h);
}
```

- [ ] **Step 6: Route `MSG_UPDATE` through `composite_rect`**

Replace the whole `case MSG_UPDATE:` block (currently the blit-only path with `hit_cur` tracking) with:

```c
            case MSG_UPDATE:
                if (m.a < MAX_WINDOWS && wins[m.a].used) {
                    struct win *wn = &wins[m.a];
                    composite_rect(wn->x, wn->y,
                                   wn->x + wn->cw + 2 * BORDER,
                                   wn->y + wn->ch + TITLE_H + 2 * BORDER);
                    if (cursor_overlaps(wn->x, wn->y,
                                        wn->cw + 2 * BORDER,
                                        wn->ch + TITLE_H + 2 * BORDER)) {
                        has_cur = 0;
                        update_cursor(mx, my);
                    }
                }
                break;
```

- [ ] **Step 7: Build, boot, no-regression check**

Build + boot. Screendump and verify:
- With `title()`: term focused title band ~(21,21,660,38), **clock title band now visible** at ~(45,49,304,66) — this was hidden before this task (the pre-existing z-order bug). The clock title region (x=45..304, y=49..66) must be title color, NOT the term's content background (16,16,16).
- Type a key (`sendkey a`) — glyph appears in the term; the term title stays intact and the clock title is NOT covered by the term update (MSG_UPDATE now redraws z-order correctly).
- Drag the clock's title bar a little and release — window still moves (full composite path is still exercised since Task 4 is not yet applied).
- No garbage, no ghost crosses; exactly one cursor cross.

- [ ] **Step 8: Commit**

```bash
git add programs/wm.c
git commit -m "refactor: add clip rect and composite_rect; route MSG_UPDATE through it"
```

---

### Task 3: WM close button (draw, hit-test, exit-sink)

**Files:**
- Modify: `programs/wm.c:61-76` (`draw_title`), `programs/wm.c` (new helper near `win_index_at`), `programs/wm.c:203-204` (spawn calls), `programs/wm.c:266-281` (press handler)

**Interfaces:**
- Consumes: `MSG_CLOSE` (Task 1), existing `win_index_at(mx,my)`, `send_msg(pid,&msg)`, `getpid()`. Renders correctly because Task 2 keeps title bars visible.
- Produces: `draw_close_btn(const struct win *)` (draws the X), `close_btn_at(int,int) -> int` (window index whose X button contains the point, or -1). The WM now receives `MSG_EXIT(pid)` when a window app dies.

- [ ] **Step 1: Draw the X button**

In `programs/wm.c`, after `blit_content` (before `composite`), add:

```c
static void draw_close_btn(const struct win *wn) {
    int bx = wn->x + BORDER + wn->cw - 18;
    int by = wn->y + BORDER + (TITLE_H - 16) / 2;
    unsigned int *fb = (unsigned int *)fb_addr;
    unsigned int pitch = fb_pitch >> 2;
    for (int i = 0; i < 16; i++) {
        fb[(unsigned)(by + i) * pitch + (unsigned)(bx + i)] = COL_TITLE_TEXT;
        fb[(unsigned)(by + i) * pitch + (unsigned)(bx + 15 - i)] = COL_TITLE_TEXT;
    }
}
```

Append a call `draw_close_btn(wn);` at the end of `draw_title`.

- [ ] **Step 2: Hit-test helper**

In `programs/wm.c`, right after `win_index_at`, add:

```c
static int close_btn_at(int mx, int my) {
    for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
        struct win *wn = &wins[i];
        if (!wn->used) continue;
        int bx = wn->x + BORDER + wn->cw - 18;
        int by = wn->y + BORDER + (TITLE_H - 16) / 2;
        if (mx >= bx && mx < bx + 16 && my >= by && my < by + 16)
            return i;
    }
    return -1;
}
```

- [ ] **Step 3: Spawn window apps with the WM as exit-sink**

In `main()`, change:

```c
    spawn("bin/term", "", 0);
    spawn("bin/clock", "", 0);
```
to:
```c
    spawn("bin/term", "", getpid());
    spawn("bin/clock", "", getpid());
```

- [ ] **Step 4: Press handler — X click closes, otherwise focus/drag**

Replace the whole `if (moved && (mb & 1) && !(last_mb & 1)) { ... }` block (currently `win_index_at` + focus + drag-start) with:

```c
        if (moved && (mb & 1) && !(last_mb & 1)) {
            int cb = close_btn_at(mx, my);
            if (cb >= 0) {
                struct aos_msg cl = {MSG_CLOSE, 0, 0, 0, 0};
                send_msg(wins[cb].pid, &cl);
            } else {
                int wi = win_index_at(mx, my);
                if (wi >= 0) {
                    if (focus_pid != wins[wi].pid) redraw = 1;
                    focus_pid = wins[wi].pid;
                    if (my >= wins[wi].y + BORDER &&
                        my < wins[wi].y + BORDER + TITLE_H) {
                        drag_i = wi;
                        drag_dx = mx - wins[wi].x;
                        drag_dy = my - wins[wi].y;
                    }
                }
            }
        }
```

- [ ] **Step 5: Build, boot, verify the X renders**

Build + boot. Screendump. Verify the clock's X is visible: it spans x=287..303, y=50..66 (clock at (44,48): `bx=44+1+260-18=287`, `by=48+1+1=50`). Check some pixels are `0xFFFFFF` there.

```python
from PIL import Image
im=Image.open('/tmp/shot.ppm').convert('RGB'); px=im.load()
xs=[(x,y) for y in range(50,67) for x in range(287,304) if px[x,y]==(255,255,255)]
print(len(xs), 'white X pixels (expect ~60)')
```

- [ ] **Step 6: Verify closing the clock**

`python3 /tmp/opencode/qmon.py "mouse_move 295 58" "screendump /tmp/before.ppm" "mouse_button 1" "mouse_button 0" "screendump /tmp/after.ppm"`

(From center (511,383): `mouse_move 295 58` moves to absolute (806,441)? **No** — `mouse_move dx dy` is a **delta**. Move from wherever the cursor is: first move the cursor to the clock X center, verify with a screendump, then press.)

```python
import socket, time
s=socket.socket(socket.AF_UNIX); s.settimeout(8); s.connect('/tmp/aos-gui.sock')
def cmd(c,w=0.5):
    s.sendall((c+'\n').encode()); time.sleep(w); s.recv(1<<20)
cmd('mouse_move -500 300')                 # bring cursor somewhere known on the desktop
cmd('mouse_move 284 10')                   # move onto clock X (295,58); 295-11? adjust to land exactly
cmd('screendump /tmp/c1.ppm')
cmd('mouse_button 1'); cmd('mouse_button 0')
time.sleep(1)
cmd('screendump /tmp/c2.ppm')
s.close()
```

Adjust the deltas so the cursor (verified in `c1.ppm` via the white cross) sits on the clock's X button at (295,58). Then `c2.ppm` must show: no clock title bar (`title()` returns only the term band at y≈21..38), no clock content, desktop color where the clock was.

- [ ] **Step 7: Verify closing the term**

Repeat the same procedure aiming at the term's X at (651,30) (`bx=20+1+640-18=643`, `by=22`; center (651,30)). After closing the term, the screendump must show only the clock (or, if the clock was already closed, a plain desktop with no windows) and no garbage pixels.

- [ ] **Step 8: Commit**

```bash
git add programs/wm.c
git commit -m "feat: add close button to window title bars"
```

---

### Task 4: Drag uses `composite_rect` (flicker fix)

**Files:**
- Modify: `programs/wm.c` drag-move block (the `if (nx != wn->x || ny != wn->y) { ... }` tail inside the drag block)

**Interfaces:**
- Consumes: `composite_rect` (Task 2), `cursor_overlaps(x,y,w,h)` (already present).
- Produces: nothing new. Drag no longer sets `redraw = 1`.

- [ ] **Step 1: Replace the drag move**

Find inside the drag block:

```c
            if (nx != wn->x || ny != wn->y) {
                wn->x = nx;
                wn->y = ny;
                redraw = 1;
            }
```

Replace with:

```c
            if (nx != wn->x || ny != wn->y) {
                int ox = wn->x, oy = wn->y;
                int ow = wn->cw + 2 * BORDER, oh = wn->ch + TITLE_H + 2 * BORDER;
                wn->x = nx;
                wn->y = ny;
                int x0 = ox < nx ? ox : nx;
                int y0 = oy < ny ? oy : ny;
                int x1 = (ox > nx ? ox : nx) + ow;
                int y1 = (oy > ny ? oy : ny) + oh;
                composite_rect(x0, y0, x1, y1);
                if (cursor_overlaps(x0, y0, x1 - x0, y1 - y0)) {
                    has_cur = 0;
                    update_cursor(mx, my);
                }
            }
```

- [ ] **Step 2: Build, boot, verify pixel-exact incremental drag**

Build + boot. Drag the term by its title bar in several small steps. After each step, screendump and record the term's title x0/y0; assert the position advances by exactly the mouse delta each step (proves the incremental path, not a full-screen redraw).

```python
import socket, time
s=socket.socket(socket.AF_UNIX); s.settimeout(8); s.connect('/tmp/aos-gui.sock')
def cmd(c,w=0.5):
    s.sendall((c+'\n').encode()); time.sleep(w); s.recv(1<<20)
cmd('mouse_move 200 150')            # onto term title (grab around x=200-260, y=22-38)
cmd('mouse_button 1')
for dx,dy in [(40,0),(40,0),(40,40),(0,40)]:
    cmd(f'mouse_move {dx} {dy}')
    cmd('screendump /tmp/drag.ppm')
cmd('mouse_button 0')
cmd('screendump /tmp/drag_end.ppm')
s.close()
```

For each `/tmp/drag*.ppm`, run the `title()` helper and check the focused term band's x0/y0 increase by (40,0),(40,0),(40,40),(0,40) respectively (allowing ±1 px). The unfocused clock band must stay at ~(45,49).

- [ ] **Step 3: Verify no full-screen flash + cursor sanity**

During a drag, capture several screendumps in quick succession. Assert NONE is an all-desktop frame (i.e. every shot still shows the clock title band and the term band). Also assert exactly one cursor cross remains after release (`/tmp/drag_end.ppm`) and no ghost crosses at old positions.

- [ ] **Step 4: Verify z-order while overlapping**

Drag the term so it slides under the clock (e.g. from (20,20) toward the clock at (44,48)); release. Screendump: where they overlap, the clock (higher z-index) must be on top — the term's title/content must not cover the clock.

- [ ] **Step 5: Commit**

```bash
git add programs/wm.c
git commit -m "fix: repaint only the dirty rect while dragging windows"
```

---

## Self-Review

- Spec Section 1 (close button) → Tasks 1+3 (message, drawing, hit-test, sink). ✓
- Spec Section 2 (flicker) → Tasks 2+4 (clip/composite_rect refactor, then drag integration). ✓
- Task 2 additionally routes `MSG_UPDATE` through `composite_rect`, fixing the pre-existing z-order bug where a lower-z window's content update covered a higher-z window's title bar (discovered during Task 1 verification; user-approved plan change, "Add fix + reorder"). ✓
- Spec "Testing" list → covered by Task 3 steps 6-7, Task 4 steps 2-4; cursor-ghost/regression checks in Task 2 step 7, Task 3 step 5, Task 4 step 3. ✓
- `composite_rect` signature consistent between Task 2 (defined) and Tasks 2+4 (used): `void composite_rect(int x0, int y0, int x1, int y1)`. `cursor_overlaps(x,y,w,h)` used in Tasks 2/4 matches the existing definition in `wm.c`. ✓
- No placeholders; every code step contains the full replacement text.
