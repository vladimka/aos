/*
 * Hardware acceleration for the framebuffer.
 *
 * Two independent features, both written as pure I/O + MSR/PTE manipulation,
 * so they are safe to use on the real hardware this kernel targets:
 *
 *  1) Bochs/VBE (BGA) VBE_DISPI "panning" --- the BGA command register pair
 *     at 0x01CE/0x01CF exposes a Y_OFFSET register (dispi index 0x9). The
 *     scanout then reads VRAM starting at Y_OFFSET * bytes/pitch *pixels*
 *     (QEMU: vbe_start_addr = (X_OFFSET*bpp/8 + Y_OFFSET*linelength) / 4),
 *     so scrolling the console by whole text rows becomes a single port
 *     write instead of a multi-MB VRAM blit. Only QEMU/VirtualBox expose the
 *     BGA port interface (real VGA hardware uses int10 only), so this is a
 *     strictly optional fast path with a full software fallback.
 *
 *  2) Framebuffer write-combining via MTRR (preferred) or PAT. On real x86
 *     the LFB is device memory; marking it write-combining turns the
 *     per-pixel blits into buffered writes. QEMU ignores both MSR sets (the
 *     guest never notices the difference), so this is a genuine
 *     real-hardware feature and is verified by the serial banner only.
 */
#include "hwaccel.h"
#include "ports.h"
#include "printf.h"
#include "paging.h"

/* -- BGA VBE_DISPI registers / ports ---------------------------- */
#define BGA_IO_INDEX 0x01CE
#define BGA_IO_DATA  0x01CF

#define BGA_INDEX_ID        0x0   /* VBE_DISPI_INDEX_ID */
#define BGA_INDEX_XRES      0x1
#define BGA_INDEX_YRES      0x2
#define BGA_INDEX_BPP       0x3
#define BGA_INDEX_ENABLE    0x4
#define BGA_INDEX_BANK      0x5
#define BGA_INDEX_VIRT_W    0x6
#define BGA_INDEX_VIRT_H    0x7
#define BGA_INDEX_X_OFFSET  0x8
#define BGA_INDEX_Y_OFFSET  0x9
#define BGA_INDEX_VIDMEM64K 0xa

/* The write-then-readback port probe: on non-BGA hardware the ports are
 * LPT/other aliases and the readback will not echo the magic values. */
#define BGA_ID0 0xB0C0
#define BGA_ID5 0xB0C5

static int bga_present;

static unsigned short bga_read(unsigned short idx) {
    outw(BGA_IO_INDEX, idx);
    return inw(BGA_IO_DATA);
}

static void bga_write(unsigned short idx, unsigned short val) {
    outw(BGA_IO_INDEX, idx);
    outw(BGA_IO_DATA, val);
}

static void bga_probe(void) {
    bga_present = 0;
    bga_write(BGA_INDEX_ID, BGA_ID0);
    if (bga_read(BGA_INDEX_ID) != BGA_ID0) return;
    bga_write(BGA_INDEX_ID, BGA_ID5);
    if (bga_read(BGA_INDEX_ID) != BGA_ID5) return;
    bga_present = 1;
}

/*
 * Return the maximum number of whole text rows (16 px each) the BGA
 * Y_OFFSET can pan through given the current linear mode, or 0 when
 * panning is unusable (no BGA, or VIRT_WIDTH does not match the guest's
 * fb_pitch, which would make Y_OFFSET rows misaligned with the guest's
 * texel layout).
 */
/* No libgcc, so no __udivdi3: do the u64/u32 division manually. */
static unsigned long udiv64_32(unsigned long long n, unsigned long d) {
    unsigned long q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((unsigned)((n >> i) & 1));
        if (r >= d) {
            r -= d;
            q |= 1ul << i;
        }
    }
    return q;
}

int hwaccel_pan_probe(unsigned int linelength, unsigned int vis_px,
                      unsigned int bpp) {
    bga_probe();
    printf("hwaccel: bga_present=%d\n", bga_present);
    if (!bga_present) return 0;
    printf("hwaccel: vw=%d ll=%d\n", bga_read(BGA_INDEX_VIRT_W),
           linelength);

    if (bga_read(BGA_INDEX_VIRT_W) * (bpp / 8) != linelength)
        return 0;

    unsigned long vram_rows = udiv64_32(
        (unsigned long long)bga_read(BGA_INDEX_VIDMEM64K) * 65536ull,
        linelength);
    if (vram_rows <= vis_px)
        return 0;
    return (int)((vram_rows - vis_px) / 16);
}

void hwaccel_pan_set(int rows) {
    bga_write(BGA_INDEX_Y_OFFSET, (unsigned short)(rows > 0 ? rows * 16 : 0));
}

void hwaccel_pan_reset(void) {
    if (bga_present) {
        bga_write(BGA_INDEX_Y_OFFSET, 0);
    }
}

unsigned int hwaccel_pan_get_pixels(void) {
    if (!bga_present) return 0;
    return bga_read(BGA_INDEX_Y_OFFSET);
}

/* -- write-combining framebuffer (MTRR / PAT) -------------------- */

static unsigned long long rdmsr(unsigned int msr) {
    unsigned int lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((unsigned long long)hi << 32) | lo;
}

static void wrmsr(unsigned int msr, unsigned long long val) {
    __asm__ __volatile__("wrmsr"
                         : : "c"(msr), "a"((unsigned int)val),
                             "d"((unsigned int)(val >> 32)));
}

static void cpuid_leaf1(unsigned int *edx) {
    unsigned int a, b, c, d;
    __asm__ __volatile__("cpuid"
                         : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                         : "a"(1));
    (void)a; (void)b; (void)c;
    *edx = d;
}

#define MTRR_CAP_MSR   0xFE
#define MTRR_BASE(i)   (0x200 + 2 * (i))
#define MTRR_MASK(i)   (0x200 + 2 * (i) + 1)
#define MTRR_DEFTYPE   0x2FF
#define PAT_MSR        0x277
#define MTRR_WC        1
#define PAT_WC         1
#define MTRR_MASK_VALID 0x800
#define MTRR_ENABLE   0xC00   /* enable + fixed-range enable, default WB */

/*
 * Mark the framebuffer physical range as write-combining. Prefers an aligned
 * power-of-two variable MTRR range; falls back to PAT index 1 = WC plus the
 * PWT bit on the framebuffer PTEs (extra_pt identity mapping).
 *
 * Only applies when the fb lives above the low 1 GB identity map: a low RAM
 * buffer (e.g. the virtio-gpu framebuffer is guest RAM) must stay write-back.
 */
void hwaccel_wc_framebuffer(unsigned int phys, unsigned int size) {
    if (size == 0 || phys < 0x10000000)
        return;

    unsigned int edx;
    cpuid_leaf1(&edx);
    if (!(edx & (1u << 12))) {           /* MTRR flag */
        printf("hwaccel: no MTRR (fb remains write-back)\n");
        return;
    }

    /* Prefer an aligned power-of-two MTRR range. */
    if (size && (size & (size - 1)) == 0 && (phys & (size - 1)) == 0) {
        unsigned long long cap = rdmsr(MTRR_CAP_MSR);
        int vcnt = (int)(cap & 0xFF);
        for (int i = 0; i < vcnt; i++) {
            if (rdmsr(MTRR_MASK(i)) & MTRR_MASK_VALID)
                continue;               /* range already used */
            wrmsr(MTRR_BASE(i),
                  (unsigned long long)phys | (unsigned long long)MTRR_WC);
            wrmsr(MTRR_MASK(i),
                  (~(unsigned long long)(size - 1) & 0xFFFFF000ull) |
                  MTRR_MASK_VALID);
            wrmsr(MTRR_DEFTYPE, MTRR_ENABLE | 6);   /* default WB */
            __asm__ __volatile__("wbinvd");
            paging_set_cr3(paging_get_cr3());
            printf("hwaccel: framebuffer %x..%x write-combining (MTRR range)\n",
                   phys, phys + size);
            return;
        }
        printf("hwaccel: no free MTRR range, falling back to PAT\n");
    }

    /* PAT: index 1 = WC, then tag the fb PTEs with PWT (index 1 = 1). */
    if (!(edx & (1u << 16))) {           /* PAT flag */
        printf("hwaccel: no PAT either (fb remains write-back)\n");
        return;
    }
    unsigned long long pat = rdmsr(PAT_MSR);
    pat &= ~0xFF00ull;
    pat |= (unsigned long long)PAT_WC << 8;
    wrmsr(PAT_MSR, pat);
    paging_mark_pwt(phys, size);
    __asm__ __volatile__("wbinvd");
    paging_set_cr3(paging_get_cr3());
    printf("hwaccel: framebuffer %x..%x write-combining (PAT)\n",
           phys, phys + size);
}