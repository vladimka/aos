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

    /* The SFS ramdisk lives at SFS_BASE (kernel/sfs.h). If the kernel image
       grows into it, SFS writes clobber kernel BSS (task table, zombie list,
       allocator metadata) at runtime. Fail the link instead. */
    ASSERT(_end <= 0x00300000, "kernel image overlaps the SFS ramdisk (move SFS_BASE up)")
}
