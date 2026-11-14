#include "adc_dbg.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "../submodules/lua/src/lauxlib.h"
#include "../ll/ads131.h"

// adc_debug()                -> table of counters since the last reset
// adc_debug('reset')         -> clear counters
// adc_debug('set', key, v)   -> change a config field (takes effect on 'apply'
//                               for clocks/regs, immediately for mode/offset/period)
// adc_debug('apply')         -> reset + reconfigure the ADS with the current config
// adc_debug('get', key)      -> read a config field

#define CFG(k) { #k, &adc_cfg.k, sizeof(adc_cfg.k) }
static const struct { const char* k; void* p; int sz; } cfg_fields[] =
    { CFG(mode), CFG(tim_period), CFG(clk1), CFG(clk2), CFG(ena)
    , CFG(spi_br), CFG(wait_us), CFG(period), CFG(offset), CFG(margin)
    , CFG(spb), CFG(edge), CFG(txcmd)
    };
#define NFIELDS (sizeof(cfg_fields)/sizeof(cfg_fields[0]))

static void seti( lua_State* L, const char* k, lua_Integer v ){
    lua_pushinteger( L, v ); lua_setfield( L, -2, k );
}

int ADC_Debug_lua( lua_State* L )
{
    const char* cmd = luaL_optstring( L, 1, "" );
    if( !strcmp( cmd, "reset" ) ){ ADC_DebugReset(); return 0; }
    if( !strcmp( cmd, "apply" ) ){ ADC_Reconfig(); return 0; }
    if( !strcmp( cmd, "capture" ) ){ // adc_debug('capture', ch) raw int16 of in[ch] (1|2)
                                     // ch 3|4: in[1|2] as the block sees it, at the DAC rate
        lua_Integer ch = luaL_optinteger( L, 2, 1 );
        adc_cap_ch = (ch < 1 || ch > 4) ? 0 : (uint8_t)(ch - 1);
        uint32_t n = luaL_optinteger( L, 3, 1024 );
        adc_cap_len = 0; // stop the writer first
        if( n > 16384 ){ n = 16384; }
        free( adc_cap );
        adc_cap = malloc( n * sizeof(int16_t) );
        if( !adc_cap ){ return luaL_error( L, "no memory" ); }
        adc_cap_n = 0;
        adc_cap_len = n;
        return 0;
    }
    if( !strcmp( cmd, "captured" ) ){ // -> count; adc_debug('captured', from, n) -> csv string
        if( lua_gettop( L ) < 2 ){ lua_pushinteger( L, adc_cap_n ); return 1; }
        int from = luaL_checkinteger( L, 2 );
        int n = luaL_checkinteger( L, 3 );
        luaL_Buffer b;
        luaL_buffinit( L, &b );
        char tmp[12];
        for( int i=from; i<from+n && i<(int)adc_cap_n; i++ ){
            int len = snprintf( tmp, sizeof(tmp), "%d,", adc_cap[i] );
            luaL_addlstring( &b, tmp, len );
        }
        luaL_pushresult( &b );
        return 1;
    }
    if( !strcmp( cmd, "set" ) || !strcmp( cmd, "get" ) ){
        const char* k = luaL_checkstring( L, 2 );
        for( unsigned i=0; i<NFIELDS; i++ ){
            if( strcmp( k, cfg_fields[i].k ) ){ continue; }
            if( cmd[0] == 's' ){
                int32_t v = (int32_t)luaL_checkinteger( L, 3 );
                switch( cfg_fields[i].sz ){
                    case 1: *(uint8_t*)cfg_fields[i].p = (uint8_t)v; break;
                    default: *(int32_t*)cfg_fields[i].p = v; break;
                }
                { extern volatile uint16_t tx_word0; tx_word0 = adc_cfg.txcmd; } // live
                return 0;
            }
            int32_t v = (cfg_fields[i].sz == 1) ? *(uint8_t*)cfg_fields[i].p
                                                : *(int32_t*)cfg_fields[i].p;
            lua_pushinteger( L, v );
            return 1;
        }
        return luaL_error( L, "no field %s", k );
    }
    ADC_dbg_t d;
    __disable_irq(); d = adc_dbg; __enable_irq();
    lua_newtable( L );
    seti( L, "frames", d.frames );
    seti( L, "f_check",  d.stat_bits[0] );
    seti( L, "f_drdy",   d.stat_bits[1] );
    seti( L, "f_resync", d.stat_bits[2] );
    seti( L, "f_wdt",    d.stat_bits[3] );
    seti( L, "f_adcin",  d.stat_bits[4] );
    seti( L, "f_spi",    d.stat_bits[5] );
    seti( L, "f_opc",    d.stat_bits[6] );
    seti( L, "bad_header", d.bad_header );
    seti( L, "last_status", d.last_status );
    seti( L, "stat_m2", d.stat_m2 );
    seti( L, "busy", d.busy );
    seti( L, "late", d.late );
    seti( L, "spin_max", d.spin_max );
    seti( L, "blk_n", d.blk_n );
    seti( L, "slips", d.slips );
    seti( L, "ring_dist", d.ring_dist );
    seti( L, "last_other", d.last_other );
    { extern volatile uint16_t tx_word0; seti( L, "tx_word0", tx_word0 ); }
    seti( L, "blk_min", d.blk_min );
    seti( L, "blk_max", d.blk_max );
    lua_pushnumber( L, d.blk_n ? (double)d.blk_sum / d.blk_n : 0 );
    lua_setfield( L, -2, "blk_avg" );
    return 1;
}
