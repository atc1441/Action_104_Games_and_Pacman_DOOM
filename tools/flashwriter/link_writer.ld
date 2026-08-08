/*
 * Flash writer: runs entirely from RAM.
 *
 *   0x20010000  code + rodata + data (16 KB reserved)
 *   0x2001C000  top of stack
 *   0x2001E000  mailbox
 *   0x20020000  data buffer, 64 KB
 *
 * 0x20000000-0x2000FFFF is left alone (bootloader variables).
 */
MEMORY
{
    RAM (rwx) : ORIGIN = 0x20010000, LENGTH = 48K
}

_estack = 0x2001C000;

ENTRY(Reset_Handler)

SECTIONS
{
    .isr_vector : { . = ALIGN(512); KEEP(*(.isr_vector)) } > RAM
    .text       : { . = ALIGN(4); *(.text*) *(.rodata*) . = ALIGN(4); } > RAM
    .data       : { . = ALIGN(4); _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > RAM
    .bss (NOLOAD) : { . = ALIGN(4); _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > RAM
    /DISCARD/   : { *(.ARM.exidx*) *(.ARM.attributes) }
}
