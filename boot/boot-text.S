.section .text
.balign 8

/* Multiboot2 boot header WITHOUT the framebuffer tag: the loader keeps the
 * console in classic VGA text mode and passes no framebuffer tag in the MBI,
 * so vga_init() takes the text path (0xB8000). Used for the "AOS (text)"
 * grub.cfg menu entry via build/aos-text.elf. */

multiboot2_start:
.long 0xE85250D6
.long 0
.long multiboot2_end - multiboot2_start
.long -(0xE85250D6 + 0 + (multiboot2_end - multiboot2_start))

.balign 8
.word 0
.word 0
.long 8
multiboot2_end:

.global start
.extern kernel_main

start:
    mov $stack_top, %esp

    push %ebx
    push %eax

    call kernel_main

    cli
hang:
    hlt
    jmp hang

.section .bss
.balign 16
stack_bottom:
    .skip 16384
stack_top:
