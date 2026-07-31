# Window close button + drag-flicker fix

Date: 2026-07-31

## Problem

Two GUI issues in AOS (QEMU, TCG):

1. **Windows cannot be closed.** There is no close affordance anywhere: no button on the title bar, no close message, and the WM cannot terminate an app process (no kill syscall; `SYS_EXIT` only exits the current task). Additionally, the WM spawns term/clock with `sink=0`, so it never receives `MSG_EXIT` when a window app dies and can never free its window.
2. **Screen flickers while dragging windows.** Every mouse move during a drag sets `redraw=1`, which triggers the full `composite()`: it erases the whole 3 MB framebuffer and redraws all windows (~100 ms in TCG). At drag speed this is a continuous blink.

## Design decisions (approved)

- **Close mechanism:** an "X" button drawn at the right end of each window's title bar. Clicking it sends a new cooperative `MSG_CLOSE` to the app; the app calls `exit()`; the kernel notifies the WM via `MSG_EXIT`; the WM frees the window and repaints.
- **Drag behavior:** live drag — the window's content follows the cursor. Flicker is eliminated by redrawing only the dirty region (union of the window's old and new rects) instead of the whole screen.

## Section 1: Close button (X)

### New IPC message

`programs/aosipc.h`: add

```c
#define MSG_CLOSE  7   // wm -> app: "please exit"   (a = unused)
```

### WM: draw the button

`programs/wm.c` `draw_title()`:

- Button rect: 16×16 px, vertically centered in the title bar, at the right end:
  `x = wn->x + BORDER + wn->cw - 18`, `y = wn->y + BORDER + (TITLE_H - 16) / 2`.
- Draw an "X" with two 1-px diagonal lines in `COL_TITLE_TEXT` (white) on the title background. Use the `snap`-free direct fb writes; no font dependency (two line loops).

### WM: hit-testing on press

In the left-press handler (before focus/drag logic):

- Compute the topmost window under the cursor (`win_index_at`).
- If the cursor is within that window's close-button rect → send `MSG_CLOSE` to `wins[wi].pid`; skip focus-change and drag-start for this click.
- Otherwise proceed as today (focus + optional title-bar drag).

### WM: receive MSG_EXIT

Change the spawn calls in `wm.c` `main()` from `sink=0` to the WM's own pid:

```c
spawn("bin/term", "", getpid());
spawn("bin/clock", "", getpid());
```

The existing `MSG_EXIT` handler (`free_windows(m.a); redraw = 1;`) then runs when an app dies, freeing its window slot. `free_windows` also clears `focus_pid` if the closed window owned focus.

### Apps: handle MSG_CLOSE

- `programs/term.c` main loop: add `case MSG_CLOSE: exit();`
- `programs/clock.c` main loop: add `case MSG_CLOSE: exit();`

If an app ignores `MSG_CLOSE` (e.g. a future third-party app), the window simply stays — cooperative close is safe by construction; no kernel changes.

## Section 2: Drag-flicker fix (incremental dirty-rect redraw)

### Clipped drawing primitives

`programs/wm.c`: introduce a global clip rectangle `clip_x0, clip_y0, clip_x1, clip_y1` and make the three window-drawing paths respect it:

- `fb_fill(...)`: intersect the fill rect with the clip rect before writing.
- `blit_content(wn)`: intersect the content rect `(x+BORDER, y+BORDER+TITLE_H, cw, ch)` with the clip rect; copy only the intersecting rows/columns (adjust source row/col offsets accordingly).
- `draw_title(wn)`: the title fill uses `fb_fill` (already clipped); clip the 16-row text blit (rows and columns) to the clip rect.

### `composite_rect(x0, y0, x1, y1)`

- Clamp to the framebuffer.
- Set the global clip rect.
- Fill the rect with `COL_DESKTOP`.
- For each window `i = 0..MAX_WINDOWS-1` (ascending = z-order, later drawn on top) that intersects the rect, draw its borders/title/content via the (now clipped) primitives.
- Reset the clip rect.

### Drag path

In the drag-move block of `main()`:

- Before updating `wn->x/wn->y`, save the old rect; after updating, compute `dirty = union(old_rect, new_rect)` where a window's rect is
  `(x, y, cw + 2*BORDER, ch + TITLE_H + 2*BORDER)`.
- Call `composite_rect(dirty)` instead of setting `redraw = 1`.
- After `composite_rect`, if `cursor_overlaps(dirty rect)` → `has_cur = 0; update_cursor(mx, my);`.

`composite()` (full-screen) stays for `MSG_CREATE`, `MSG_EXIT`, focus changes (rare events).

## Testing

In QEMU (headless, monitor on unix socket, screendump analysis):

1. **Close term**: drag a window aside, click the term's X → term window and its title disappear, only the clock remains, no cursor ghosts, no garbage pixels.
2. **Close clock**: click the clock's X → clock disappears; desktop fully redrawn behind it.
3. **Focus/keys regression**: click term body → focused title color; `sendkey a` → glyph appears.
4. **Drag flicker**: hold left button on term title, move — consecutive screendumps during the drag must NOT show a full-screen desktop flash; the term window follows the cursor (delta between consecutive frames ≈ mouse delta) and z-order is preserved (clock stays on top where overlapping).
5. **Cursor persistence**: after the drag, the cursor stays visible through subsequent clock ticks; single cross, no ghosts.

## Files touched

- `programs/aosipc.h` — add `MSG_CLOSE`.
- `programs/wm.c` — X button, press hit-testing, spawn sink, clip rect, `composite_rect`, drag dirty-redraw.
- `programs/term.c` — handle `MSG_CLOSE`.
- `programs/clock.c` — handle `MSG_CLOSE`.
