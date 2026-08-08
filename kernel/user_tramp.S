.section .text

# void launch_user_asm(void (*entry)(void))
# Enter ring 3 at `entry` with a fresh user stack. Never returns.
# On exit, the CPU's iret returns to the kernel stack that was in use at
# the point this was called; saved_esp lets user_program_exit unwind to it.
.global launch_user_asm
launch_user_asm:
    movl 4(%esp), %ecx          # entry point
    movl %esp, saved_esp        # kernel stack to return to on program exit
    movl %ebx, saved_ebx        # preserve callee-saved regs for user_exit_asm
    movl %esi, saved_esi
    movl %edi, saved_edi
    movl %ebp, saved_ebp
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

# void launch_user_linux(void (*entry)(void), unsigned int esp)
# Like launch_user_asm, but takes the initial user ESP from the caller
# (used for Linux binaries loaded at 0x08048000+).
.global launch_user_linux
launch_user_linux:
    movl 4(%esp), %ecx          # entry point
    movl 8(%esp), %edx          # user ESP
    movl %esp, saved_esp        # kernel stack to return to on program exit
    movl %ebx, saved_ebx        # preserve callee-saved regs for user_exit_asm
    movl %esi, saved_esi
    movl %edi, saved_edi
    movl %ebp, saved_ebp
    movl $0x23, %eax            # user data segment
    movl %eax, %ds
    movl %eax, %es
    movl %eax, %fs
    movl %eax, %gs
    pushl $0x23                 # user SS
    pushl %edx                  # user ESP
    pushl $0x202                # EFLAGS: IF set, IOPL 0
    pushl $0x1B                 # user CS
    pushl %ecx                  # EIP
    iret

# void user_exit_asm(void)
# Terminate the current user program: switch back to the kernel stack that
# was active at launch, restore the callee-saved registers saved there, and
# return into the shell as if launch_user_* had returned. Never returns
# normally.
.global user_exit_asm
user_exit_asm:
    # Get off the shared sys_stack BEFORE enabling interrupts: the frame
    # behind us is the exiting program's ring-3 syscall frame. An IRQ taken
    # while still here would make task_switch_kernel record kernel_esp[0] on
    # sys_stack, and the next resume of task 0 would iret back into the dead
    # program (ring 3, garbage EIP/ESP) instead of the shell -> panic.
    cli
    movl saved_esp, %esp
    # Restore the callee-saved registers captured at launch. The guest's
    # exit leaves them clobbered; exec_from_path's POSTRUN block and epilogue
    # read the frame pointer (%ebp) and the trace flag (%esi) from them.
    movl saved_ebx, %ebx
    movl saved_esi, %esi
    movl saved_edi, %edi
    movl saved_ebp, %ebp
    # Build a synthetic IRQ-style resume frame (same layout switch_and_restore
    # restores: gs fs es ds + pusha + err/num + eip cs eflags) on top of the
    # saved return point, and point task 0's kernel_esp at it. Until the idle
    # loop takes its next real IRQ this is the only valid resume frame the
    # scheduler can use for task 0; without it the guard would see the stale
    # sys_stack frame and either starve every other task or crash on the next
    # task exit. The frame iret's to shell_resume, which rets into the actual
    # continuation saved at launch.
    pushl $0x202          # eflags: IF set, IOPL 0
    pushl $0x8            # cs: kernel
    pushl $shell_resume   # eip
    pushl $0              # err slot
    pushl $0              # num slot
    pushl %eax            # pusha: eax
    pushl %ecx            #        ecx
    pushl %edx            #        edx
    pushl %ebx            #        ebx
    pushl %esp            #        esp (discarded by popa)
    pushl %ebp            #        ebp
    pushl %esi            #        esi
    pushl %edi            #        edi
    pushl $0              # ds
    pushl $0              # es
    pushl $0              # fs
    pushl $0              # gs  (frame base = candidate kernel_esp[0])
    pushl %esp            # arg: frame address
    call task_set_kernel_esp0
    addl $4, %esp
    # The direct iret must see eip/cs/eflags at [esp..esp+8]; the scheduler
    # path (task_restore) pops gs/fs/es/ds + popa + err/num first, so the
    # frame stores them at +0x38. Skip the register slots here.
    addl $0x38, %esp
    iret                  # -> shell_resume

shell_resume:
    # Arrives with ESP = saved_esp (iret popped the synthetic frame above).
    ret                   # pop the continuation saved at launch
