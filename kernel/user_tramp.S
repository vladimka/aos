.section .text

# void launch_user_asm(void (*entry)(void))
# Enter ring 3 at `entry` with a fresh user stack. Never returns.
# On exit, the CPU's iret returns to the kernel stack that was in use at
# the point this was called; saved_esp lets user_program_exit unwind to it.
.global launch_user_asm
launch_user_asm:
    movl 4(%esp), %ecx          # entry point
    movl %esp, saved_esp        # kernel stack to return to on program exit
    movl $0x23, %eax            # user data segment
    movl %eax, %ds
    movl %eax, %es
    movl %eax, %fs
    movl %eax, %gs
    pushl $0x23                 # user SS
    pushl $0x01804000           # user ESP
    pushl $0x202                # EFLAGS: IF set, IOPL 0
    pushl $0x1B                 # user CS
    pushl %ecx                  # EIP
    iret

# void user_exit_asm(void)
# Terminate the current user program: switch back to the kernel stack that
# was active at launch and return into the shell as if launch_user_asm
# had returned. Never returns normally.
.global user_exit_asm
user_exit_asm:
    sti
    movl saved_esp, %esp
    ret
