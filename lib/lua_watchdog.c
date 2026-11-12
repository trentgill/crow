#include "lua_watchdog.h"

#include "../submodules/lua/src/lauxlib.h" // luaL_error()
#include "stm32f7xx_hal.h" // HAL_GetTick()
#include "caw.h"           // Caw_send_luachunk()

// the hook runs every WATCHDOG_CHECK VM instructions, so the clock is looked at
// often enough to catch a loop of slow C calls, while the instruction budget is
// counted in units of WATCHDOG_CHECK.
#define WATCHDOG_CHECK      1024
#define WATCHDOG_CHECKS     (LUA_WATCHDOG_INSTRUCTIONS / WATCHDOG_CHECK)

#define WATCHDOG_ERROR      "user code timeout exceeded"

static lua_State* Lmain = NULL;
static int      depth = 0;          // nesting of Lua_watchdog_enter()
static int      checks_left;        // hook calls left in the instruction budget
static uint32_t start_ms;           // HAL_GetTick() at the top-level enter
static uint32_t exempt_at_start;    // exempt_ms at the top-level enter
static uint32_t exempt_ms = 0;      // total time spent in exempt C
static int      killed = 0;         // a kill is unwinding to the top-level call
static uint32_t kills = 0;          // count of kills since boot

static void hook( lua_State* L, lua_Debug* ar );

static void arm( lua_State* L )
{
    lua_sethook( L, hook, LUA_MASKCOUNT, WATCHDOG_CHECK );
}

// raise on every instruction until the top-level call returns, so neither a
// user pcall nor a dying coroutine can swallow the kill.
// NB: Lua disables hooks while an xpcall message handler runs for an error
// raised from a hook, so a handler that itself never returns is not caught.
static void error_until_top( lua_State* L )
{
    lua_sethook( L, hook, LUA_MASKCOUNT, 1 );
}

static uint32_t elapsed_ms( void )
{
    return (HAL_GetTick() - start_ms) - (exempt_ms - exempt_at_start);
}

static void hook( lua_State* L, lua_Debug* ar )
{
    if( killed ){
        error_until_top( L );
        luaL_error( L, WATCHDOG_ERROR );
    }
    // a coroutine left in error_until_top mode by an earlier (settled) kill
    if( lua_gethookcount( L ) != WATCHDOG_CHECK ){ arm( L ); }

    if( depth == 0 ){ return; } // crow's own Lua, outside any user call

    if( --checks_left <= 0
     || elapsed_ms() > LUA_DEADLINE_MS ){
        killed = 1;
        kills++;
        Caw_send_luachunk("CPU timed out.");
        error_until_top( L );
        if( L != Lmain ){ error_until_top( Lmain ); } // killed in a coroutine
        luaL_error( L, WATCHDOG_ERROR );
    }
}

void Lua_watchdog_init( lua_State* L )
{
    Lmain  = L;
    depth  = 0;
    killed = 0;
    arm( L ); // permanent. coroutines copy it from their creator.
}

void Lua_watchdog_enter( void )
{
    if( depth == 0 ){ // top-level call: fresh deadline & budget
        start_ms        = HAL_GetTick();
        exempt_at_start = exempt_ms;
        checks_left     = WATCHDOG_CHECKS;
        arm( Lmain ); // restart the instruction count from here
    }
    depth++;
}

void Lua_watchdog_exit( void )
{
    if( depth > 0 ){ depth--; }
    if( depth == 0 && killed ){ // top-level call has returned: back to normal
        killed = 0;
        arm( Lmain );
    }
}

uint32_t Lua_watchdog_exempt_begin( void )
{
    return HAL_GetTick();
}

void Lua_watchdog_exempt_end( uint32_t token )
{
    exempt_ms += HAL_GetTick() - token;
}

uint32_t Lua_watchdog_kills( void )
{
    return kills;
}
