// host test for lib/lua_watchdog.c, run against the real Lua in submodules/lua.
// run: sh tests/host/run.sh
//
// HAL_GetTick() is a fake clock that only moves when Lua calls slow(ms) or
// exempt(ms), so the wall-clock limit can be tested apart from the
// instruction budget (on the host 2M instructions take a few ms).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib/lua_watchdog.h"
#include "submodules/lua/src/lauxlib.h"
#include "submodules/lua/src/lualib.h"

static uint32_t fake_tick = 0;
uint32_t HAL_GetTick( void ){ return fake_tick; }

static int banners = 0; // count of "CPU timed out."
void Caw_send_luachunk( char* text )
{
    if( !strcmp( text, "CPU timed out." ) ){ banners++; }
    else { printf( "    lua: %s\n", text ); }
}

static lua_State* L;
static char last_err[512];
static int nested_status = -1;
static int failures = 0;

#define CHECK( cond ) do{ if( !(cond) ){ failures++; \
    printf( "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); } }while(0)

static int handler( lua_State* L )
{
    const char* m = lua_tostring( L, 1 );
    snprintf( last_err, sizeof last_err, "%s", m ? m : "(non-string error)" );
    return 1;
}

// same shape as Lua_call_usercode() in lib/lualink.c
static int call_usercode( lua_State* L, int nargs, int nresults )
{
    Lua_watchdog_enter();
    int errFunc = lua_gettop( L ) - nargs;
    lua_pushcfunction( L, handler );
    lua_insert( L, errFunc );
    int status = lua_pcall( L, nargs, nresults, errFunc );
    lua_remove( L, errFunc );
    Lua_watchdog_exit();
    return status;
}

// like Lua_eval(): a top-level call
static int run( const char* code )
{
    last_err[0] = '\0';
    if( luaL_loadstring( L, code ) != LUA_OK ){
        printf( "  syntax: %s\n", lua_tostring( L, -1 ) ); exit( 1 );
    }
    int s = call_usercode( L, 0, 0 );
    if( s != LUA_OK ){ lua_pop( L, 1 ); }
    return s;
}

static lua_Number num( const char* global )
{
    lua_getglobal( L, global );
    lua_Number n = lua_tonumber( L, -1 );
    lua_pop( L, 1 );
    return n;
}

static int isnil( const char* global )
{
    lua_getglobal( L, global );
    int r = lua_isnil( L, -1 );
    lua_pop( L, 1 );
    return r;
}

static int timed_out( void ){ return strstr( last_err, "user code timeout exceeded" ) != NULL; }

// C functions for the Lua side
static int _slow( lua_State* L ){ fake_tick += luaL_checkinteger( L, 1 ); return 0; }
static int _exempt( lua_State* L )
{
    uint32_t t = Lua_watchdog_exempt_begin();
    fake_tick += luaL_checkinteger( L, 1 );
    Lua_watchdog_exempt_end( t );
    return 0;
}
// a nested call into user code, like the ii follower query handler which
// re-enters Lua_call_usercode() from the I2C interrupt.
static int _nested( lua_State* L )
{
    const char* code = luaL_checkstring( L, 1 );
    luaL_loadstring( L, code );
    nested_status = call_usercode( L, 0, 0 );
    if( nested_status != LUA_OK ){ lua_pop( L, 1 ); }
    lua_pushboolean( L, nested_status == LUA_OK );
    return 1;
}

// the watchdog as it was before this change, to compare the instruction budget
static int old_watchdog;
static void old_hook( lua_State* L, lua_Debug* ar )
{
    if( --old_watchdog <= 0 ){
        banners++;
        lua_sethook( L, old_hook, LUA_MASKLINE, 0 );
        luaL_error( L, "user code timeout exceeded" );
    }
}
static int old_run( const char* code )
{
    luaL_loadstring( L, code );
    lua_sethook( L, old_hook, LUA_MASKCOUNT, 0x100000 );
    old_watchdog = 2;
    int s = lua_pcall( L, 0, 0, 0 );
    if( s != LUA_OK ){ lua_pop( L, 1 ); }
    lua_sethook( L, old_hook, 0, 0 );
    return s;
}

int main( void )
{
    L = luaL_newstate();
    luaL_openlibs( L );
    lua_register( L, "slow", _slow );
    lua_register( L, "exempt", _exempt );
    lua_register( L, "nested", _nested );

    const char* empty_loop = "n = 0 while true do n = n + 1 end";

    printf( "old watchdog: empty loop\n" );
    CHECK( old_run( empty_loop ) != LUA_OK );
    lua_Number n_old = num( "n" );
    printf( "  killed after %.0f iterations\n", n_old );

    Lua_watchdog_init( L ); // as Lua_Init() does, before any coroutine exists
    luaL_dostring( L, "boot_co = coroutine.create(function() while true do end end)" );

    printf( "empty loop: instruction budget unchanged\n" );
    banners = 0;
    CHECK( run( empty_loop ) != LUA_OK );
    CHECK( timed_out() );
    CHECK( banners == 1 );
    CHECK( num( "n" ) == n_old );
    CHECK( fake_tick == 0 ); // killed by the budget, not the clock
    printf( "  killed after %.0f iterations, err: %s\n", num( "n" ), last_err );

    printf( "loop of slow C calls: wall clock\n" );
    uint32_t t0 = fake_tick;
    CHECK( run( "n = 0 while true do slow(1) n = n + 1 end" ) != LUA_OK );
    CHECK( timed_out() );
    printf( "  killed after %u ms, %.0f iterations\n", fake_tick - t0, num( "n" ) );
    CHECK( fake_tick - t0 > LUA_DEADLINE_MS );
    CHECK( fake_tick - t0 < LUA_DEADLINE_MS + 200 );

    printf( "pcall / xpcall cannot swallow a kill\n" );
    luaL_dostring( L, "after = nil" );
    CHECK( run( "while true do pcall(function() while true do end end) end" ) != LUA_OK );
    CHECK( timed_out() );
    CHECK( run( "ok = pcall(function() while true do end end) after = true" ) != LUA_OK );
    CHECK( isnil( "after" ) && isnil( "ok" ) );
    CHECK( run( "xpcall(function() while true do end end, function(e) return e end) after = true" ) != LUA_OK );
    CHECK( isnil( "after" ) );

    printf( "coroutine created in one call, resumed in another\n" );
    CHECK( run( "co = coroutine.create(function() while true do end end)" ) == LUA_OK );
    CHECK( run( "res = coroutine.resume(co) after = true" ) != LUA_OK );
    CHECK( timed_out() );
    CHECK( isnil( "after" ) && isnil( "res" ) );
    CHECK( run( "st = coroutine.status(co)" ) == LUA_OK );

    printf( "coroutine created outside any call (crow bootstrap)\n" );
    CHECK( run( "coroutine.resume(boot_co) after = true" ) != LUA_OK );
    CHECK( timed_out() );
    CHECK( isnil( "after" ) );

    printf( "coroutine killed by the wall clock\n" );
    t0 = fake_tick;
    CHECK( run( "co = coroutine.create(function() while true do slow(1) end end)" ) == LUA_OK );
    CHECK( run( "coroutine.resume(co) after = true" ) != LUA_OK );
    CHECK( timed_out() && isnil( "after" ) );
    CHECK( fake_tick - t0 > LUA_DEADLINE_MS );

    printf( "clock.lua coroutine, resumed like L_handle_clock_resume\n" );
    CHECK( run( "clock_schedule_sleep = function(id, t) sleeping = id end\n"
                "clock_cancel = function() end\n"
                "clock = dofile('lua/clock.lua')\n"
                "clock.run(function() clock.sleep(0.1) while true do end end)" ) == LUA_OK );
    CHECK( num( "sleeping" ) == 1 );
    lua_getglobal( L, "clock_resume_handler" );
    lua_pushinteger( L, 1 );
    last_err[0] = '\0';
    int s = call_usercode( L, 1, 0 );
    if( s != LUA_OK ){ lua_pop( L, 1 ); }
    CHECK( s != LUA_OK && timed_out() ); // not reduced to a print() in clock.resume

    printf( "after a kill, the next call runs normally\n" );
    CHECK( lua_gethookcount( L ) == 1024 );
    CHECK( run( "s = 0 for i = 1, 400000 do s = s + i end" ) == LUA_OK );
    CHECK( run( "co = coroutine.wrap(function() for i=1,3 do coroutine.yield(i) end end)"
                " a = co() + co() + co()" ) == LUA_OK );
    CHECK( num( "a" ) == 6 );

    printf( "budget and deadline restart for each top-level call\n" );
    for( int i = 0; i < 3; i++ ){
        CHECK( run( "for i = 1, 1500000 do end" ) == LUA_OK );
        CHECK( run( "slow(800) for i = 1, 5000 do end" ) == LUA_OK );
    }

    printf( "exempt C does not count\n" );
    CHECK( run( "exempt(1200) for i = 1, 5000 do end slow(800) for i = 1, 5000 do end" ) == LUA_OK );
    CHECK( run( "slow(1200) for i = 1, 5000 do end" ) != LUA_OK );
    CHECK( timed_out() );

    printf( "nested calls share the deadline\n" );
    luaL_dostring( L, "after = nil" );
    CHECK( run( "slow(600) nested('slow(600) for i = 1, 5000 do end') after = true" ) != LUA_OK );
    CHECK( nested_status != LUA_OK ); // the nested call was killed...
    CHECK( isnil( "after" ) );        // ...and so was the outer one
    CHECK( run( "slow(600) nested('x = 1') slow(600) for i = 1, 5000 do end after = true" ) != LUA_OK );
    CHECK( nested_status == LUA_OK ); // leaving the nested call...
    CHECK( isnil( "after" ) );        // ...did not disarm the outer one

    printf( "nested calls share the budget\n" );
    CHECK( run( "for i = 1, 1500000 do end nested('for i = 1, 1000000 do end') after = true" ) != LUA_OK );
    CHECK( nested_status != LUA_OK && isnil( "after" ) );
    CHECK( run( "for i = 1, 1000000 do end nested('for i = 1, 500000 do end') after = true" ) == LUA_OK );
    CHECK( !isnil( "after" ) );

    printf( "kill count: %u\n", Lua_watchdog_kills() );
    CHECK( Lua_watchdog_kills() == 13 );

    lua_close( L );
    printf( failures ? "watchdog_test: %d FAILED\n" : "watchdog_test: ok\n", failures );
    return failures ? 1 : 0;
}
