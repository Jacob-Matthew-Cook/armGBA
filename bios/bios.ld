/* The BIOS image: 16 KiB of code and constants at address 0, and no writable data, since the BIOS
 * runs from ROM. */
OUTPUT_FORMAT("elf32-littlearm")
ENTRY(vectors)

MEMORY {
	rom (rx) : ORIGIN = 0x00000000, LENGTH = 16K
}

SECTIONS {
	.text : {
		KEEP(*(.vectors))
		*(.text .text.*)
		*(.rodata .rodata.*)
	} > rom
	.data : { *(.data .data.*) } > rom
	.bss : { *(.bss .bss.* COMMON) } > rom
	/DISCARD/ : { *(.comment) *(.ARM.attributes) *(.ARM.exidx*) *(.note*) }
}

ASSERT(SIZEOF(.data) == 0 && SIZEOF(.bss) == 0, "the BIOS can't have writable variables: it runs from ROM")
