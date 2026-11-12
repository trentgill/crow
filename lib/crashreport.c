#include "crashreport.h"

#include <stdio.h>  // snprintf(), printf()
#include <string.h> // memset()
#include <stddef.h> // offsetof()

#include "stm32f7xx_hal.h"  // HAL_GetTick(), NVIC_SystemReset(), SCB
#include "lib/caw.h"        // Caw_send_luachunk()
#include "usbd/usbd_main.h" // USBD_HandleTypeDef, USB_tx_is_ready()

// The fault handlers and the Lua panic handler fill in a report in .noinit RAM
// (see stm32_flash.ld: the startup code neither copies nor zeroes it, so it
// survives NVIC_SystemReset), then reset. On the next boot the report is
// copied out and printed over USB once a host is connected.
//
// .noinit sits in DTCM, which is never cached, so the report is in RAM (not
// waiting in the D-cache) when the reset happens.

#define CRASH_MAGIC      0x43525348 // 'CRSH'. change it if Crash_t changes
#define CRASH_MSG_LEN    64         // lua panic message, including the '\0'

#define CRASH_PRINTS     3     // send the report this many times
#define CRASH_FIRST_MS   1000  // after USB is configured (> CONNECTION_DELAY)
#define CRASH_EVERY_MS   2000  // between repeats

#define CRASH_SURVIVE_MS 10000 // a boot that runs this long is not looping
#define CRASH_STREAK_MAX 3     // crashed boots in a row before skipping the script

#define RAM_START        0x20000000

typedef struct{
    uint32_t magic;
    uint32_t kind;      // crashreport_kind_t, CRASH_none once reported
    uint32_t r0, r1, r2, r3, r12, lr, pc, psr; // stacked by the exception
    uint32_t sp;        // where the stacked frame was
    uint32_t cfsr, hfsr, mmfar, bfar;
    uint32_t uptime_ms;
    uint32_t count;     // crash resets since power-on
    uint32_t streak;    // boots in a row that crashed within CRASH_SURVIVE_MS
    char     msg[CRASH_MSG_LEN];
    uint32_t csum;
} Crash_t;

__attribute__((section(".noinit")))
static Crash_t noinit; // survives reset
static Crash_t last;   // copy taken at boot, for printing and the getters

static bool     skip_script;
static int      pending_prints;
static uint32_t next_print;

extern USBD_HandleTypeDef USBD_Device;
extern uint32_t _estack; // linker script: top of RAM


// private

static uint32_t crash_csum( const Crash_t* c )
{
    const uint8_t* p = (const uint8_t*)c;
    uint32_t s = CRASH_MAGIC;
    for( size_t i=0; i<offsetof(Crash_t, csum); i++ ){
        s = (s << 5) + s + p[i];
    }
    return s;
}

static bool crash_valid( void )
{
    return noinit.magic == CRASH_MAGIC
        && noinit.csum  == crash_csum( &noinit );
}

static void crash_seal( void )
{
    noinit.magic = CRASH_MAGIC;
    noinit.csum  = crash_csum( &noinit );
}

// start a new report, carrying the counters over from earlier boots
static void crash_begin( uint32_t kind )
{
    uint32_t count  = 0;
    uint32_t streak = 0;
    if( crash_valid() ){
        count  = noinit.count;
        streak = noinit.streak;
    }
    memset( &noinit, 0, sizeof(noinit) );
    noinit.kind      = kind;
    noinit.uptime_ms = HAL_GetTick();
    noinit.count     = count + 1;
    // after CRASH_SURVIVE_MS the streak was already cleared by crashreport_tick
    noinit.streak    = (noinit.uptime_ms < CRASH_SURVIVE_MS) ? streak + 1 : 0;
}

__attribute__((noreturn)) static void crash_reset( void )
{
    crash_seal();
    NVIC_SystemReset(); // includes a __DSB()
    while(1){}
}

static void crash_format( char* s, int len )
{
    if( last.kind == CRASH_luapanic ){
        snprintf( s, len, "!crash: lua panic: %s"
                , last.msg[0] ? last.msg : "(error object is not a string)" );
    } else {
        snprintf( s, len, "!crash: %s pc=0x%08lx lr=0x%08lx psr=0x%08lx"
                          " sp=0x%08lx cfsr=0x%08lx hfsr=0x%08lx"
                          " mmfar=0x%08lx bfar=0x%08lx uptime=%lums"
                , crashreport_kind_str( last.kind )
                , (unsigned long)last.pc,   (unsigned long)last.lr
                , (unsigned long)last.psr,  (unsigned long)last.sp
                , (unsigned long)last.cfsr, (unsigned long)last.hfsr
                , (unsigned long)last.mmfar,(unsigned long)last.bfar
                , (unsigned long)last.uptime_ms );
    }
}

static void crash_print( void )
{
    char s[200];
    if( last.kind != CRASH_none ){
        crash_format( s, sizeof(s) );
        Caw_send_luachunk( s );
    }
    if( skip_script ){
        Caw_send_luachunk( "!crash: user script skipped after repeated crashes" );
    }
}


// fault handlers

// Every fault enters here. The stack may be what broke (an overflow), so the
// entry only passes on where the exception frame should be, and moves MSP to
// a private stack before running any C. Crow never uses PSP, but check anyway.
__attribute__((used, aligned(8)))
static uint32_t crash_stack[128];
_Static_assert( sizeof(crash_stack) == 512, "crash_stack+512 below" );

#define CRASH_ENTRY() __asm volatile(                \
        " tst   lr, #4                          \n"  \
        " ite   eq                              \n"  \
        " mrseq r0, msp                         \n"  \
        " mrsne r0, psp                         \n"  \
        " movw  r1, #:lower16:(crash_stack+512) \n"  \
        " movt  r1, #:upper16:(crash_stack+512) \n"  \
        " msr   msp, r1                         \n"  \
        " b     crashreport_fault               \n"  \
    )

__attribute__((naked)) void NMI_Handler( void ){ CRASH_ENTRY(); }
__attribute__((naked)) void HardFault_Handler( void ){ CRASH_ENTRY(); }
__attribute__((naked)) void MemManage_Handler( void ){ CRASH_ENTRY(); }
__attribute__((naked)) void BusFault_Handler( void ){ CRASH_ENTRY(); }
__attribute__((naked)) void UsageFault_Handler( void ){ CRASH_ENTRY(); }

// only reached from CRASH_ENTRY, on crash_stack
void crashreport_fault( uint32_t* frame ) __attribute__((used, noreturn));
void crashreport_fault( uint32_t* frame )
{
    crash_begin( __get_IPSR() & 0x1FF ); // exception number == crashreport_kind_t
    noinit.sp = (uint32_t)frame;
    // only read the frame if all 8 words are in RAM
    if( ((uint32_t)frame & 3) == 0
     && (uint32_t)frame >= RAM_START
     && (uint32_t)frame <= (uint32_t)&_estack - 8*4 ){
        noinit.r0  = frame[0];
        noinit.r1  = frame[1];
        noinit.r2  = frame[2];
        noinit.r3  = frame[3];
        noinit.r12 = frame[4];
        noinit.lr  = frame[5];
        noinit.pc  = frame[6];
        noinit.psr = frame[7];
    }
    noinit.cfsr  = SCB->CFSR;
    noinit.hfsr  = SCB->HFSR;
    noinit.mmfar = SCB->MMFAR;
    noinit.bfar  = SCB->BFAR;
    crash_reset();
}


// public

void crashreport_init( void )
{
    // give these their own handlers, rather than escalating to HardFault
    SCB->SHCSR |= SCB_SHCSR_USGFAULTENA_Msk
                | SCB_SHCSR_BUSFAULTENA_Msk
                | SCB_SHCSR_MEMFAULTENA_Msk;

    if( !crash_valid() ){ // power-on (or a firmware with another layout)
        memset( &noinit, 0, sizeof(noinit) );
    }
    last = noinit;
    last.msg[CRASH_MSG_LEN-1] = '\0';
    if( noinit.kind == CRASH_none ){
        noinit.streak = 0; // the last boot ended without crashing
    }
    skip_script = (noinit.streak >= CRASH_STREAK_MAX);
    noinit.kind = CRASH_none; // report each crash once
    crash_seal();

    if( last.kind != CRASH_none || skip_script ){
        pending_prints = CRASH_PRINTS;
#ifndef RELEASE
        char s[200];
        crash_format( s, sizeof(s) );
        printf( "%s\n", s );
        printf( "  r0=0x%08lx r1=0x%08lx r2=0x%08lx r3=0x%08lx r12=0x%08lx\n"
              , (unsigned long)last.r0, (unsigned long)last.r1
              , (unsigned long)last.r2, (unsigned long)last.r3
              , (unsigned long)last.r12 );
        printf( "  crashes=%lu streak=%lu%s\n"
              , (unsigned long)last.count, (unsigned long)last.streak
              , skip_script ? " (user script skipped)" : "" );
#endif
    }
}

void crashreport_tick( uint32_t now_ms )
{
    if( noinit.streak && now_ms >= CRASH_SURVIVE_MS ){
        noinit.streak = 0; // this boot survived
        crash_seal();
    }

    if( !pending_prints ){ return; }
    if( USBD_Device.dev_state != USBD_STATE_CONFIGURED ){
        next_print = 0; // wait for the host (again)
        return;
    }
    if( !next_print ){
        next_print = now_ms + CRASH_FIRST_MS;
        return;
    }
    // only when the tx buffer is empty: the host is reading, and the line
    // can't be cut short by a full buffer
    if( now_ms >= next_print && USB_tx_is_ready() ){
        crash_print();
        next_print = now_ms + CRASH_EVERY_MS;
        pending_prints--;
    }
}

bool crashreport_skip_userscript( void )
{
    return skip_script;
}

void crashreport_lua_panic( const char* msg )
{
    crash_begin( CRASH_luapanic );
    for( int i=0; msg && msg[i] && i<CRASH_MSG_LEN-1; i++ ){
        char c = msg[i];
        noinit.msg[i] = (c < ' ') ? ' ' : c; // keep it on one line
    }
    crash_reset();
}

bool crashreport_was_crash( void )
{
    return last.kind != CRASH_none;
}

crashreport_kind_t crashreport_kind( void )
{
    return (crashreport_kind_t)last.kind;
}

uint32_t crashreport_pc( void )
{
    return (last.kind != CRASH_none) ? last.pc : 0;
}

uint32_t crashreport_count( void )
{
    return noinit.count;
}

const char* crashreport_kind_str( crashreport_kind_t kind )
{
    switch( kind ){
        case CRASH_none:       return "none";
        case CRASH_nmi:        return "NMI";
        case CRASH_hardfault:  return "HardFault";
        case CRASH_memmanage:  return "MemManage";
        case CRASH_busfault:   return "BusFault";
        case CRASH_usagefault: return "UsageFault";
        case CRASH_luapanic:   return "LuaPanic";
    }
    return "Fault";
}


// testing

// each call takes another ~70 bytes of stack. reading pad[0] back (volatile)
// keeps the compiler from seeing that it never ends
static void __attribute__((noinline)) crash_recurse( uint32_t depth )
{
    volatile uint32_t pad[16];
    pad[0] = depth;
    if( pad[0] ){ crash_recurse( depth + 1 ); }
    pad[1] = pad[0]; // work after the call, so it isn't a tail call
}

void crashreport_test_fault( int kind )
{
    switch( kind ){
        case CRASH_TEST_BUSFAULT:
            // reserved address above SRAM2. a read faults precisely: pc & bfar are exact
            (void)*(volatile uint32_t*)0x30000000;
            break;
        case CRASH_TEST_UNDEFINED:
            __asm volatile( "udf #0" );
            break;
        case CRASH_TEST_OVERFLOW:
            // the stack is at the bottom of RAM (stm32_flash.ld), so this faults
            // as soon as it runs out. mask interrupts (not faults) on the way down
            __set_BASEPRI( 1 << (8 - __NVIC_PRIO_BITS) );
            crash_recurse( 1 );
            break;
        default: break;
    }
}
