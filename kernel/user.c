#include "user.h"
#include "gdt.h"
#include "terminal.h"
#include "serial.h"
#include "task.h"

#define USER_STACK_TOP 0x01804000

// Stack the CPU switches to (via TSS) on every int 0x80 / IRQ from ring 3.
static unsigned char sys_stack[8192] __attribute__((aligned(16)));

// Kernel stack pointer captured at launch; user_program_exit restores it.
unsigned int saved_esp;

// Callee-saved registers captured at launch. The in-place guest runs as a
// tail call from exec_from_path, so launch_user_* never returns through a
// normal epilogue; the guest's exit leaves %ebx/%esi/%edi/%ebp clobbered.
// user_exit_asm restores these so exec_from_path resumes with a live frame
// pointer and the trace flag intact.
unsigned int saved_ebx;
unsigned int saved_esi;
unsigned int saved_edi;
unsigned int saved_ebp;

static int program_active = 0;

void launch_user_asm(void (*entry)(void));
void launch_user_linux(void (*entry)(void), unsigned int esp);
void user_exit_asm(void) __attribute__((noreturn));

void user_init(void) {
    tss_set_esp0((unsigned int)&sys_stack[8192]);
    terminal_reset_keys();
}

unsigned int user_kstack_top(void) {
    return (unsigned int)&sys_stack[8192];
}

int user_program_active(void) {
    return program_active;
}

void user_program_start(void (*entry)(void)) {
    program_active = 1;
    launch_user_asm(entry);
}

void user_program_start_linux(void (*entry)(void), unsigned int esp) {
    program_active = 1;
    launch_user_linux(entry, esp);
}

void user_program_exit(void) {
    serial_print("EXIT: saved_esp=0x");
    serial_print_hex(saved_esp);
    serial_print(" pid=");
    serial_print_dec(task_current_pid());
    serial_print(" kesp0=0x");
    serial_print_hex(task_kernel_esp(0));
    serial_print(" active=");
    serial_print_dec(program_active);
    serial_print("\n");
    program_active = 0;
    user_exit_asm();
}
