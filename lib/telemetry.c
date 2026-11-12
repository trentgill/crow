#include "telemetry.h"

#ifdef TELEMETRY

#include <stm32f7xx.h>
#include <stm32f7xx_hal.h> // HAL_GetTick(), HAL_RCC_GetPCLK1Freq()
#include <malloc.h> // mallinfo()
#include <string.h>

#include "../submodules/lua/src/lauxlib.h"
#include "crashreport.h"

// linker symbols
extern char _sdata;
extern char _ebss;
extern char _sheap;  // the heap's region
extern char _eheap;
extern char _sstack; // the stack's region
extern char _estack;
void* _sbrk( int incr );

#define STACK_PAINT 0xC0DEC0DE

uint32_t tm_counters[TM_COUNT];

// main loop: every pass adds its duration to the window. a pass that takes
// under twice the shortest one seen was neither interrupted nor given work, so
// it counts as idle time. everything else in the window is cpu in use.
static uint32_t loop_last;
static uint32_t loop_count;
static uint32_t loop_min;
static uint32_t loop_max;
static uint64_t window; // cycles since the last reset
static uint64_t idle;   // of which, spent in idle passes

// audio block interrupt
static volatile uint32_t dsp_count;
static volatile uint32_t dsp_max;
static volatile uint64_t dsp_sum;

static int event_max;
static uint32_t reset_cause;
static volatile int panic_request;

static void window_reset( void )
{
    uint32_t old_primask = __get_PRIMASK();
    __disable_irq();
    dsp_count = 0;
    dsp_max   = 0;
    dsp_sum   = 0;
    __set_PRIMASK( old_primask );
    loop_count = 0;
    loop_min   = 0xFFFFFFFF;
    loop_max   = 0;
    window     = 0;
    idle       = 0;
    event_max  = 0;
    loop_last  = DWT->CYCCNT;
}

void Telemetry_init( void )
{
    // why did we boot? (flags persist until cleared)
    reset_cause = RCC->CSR >> 24;
    RCC->CSR |= RCC_CSR_RMVF;

    // free-running cycle counter
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->LAR = 0xC5ACCE55; // unlock (cortex-m7)
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    // paint the unused part of the stack to find its low-water mark
    register uint32_t* sp __asm("sp");
    uint32_t* p = (uint32_t*)&_sstack;
    uint32_t* top = sp - 64; // stay clear of the live frames
    while( p < top ){ *p++ = STACK_PAINT; }

    window_reset();
}

void Telemetry_loop( void )
{
    uint32_t now = DWT->CYCCNT;
    uint32_t dt = now - loop_last;
    loop_last = now;
    window += dt;
    loop_count++;
    if( dt < loop_min ){ loop_min = dt; }
    if( dt < 2 * loop_min ){ idle += dt; }
    if( dt > loop_max ){ loop_max = dt; }

    if( panic_request ){ // an error raised outside any pcall
        extern lua_State* L;
        panic_request = 0;
        luaL_error( L, "telemetry test panic" );
    }
}

void Telemetry_dsp( uint32_t cycles )
{
    dsp_count++;
    dsp_sum += cycles;
    if( cycles > dsp_max ){ dsp_max = cycles; }
}

void Telemetry_event_depth( int depth )
{
    if( depth > event_max ){ event_max = depth; }
}

// lowest address the stack has reached
static uint32_t stack_low_water( void )
{
    uint32_t* p = (uint32_t*)&_sstack;
    while( p < (uint32_t*)&_estack && *p == STACK_PAINT ){ p++; }
    return (uint32_t)p;
}

static void setint( lua_State* L, const char* k, uint32_t v )
{
    lua_pushinteger( L, (lua_Integer)v );
    lua_setfield( L, -2, k );
}

static void setnum( lua_State* L, const char* k, float v )
{
    lua_pushnumber( L, v );
    lua_setfield( L, -2, k );
}

static void test_fault( int kind )
{
    if( kind == 2 ){ panic_request = 1; } // lua panic, raised from the main loop
    else { crashreport_test_fault( kind ); } // 0 bus fault, 1 undefined instruction, 3 stack overflow
}

// sys_telemetry()          -> table of measurements since the last reset
// sys_telemetry('reset')   -> start a new measurement window
// sys_telemetry('us')      -> microseconds from the cycle counter (wraps every ~19s)
// sys_telemetry('fault',n) -> test hook: crash on purpose (see test_fault)
int Telemetry_lua( lua_State* L )
{
    const char* cmd = luaL_optstring( L, 1, "" );
    if( cmd[0] == 'r' ){
        window_reset();
        memset( tm_counters, 0, sizeof(tm_counters) );
        return 0;
    } else if( cmd[0] == 'u' ){
        lua_pushinteger( L, DWT->CYCCNT / (SystemCoreClock / 1000000) );
        return 1;
    } else if( cmd[0] == 'f' ){
        test_fault( luaL_optinteger( L, 2, 0 ) );
        return 0;
    }

    uint32_t old_primask = __get_PRIMASK();
    __disable_irq();
    uint32_t d_count = dsp_count;
    uint32_t d_max   = dsp_max;
    uint64_t d_sum   = dsp_sum;
    __set_PRIMASK( old_primask );

    const float cyc_per_us = (float)(SystemCoreClock / 1000000);
    float win = (float)window;
    struct mallinfo mi = mallinfo();
    uint32_t heap_top = (uint32_t)_sbrk(0);
    uint32_t low = stack_low_water();

    lua_settop( L, 0 );
    lua_newtable( L );
    setint( L, "uptime_ms"   , HAL_GetTick() );
    setint( L, "reset_cause" , reset_cause ); // RCC_CSR[31:24]
    setint( L, "clock_hz"    , SystemCoreClock );
    setint( L, "pclk1_hz"    , HAL_RCC_GetPCLK1Freq() );
    setint( L, "pclk2_hz"    , HAL_RCC_GetPCLK2Freq() );
    setint( L, "vos"         , (PWR->CR1 >> 14) & 3 ); // PWR_CR1.VOS: 3 is scale 1, 2 is scale 2
        // cpu
    setnum( L, "win_ms"      , win / (cyc_per_us * 1000.0f) );
    setint( L, "loop_n"      , loop_count );
    setnum( L, "loop_min_us" , (float)loop_min / cyc_per_us );
    setnum( L, "loop_max_us" , (float)loop_max / cyc_per_us );
    setnum( L, "cpu_busy"    , (win > 0.0f) ? 1.0f - (float)idle / win : 0.0f );
    setint( L, "dsp_n"       , d_count );
    setnum( L, "dsp_avg_us"  , d_count ? ((float)d_sum / (float)d_count) / cyc_per_us : 0.0f );
    setnum( L, "dsp_max_us"  , (float)d_max / cyc_per_us );
    setnum( L, "dsp_load"    , (win > 0.0f) ? (float)d_sum / win : 0.0f );
        // ram (bytes)
    setint( L, "ram_static"  , (uint32_t)(&_ebss - &_sdata) );
    setint( L, "heap_arena"  , heap_top - (uint32_t)&_sheap ); // claimed from the system
    setint( L, "heap_used"   , mi.uordblks );
    setint( L, "heap_free"   , mi.fordblks );               // free inside the arena
    setint( L, "stack_peak"  , (uint32_t)&_estack - low );
    setint( L, "stack_size"  , (uint32_t)(&_estack - &_sstack) );
    setint( L, "ram_gap"     , (uint32_t)&_eheap - heap_top ); // the heap can still grow by this
    setint( L, "lua_bytes"   , lua_gc( L, LUA_GCCOUNT, 0 ) * 1024
                             + lua_gc( L, LUA_GCCOUNTB, 0 ) );
        // last reset
    setint( L, "crash_kind"  , crashreport_kind() );  // 0 unless this boot follows a crash
    setint( L, "crash_pc"    , crashreport_pc() );
    setint( L, "crash_count" , crashreport_count() ); // crash resets since power-on
        // serial & events
    setint( L, "rx_bytes"    , tm_counters[TM_rx_bytes] );
    setint( L, "tx_bytes"    , tm_counters[TM_tx_bytes] );
    setint( L, "tx_dropped"  , tm_counters[TM_tx_dropped] );
    setint( L, "rx_overflow" , tm_counters[TM_rx_overflow] );
    setint( L, "event_full"  , tm_counters[TM_event_full] );
    setint( L, "event_max"   , event_max );
    return 1;
}

#endif // TELEMETRY
