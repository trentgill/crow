#include "l_bootstrap.h"

#include <string.h>
#include <stdbool.h>

#include "l_crowlib.h"

// Lua libs wrapped in C-headers
#include "build/crowlib.h"
#include "build/asl.h"
#include "build/asllib.h"
#include "build/clock.h"
#include "build/metro.h"
#include "build/public.h"
#include "build/input.h"
#include "build/output.h"
#include "build/ii.h"
// #include "build/iihelp.h"    // generated lua stub for loading i2c modules
#include "build/calibrate.h"
#include "build/sequins.h"
#include "build/quote.h"
#include "build/timeline.h"
#include "build/hotswap.h"
#include "build/midi.h"

// #include "build/ii_lualink.h" // generated C header for linking to lua

struct lua_lib_locator{
    const char* name;
    const unsigned char* addr_of_luacode;
    const bool stripped;
    const unsigned int len;
};

static int _open_lib( lua_State *L, const struct lua_lib_locator* lib, const char* name );
static void lua_full_gc(lua_State* L);

// mark the 3rd arg 'false' if you need to debug that library
const struct lua_lib_locator Lua_libs[] =
    { { "lua_crowlib"   , build_crowlib_lc   , true, build_crowlib_lc_len}
    , { "lua_asl"       , build_asl_lc       , true, build_asl_lc_len}
    , { "lua_asllib"    , build_asllib_lc    , true, build_asllib_lc_len}
    , { "lua_clock"     , build_clock_lc     , true, build_clock_lc_len}
    , { "lua_metro"     , build_metro_lc     , true, build_metro_lc_len}
    , { "lua_input"     , build_input_lc     , true, build_input_lc_len}
    , { "lua_output"    , build_output_lc    , true, build_output_lc_len}
    , { "lua_public"    , build_public_lc    , true, build_public_lc_len}
    , { "lua_ii"        , build_ii_lc        , true, build_ii_lc_len}
    // , { "build_iihelp"  , build_iihelp_lc    , true, build_iihelp_lc_len}
    , { "lua_calibrate" , build_calibrate_lc , true, build_calibrate_lc_len}
    , { "lua_sequins"   , build_sequins_lc   , true, build_sequins_lc_len}
    , { "lua_quote"     , build_quote_lc     , true, build_quote_lc_len}
    , { "lua_timeline"  , build_timeline_lc  , true, build_timeline_lc_len}
    , { "lua_hotswap"   , build_hotswap_lc   , true, build_hotswap_lc_len}
    , { "lua_midi"      , build_midi_lc      , true, build_midi_lc_len}
    , { NULL            , NULL               , true, 0}
    };


void l_bootstrap_init(lua_State* L){
    // collectgarbage('setpause', 55)
    lua_gc(L, LUA_GCSETPAUSE, 55);
    lua_gc(L, LUA_GCSETSTEPMUL, 260);

    // dofile just calls c_dofile
    lua_getglobal(L, "c_dofile");
    lua_setglobal(L, "dofile");

    // crowlib.lua now only contains our print() definition
    // _c = dofile('lua/crowlib.lua')
    lua_pushliteral(L, "lua/crowlib.lua");
    l_bootstrap_dofile(L); // hotrod without l_call
    lua_settop(L, 0);

    // _c = {}
    lua_newtable(L);
    lua_setglobal(L, "_c");

    // crow = _c
    lua_getglobal(L, "_c");
    lua_setglobal(L, "crow");

    // crowlib C extensions
    l_crowlib_init(L);

    // track all user-created globals 
    luaL_dostring(L,
        "_user={}\n"
        "local function trace(t,k,v)\n"
            "_user[k]=true\n"
            "rawset(t,k,v)\n"
        "end\n"
        "setmetatable(_G,{ __newindex = trace })\n"
        );

    // perform two full garbage collection cycles for full cleanup
    lua_full_gc(L);
}


int l_bootstrap_dofile(lua_State* L)
{
    const char* l_name = luaL_checkstring(L, 1);
    int l_len = strlen(l_name);
    if(l_len > 32) printf("FIXME bootstrap: filepath >32bytes!\n\r");

    // simple C version of "luapath_to_cpath"
    // l_name is a lua native path: "lua/asl.lua"
    char cname[32]; // 32bytes is more than enough for any path
    int p=0; // pointer into cname
    for(int i=0; i<l_len; i++){
        switch(l_name[i]){
            case '/':{ cname[p++] = '_'; } break;
            case '.':{ cname[p++] = 0; } goto strcomplete;
            default:{ cname[p++] = l_name[i]; } break;
        }
    }
    // goto fail; // no match was found, so error out (silently?)

strcomplete:
    lua_pop( L, 1 );
    switch( _open_lib( L, Lua_libs, cname ) ){
        case -1: goto fail;
        case 1: lua_full_gc(L); return 1;
        default: break;
    }
    // switch( _open_lib( L, Lua_ii_libs, cname ) ){
    //     case -1: goto fail;
    //     case 1: lua_full_gc(L); return 1;
    //     default: break;
    // }
    printf("can't open library: %s\n", (char*)cname);

fail:
    lua_pushnil(L);
    return 1;
}






/////////// private defns

static int _open_lib( lua_State *L, const struct lua_lib_locator* lib, const char* name )
{
    uint8_t i = 0;
    while( lib[i].addr_of_luacode != NULL ){
        if( !strcmp( name, lib[i].name ) ){ // if the strings match
            if( luaL_loadbuffer(L, (const char*)lib[i].addr_of_luacode
                                 , lib[i].len
                                 , lib[i].name) ){
                printf("can't load library: %s\n", (char*)lib[i].name );
                printf( "%s\n", (char*)lua_tostring( L, -1 ) );
                lua_pop( L, 1 );
                return -1; // error
            }
            if( lua_pcall(L, 0, LUA_MULTRET, 0) ){
                printf("can't exec library: %s\n", (char*)lib[i].name );
                printf( "%s\n", (char*)lua_tostring( L, -1 ) );
                lua_pop( L, 1 );
                return -1; // error
            }
            return 1; // table is left on the stack as retval
        }
        i++;
    }
    return 0; // not found
}

static void lua_full_gc(lua_State* L){
    lua_gc(L, LUA_GCCOLLECT, 1);
    lua_gc(L, LUA_GCCOLLECT, 1);
}
