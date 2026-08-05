#include "block.h"
#include "vblk.h"
#include "serial.h"
#include "string.h"

static struct sdev *dev;

static struct sdev sdev_ram = {
    .present = 1,
    .capacity_sectors = RAMDISK_SECTORS,
};

static struct sdev sdev_vblk;

static int sdev_ram_read(unsigned int lba, void *buf) {
    memcpy(buf, (void *)(RAMDISK_BASE + lba * BLOCK_SIZE), BLOCK_SIZE);
    return 0;
}

static int sdev_ram_write(unsigned int lba, const void *buf) {
    memcpy((void *)(RAMDISK_BASE + lba * BLOCK_SIZE), buf, BLOCK_SIZE);
    return 0;
}

static int sdev_vblk_read(unsigned int lba, void *buf) {
    return vblk_read(lba, buf);
}

static int sdev_vblk_write(unsigned int lba, const void *buf) {
    return vblk_write(lba, buf);
}

static unsigned char cache[BLOCK_CACHE_SECTORS][BLOCK_SIZE];
static unsigned int  cache_lba[BLOCK_CACHE_SECTORS];
static unsigned int  cache_age[BLOCK_CACHE_SECTORS];
static unsigned char cache_valid[BLOCK_CACHE_SECTORS];
static unsigned char cache_dirty[BLOCK_CACHE_SECTORS];
static unsigned char cache_pin[BLOCK_CACHE_SECTORS];
static unsigned int  age_counter;

static int cache_lookup(unsigned int lba) {
    for (unsigned int i = 0; i < BLOCK_CACHE_SECTORS; i++)
        if (cache_valid[i] && cache_lba[i] == lba) return (int)i;
    return -1;
}

static void cache_evict(unsigned int slot) {
    if (cache_valid[slot] && cache_dirty[slot] && dev && dev->write)
        dev->write(cache_lba[slot], cache[slot]);
    cache_valid[slot] = 0;
    cache_dirty[slot] = 0;
    cache_pin[slot] = 0;
}

static int cache_alloc_slot(void) {
    for (unsigned int i = 0; i < BLOCK_CACHE_SECTORS; i++)
        if (!cache_valid[i]) return (int)i;
    int victim = -1;
    unsigned int oldest = 0xFFFFFFFFu;
    for (unsigned int i = 0; i < BLOCK_CACHE_SECTORS; i++) {
        if (cache_pin[i]) continue;
        if (cache_age[i] < oldest) {
            oldest = cache_age[i];
            victim = (int)i;
        }
    }
    if (victim >= 0) cache_evict((unsigned int)victim);
    return victim;
}

static int cache_load(unsigned int slot, unsigned int lba) {
    if (!dev || !dev->read) return -1;
    if (dev->read(lba, cache[slot]) != 0) return -1;
    cache_lba[slot] = lba;
    cache_age[slot] = age_counter++;
    cache_valid[slot] = 1;
    cache_dirty[slot] = 0;
    cache_pin[slot] = 0;
    return 0;
}

unsigned char *block_pin(unsigned int lba) {
    int slot = cache_lookup(lba);
    if (slot < 0) {
        slot = cache_alloc_slot();
        if (slot < 0) return 0;
        if (cache_load((unsigned int)slot, lba) != 0) return 0;
    }
    cache_age[slot] = age_counter++;
    cache_pin[slot] = 1;
    return cache[slot];
}

void block_unpin(unsigned int lba) {
    int slot = cache_lookup(lba);
    if (slot >= 0) cache_pin[slot] = 0;
}

void block_mark_dirty(unsigned int lba) {
    int slot = cache_lookup(lba);
    if (slot >= 0) cache_dirty[slot] = 1;
}

void block_flush(void) {
    if (!dev || !dev->write) return;
    for (unsigned int i = 0; i < BLOCK_CACHE_SECTORS; i++) {
        if (!cache_valid[i] || !cache_dirty[i]) continue;
        if (dev->write(cache_lba[i], cache[i]) == 0)
            cache_dirty[i] = 0;
    }
}

struct sdev *block_get_sdev(void) {
    return dev;
}

void block_set_sdev(struct sdev *d) {
    dev = d;
}

void block_reset(void) {
    block_flush();
    for (unsigned int i = 0; i < BLOCK_CACHE_SECTORS; i++)
        cache_valid[i] = cache_dirty[i] = cache_pin[i] = 0;
}

int block_disk_present(void) {
    return dev == &sdev_vblk && sdev_vblk.present;
}

int block_init(void) {
    if (vblk_present()) {
        sdev_vblk.read = sdev_vblk_read;
        sdev_vblk.write = sdev_vblk_write;
        sdev_vblk.present = 1;
        sdev_vblk.capacity_sectors = vblk_capacity_bytes() / BLOCK_SIZE;
        dev = &sdev_vblk;
        serial_print("block: disk backend, ");
        serial_print_dec(sdev_vblk.capacity_sectors);
        serial_print(" sectors\n");
    } else {
        sdev_ram.read = sdev_ram_read;
        sdev_ram.write = sdev_ram_write;
        dev = &sdev_ram;
        serial_print("block: ram backend, ");
        serial_print_dec(RAMDISK_SECTORS);
        serial_print(" sectors\n");
    }
    return 0;
}
