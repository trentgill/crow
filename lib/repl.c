#include "repl.h"

#include <stdlib.h> // malloc(), free()
#include <string.h> // memcpy()
#include <stdbool.h>
#include <stdio.h>

#include <stm32f7xx_hal.h> // HAL_Delay()
#include "lib/flash.h"     // Flash_write_(), Flash_which_userscript(), Flash_read()
#include "lib/caw.h"       // Caw_send_raw(), Caw_send_luachunk(), Caw_send_luaerror()
#include "lib/crashreport.h" // crashreport_skip_userscript()

// types
typedef enum{ REPL_normal
            , REPL_reception
            , REPL_discard
} L_repl_mode;

// global variables
lua_State*  Lua;
L_repl_mode repl_mode = REPL_normal;
char        new_script[USER_SCRIPT_SIZE]; // static alloc avoids malloc failures
uint16_t    new_script_len;
static bool running_from_mem;
static char running_script_name[64];

// prototypes
static bool REPL_new_script_buffer( uint32_t len );
static bool REPL_run_script( USERSCRIPT_t mode, char* buf, uint32_t len );
static void REPL_receive_script( char* buf, uint32_t len, ErrorHandler_t errfn );
static char* REPL_script_name_from_mem( char* dest, char* src, int max_len );

// public interface
void REPL_init( lua_State* lua )
{
    Lua = lua;
    running_from_mem = false;

    USERSCRIPT_t script = Flash_which_user_script();
    if( crashreport_skip_userscript() ){
        // the last boots all crashed soon after starting. run without a
        // script this once so the host can get in. flash is left as it is
        script = USERSCRIPT_Clear;
    }
    switch( script ){
        case USERSCRIPT_Default:
            REPL_run_script( USERSCRIPT_Default, NULL, 0 );
            break;
        case USERSCRIPT_User:
        {
            uint16_t flash_len = Flash_read_user_scriptlen();
            REPL_new_script_buffer( flash_len );
            if( Flash_read_user_script( new_script )
             || !REPL_run_script( USERSCRIPT_User, new_script, flash_len ) ){
                printf("failed to load user script\n");
                Caw_send_luachunk("failed to load user script");
            }
            break;
        }
        case USERSCRIPT_Clear:
            REPL_run_script( USERSCRIPT_Clear, NULL, 0 );
            break;
    }
}

void REPL_begin_upload( void )
{
    REPL_reset(); // free up memory
    if( REPL_new_script_buffer( USER_SCRIPT_SIZE ) ){
        repl_mode = REPL_reception;
    } else {
        repl_mode = REPL_discard;
    }
}

void REPL_upload( int flash )
{
    if( repl_mode == REPL_discard ){
        Caw_send_luachunk("upload failed, returning to normal mode");
    } else {
        if( REPL_run_script( USERSCRIPT_User
                           , new_script
                           , new_script_len ) ){ // successful load
            if( flash ){
                // TODO if we're setting init() should check it doesn't crash
                if( Flash_write_user_script( new_script
                                           , new_script_len
                                           ) ){
                    printf("flash write failed\n");
                    Caw_send_luachunk("User script upload failed!");
                } else {
                    printf("script saved, len: %i\n", new_script_len);
                    Caw_send_luachunk("User script updated.");
                    running_from_mem = false;
                    REPL_print_script_name();
                    Lua_crowbegin();
                }
            } else {
                running_from_mem = true;
                REPL_print_script_name();
                Lua_crowbegin();
            }
        } else {
            Caw_send_luachunk("User script evaluation failed.");
        }
    }
    repl_mode = REPL_normal;
}

void REPL_clear_script( void )
{
    REPL_reset();
    Flash_clear_user_script();
    running_from_mem = false;
    REPL_run_script( USERSCRIPT_Clear, NULL, 0 );
    Caw_send_luachunk("User script cleared.");
    REPL_print_script_name();
    Lua_crowbegin();
}

void REPL_default_script( void )
{
    REPL_reset();
    Flash_default_user_script();
    running_from_mem = false;
    REPL_run_script( USERSCRIPT_Default, NULL, 0 );
    Caw_send_luachunk("Using default script.");
    REPL_print_script_name();
    Lua_crowbegin();
}

void REPL_reset( void )
{
    Lua = Lua_Reset();
}

bool REPL_run_script( USERSCRIPT_t mode, char* buf, uint32_t len )
{
    switch (mode)
    {
        case USERSCRIPT_Default:
            Lua_load_default_script();
            strcpy( running_script_name, "Running: First.lua" );
            break;
        case USERSCRIPT_User:
            if ( Lua_eval( Lua, buf, len, "=userscript" ) ){
                return false;
            }
            strcpy( running_script_name, "Running: " );
            REPL_script_name_from_mem( &running_script_name[9], buf, 64-10);
            break;
        case USERSCRIPT_Clear:
            strcpy( running_script_name, "No user script." );
            break;
    }
    return true;
}

void REPL_eval( char* buf, uint32_t len, ErrorHandler_t errfn )
{
    if( repl_mode == REPL_normal ){
        if(Lua_eval( Lua, buf
                   , len
                   , "=repl"
                   )){
            printf("!eval\n");
        }
    } else if( repl_mode == REPL_reception ){ // REPL_reception
        REPL_receive_script( buf, len, errfn );
    } // else REPL_discard
}

void REPL_print_script( void )
{
    if( !running_from_mem && Flash_which_user_script() == USERSCRIPT_User ){
        uint16_t length = Flash_read_user_scriptlen();
        char* addr = Flash_read_user_scriptaddr();
        const int chunk = 0x200;
        while( length > chunk ){
            Caw_send_raw( (uint8_t*)addr, chunk );
            length -= chunk;
            addr += chunk;
            HAL_Delay(3); // wait for usb tx
        }
        Caw_send_raw( (uint8_t*)addr, length );
    } else {
        REPL_print_script_name();
    }
}

void REPL_print_script_name( void )
{
    Caw_send_luachunk( running_script_name );
}

// private funcs
static void REPL_receive_script( char* buf, uint32_t len, ErrorHandler_t errfn )
{
    if( new_script_len + len >= USER_SCRIPT_SIZE ){
        Caw_send_luachunk("!ERROR! Script is too long.");
        repl_mode = REPL_discard;
    } else {
        memcpy( &new_script[new_script_len], buf, len );
        new_script_len += len;
    }
}

static bool REPL_new_script_buffer( uint32_t len )
{
    new_script_len = 0; // reset counter as the buffer is empty
    return true;
}

static char* REPL_script_name_from_mem( char* dest, char* src, int max_len )
{
    while( *src == '-' ){ src++; } // skip commments
    while( *src == ' ' ){ src++; } // skip spaces
    char* linebreak = strchr( src, '\n' );
    int len = linebreak ? (int)(linebreak - src) : max_len;
    if( len >= max_len ){ len = max_len - 1; }
// disable this warning bc we take care to null terminate our string
// *dest is a static 64byte array, so there's always room for it
#pragma GCC diagnostic ignored "-Wstringop-truncation"
    strncpy( dest, src, len );
#pragma GCC diagnostic pop
    dest[len] = '\0';
    return dest;
}
