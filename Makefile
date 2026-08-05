CC      = gcc
AS      = gcc
LD      = ld
PYTHON  = python3
CFLAGS  = -ffreestanding -Wall -Wextra -O2 -std=c11 -nostdlib -fno-builtin \
          -fno-stack-protector -fno-pie -fno-pic -m32 -mno-sse -mno-mmx -mno-80387 \
          -MMD -MP
CFLAGS  += -Ikernel -Idrivers -Iarch/i386 -Iboot -Iprograms
ASFLAGS = -m32 -c -x assembler-with-cpp
LDFLAGS = -T linker.ld -m elf_i386 -nostdlib --no-warn-rwx-segments

KERNEL_OBJS = boot/boot.o boot/isr.o kernel/kernel.o drivers/vga.o \
              drivers/serial.o drivers/mouse.o drivers/pci.o drivers/uhci.o drivers/virtio.o \
              drivers/vrng.o drivers/vblk.o drivers/vnet.o drivers/rtc.o \
              kernel/terminal.o kernel/commands.o \
              kernel/sfs.o kernel/string.o arch/i386/gdt.o arch/i386/idt.o \
              kernel/interrupts.o kernel/elf.o kernel/syscall.o \
              kernel/progload.o kernel/paging.o kernel/pmm.o kernel/kmm.o \
              kernel/config.o kernel/user.o \
              kernel/user_tramp.o kernel/printf.o kernel/progs.o \
              kernel/task.o kernel/linux_syscall.o kernel/block.o

PROGRAMS = help uptime clear echo tick info reboot panic ls cat rm format shutdown test wm term clock date ipctest notepad many linrun sleeptest exitto random
PROG_ELFS = $(addprefix programs/, $(addsuffix .elf, $(PROGRAMS)))
PROG_OBJS = $(addprefix programs/, $(addsuffix .o, $(PROGRAMS))) programs/libaos.o programs/ico.o

# The Linux ELF payload (lin/*) is built with a static musl i386 toolchain.
# If it is not installed, the payload is skipped: the kernel still builds and
# the AOS programs run, only the musl binaries are not embedded in the ramdisk.
LINUX_CC  = tools/musl-i686/bin/i686-linux-musl-gcc
LINUX_SRCS = $(wildcard tools/linux/*.c)
LINUX_BINS = $(if $(wildcard $(LINUX_CC)),$(patsubst tools/linux/%.c,build/linux/%,$(LINUX_SRCS)))
LINUX_EMBED = $(if $(LINUX_BINS),--data lin/hello=build/linux/hello --data lin/ls=build/linux/ls --data lin/cat=build/linux/cat --data lin/test.txt=tools/linux/test.txt)

all: aos.iso

boot/%.o: boot/%.S
	$(AS) $(ASFLAGS) -o $@ $<

kernel/%.o: kernel/%.S
	$(AS) $(ASFLAGS) -o $@ $<

kernel/%.o: kernel/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

drivers/%.o: drivers/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

arch/i386/%.o: arch/i386/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

programs/%.o: programs/%.c programs/libaos.h programs/programs.ld
	$(CC) $(CFLAGS) -Iprograms -c -o $@ $<

programs/libaos.o: programs/libaos.c programs/libaos.h
	$(CC) $(CFLAGS) -Iprograms -c -o $@ $<

programs/%.elf: programs/%.o programs/libaos.o programs/programs.ld
	$(LD) -T programs/programs.ld -m elf_i386 -static -nostdlib -n --no-warn-rwx-segments -o $@ programs/libaos.o $<

# wm links the pure-C ICO decoder in addition to libaos.
programs/wm.elf: programs/wm.o programs/ico.o programs/libaos.o programs/programs.ld
	$(LD) -T programs/programs.ld -m elf_i386 -static -nostdlib -n --no-warn-rwx-segments -o $@ programs/libaos.o programs/wm.o programs/ico.o

scripts/demo.ico: scripts/gen_ico.py
	$(PYTHON) scripts/gen_ico.py > $@

build/linux/%: tools/linux/%.c
	@mkdir -p build/linux
	$(LINUX_CC) -static -no-pie -Os -o $@ $<

kernel/progs.c: $(PROG_ELFS) scripts/demo.ico scripts/gen_progs.py $(LINUX_BINS)
	$(PYTHON) scripts/gen_progs.py $(PROG_ELFS) --data demo.ico=scripts/demo.ico \
		$(LINUX_EMBED) > $@

compile_commands.json: scripts/gen_compile_commands.py $(wildcard kernel/*.c drivers/*.c arch/i386/*.c boot/*.c programs/*.c)
	$(PYTHON) scripts/gen_compile_commands.py

aos.elf: $(KERNEL_OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)

aos.iso: aos.elf
	mkdir -p iso/boot/grub
	cp $< iso/boot/aos.elf
	printf 'set timeout=0\nmenuentry "AOS" {\n  insmod all_video\n  multiboot2 /boot/aos.elf\n}\n' > iso/boot/grub/grub.cfg
	grub-mkrescue -o $@ iso

disk.img:
	truncate -s 4M $@

run: aos.iso disk.img
	qemu-system-i386 -m 256 -rtc base=localtime -display gtk,grab-on-hover=on -cdrom $< \
	  -drive file=disk.img,format=raw,if=none,id=d0 \
	  -device virtio-blk-pci,disable-modern=on,drive=d0 \
	  -device virtio-rng-pci,disable-modern=on \
	  -netdev socket,id=n0,listen=127.0.0.1:9400 \
	  -device virtio-net-pci,disable-modern=on,mac=52:54:00:12:34:56,netdev=n0

# Headless regression suite: each script boots aos.iso under QEMU, drives the
# GUI via the monitor socket, and asserts on serial log + PPM screenshots.
# linhello/lincat need the musl Linux payload, so they are included only when
# the musl toolchain is installed.
LINUX_TESTS = $(if $(LINUX_BINS),linhello lincat)
TESTS = ipctest manytest notepadtest sleeptest rngtest blktest virtiotest netlooptest rtctest configtest $(LINUX_TESTS)

test: aos.iso
	@set -e; for t in $(TESTS); do \
		echo "===== $$t ====="; \
		$(PYTHON) scripts/$$t.py; \
	done
	@echo "ALL $(words $(TESTS)) TESTS PASSED"

clean:
	rm -f $(KERNEL_OBJS) $(PROG_ELFS) $(PROG_OBJS) *.elf *.bin *.iso disk.img kernel/progs.c
	rm -f $(KERNEL_OBJS:.o=.d) $(PROG_OBJS:.o=.d)
	rm -rf iso
	rm -rf build

-include $(KERNEL_OBJS:.o=.d) $(PROG_OBJS:.o=.d)

.SECONDARY: $(KERNEL_OBJS) $(PROG_OBJS) $(PROG_ELFS)

.PHONY: all run test clean
