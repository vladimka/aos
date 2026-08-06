ENTRY(start)

SECTIONS {
    . = 1M;
    _start = .;

    .text : {
        *(.text)
    }

    .rodata : {
        *(.rodata)
    }

    .data : {
        *(.data)
    }

    .bss : {
        *(COMMON)
        *(.bss)
    }

    . = ALIGN(4096);
    _end = .;

    /* The ramdisk lives at RAMDISK_BASE (kernel/block.h). If the kernel image
       grows into it, SFS2 writes clobber kernel BSS (task table, zombie list,
       allocator metadata) at runtime. Fail the link instead. */
    ASSERT(_end <= 0x00400000, "kernel image overlaps the ramdisk (move RAMDISK_BASE up)")
}
