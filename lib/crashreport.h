#pragma once

#include <stdint.h>
#include <stdbool.h>

// Faults and Lua panics record a small report in .noinit RAM and reset the
// module, instead of freezing it. After the reboot the report is printed over
// USB as plain text lines starting with "!crash:".

// values of the fault kinds match the exception number (IPSR)
typedef enum{ CRASH_none       = 0
            , CRASH_nmi        = 2
            , CRASH_hardfault  = 3
            , CRASH_memmanage  = 4
            , CRASH_busfault   = 5
            , CRASH_usagefault = 6
            , CRASH_luapanic   = 16
} crashreport_kind_t;

// arguments for crashreport_test_fault()
#define CRASH_TEST_BUSFAULT  0 // read from an unmapped address
#define CRASH_TEST_UNDEFINED 1 // execute an undefined instruction
#define CRASH_TEST_OVERFLOW  3 // unbounded recursion overflows the stack

// call once at boot, before REPL_init()
void crashreport_init( void );

// call every millisecond from the main loop
void crashreport_tick( uint32_t now_ms );

// true if the last few boots all crashed soon after starting
bool crashreport_skip_userscript( void );

// record a Lua panic and reset. msg may be NULL
void crashreport_lua_panic( const char* msg ) __attribute__((noreturn));

// about the reset that started this boot
bool crashreport_was_crash( void );
crashreport_kind_t crashreport_kind( void ); // CRASH_none if not a crash
uint32_t crashreport_pc( void );             // 0 if not a fault
uint32_t crashreport_count( void );          // crash resets since power-on
const char* crashreport_kind_str( crashreport_kind_t kind );

// deliberately crash, for testing. kind is one of CRASH_TEST_*
void crashreport_test_fault( int kind );
