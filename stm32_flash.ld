/*
*****************************************************************************
**
**  File        : stm32_flash.ld
**
**  Abstract    : Linker script for STM32F765 Device with
**                2048KByte FLASH, 512KByte RAM
**
**                Set heap size, stack size and stack location according
**                to application requirements.
**
**                Set memory bank area and size if external memory is used.
**
**  Target      : STMicroelectronics STM32
**
**  Distribution: The file is distributed “as is,” without any warranty
**                of any kind.
**
**  (c)Copyright Atollic AB.
**  You may use this file as-is or modify it according to the needs of your
**  project. Distribution of this file (unmodified or modified) is not
**  permitted. Atollic AB permit registered Atollic TrueSTUDIO(R) users the
**  rights to distribute the assembled, compiled & linked contents of this
**  file as part of an application binary file, provided that it is built
**  using the Atollic TrueSTUDIO(R) toolchain.
**
*****************************************************************************
*/

/* Entry Point */
ENTRY(Reset_Handler)

/* RAM layout
 *
 * DTCM  0x20000000  64K   wired straight to the core: no bus wait, never cached,
 *                         and reachable by DMA. holds the stack & every static.
 * SRAM  0x20010000  192K  SRAM1 + SRAM2. a few KB of USB buffers (.sram_noinit),
 *                         then all the rest is the heap (lua).
 *
 * The stack sits at the very bottom of RAM and grows down, so running out of
 * it faults on the unmapped space below 0x20000000 instead of quietly
 * overwriting the heap or the statics.
 *
 * 0x20006666 is the word the bootloader checks after a reset (lib/bootloader.c
 * and the bootloader's main.c). Nothing may live there: a reset while it happened
 * to hold the magic value would land in DFU mode.
 */
_Stack_Size   = 0x6660;
_Boot_Word    = 0x20006666;

/* Specify the memory areas */
MEMORY
{
  /* FLASH (rx)      : ORIGIN = 0x00200000, LENGTH = 512K */ /* ITCM interface */
  FLASH (rx)      : ORIGIN = 0x08020000, LENGTH = 384K /* AXIM interface */
  DTCM (xrw)      : ORIGIN = 0x20000000, LENGTH = 64K
  SRAM (xrw)      : ORIGIN = 0x20010000, LENGTH = 192K
  /* MEMORY_B1 (rx)  : ORIGIN = 0x60000000, LENGTH = 0K */
  /* CCMRAM (rw)     : ORIGIN = 0x10000000, LENGTH = 64K */
}

/* Define output sections */
SECTIONS
{
  /* The startup code goes first into FLASH */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector)) /* Startup code */
    . = ALIGN(4);
  } >FLASH

  /* The program code and other data goes into FLASH */
  .text :
  {
    . = ALIGN(4);
    *(.text)           /* .text sections (code) */
    *(.text*)          /* .text* sections (code) */
    *(.glue_7)         /* glue arm to thumb code */
    *(.glue_7t)        /* glue thumb to arm code */
    *(.eh_frame)

    KEEP (*(.init))
    KEEP (*(.fini))

    . = ALIGN(4);
    _etext = .;        /* define a global symbols at end of code */
  } >FLASH

  /* Constant data goes into FLASH */
  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)         /* .rodata sections (constants, strings, etc.) */
    *(.rodata*)        /* .rodata* sections (constants, strings, etc.) */
    . = ALIGN(4);
  } >FLASH

  .ARM.extab   : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
  .ARM : {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  .preinit_array     :
  {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } >FLASH
  .init_array :
  {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
  } >FLASH
  .fini_array :
  {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } >FLASH

  /* the stack, then the bootloader's word, before anything else in RAM */
  .stack (NOLOAD) :
  {
    _sstack = .;
    . = . + _Stack_Size;
    _estack = .;
    . = . + 0x10; /* 0x20006660..0x2000666F stays empty */
  } >DTCM
  ASSERT( _estack <= _Boot_Word && _Boot_Word + 4 <= ADDR(.stack) + SIZEOF(.stack)
        , "the bootloader's word must sit in the reserved gap above the stack" )

  /* used by the startup to initialize data */
  _sidata = LOADADDR(.data);

  /* Initialized data sections goes into RAM, load LMA copy after code */
  .data : 
  {
    . = ALIGN(4);
    _sdata = .;        /* create a global symbol at data start */
    *(.data)           /* .data sections */
    *(.data*)          /* .data* sections */

    . = ALIGN(4);
    _edata = .;        /* define a global symbol at data end */
  } >DTCM AT> FLASH

  /* Not copied or zeroed by the startup code, so it survives a reset.
     Holds the crash report (lib/crashreport.c). */
  .noinit (NOLOAD) :
  {
    . = ALIGN(4);
    *(.noinit)
    *(.noinit*)
    . = ALIGN(4);
  } >DTCM

  
  /* Uninitialized data section */
  . = ALIGN(4);
  .bss :
  {
    /* This is used by the startup in order to initialize the .bss secion */
    _sbss = .;         /* define a global symbol at bss start */
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)

    . = ALIGN(4);
    _ebss = .;         /* define a global symbol at bss end */
    __bss_end__ = _ebss;
  } >DTCM

  /* Buffers that are big, only touched by the CPU and set up by their owner
     before use (USB packet rings), mark themselves SRAM_NOINIT to stay out of
     DTCM. Not zeroed by the startup code. The heap starts after them. */
  .sram_noinit (NOLOAD) :
  {
    . = ALIGN(4);
    *(.sram_noinit)
    *(.sram_noinit*)
    . = ALIGN(4);
    _esram_noinit = .;
  } >SRAM

  /* the heap: see _sbrk() in syscalls.c */
  _sheap = _esram_noinit;
  _eheap = ORIGIN(SRAM) + LENGTH(SRAM);

  

  /* Remove information from the standard libraries */
  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
