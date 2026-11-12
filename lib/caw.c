#include "caw.h"

#include <string.h>
#include <stdio.h>

#include "../usbd/usbd_main.h"
#include "../ll/status_led.h" // for blinking led when tx/rx data
#include "telemetry.h"

#define USB_RX_BUFFER 2048
static char reader[USB_RX_BUFFER];
static uint32_t pReader = 0;

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

// called from the main loop: start any output queued from interrupts, and
// let the host send again if the receive ring has room
void Caw_send_queued( void )
{
    USB_tx_kick();
    USB_rx_rearm();
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

// RECEIVE
//
// Caw_try_receive turns the packets from usbd_cdc_interface.c into REPL
// chunks and ^^ commands, one per call. It can stop partway through a packet
// and carry on from there on the next call.
// - "^^" and a command letter is a command anywhere in the stream, even split
//   across packets: a packet ending in '^' or "^^" holds them back until the
//   next one shows what follows. Text ahead of a command in its packet is
//   handed to the REPL first. The rest of the command's line (up to its line
//   ending, else to the end of the packet) is discarded; anything after that
//   is processed as usual. A line ending that the packet boundary split from
//   its command is dropped too, if the next packet is part of the same write.
// - Between ^^s and ^^e/^^w (upload) all text goes to the REPL as it arrives.
//   Only commands and ESC are interpreted.
// - Otherwise (the REPL), as before: a packet starting with ESC clears the
//   pending text (and an unclosed multiline block), a packet starting with
//   ``` opens or closes a multiline block, and text is evaluated once a packet
//   ends with \n, \r or \0, so that one host write is one chunk. Two additions:
//   a full packet ending a line may be followed by more of the same write, so
//   it waits until no packet has come for CAW_CONTINUE_MS; and on overflow the
//   text up to the last line ending is evaluated and the rest kept.
//
// Packets of one host write arrive back to back: a packet that arrives within
// CAW_CONTINUE_MS of the one before is taken to continue the same write.

#define CAW_CONTINUE_MS 2 // a write's next packet comes sooner than this
#define USB_PACKET      CDC_DATA_FS_OUT_PACKET_SIZE

static uint8_t  packet[USB_PACKET];
static uint32_t pkt_len = 0;
static uint8_t  pkt_gap = 0;     // ms since the packet before it arrived
static uint32_t pkt_pos = 0;     // next byte to look at
static uint8_t  pkt_fresh = 0;   // its packet-start rules are still to apply
static uint8_t  pkt_drop = 0;    // ESC led the packet: its text is discarded
static uint32_t pkt_kept = 0;    // text it added since its last command
static uint8_t  carets = 0;      // '^' held back from the end of the last packet
static uint8_t  carets_kept = 0; // ..which had text of its own before them
static uint8_t  carets_drop = 0; // ..which was discarded (ESC, or too long)
static char     cmd_eol = 0;     // a command's line ending, cut off by the
                                 // packet end: 1 any, or '\n' after '\r', '\r' after '\n'
static uint8_t  multiline = 0;
static uint8_t  upload = 0;      // between ^^s and ^^e/^^w, as in repl.c
static C_cmd_t  pending = C_none; // a command waiting behind its leading text
static uint32_t read_len = 0;    // length of the chunk handed to the REPL
static uint32_t consumed = 0;    // ..once taken: removed from reader next call

static C_cmd_t _cmd_letter( char c )
{
    switch( c ){
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

static uint8_t _is_end( char c )
{
    return (c == '\0' || c == '\n' || c == '\r');
}

// the text ends a line, but the full packet it came in may be continued
static uint8_t _chunk_waiting( void )
{
    return !upload && !multiline && !carets
        && pReader && _is_end( reader[pReader-1] );
}

static C_cmd_t _hand_over( uint32_t len )
{
    read_len = len;
    status_led_xor(); // blink status light
    return C_repl;
}

// add text to reader. returns C_repl if reader must be handed over first:
// nothing is added then, and the caller comes back with the same text
static C_cmd_t _append( const uint8_t* buf, uint32_t len, uint8_t* added )
{
    *added = 0;
    if( (pReader + len) >= USB_RX_BUFFER ){ // overflow protection
        if( upload ){ return _hand_over( pReader ); }
        if( !multiline ){ // evaluate up to the last line ending, keep the rest
            uint32_t end = pReader;
            while( end && !_is_end( reader[end-1] ) ){ end--; }
            if( end ){ return _hand_over( end ); }
        }
        pReader = 0;
        Telemetry_count( TM_rx_overflow, 1 );
        Caw_send_luachunk("!chunk too long!");
        printf("!chunk too long!\n");
        return C_none;
    }
    memcpy( &reader[pReader], buf, len );
    pReader += len;
    *added = 1;
    return C_none;
}

// the complement of e that makes \r\n or \n\r one line ending
static char _eol_pair( char e )
{
    return (e == '\r') ? '\n' : (e == '\n') ? '\r' : 0;
}

// a command was found and pkt_pos is just after it
static C_cmd_t _command( C_cmd_t c, uint32_t kept )
{
    // the rest of the line belongs to the command
    while( pkt_pos < pkt_len && !_is_end( packet[pkt_pos] ) ){ pkt_pos++; }
    cmd_eol = 1; // (the line ending may be in the next packet)
    if( pkt_pos < pkt_len ){
        char e = packet[pkt_pos++];
        cmd_eol = _eol_pair( e );
        if( pkt_pos < pkt_len && cmd_eol ){
            if( packet[pkt_pos] == cmd_eol ){ pkt_pos++; }
            cmd_eol = 0;
        }
    }
    pkt_kept = 0;

    // text ahead of the command is handed over first. in the REPL only whole
    // lines are: a line the command cut short was never meant to run, and is
    // discarded as it always was.
    if( !upload && !multiline && kept && kept <= pReader
     && !_is_end( reader[pReader-1] ) ){
        pReader -= kept;
    }
    uint8_t first = upload ? (pReader > 0)
                           : ( !multiline && pReader
                            && _is_end( reader[pReader-1] ) );
    switch( c ){ // follow repl.c in and out of upload
        case C_startupload: upload = 1; break;
        case C_endupload:   // fall through ->
        case C_flashupload: upload = 0; break;
        default: break;
    }
    if( first ){
        pending = c;
        return _hand_over( pReader );
    }
    return c;
}

static C_cmd_t _parse( void )
{
    if( pkt_fresh ){
        if( cmd_eol ){ // the last command's line ending, split off by the packet end
            if( pkt_gap <= CAW_CONTINUE_MS ){ // ..in the same write
                if( cmd_eol == 1 && _is_end( packet[0] ) ){
                    pkt_pos = 1;
                    if( pkt_len > 1 && _eol_pair( packet[0] )
                     && packet[1] == _eol_pair( packet[0] ) ){ pkt_pos = 2; }
                } else if( packet[0] == cmd_eol ){
                    pkt_pos = 1;
                }
            }
            cmd_eol = 0;
        }
        // a waiting chunk is complete unless this packet continues its write
        if( _chunk_waiting()
         && ( pkt_gap > CAW_CONTINUE_MS
           || packet[0] == '\e' || _is_multiline( (char*)packet, pkt_len ) ) ){
            return _hand_over( pReader );
        }
        if( carets ){ // did the last packet end partway into a command?
            C_cmd_t c = C_none;
            if( carets == 2 ){
                if( (c = _cmd_letter( packet[0] )) != C_none ){ pkt_pos = 1; }
            } else if( packet[0] == '^' ){
                if( pkt_len == 1 ){ // still undecided
                    carets = 2;
                    pkt_pos = 1;
                    return C_none;
                }
                if( (c = _cmd_letter( packet[1] )) != C_none ){ pkt_pos = 2; }
            }
            if( c != C_none ){
                carets = 0;
                pkt_fresh = 0;
                return _command( c, carets_kept );
            }
            // they were text after all, the end of the last packet's
            if( packet[0] != '\e' // (which ESC would clear)
             && !carets_drop ){
                uint8_t added;
                C_cmd_t r = _append( (const uint8_t*)"^^", carets, &added );
                if( r != C_none ){ return r; }
            }
            carets = 0;
        }
        pkt_fresh = 0;
        if( packet[0] == '\e' ){ // escape key
            pReader = 0;   // clear buffer
            multiline = 0; // and any unclosed multiline block
            pkt_drop = 1;  // the packet's text is discarded
            pkt_pos = 1;
        } else if( !upload && _is_multiline( (char*)packet, pkt_len ) ){
            multiline ^= 1;
            if( !multiline ){ // closing: evaluate, drop the rest of the packet
                pkt_drop = 1;
                pkt_pos = 3;
                read_len = pReader;
                return C_repl;
            }
            // started new multiline. content follows the backticks
            pkt_pos = (pkt_len > 4) ? 4 : pkt_len;
        }
    }

    // text runs up to the next command, or to a '^' or "^^" ending the packet
    uint32_t start = pkt_pos;
    uint32_t stop  = pkt_len;
    uint8_t  hold  = 0;
    C_cmd_t  c     = C_none;
    for( uint32_t i=start; i<pkt_len; i++ ){
        if( packet[i] != '^' ){ continue; }
        if( i+1 == pkt_len ){ hold = 1; stop = i; break; }
        if( packet[i+1] != '^' ){ continue; }
        if( i+2 == pkt_len ){ hold = 2; stop = i; break; }
        if( (c = _cmd_letter( packet[i+2] )) != C_none ){ stop = i; break; }
    }
    uint8_t added = 0;
    if( stop > start && !pkt_drop ){
        C_cmd_t r = _append( &packet[start], stop - start, &added );
        if( r != C_none ){ return r; }
        if( added ){ pkt_kept += stop - start; }
    }
    if( c != C_none ){
        pkt_pos = stop + 3;
        return _command( c, pkt_kept );
    }
    pkt_pos = pkt_len; // done with this packet
    if( hold ){
        carets = hold;
        carets_kept = (pkt_kept > 0);
        carets_drop = pkt_drop || (stop > start && !added); // text thrown away
    }
    if( upload ){ // stream it
        return pReader ? _hand_over( pReader ) : C_none;
    }
    if( added && !hold && !multiline
     && _is_end( packet[pkt_len-1] ) ){ // the packet ends a line
        if( pkt_len == USB_PACKET ){ return C_none; } // ..the write may go on
        return _hand_over( pReader );
    }
    return C_none;
}

C_cmd_t Caw_try_receive( void )
{
    if( consumed ){ // the REPL has taken the last chunk
        pReader -= consumed;
        memmove( reader, &reader[consumed], pReader );
        consumed = 0;
    }
    if( pending != C_none ){ // a command behind the chunk just handed over
        C_cmd_t c = pending;
        pending = C_none;
        return c;
    }
    if( pkt_pos >= pkt_len ){ // next packet
        if( _chunk_waiting() && !USB_rx_pending() ){
            // complete once no more of the write has come for CAW_CONTINUE_MS
            // (the tick is in whole ms: more than 2 means at least 2 have passed)
            if( (HAL_GetTick() - USB_rx_last_tick()) <= CAW_CONTINUE_MS ){
                return C_none;
            }
            return _hand_over( pReader );
        }
        pkt_len = USB_rx_dequeue( packet, &pkt_gap );
        if( pkt_len == 0 ){ return C_none; }
        pkt_pos   = 0;
        pkt_fresh = 1;
        pkt_drop  = 0;
        pkt_kept  = 0;
    }
    return _parse();
}

char* Caw_get_read( void )
{
    return reader;
}

uint32_t Caw_get_read_len( void )
{
    consumed = read_len;
    read_len = 0;
    return consumed;
}
