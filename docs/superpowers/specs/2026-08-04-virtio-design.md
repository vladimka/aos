# Design: virtio-pci support (blk, net, rng) — legacy transport

Date: 2026-08-04
Status: approved (design review complete)

## Problem

AOS has no persistent storage (SFS lives in a volatile 1 MiB RAM image at
`0x200000`), no network, and no entropy source. QEMU's `pc-i440fx` machine
provides cheap, well-specified virtio-pci devices for all three. The kernel
already has the right foundations — PCI config access (`drivers/pci.c`),
I/O ports (`drivers/ports.h`), PIC IRQ handlers (`kernel/interrupts.c`),
identity-mapped paging (physical == virtual, so DMA needs no bounce), and a
buddy page allocator (`kernel/pmm.c`).

Goal: a legacy (virtio 0.9.5) virtio-pci driver stack enabling:

- **virtio-blk**: persistent SFS on a QEMU `-drive` (files survive reboot).
- **virtio-rng**: entropy via a new `SYS_RANDOM` syscall + `random` command.
- **virtio-net**: TX/RX of raw Ethernet frames only (no TCP/IP this iteration).

## Decisions (from design review)

- **Transport: legacy virtio-pci (0.9.5)**, forced in QEMU with
  `disable-modern=on`. Single I/O BAR (BAR0) with the classic register map,
  INTx PIC interrupts, `QUEUE_PFN` physical-addressing. Matches the existing
  UHCI driver pattern (`drivers/uhci.c`) and needs no APIC/MSI.
- **Devices**: virtio-blk (0x1001), virtio-rng (0x1005), virtio-net (0x1000),
  vendor 0x1AF4. All three this iteration, in that priority order.
- **SFS persistence**: the 1 MiB FS image stays cached in RAM (`FS_MEM`); a
  dirty-sector bitmap (2048 sectors -> 256 bytes) flushes only the sectors a
  mutation touched. RAM-only mode (no disk) stays the fallback.
- **Net scope**: driver only — TX fire-and-forget + RX buffer pool. No
  ARP/IP/TCP (TODO P2 "Стек TCP/IP").
- **No virtio-input / virtio-gpu** (USB tablet + framebuffer already work).

## Verified QEMU facts (probe, QEMU 10.2.1, ISO boot)

`info pci` after full boot (`-drive file=x,if=none,id=d0` +
`-device virtio-blk-pci,disable-modern=on,drive=d0` +
`-device virtio-rng-pci,disable-modern=on` +
`-netdev socket,id=n0,listen=127.0.0.1:9000` +
`-device virtio-net-pci,disable-modern=on,mac=52:54:00:12:34:56,netdev=n0`):

| device | bus/dev/fn | ID | BAR0 | IRQ |
|---|---|---|---|---|
| virtio-blk | 00:03.0 | 1af4:1001 | I/O | **11** |
| virtio-rng | 00:04.0 | 1af4:1005 | I/O | **11** |
| virtio-net | 00:05.0 | 1af4:1000 | I/O | **10** |

- **blk and rng share IRQ 11**; net is on IRQ 10. Neither line is used by AOS
  (timer=0, kbd=1, mouse=12; UHCI is polled and installs no handler), so the
  shared-line problem is contained to virtio itself. The IRQ line register
  (config 0x3C) reads **0 immediately at QEMU start** and is programmed by
  SeaBIOS during POST — a driver must read it after boot has progressed (it
  will, since `virtio_init()` runs from `kernel_main`).
- AOS boots and runs unchanged with all three virtio devices attached but no
  driver (serial log: WM spawned, fb OK) — legacy tests without a `-drive`
  keep RAM-only SFS.
- `info pci` shows BAR0 as "I/O"; legacy registers are reached via
  `inb/inw/inl`/`outb/outw/outl`.

## Architecture

New files:

- `drivers/virtio.h`, `drivers/virtio.c` — legacy transport: register map,
  feature negotiation, virtqueue management, IRQ dispatch.
- `drivers/vblk.h`, `drivers/vblk.c` — virtio-blk sector read/write.
- `drivers/vrng.h`, `drivers/vrng.c` — virtio-rng entropy fill.
- `drivers/vnet.h`, `drivers/vnet.c` — virtio-net TX/RX.
- `programs/random.c` — `random` command (prints hex entropy via `SYS_RANDOM`).

Modified:

- `drivers/pci.h`, `drivers/pci.c` — generalized bus-0 scan
  (`pci_find_all()` with a callback / result array, `pci_write_config()`,
  I/O enable on the command register).
- `kernel/sfs.c`, `kernel/sfs.h` — disk backend: boot-time mount, dirty
  sector tracking, flush-on-mutation.
- `kernel/syscall.c`, `kernel/syscall.h` — `SYS_RANDOM`.
- `kernel/kernel.c` — `virtio_init()` in the boot order.
- `Makefile` — objects, `disk.img` rule, QEMU flags.

### virtio core (`drivers/virtio.c`)

Legacy I/O registers (offsets from BAR0):

```
+0x00 HOST_FEATURES  (u32, read)      +0x08 QUEUE_PFN (u32, r/w)
+0x04 GUEST_FEATURES (u32, write)     +0x0C QUEUE_NUM (u16, read)
+0x0E QUEUE_SEL (u16, write)          +0x10 QUEUE_NOTIFY (u16, write)
+0x12 STATUS (u8)                     +0x13 ISR (u8, read clears)
+0x14 device config (device-specific)
```

Status bits: ACKNOWLEDGE=1, DRIVER=2, DRIVER_OK=4, FAILED=0x80.

Device API:

- `int virtio_dev_legacy_probe(unsigned int bar)` — sanity: reading
  `HOST_FEATURES` must not return `0xFFFFFFFF` (else not a legacy device).
- `void virtio_dev_reset(dev)` — write STATUS=0.
- `int virtio_dev_init(dev, u32 supported_features, u32 *features)` — reset,
  ACKNOWLEDGE, DRIVER, read HOST_FEATURES, AND with `supported_features`,
  write GUEST_FEATURES, DRIVER_OK. Returns the negotiated set.
- `int virtio_setup_queue(dev, idx, n)` — QUEUE_SEL=idx; read QUEUE_NUM;
  require `QUEUE_NUM >= n`; allocate one **contiguous** block via
  `page_alloc_order(2)` (4 pages, 16 KiB — the smallest power-of-two block
  covering the 12 KiB vring); lay out desc table at `base`, avail ring at
  `base + 4096`, used ring at `base + 8192` (the device computes these
  offsets itself from QUEUE_PFN, so the three rings MUST be contiguous in
  legacy mode); write `QUEUE_PFN = (u32)base >> 12` (page frame number).
  `page_alloc_order` guarantees the block is naturally aligned and identity
  mapped, so physical == virtual.

Queue layout (legacy vring, queue of `n` descriptors):

- offset +0x000: `vring_desc[n]` — `u64 addr; u32 len; u16 flags; u16 next`
  (flags: NEXT=1, WRITE=2), `free_head` linked list.
- offset +0x1000: `vring_avail` — `u16 flags; u16 idx; u16 ring[n]`.
- offset +0x2000: `vring_used` — `u16 flags; u16 idx; struct{v u32 id, len} ring[n]`.

For `n = 16` each ring fits one page; the 4th page of the order-2 block is
unused. Queue pages stay allocated for the driver's lifetime (never freed);
they land below 0x08000000 because the buddy allocates low-first (out of the
Linux task window).

### IRQ dispatch (shared lines)

`virtio_irq_dispatch()` iterates every registered `struct virtio_dev` and,
for those whose IRQ equals the current one, reads `ISR` (inb BAR0+0x13):

- `ISR & 1` -> queue interrupt: call `dev->on_irq()`.
- `ISR & 2` -> config change: log, ignore.

`virtio_init()` registers the dispatch via `irq_install_handler(irq, ...)`
for each distinct IRQ found among virtio devices (10 and 11 in the probe).
Multiple virtio devices on one line are handled because the dispatch checks
each device's ISR register. blk and rng are synchronous and do not depend on
their IRQ; only net RX relies on the dispatch.

### virtio-blk (`drivers/vblk.c`)

Synchronous, one 512-byte sector per request, polled (no IRQ dependency):

```
struct virtio_blk_req { u32 type; u32 reserved; u64 sector; };  // header
type: VIRTIO_BLK_T_IN=0 (read), VIRTIO_BLK_T_OUT=1 (write)
```

Descriptor chain per request — `[header|WRITE] [data|WRITE for IN / READ for
OUT] [status(u8)|WRITE]`. Status byte: 0=OK, 1=IOERR, 2=UNSUPPORTED.

- `int vblk_read(u32 lba, void *buf)` / `int vblk_write(u32 lba, const void *buf)`
  — submit, kick, spin on `vq_used_pop` with a `tick`-based timeout
  (`(int)(tick - start) >= 500` like `wait_td_done`), return 0/negative.
- Capacity read from device config (u64 at BAR0+0x14) at init; stored in
  `vblk_nsectors` and capped at SFS needs.
- Data buffer: one `page_alloc()` page reused across requests (identity
  mapped); the request writes into it, then the caller's `memcpy`.

### virtio-rng (`drivers/vrng.c`)

One queue, one WRITE descriptor holding the output buffer, polled like blk.
`vrng_bytes(void *buf, u32 n)` (n <= 4096) fills from used-ring length.
Used by `SYS_RANDOM`: `sys_random(char *ubuf, u32 maxlen)` copies up to
`maxlen` bytes into the user buffer (validated with `in_user()`); the
`random` program prints them as hex.

### virtio-net (`drivers/vnet.c`)

- **RX**: `RX_BUFS=8` 2048-byte buffers (one `page_alloc()` page each) laid
  as WRITE descriptors. On a queue interrupt (`dev->on_irq`), drain
  `vq_used_pop()`: for each completed RX descriptor, copy the frame to the
  kernel log (`net: RX frame len=%u`), then **echo it back via TX** (loopback
  test needs no GUI interaction) and re-submit a fresh WRITE descriptor, kick.
- **TX**: `vnet_send(frame, len)` — one descriptor chain `[virtio_net_hdr(10
  bytes, zeroed)|READ] [frame|READ]`, kick, return immediately; descriptors
  are reclaimed lazily from the used ring on the next send or interrupt.
  `vnet_tx_count` bumped. Header fields stay zero because no net features
  (CSUM etc.) are negotiated.
- MAC: negotiate `VIRTIO_NET_F_MAC` (bit 5) if offered; read 6 bytes from
  device config (BAR0+0x14). Only logged, not required for TX/RX.

## Data flow

```
virtio_init()  (kernel.c, after usb_init, before fs_init)
  -> pci_find_all()  -> for each virtio device: probe, dev_init,
     setup_queue, register IRQ dispatch
  -> vblk_init()   (if found)  vblk_nsectors = capacity/512
  -> vrng_init()   (if found)
  -> vnet_init()   (if found)  RX buffer pool

fs_init() (existing call site)
  -> sfs_disk_present? yes: read FS image from disk into FS_MEM, validate
     magic; bad magic -> fs_format() + sfs_flush()
     no: existing RAM-only path

fs_write / fs_create / fs_delete / fs_format
  -> sfs_touch(off, len) marks dirty sectors
  -> sfs_flush() writes only dirty sectors via vblk_write, clears bitmap
```

## Error handling

- No virtio devices found: `virtio_init()` returns silently; RAM-only SFS,
  `SYS_RANDOM` returns -1, net functions no-op. ISO boot without `-drive`
  stays fully functional.
- Legacy probe fails (`HOST_FEATURES == 0xFFFFFFFF`): log, skip device.
- Queue setup fails (QUEUE_NUM too small, page_alloc fail): log, mark
  device unusable, continue.
- blk/rng timeout: return error, log `virtio: blk timeout`; SFS flush errors
  are logged but do not panic.
- Bad SFS magic on disk: reformat (like today's `fs_init`) and flush — a
  corrupt/garbage disk is recovered automatically.
- Shared IRQ collision with timer/keyboard/mouse would break input: the
  probe shows virtio lands on 10/11, both free; the test harness asserts
  `info pci`/serial log shows no conflict.

## Testing

New `make test` entries (each boots `aos.iso` under QEMU headless and asserts
on serial log; persistence test reuses a raw disk image across two boots):

1. **virtiotest.py** — persistence:
   - Boot A with `-drive file=<tmp>.img` (4 MiB raw). After boot, host-side
     assert the image contains `SFS1` at offset 0 and a `bin/hello` filename
     string somewhere (proves flush wrote the FS to disk). Boot B with the
     same image: assert the serial log shows `SFS mounted from disk` (not
     "formatted") — proves the FS was loaded from disk.
2. **rngtest.py** — boot with `-device virtio-rng-pci`; assert serial output
   has `rng: selftest OK` (vrng_init fetches 16 bytes at boot and prints a
   non-zero hex).
3. **netlooptest.py** — boot with `-netdev socket,listen=127.0.0.1:PORT` +
   `virtio-net-pci`. A host Python client connects and sends one 60-byte
   Ethernet frame; the guest logs `net: RX frame len=60`, then **echoes the
   frame back** — assert the host socket receives the identical bytes. This
   proves RX (IRQ + pool) and TX (send + header) with no GUI interaction.
4. Existing `linhello lincat ipctest manytest notepadtest` stay green
   (boot without a drive -> RAM SFS unchanged).

Manual: `make run` gains the three virtio devices and an auto-created
`disk.img`, so GUI usage exercises blk (persistent files) live.

## Risks

- **Legacy-only**: `disable-modern=on` is required in every QEMU invocation
  (Makefile + tests); a future modern-mode driver is a separate effort.
- **Shared IRQ 11** (blk+rng): contained by the per-device ISR dispatch;
  blk/rng are polled anyway, so only net RX is IRQ-sensitive (IRQ 10).
- **Flush-on-every-mutation**: a burst of FS writes is 1–2 sector requests
  each; negligible in QEMU, but the whole-image flush on `format` (2048
  sectors) takes a short polled loop — acceptable for a demo.
- **Queue page lifetime**: pages are never freed; 3 pages per queue (blk 3,
  rng 3, net ~9 incl. RX buffers) is a few KiB — no concern at 256 MB.
