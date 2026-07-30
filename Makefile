CC      = gcc
AS      = gcc
LD      = ld
PYTHON  = python3
CFLAGS  = -ffreestanding -Wall -Wextra -O2 -std=c11 -nostdlib -fno-builtin \
          -fno-stack-protector -m32 -mno-sse -mno-mmx -mno-80387
CFLAGS  += -Ikernel -Idrivers -Iarch/i386 -Iboot
ASFLAGS = -m32 -c -x assembler-with-cpp
LDFLAGS = -T linker.ld -m elf_i386 -nostdlib --no-warn-rwx-segments

KERNEL_OBJS = boot/boot.o boot/isr.o kernel/kernel.o drivers/vga.o \
              drivers/serial.o drivers/mouse.o kernel/terminal.o kernel/commands.o \
              kernel/sfs.o kernel/string.o arch/i386/gdt.o arch/i386/idt.o \
              kernel/interrupts.o kernel/elf.o kernel/syscall.o \
              kernel/progload.o kernel/progs.o

PROGRAMS = help uptime clear echo tick info reboot panic ls cat rm format shutdown test
PROG_ELFS = $(addprefix programs/, $(addsuffix .elf, $(PROGRAMS)))

all: aos.iso

boot/%.o: boot/%.S
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

kernel/progs.c: $(PROG_ELFS) scripts/gen_progs.py
	$(PYTHON) scripts/gen_progs.py $(PROG_ELFS) > $@

aos.elf: $(KERNEL_OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)

aos.iso: aos.elf
	mkdir -p iso/boot/grub
	cp $< iso/boot/aos.elf
	printf 'set timeout=0\nmenuentry "AOS" {\n  insmod all_video\n  multiboot2 /boot/aos.elf\n}\n' > iso/boot/grub/grub.cfg
	grub-mkrescue -o $@ iso

run: aos.iso
	qemu-system-i386 -cdrom $<

clean:
	rm -f $(KERNEL_OBJS) $(PROG_ELFS) *.elf *.bin *.iso kernel/progs.c
	rm -rf iso programs/*.o

.PHONY: all run clean
