/*
 * DOOM image: XIP from flash, all of RAM for heap and framebuffer.
 *
 * The bootloader jumps to 0x08004000 and expects an initial SP and reset
 * handler there. Its own RAM below 0x20002000 is not needed after the
 * handover and is free for us.
 *
 * SRAM size measured on the device: 0x20000000-0x20045FFF, i.e. 280 KB.
 * A core read at 0x20046000 triggers a system reset. Over SWD that same
 * range reads back 0xAA without complaint - that is just how the bus
 * answers for absent memory, not a fill pattern. Do not be fooled by it.
 *
 * At the very bottom sits a small fixed area for the mailbox the host
 * reads over SWD. Earlier versions used guessed addresses around
 * 0x2000FD00, which land inside the zone heap - it starts past .bss
 * around 0x2000F870 and grows over them, so firmware and host quietly
 * overwrote each other. Down here the space is permanently free (the
 * vector table lives in flash) and the addresses stay put no matter how
 * .bss grows.
 */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08004000, LENGTH = 1M      /* to 0x08104000 */
    MBOX  (rw)  : ORIGIN = 0x20000000, LENGTH = 256
    RAM   (rwx) : ORIGIN = 0x20000100, LENGTH = 280K - 256
}

_estack = 0x20046000;

ENTRY(Reset_Handler)

SECTIONS
{
    .mailbox (NOLOAD) : { KEEP(*(.mailbox)) } > MBOX
    .isr_vector : { . = ALIGN(512); KEEP(*(.isr_vector)) } > FLASH
    .text       : { . = ALIGN(4); *(.text*) *(.rodata*) . = ALIGN(4); } > FLASH
    .data       : { . = ALIGN(4); _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > RAM AT > FLASH
    _sidata = LOADADDR(.data);
    .bss (NOLOAD) : { . = ALIGN(4); _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > RAM
    _end = .;
    end = .;          /* newlib's _sbrk expects this symbol */
    PROVIDE(__heap_start = .);
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) }
}
