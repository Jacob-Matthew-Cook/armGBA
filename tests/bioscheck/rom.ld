OUTPUT_FORMAT("elf32-littlearm")
ENTRY(_start)
MEMORY {
	rom (rx) : ORIGIN = 0x08000000, LENGTH = 32M
}
SECTIONS {
	.text : {
		KEEP(*(.header))
		*(.text .text.*)
		*(.rodata .rodata.*)
	} > rom
	.data : { *(.data .data.*) } > rom
	.bss : { *(.bss .bss.* COMMON) } > rom
	/DISCARD/ : { *(.comment) *(.ARM.attributes) *(.ARM.exidx*) *(.note*) }
}
ASSERT(SIZEOF(.data) == 0 && SIZEOF(.bss) == 0, "the test ROM keeps its variables on the stack or in EWRAM")
