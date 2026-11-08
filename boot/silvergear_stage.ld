ENTRY(silvergear_stage)
SECTIONS {
 . = 0x3c20;
 .text : { *(.text.entry) *(.text*) }
 /DISCARD/ : { *(.comment) *(.note*) }
}
ASSERT(. <= 0x3e00, "Silvergear stage overlaps request block")
