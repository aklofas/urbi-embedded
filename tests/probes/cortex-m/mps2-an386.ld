MEMORY {
  FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 4M
  SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 4M
}
ENTRY(Reset_Handler)
SECTIONS {
  .isr : { KEEP(*(.isr_vector)) } > FLASH
  .text : {
    *(.text*) *(.rodata*)
    . = ALIGN(4); __init_array_start = .; KEEP(*(.init_array*)) __init_array_end = .;
  } > FLASH
  .ARM.exidx : { *(.ARM.exidx*) } > FLASH
  __etext = .;
  .data : AT(__etext) { __data_start__ = .; *(.data*) . = ALIGN(4); __data_end__ = .; } > SRAM
  .bss : { __bss_start__ = .; *(.bss*) *(COMMON) . = ALIGN(4); __bss_end__ = .; } > SRAM
  . = ALIGN(8);
  end = .; __end__ = .;
  __HeapLimit = ORIGIN(SRAM) + LENGTH(SRAM) - 0x8000;
  __StackTop  = ORIGIN(SRAM) + LENGTH(SRAM);
  __StackLimit = __StackTop - 0x8000;
}
