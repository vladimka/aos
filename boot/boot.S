.section .text
.balign 8

multiboot2_start:
.long 0xE85250D6
.long 0
.long multiboot2_end - multiboot2_start
.long -(0xE85250D6 + 0 + (multiboot2_end - multiboot2_start))

.balign 8
.word 5
.word 0
.long 20
.long 1024
.long 768
.long 32

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
