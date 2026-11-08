ENTRY(_start)
SECTIONS {
 . = 0x2000;
 .text : { KEEP(*(.text.entry)) *(.text .text.*) }
 .rodata : { *(.rodata .rodata.*) }
 .data : { *(.data .data.* .sdata .sdata.*) }
 .bss (NOLOAD) : { *(.bss .bss.* .sbss .sbss.* COMMON) }
 /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) }
}
ASSERT(SIZEOF(.data) == 0 && SIZEOF(.bss) == 0, "bringup state belongs in request SRAM")
ASSERT(. <= 0x2f00, "bringup payload overlaps boot marks or private stack")
