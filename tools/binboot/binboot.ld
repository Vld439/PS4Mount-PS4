/* One flat section: _start first, then the rest; __payload marks where build.sh appends the ELF. */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)
SECTIONS {
    . = 0;
    .text : {
        *(.text.start)
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(16);
    }
    __payload = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.data*) *(.bss*) }
}
