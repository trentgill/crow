#pragma once

#include <stdint.h>

#include "../submodules/lua/src/lua.h" // lua_State*

// Runtime measurements for the hardware test suite (tests/hw).
// Built in by default; `make TELEMETRY=0` compiles every hook to nothing.
// The only user-visible surface is the Lua function sys_telemetry().

typedef enum{ TM_rx_bytes      // usb bytes accepted from the host
            , TM_tx_bytes      // usb bytes queued for the host
            , TM_tx_dropped    // bytes thrown away because the tx buffer was full
            , TM_rx_overflow   // receive buffers discarded ('!chunk too long!')
            , TM_event_full    // events dropped because the queue was full
            , TM_COUNT
} TM_counter_t;

#ifdef TELEMETRY

extern uint32_t tm_counters[TM_COUNT];

void Telemetry_init( void );
void Telemetry_loop( void );           // once per pass of the main loop
void Telemetry_event_depth( int depth ); // events waiting after a post
int  Telemetry_lua( lua_State* L );    // sys_telemetry()

// the cycle counter is free-running at the core clock
#define TM_CYCLES()        (DWT->CYCCNT)
void Telemetry_dsp( uint32_t cycles ); // one audio block took this long

static inline void Telemetry_count( TM_counter_t c, uint32_t n ){
    tm_counters[c] += n;
}

#else // !TELEMETRY

#define Telemetry_init()
#define Telemetry_loop()
#define Telemetry_event_depth(d)
#define TM_CYCLES()        (0)
#define Telemetry_dsp(c)   ((void)(c))
#define Telemetry_count(c,n)

#endif // TELEMETRY
