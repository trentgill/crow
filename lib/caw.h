#pragma once

#include <stm32f7xx.h>
#include <stdarg.h>

typedef enum{ C_none
            , C_repl
            , C_boot
            , C_startupload
            , C_endupload
            , C_flashupload
            , C_flashclear
            , C_restart
            , C_print
            , C_version
            , C_identity
            , C_killlua
            , C_loadFirst
} C_cmd_t;

void Caw_Init( void );
void Caw_DeInit( void );

void Caw_send_raw( uint8_t* buf, uint32_t len );
void Caw_printf( char* text, ... );
void Caw_send_luachunk( char* text );
void Caw_send_luaerror( char* error_msg );
void Caw_send_value( uint8_t type, float value ); // enum the type
void Caw_stream_constchar( const char* stream );
void Caw_stream_raw( const uint8_t* buf, uint32_t len );
void Caw_stream_wait( void );
void Caw_send_queued( void );

C_cmd_t Caw_try_receive( void );
char* Caw_get_read( void );
uint32_t Caw_get_read_len( void );
