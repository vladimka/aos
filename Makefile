CC      = gcc
AS      = gcc
LD      = ld
PYTHON  = python3
CFLAGS  = -ffreestanding -Wall -Wextra -O2 -std=c11 -nostdlib -fno-builtin \
          -fno-stack-protector -fno-pie -fno-pic -fno-omit-frame-pointer \
          -m32 -mno-sse -mno-mmx -mno-80387 -D__AOS_KERNEL__ -MMD -MP
CFLAGS  += -Ikernel -Idrivers -Iarch/i386 -Iboot -Iprograms
ASFLAGS = -m32 -c -x assembler-with-cpp
LDFLAGS = -T linker.ld -m elf_i386 -nostdlib --no-warn-rwx-segments

KERNEL_OBJS = boot/boot.o boot/isr.o kernel/kernel.o drivers/vga.o \
              drivers/serial.o drivers/mouse.o drivers/pci.o drivers/uhci.o drivers/virtio.o \
              drivers/virtio_modern.o \
              drivers/vrng.o drivers/vblk.o drivers/vnet.o drivers/rtc.o drivers/ata.o \
              drivers/ahci.o drivers/virtio_gpu.o \
              kernel/terminal.o kernel/commands.o \
              kernel/vfs.o kernel/vfscompat.o kernel/procfs.o kernel/string.o arch/i386/gdt.o arch/i386/idt.o \
               kernel/interrupts.o kernel/bt.o kernel/elf.o kernel/syscall.o kernel/aos_gui.o \
               kernel/progload.o kernel/paging.o kernel/pmm.o kernel/kmm.o \
              kernel/config.o kernel/user.o \
              kernel/user_tramp.o kernel/printf.o kernel/progs.o \
               kernel/task.o kernel/linux_syscall.o kernel/pipe.o kernel/block.o kernel/sfs2.o \
               kernel/klog.o kernel/trace.o kernel/symtab.o

PROGRAMS = help uptime clear echo tick info reboot panic ls cat rm format shutdown test wm term clock date ipctest notepad many linrun sleeptest sh exitto random fstest procinfo bgspawn cp mv mkdir rmdir head wc sync envp ps init kill strace whoami id sudo login useradd passwd su files

# All AOS programs and the Linux ELF payload (lin/*) are built with the
# static musl i386 toolchain. It is a hard build dependency: without it the
# ramdisk would have no programs and the GUI tests would fail. Install it
# with `make install`.
MUSL_CC  = tools/musl-i686/bin/i686-linux-musl-gcc
PROG_ELFS = $(addprefix build/prog/,$(addsuffix .elf,$(PROGRAMS)))

LINUX_CC  = $(MUSL_CC)
LINUX_SRCS = $(wildcard tools/linux/*.c)
LINUX_BINS = $(patsubst tools/linux/%.c,build/linux/%,$(LINUX_SRCS))
LINUX_EMBED = --data lin/hello=build/linux/hello --data lin/ls=build/linux/ls \
	--data lin/cat=build/linux/cat --data lin/piptest=build/linux/piptest \
	--data lin/test.txt=tools/linux/test.txt

all: check-toolchain aos.iso

# Fail fast with a clear message instead of silently building a program-less
# kernel when the musl i386 cross toolchain is missing (it is gitignored).
check-toolchain:
	@if [ ! -x "$(MUSL_CC)" ]; then \
		echo "ERROR: musl i386 toolchain not found at $(MUSL_CC)"; \
		echo "Install it with: make install"; \
		exit 1; \
	fi

# ---------------------------------------------------------------------------
# Dependency installation (Ubuntu/Debian). `make install` installs everything
# needed to build the ISO and run the test suite. Requires sudo for apt.
# ---------------------------------------------------------------------------
SUDO ?= sudo

MUSL_URL = https://musl.cc/i686-linux-musl-cross.tgz
MUSL_TGZ = /tmp/i686-linux-musl-cross.tgz

BUILD_DEPS = build-essential gcc-multilib binutils make curl \
	grub-pc-bin xorriso mtools
TEST_DEPS = qemu-system-x86 python3

install: install-deps install-toolchain

install-deps:
	$(SUDO) apt-get update
	$(SUDO) apt-get install -y $(BUILD_DEPS) $(TEST_DEPS)

install-toolchain:
	@if [ -x "$(MUSL_CC)" ]; then \
		echo "musl i386 toolchain already installed at $(MUSL_CC)"; \
	else \
		echo "Downloading musl i386 cross toolchain from $(MUSL_URL)"; \
		curl -L -o $(MUSL_TGZ) $(MUSL_URL); \
		mkdir -p tools; \
		rm -rf tools/musl-i686; \
		mkdir -p tools/musl-i686; \
		tar -xzf $(MUSL_TGZ) --strip-components=1 -C tools/musl-i686; \
		rm -f $(MUSL_TGZ); \
		echo "musl i386 toolchain installed at tools/musl-i686"; \
	fi

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

# Programs are static musl ELFs (Task 30). wm additionally links the pure-C
# ICO decoder (programs/musl/ico.c); the GUI apps link the shared theme loader
# (programs/musl/theme.c).
build/prog/%.elf: programs/musl/%.c programs/musl/uutils.c programs/aosabi.h
	@mkdir -p build/prog
	$(MUSL_CC) -static -no-pie -Os -Wall -Wextra -Iprograms -o $@ $< programs/musl/uutils.c

build/prog/wm.elf: programs/musl/wm.c programs/musl/ico.c programs/musl/theme.c programs/aosabi.h
	@mkdir -p build/prog
	$(MUSL_CC) -static -no-pie -Os -Wall -Wextra -Iprograms -o $@ programs/musl/wm.c programs/musl/ico.c programs/musl/theme.c

build/prog/term.elf: programs/musl/term.c programs/musl/gui.c programs/musl/theme.c programs/aosabi.h
	@mkdir -p build/prog
	$(MUSL_CC) -static -no-pie -Os -Wall -Wextra -Iprograms -o $@ programs/musl/term.c programs/musl/gui.c programs/musl/theme.c

build/prog/clock.elf: programs/musl/clock.c programs/musl/gui.c programs/musl/theme.c programs/aosabi.h
	@mkdir -p build/prog
	$(MUSL_CC) -static -no-pie -Os -Wall -Wextra -Iprograms -o $@ programs/musl/clock.c programs/musl/gui.c programs/musl/theme.c

build/prog/notepad.elf: programs/musl/notepad.c programs/musl/gui.c programs/musl/theme.c programs/aosabi.h
	@mkdir -p build/prog
	$(MUSL_CC) -static -no-pie -Os -Wall -Wextra -Iprograms -o $@ programs/musl/notepad.c programs/musl/gui.c programs/musl/theme.c

build/prog/files.elf: programs/musl/files.c programs/musl/gui.c programs/musl/theme.c programs/aosabi.h
	@mkdir -p build/prog
	$(MUSL_CC) -static -no-pie -Os -Wall -Wextra -Iprograms -o $@ programs/musl/files.c programs/musl/gui.c programs/musl/theme.c

scripts/demo.ico: scripts/gen_ico.py
	$(PYTHON) scripts/gen_ico.py > $@

build/linux/%: tools/linux/%.c
	@mkdir -p build/linux
	$(LINUX_CC) -static -no-pie -Os -o $@ $<

kernel/progs.c: $(PROG_ELFS) scripts/demo.ico scripts/init.conf scripts/gen_progs.py $(LINUX_BINS)
	$(PYTHON) scripts/gen_progs.py $(PROG_ELFS) --data demo.ico=scripts/demo.ico \
		--data etc/init.conf=scripts/init.conf \
		$(LINUX_EMBED) > $@

compile_commands.json: scripts/gen_compile_commands.py $(wildcard kernel/*.c drivers/*.c arch/i386/*.c boot/*.c programs/musl/*.c)
	$(PYTHON) scripts/gen_compile_commands.py

# Two-pass link: link once (with the previous kernel/symtab.c), nm it to
# regenerate kernel/symtab.c, recompile kernel/symtab.o, relink. symtab.o is
# the LAST object, so its own .rodata never shifts other symbols' addresses.
aos.elf: $(KERNEL_OBJS) linker.ld scripts/gen_symtab.py
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)
	$(PYTHON) scripts/gen_symtab.py $@ kernel/symtab.c
	$(CC) $(CFLAGS) -c -o kernel/symtab.o kernel/symtab.c
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)

aos.iso: aos.elf aos-text.elf
	mkdir -p iso/boot/grub
	cp aos.elf iso/boot/aos.elf
	cp aos-text.elf iso/boot/aos-text.elf
	printf 'set timeout=60\nset default=0\nmenuentry "AOS" {\n  insmod all_video\n  multiboot2 /boot/aos.elf\n}\nmenuentry "AOS (text)" {\n  multiboot2 /boot/aos-text.elf\n}\n' > iso/boot/grub/grub.cfg
	grub-mkrescue -o $@ iso

# Text-mode kernel: same objects, but the MB2 header requests no framebuffer,
# so GRUB leaves the console in VGA text mode (vga_init() takes the text path).
# Depends on aos.elf so the two-pass symtab link has already produced the
# final kernel/symtab.o.
aos-text.elf: aos.elf boot/boot-text.o $(filter-out boot/boot.o,$(KERNEL_OBJS))
	$(LD) $(LDFLAGS) -o $@ boot/boot-text.o $(filter-out boot/boot.o,$(KERNEL_OBJS))

disk.img:
	truncate -s 4M $@

run: aos.iso disk.img
	qemu-system-i386 -m 256 -rtc base=localtime -display gtk,grab-on-hover=on,show-cursor=on -cdrom $< \
	  -vga none -device virtio-vga,disable-modern=on \
	  -drive file=disk.img,format=raw,if=none,id=d0 \
	  -device virtio-blk-pci,disable-modern=on,drive=d0 \
	  -device virtio-rng-pci,disable-modern=on \
	  -netdev socket,id=n0,listen=127.0.0.1:9400 \
	  -device virtio-net-pci,disable-modern=on,mac=52:54:00:12:34:56,netdev=n0 \
	  -device piix3-usb-uhci -device usb-tablet

# Headless debug launch: VNC + QMP + serial Unix sockets for the qemu-vnc MCP
# tools (vm_connect vnc_port=5907, qmp_socket=/tmp/aos-debug.qmp,
# serial_socket=/tmp/aos-debug.serial).
debug: aos.iso
	scripts/qemu-debug.sh

# Headless regression suite: each script boots aos.iso under QEMU, drives the
# GUI via the monitor socket, and asserts on serial log + PPM screenshots.
LINUX_TESTS = linhello lincat lindirtest pipetest
TESTS = ipctest manytest notepadtest sleeptest rngtest blktest atatest virtiotest netlooptest rtctest configtest klogtest stracetest stracelive shelltest panictest fstoolstest toolflags lsflagstest sgrcolor pstest inittest textmodetest termscrolltest $(LINUX_TESTS) vguitest powertest tablettest

# Fast subset for CI: quick boots, no extra virtio devices.
FAST_TESTS = ipctest linhello lincat

test: check-toolchain aos.iso
	@set -e; for t in $(TESTS); do \
		echo "===== $$t ====="; \
		$(PYTHON) scripts/$$t.py; \
	done
	@echo "ALL $(words $(TESTS)) TESTS PASSED"

test-fast: check-toolchain aos.iso
	@set -e; for t in $(FAST_TESTS); do \
		echo "===== $$t ====="; \
		$(PYTHON) scripts/$$t.py; \
	done
	@echo "ALL $(words $(FAST_TESTS)) FAST TESTS PASSED"

clean:
	rm -f $(KERNEL_OBJS) $(PROG_ELFS) *.elf *.bin *.iso disk.img kernel/progs.c
	rm -f $(KERNEL_OBJS:.o=.d)
	rm -rf iso build
	rm -f $(MUSL_TGZ)

-include $(KERNEL_OBJS:.o=.d)
-include boot/boot-text.d

.SECONDARY: $(KERNEL_OBJS)

.PHONY: all run test test-fast clean install install-deps install-toolchain check-toolchain
