#pragma once

#include <stdint.h>
#include <stdbool.h>

// Lua itself
#include "../submodules/lua/src/lua.h" // lua_State*

typedef void (*ErrorHandler_t)(char* error_message);

extern volatile int CPU_count; // count from main.c

lua_State* Lua_Init(void);
lua_State* Lua_Reset( void );
void Lua_DeInit(void);

void Lua_crowbegin( void );
uint8_t Lua_eval( lua_State*     L
                , const char*    script
                , size_t         script_len
                , const char*    chunkname
                );
void Lua_load_default_script( void );

// Event enqueue wrappers
extern void L_queue_asl_done( int id );
extern void L_queue_metro( int id, int state );
extern void L_queue_stream( int id, float state );
extern void L_queue_change( int id, float state );
extern void L_queue_window( int id, float window );
extern void L_queue_volume( int id, float level );
extern void L_queue_peak( int id, float ignore );
extern void L_queue_freq( int id, float freq );
extern void L_queue_in_scale( int id, float note );
extern void L_queue_ii_leadRx( uint8_t address, uint8_t cmd, float data, uint8_t arg );
extern void L_queue_ii_followRx( void );
extern void L_queue_clock_resume( int coro_id );
extern void L_queue_clock_start( void );
extern void L_queue_clock_stop( void );
extern void L_queue_tempo_change( float tempo );
extern void L_handle_midi( const uint8_t* msg, uint8_t len );

// Callback declarations
extern float L_handle_ii_followRxTx( uint8_t cmd, int args, float* data );
extern void L_handle_ii_followRx_cont( uint8_t cmd, int args, float* data );
