#pragma once

#include <stdint.h>

#include "../submodules/lua/src/lua.h" // lua_State*

// Watchdog for runaway user Lua.
//
// A top-level call into user Lua (a REPL line, a script upload, init(), an event
// handler, a clock resume) is killed with "CPU timed out." and the error
// "user code timeout exceeded" when EITHER limit is reached:
//   - it has executed LUA_WATCHDOG_INSTRUCTIONS VM instructions, or
//   - it has run for more than LUA_DEADLINE_MS of wall-clock time.
// Nested calls share the enclosing call's deadline & budget.
// Time spent in C bracketed by Lua_watchdog_exempt_begin/end does not count.

#define LUA_DEADLINE_MS             1000             // wall-clock limit (ms)
#define LUA_WATCHDOG_INSTRUCTIONS   (2 * 0x100000)   // instruction limit (unchanged)

void Lua_watchdog_init( lua_State* L ); // call once, right after luaL_newstate()

// bracket every call into user code. nested pairs share the outer deadline.
void Lua_watchdog_enter( void );
void Lua_watchdog_exit( void );

// bracket blocking C called from Lua (eg. flash writes). returns a token for _end.
uint32_t Lua_watchdog_exempt_begin( void );
void Lua_watchdog_exempt_end( uint32_t token );

uint32_t Lua_watchdog_kills( void ); // number of kills since boot
