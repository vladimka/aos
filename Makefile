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
              drivers/serial.o drivers/mouse.o drivers/pci.o drivers/uhci.o \
              kernel/terminal.o kernel/commands.o \
              kernel/sfs.o kernel/string.o arch/i386/gdt.o arch/i386/idt.o \
              kernel/interrupts.o kernel/elf.o kernel/syscall.o \
              kernel/progload.o kernel/paging.o kernel/pmm.o kernel/user.o \
              kernel/user_tramp.o kernel/printf.o kernel/progs.o \
              kernel/task.o

PROGRAMS = help uptime clear echo tick info reboot panic ls cat rm format shutdown test wm term clock ipctest notepad
PROG_ELFS = $(addprefix programs/, $(addsuffix .elf, $(PROGRAMS)))
PROG_OBJS = $(addprefix programs/, $(addsuffix .o, $(PROGRAMS))) programs/libaos.o programs/ico.o

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

kernel/progs.c: $(PROG_ELFS) scripts/demo.ico scripts/gen_progs.py
	$(PYTHON) scripts/gen_progs.py $(PROG_ELFS) --data demo.ico=scripts/demo.ico > $@

compile_commands.json: scripts/gen_compile_commands.py $(wildcard kernel/*.c drivers/*.c arch/i386/*.c boot/*.c programs/*.c)
	$(PYTHON) scripts/gen_compile_commands.py

aos.elf: $(KERNEL_OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)

aos.iso: aos.elf
	mkdir -p iso/boot/grub
	cp $< iso/boot/aos.elf
	printf 'set timeout=0\nmenuentry "AOS" {\n  insmod all_video\n  multiboot2 /boot/aos.elf\n}\n' > iso/boot/grub/grub.cfg
	grub-mkrescue -o $@ iso

run: aos.iso
	qemu-system-i386 -display gtk,grab-on-hover=on -cdrom $<

clean:
	rm -f $(KERNEL_OBJS) $(PROG_ELFS) $(PROG_OBJS) *.elf *.bin *.iso kernel/progs.c
	rm -f $(KERNEL_OBJS:.o=.d) $(PROG_OBJS:.o=.d)
	rm -rf iso

-include $(KERNEL_OBJS:.o=.d) $(PROG_OBJS:.o=.d)

.SECONDARY: $(KERNEL_OBJS) $(PROG_OBJS) $(PROG_ELFS)

.PHONY: all run clean
