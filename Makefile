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
              drivers/vrng.o drivers/vblk.o drivers/vnet.o drivers/rtc.o \
              kernel/terminal.o kernel/commands.o \
              kernel/vfs.o kernel/vfscompat.o kernel/procfs.o kernel/string.o arch/i386/gdt.o arch/i386/idt.o \
               kernel/interrupts.o kernel/bt.o kernel/elf.o kernel/syscall.o kernel/aos_gui.o \
               kernel/progload.o kernel/paging.o kernel/pmm.o kernel/kmm.o \
              kernel/config.o kernel/user.o \
              kernel/user_tramp.o kernel/printf.o kernel/progs.o \
               kernel/task.o kernel/linux_syscall.o kernel/block.o kernel/sfs2.o \
               kernel/klog.o kernel/trace.o kernel/symtab.o

PROGRAMS = help uptime clear echo tick info reboot panic ls cat rm format shutdown test wm term clock date ipctest notepad many linrun sleeptest exitto random fstest procinfo bgspawn cp mv mkdir rmdir head wc

# All AOS programs are now built with the static musl i386 toolchain (Task 30).
# If it is not installed, the program ELFs are skipped (fallback like lin/*):
# the kernel still builds, only the ramdisk program payload is not embedded.
MUSL_CC  = tools/musl-i686/bin/i686-linux-musl-gcc
MUSL_OK  = $(wildcard $(MUSL_CC))
PROG_ELFS = $(if $(MUSL_OK),$(addprefix build/prog/,$(addsuffix .elf,$(PROGRAMS))))

# The Linux ELF payload (lin/*) is built with the same static musl i386 toolchain.
LINUX_CC  = $(MUSL_CC)
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

# Programs are static musl ELFs (Task 30). wm additionally links the pure-C
# ICO decoder (programs/musl/ico.c); the GUI apps link the shared theme loader
# (programs/musl/theme.c).
build/prog/%.elf: programs/musl/%.c programs/aosabi.h
	@mkdir -p build/prog
	$(MUSL_CC) -static -no-pie -Os -Wall -Wextra -Iprograms -o $@ $<

build/prog/wm.elf: programs/musl/wm.c programs/musl/ico.c programs/musl/theme.c programs/aosabi.h
	@mkdir -p build/prog
	$(MUSL_CC) -static -no-pie -Os -Wall -Wextra -Iprograms -o $@ programs/musl/wm.c programs/musl/ico.c programs/musl/theme.c

build/prog/term.elf: programs/musl/term.c programs/musl/theme.c programs/aosabi.h
	@mkdir -p build/prog
	$(MUSL_CC) -static -no-pie -Os -Wall -Wextra -Iprograms -o $@ programs/musl/term.c programs/musl/theme.c

build/prog/clock.elf: programs/musl/clock.c programs/musl/theme.c programs/aosabi.h
	@mkdir -p build/prog
	$(MUSL_CC) -static -no-pie -Os -Wall -Wextra -Iprograms -o $@ programs/musl/clock.c programs/musl/theme.c

build/prog/notepad.elf: programs/musl/notepad.c programs/musl/theme.c programs/aosabi.h
	@mkdir -p build/prog
	$(MUSL_CC) -static -no-pie -Os -Wall -Wextra -Iprograms -o $@ programs/musl/notepad.c programs/musl/theme.c

scripts/demo.ico: scripts/gen_ico.py
	$(PYTHON) scripts/gen_ico.py > $@

build/linux/%: tools/linux/%.c
	@mkdir -p build/linux
	$(LINUX_CC) -static -no-pie -Os -o $@ $<

kernel/progs.c: $(PROG_ELFS) scripts/demo.ico scripts/gen_progs.py $(LINUX_BINS)
	$(PYTHON) scripts/gen_progs.py $(PROG_ELFS) --data demo.ico=scripts/demo.ico \
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

# Headless debug launch: VNC + QMP + serial Unix sockets for the qemu-vnc MCP
# tools (vm_connect vnc_port=5907, qmp_socket=/tmp/aos-debug.qmp,
# serial_socket=/tmp/aos-debug.serial).
debug: aos.iso
	scripts/qemu-debug.sh

# Headless regression suite: each script boots aos.iso under QEMU, drives the
# GUI via the monitor socket, and asserts on serial log + PPM screenshots.
# linhello/lincat need the musl Linux payload, so they are included only when
# the musl toolchain is installed.
LINUX_TESTS = $(if $(LINUX_BINS),linhello lincat lindirtest)
TESTS = ipctest manytest notepadtest sleeptest rngtest blktest virtiotest netlooptest rtctest configtest klogtest stracetest stracelive shelltest panictest $(LINUX_TESTS)

# Fast subset for CI: quick boots, no extra virtio devices. The musl Linux
# tests are included only when the musl toolchain is installed.
FAST_TESTS = ipctest $(if $(LINUX_BINS),linhello lincat)

test: aos.iso
	@set -e; for t in $(TESTS); do \
		echo "===== $$t ====="; \
		$(PYTHON) scripts/$$t.py; \
	done
	@echo "ALL $(words $(TESTS)) TESTS PASSED"

test-fast: aos.iso
	@set -e; for t in $(FAST_TESTS); do \
		echo "===== $$t ====="; \
		$(PYTHON) scripts/$$t.py; \
	done
	@echo "ALL $(words $(FAST_TESTS)) FAST TESTS PASSED"

clean:
	rm -f $(KERNEL_OBJS) $(PROG_ELFS) *.elf *.bin *.iso disk.img kernel/progs.c
	rm -f $(KERNEL_OBJS:.o=.d)
	rm -rf iso build

-include $(KERNEL_OBJS:.o=.d)

.SECONDARY: $(KERNEL_OBJS)

.PHONY: all run test test-fast clean
