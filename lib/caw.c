#include "caw.h"

#include <string.h>
#include <stdio.h>

#include "../usbd/usbd_main.h"
#include "../ll/status_led.h" // for blinking led when tx/rx data
#include "telemetry.h"

#define USB_RX_BUFFER 2048
static char reader[USB_RX_BUFFER];
static int16_t pReader = 0;

void Caw_Init( void )
{
    for( int i=0; i<USB_RX_BUFFER; i++ ){ reader[i] = 0; }
    USB_CDC_Init();
}

void Caw_DeInit( void )
{
    USB_CDC_DeInit();
}

// each of the send functions below queues one message: it reaches the host
// whole, or (when it cannot be queued) not at all
void Caw_send_raw( uint8_t* buf, uint32_t len )
{
    USB_tx_enqueue( NULL, 0, buf, len, NULL, 0 );
    status_led_xor(); // blink status light
}

static const uint8_t newline[] = "\n\r";

void Caw_printf( char* text, ... )
{
    va_list aptr;
    va_start(aptr, text);
    size_t len = vsnprintf( NULL, 0, text, aptr ); // get length of string
    va_end(aptr);
    char b[len+1]; // buffer to fit string plus NULL
    va_start(aptr, text);
    vsnprintf( b, len+1, text, aptr ); // put string in b
    va_end(aptr);

    USB_tx_enqueue( NULL, 0, (uint8_t*)b, len, newline, 2 );
    status_led_xor(); // blink status light
}

// luachunk expects a \0 terminated string
void Caw_send_luachunk( char* text )
{
    USB_tx_enqueue( NULL, 0, (uint8_t*)text, strlen(text), newline, 2 );
    status_led_xor(); // blink status light
}

// stream must stay valid until sent (it is not copied). followed by newline
void Caw_stream_constchar( const char* stream )
{
    USB_tx_stream( (const uint8_t*)stream, strlen(stream), newline, 2 );
    status_led_xor(); // blink status light
}

// buf must stay valid until sent (it is not copied). sent as is
void Caw_stream_raw( const uint8_t* buf, uint32_t len )
{
    USB_tx_stream( buf, len, NULL, 0 );
    status_led_xor(); // blink status light
}

// wait for a stream to finish (eg. before rewriting the flash it reads)
void Caw_stream_wait( void )
{
    USB_tx_stream_wait();
}

// called from the main loop: start any output queued from interrupts
void Caw_send_queued( void )
{
    USB_tx_kick();
}

void Caw_send_luaerror( char* error_msg )
{
    const uint8_t leader[] = "\\";
    USB_tx_enqueue( leader, 1, (uint8_t*)error_msg, strlen(error_msg), newline, 2 );
    status_led_xor(); // blink status light
}

void Caw_send_value( uint8_t type, float value )
{
    //
}

static C_cmd_t _find_cmd( char* str, uint32_t len )
{
    char* pStr = str;
    while( len-- ){ // FIXME should decrement first?
        if( *pStr++ == '^' ){
            if( *pStr++ == '^' ){
                switch( *pStr ){
                    case 'b': return C_boot;
                    case 's': return C_startupload;
                    case 'e': return C_endupload;
                    case 'w': return C_flashupload;
                    case 'c': return C_flashclear;
                    case 'r': return C_restart;
                    case 'p': return C_print;
                    case 'v': return C_version;
                    case 'i': return C_identity;
                    case 'k': return C_killlua;
                    case 'f': // fall through ->
                    case 'F': return C_loadFirst;
                }
            }
        }
    }
    return C_none;
}

static uint8_t _is_multiline( char* first_char, uint32_t len )
{
    // need at least 3 chars to be a valid multiline sequence
    if(len < 3){ return 0; }
    // buffer must lead with the backticks
    if( *first_char++ == '`' ){
        if( *first_char++ == '`' ){
            if( *first_char == '`' ){ return 1; }
        }
    }
    return 0;
}

static uint8_t _packet_complete( char* last_char )
{
    if( *last_char == '\0'
     || *last_char == '\n'
     || *last_char == '\r' ){ return 1; }
    return 0;
}

C_cmd_t Caw_try_receive( void )
{
    static uint8_t* buf;
    static uint32_t len;
    static uint8_t multiline = 0;

    C_cmd_t retcmd = C_none; // if nothing to dequeue do nothing

    if( USB_rx_dequeue_LOCK( &buf, &len ) ){
        retcmd = _find_cmd( (char*)buf, len ); // check for a system command
        if( retcmd != C_none ){ goto exit; } // sys command received, so skip ahead
        if( *buf == '\e' ){ // escape key
            pReader = 0;    // clear buffer
            retcmd = C_none;  // no action
            goto exit;
        }
        if( _is_multiline( (char*)buf, len ) ){
            multiline ^= 1;
            if(!multiline){
                retcmd = C_repl;
                goto exit;
            } else { // started new multiline
                if( len > 4 ){ // content follows the backticks
                    len -= 4;
                    buf = &buf[4];
                } else {
                    retcmd = C_none;
                    goto exit;
                }
            }
        }
        if( (pReader + len) >= USB_RX_BUFFER ){ // overflow protection
            pReader = 0;
            Telemetry_count( TM_rx_overflow, 1 );
            Caw_send_luachunk("!chunk too long!");
            printf("!chunk too long!\n");
            //multiline = 0; // FIXME can we know whether this is high / low?
            retcmd = C_none;
            goto exit;
        }
    // receive code for repl/flash
        memcpy( &(reader[pReader])
              , (char*)buf
              , len
              );
        pReader += len;
        if( !multiline
         && _packet_complete( &reader[pReader-1] ) ){
            retcmd = C_repl;
            status_led_xor(); // blink status light
            goto exit;
        }
    exit:
        USB_rx_dequeue_UNLOCK();
    }
    return retcmd;
}

char* Caw_get_read( void )
{
    return reader;
}

uint32_t Caw_get_read_len( void )
{
    uint32_t len = pReader;
    pReader = 0;
    return len;
}
