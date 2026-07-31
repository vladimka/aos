.section .text
.extern isr_handler
.extern irq_handler
.extern syscall_handler
.extern task_switch_kernel

.macro isr_noerr index
.global isr\index
isr\index:
    push $0
    push $\index
    jmp isr_common
.endm

.macro isr_err index
.global isr\index
isr\index:
    push $\index
    jmp isr_common
.endm

isr_noerr 0
isr_noerr 1
isr_noerr 2
isr_noerr 3
isr_noerr 4
isr_noerr 5
isr_noerr 6
isr_noerr 7
isr_err   8
isr_noerr 9
isr_err   10
isr_err   11
isr_err   12
isr_err   13
isr_err   14
isr_noerr 15
isr_noerr 16
isr_noerr 17
isr_noerr 18
isr_noerr 19
isr_noerr 20
isr_noerr 21
isr_noerr 22
isr_noerr 23
isr_noerr 24
isr_noerr 25
isr_noerr 26
isr_noerr 27
isr_noerr 28
isr_noerr 29
isr_noerr 30
isr_noerr 31

isr_common:
    pusha
    push %ds
    push %es
    push %fs
    push %gs
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    push %esp
    mov $isr_handler, %eax
    call *%eax
    add $4, %esp
    jmp switch_and_restore

.macro irq index num
.global irq\index
irq\index:
    push $0
    push $\num
    jmp irq_common
.endm

irq 0, 32
irq 1, 33
irq 2, 34
irq 3, 35
irq 4, 36
irq 5, 37
irq 6, 38
irq 7, 39
irq 8, 40
irq 9, 41
irq 10, 42
irq 11, 43
irq 12, 44
irq 13, 45
irq 14, 46
irq 15, 47

.global isr128
isr128:
    push $0
    push $128
    jmp syscall_common

syscall_common:
    pusha
    push %ds
    push %es
    push %fs
    push %gs
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    push %esp
    mov $syscall_handler, %eax
    call *%eax
    add $4, %esp
    jmp switch_and_restore

irq_common:
    pusha
    push %ds
    push %es
    push %fs
    push %gs
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    push %esp
    mov $irq_handler, %eax
    call *%eax
    add $4, %esp
    jmp switch_and_restore

# ---- Scheduler exit path ----
# After the C handler returns, ESP points at the current task's saved frame
# (its kernel_esp). Let the scheduler pick the next task, switch page
# directories + TSS esp0, then restore that task's frame.
switch_and_restore:
    push %esp               # current kernel_esp
    call task_switch_kernel # returns next task's kernel_esp in %eax
    add $4, %esp
    mov %eax, %esp
task_restore:
    pop %gs
    pop %fs
    pop %es
    pop %ds
    popa
    add $8, %esp
    iret
