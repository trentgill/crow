#include <errno.h>
#include <stdio.h>
#include <stm32f7xx.h>
#include "ll/debug_usart.h"

extern int errno;
// the heap has SRAM1 & SRAM2 to itself (see stm32_flash.ld)
extern char _sheap;
extern char _eheap;

// TODO implement a str_buffer here to avoid overflowing the TRACE pin?

int _write(int file, char* data, int len)
{
#ifndef RELEASE
#ifdef TRACE
    int count = len;
    while(count--){
        fputc((uint32_t)*data++, (FILE*)file);
    }
#else // TRACE using USART to debug
    U_Print(data,len); // enqueues a string
#endif // TRACE
#endif // RELEASE
    return len;
}

// use an #ifdef to choose ITM or USART (CLI flag?)
int fputc(int ch, FILE *f)
{
    f = f;
    ITM_SendChar( (uint32_t)ch );
    return ch;
}

void* _sbrk(int incr)
{
    static char *heap_end;
    char *prev_heap_end;

    if (heap_end == 0)
        heap_end = &_sheap;

    prev_heap_end = heap_end;
    if (heap_end + incr > &_eheap || heap_end + incr < &_sheap)
    {
        errno = ENOMEM;
        return (void*) -1;
    }

    heap_end += incr;

    return (void*) prev_heap_end;
}

// empty syscall functions to quiet the compiler
// perhaps we should actually implement them to make some stdlib stuff work?
// perhaps we should add debug prints so we know if/when they're being called
void _close(void){}
void _fstat_r(void){}
void _getpid_r(void){}
void _gettimeofday_r(void){}
void _isatty_r(void){}
void _kill_r(void){}
void _link_r(void){}
void _lseek(void){}
void _read(void){}
void _open_r(void){}
void _times_r(void){}
void _unlink_r(void){}
