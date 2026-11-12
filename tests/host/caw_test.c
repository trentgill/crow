// Host-side tests for the USB serial transport: the real lib/caw.c and
// usbd/usbd_cdc_interface.c, compiled against tests/host/stub/ with a fake
// USB device library, a fake clock and a fake host on the far end.
// Build and run with tests/host/run_caw.sh.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../../lib/caw.c" // its file-local state is reset by the RX tests
#include "../../lib/telemetry.h"
#include "../../usbd/usbd_cdc_interface.h"
#include "../../ll/status_led.h"

/////////////////////////////////////////////////////
// fake core

uint32_t test_ipsr = 0;
uint32_t test_primask = 0;
uint32_t tm_counters[TM_COUNT];

#define IRQ_OTG (16 + 67) // OTG_FS
#define IRQ_DMA (16 + 56) // stands for the audio DMA interrupt (priority 5)

static int failures = 0;
#define CHECK(c) do{ if(!(c)){ failures++; \
    printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } }while(0)

// the clock only moves when a test moves it, or while thread-mode code with
// interrupts enabled polls it (a wait loop): then `idle_hook` runs, standing
// for the interrupts that would fire meanwhile
static uint32_t now_ms = 0;
static void (*idle_hook)( void ) = NULL;
uint32_t HAL_GetTick( void )
{
    if( test_ipsr == 0 && test_primask == 0 && idle_hook ){ idle_hook(); }
    return now_ms;
}

void Lua_watchdog_exempt_end( uint32_t token ){ (void)token; }
void status_led_xor( void ){}
void status_led_fast( LED_SPEED is_fast ){ (void)is_fast; }
void USB_CDC_Init( void ){}
void USB_CDC_DeInit( void ){}

/////////////////////////////////////////////////////
// fake USB device library

USBD_HandleTypeDef USBD_Device;
static USBD_CDC_HandleTypeDef hcdc;
static int      rx_armed = 0;      // OUT endpoint ready for a packet
static uint32_t tx_submits = 0;
static uint32_t tx_max_len = 0;
static const uint8_t* src_lo = NULL; // transfers taken from [src_lo, src_hi)
static const uint8_t* src_hi = NULL;
static uint32_t src_hits = 0;

uint8_t USBD_CDC_SetTxBuffer( USBD_HandleTypeDef* pdev, uint8_t* pbuff, uint32_t length )
{
    (void)pdev;
    hcdc.TxBuffer = pbuff;
    hcdc.TxLength = length;
    return USBD_OK;
}

uint8_t USBD_CDC_SetRxBuffer( USBD_HandleTypeDef* pdev, uint8_t* pbuff )
{
    (void)pdev;
    hcdc.RxBuffer = pbuff;
    return USBD_OK;
}

uint8_t USBD_CDC_TransmitPacket( USBD_HandleTypeDef* pdev )
{
    // submitting is only safe where the OTG interrupt cannot be preempted
    // halfway through its own endpoint bookkeeping
    CHECK( (test_ipsr == 0 && test_primask == 1) || test_ipsr == IRQ_OTG );
    CHECK( pdev->dev_state == USBD_STATE_CONFIGURED && pdev->pClassData );
    CHECK( hcdc.TxLength > 0 );
    CHECK( hcdc.TxState == 0 ); // the interface must know a transfer is out
    if( hcdc.TxState ){ return USBD_BUSY; }
    hcdc.TxState = 1;
    tx_submits++;
    if( hcdc.TxLength > tx_max_len ){ tx_max_len = hcdc.TxLength; }
    if( hcdc.TxBuffer >= src_lo && hcdc.TxBuffer < src_hi ){
        CHECK( hcdc.TxBuffer + hcdc.TxLength <= src_hi );
        src_hits++;
    }
    return USBD_OK;
}

uint8_t USBD_CDC_ReceivePacket( USBD_HandleTypeDef* pdev )
{
    CHECK( (test_ipsr == 0 && test_primask == 1) || test_ipsr == IRQ_OTG );
    CHECK( pdev->pClassData != NULL );
    CHECK( !rx_armed ); // never armed twice
    rx_armed = 1;
    return USBD_OK;
}

static void otg_enter( void ){ CHECK( test_ipsr == 0 ); test_ipsr = IRQ_OTG; }
static void otg_exit( void ){ test_ipsr = 0; }

// SET_CONFIGURATION: the class allocates a fresh handle, calls Init, then
// arms the OUT endpoint itself
static void usb_configure( void )
{
    memset( &hcdc, 0, sizeof hcdc );
    USBD_Device.pClassData = &hcdc;
    otg_enter();
    USBD_CDC_fops.Init();
    otg_exit();
    rx_armed = 1;
    USBD_Device.dev_state = USBD_STATE_CONFIGURED;
}

// bus reset / cable pulled: whatever was on the wire is gone
static void usb_disconnect( void )
{
    otg_enter();
    USBD_CDC_fops.DeInit();
    otg_exit();
    USBD_Device.pClassData = NULL;
    USBD_Device.dev_state = USBD_STATE_DEFAULT;
    memset( &hcdc, 0, sizeof hcdc );
    rx_armed = 0;
}

/////////////////////////////////////////////////////
// fake host: what it has read

static uint8_t cap[1 << 22];
static size_t  cap_len = 0;

// the host reads the transfer on the wire (the bytes are only looked at now,
// so anything that overwrote them while in flight shows up here)
static int host_complete( void )
{
    if( !hcdc.TxState ){ return 0; }
    CHECK( cap_len + hcdc.TxLength <= sizeof cap );
    memcpy( &cap[cap_len], hcdc.TxBuffer, hcdc.TxLength );
    cap_len += hcdc.TxLength;
    hcdc.TxState = 0;
    otg_enter();
    USBD_CDC_fops.TransmitCplt( hcdc.TxBuffer, &hcdc.TxLength, 1 );
    otg_exit();
    return 1;
}

// the host sends one packet (the endpoint must be armed)
static void host_send( const uint8_t* d, uint32_t len )
{
    CHECK( rx_armed );
    rx_armed = 0;
    memcpy( hcdc.RxBuffer, d, len );
    uint32_t l = len;
    otg_enter();
    USBD_CDC_fops.Receive( hcdc.RxBuffer, &l );
    otg_exit();
}

static void host_drain( void )
{
    for( int i=0; i<100000; i++ ){
        Caw_send_queued(); // the main loop's kick
        if( !host_complete() ){ break; }
    }
}

static uint32_t tm( TM_counter_t c ){ return tm_counters[c]; }

// a fresh, unconfigured device with empty rings
static void reset_all( void )
{
    if( USBD_Device.pClassData ){ usb_disconnect(); }
    idle_hook = NULL;
    now_ms = 1000;
    CDC_clear_buffers();
    memset( tm_counters, 0, sizeof tm_counters );
    cap_len = 0;
    tx_submits = 0;
    tx_max_len = 0;
    test_ipsr = 0;
    test_primask = 0;
}

// configured, and past the hold-off
static void connect_now( void )
{
    usb_configure();
    now_ms += 500;
}

/////////////////////////////////////////////////////
// TX tests

// a host that reads every call (each call is a ms)
static void hook_host_reads( void ){ now_ms++; host_complete(); }

static void test_tx_holdoff( void )
{
    printf("tx: boot text kept, 500ms hold-off after configure\n");
    reset_all();
    Caw_send_luachunk( "^^ready()" ); // before any host
    Caw_send_queued();
    CHECK( tx_submits == 0 );
    usb_configure();
    for( int i=0; i<500; i++ ){ Caw_send_queued(); now_ms++; }
    CHECK( tx_submits == 0 ); // nothing during the hold-off
    now_ms--;
    Caw_send_queued();
    CHECK( tx_submits == 0 ); // 499 ms
    now_ms++;
    Caw_send_queued();
    CHECK( tx_submits == 1 ); // 500 ms
    host_drain();
    CHECK( cap_len == 11 && !memcmp( cap, "^^ready()\n\r", 11 ) );
    CHECK( USB_tx_is_ready() );
    CHECK( tm( TM_tx_bytes ) == 11 && tm( TM_tx_dropped ) == 0 );
}

static void test_tx_formats( void )
{
    printf("tx: bytes of each send function\n");
    reset_all();
    connect_now();
    idle_hook = hook_host_reads; // the second stream waits for the first
    Caw_send_raw( (uint8_t*)"raw", 3 );
    Caw_printf( "%s=%d", "x", 42 );
    Caw_send_luachunk( "chunk" );
    Caw_send_luaerror( "oops" );
    Caw_stream_constchar( "help" );
    Caw_stream_constchar( "" );
    Caw_stream_raw( (const uint8_t*)"script", 6 );
    idle_hook = NULL;
    host_drain();
    const char* want = "rawx=42\n\rchunk\n\r\\oops\n\rhelp\n\r\n\rscript";
    CHECK( cap_len == strlen(want) && !memcmp( cap, want, cap_len ) );
    CHECK( tm( TM_tx_bytes ) == strlen(want) );
}

static void test_tx_is_ready( void )
{
    printf("tx: USB_tx_is_ready\n");
    reset_all();
    CHECK( USB_tx_is_ready() );
    Caw_send_raw( (uint8_t*)"a", 1 );
    CHECK( !USB_tx_is_ready() ); // queued
    connect_now();
    Caw_send_queued();
    CHECK( tx_submits == 1 );
    CHECK( !USB_tx_is_ready() ); // in flight
    host_complete();
    CHECK( USB_tx_is_ready() );
    Caw_stream_raw( (const uint8_t*)"abc", 3 );
    CHECK( !USB_tx_is_ready() ); // streaming
    host_drain();
    CHECK( USB_tx_is_ready() );
}

// messages are "<id:payload>" so the capture can be checked message by message
#define MAX_MSGS 20000
static uint32_t exp_id[MAX_MSGS];
static uint32_t exp_len[MAX_MSGS];
static int      exp_n = 0;
static uint32_t next_id = 0;
static uint32_t dropped_bytes = 0;
static char     msg_buf[8192];

static uint32_t make_msg( uint32_t id, uint32_t len )
{
    // len includes the frame; the payload letter depends on the id
    int n = snprintf( msg_buf, sizeof msg_buf, "<%u:", id );
    while( (uint32_t)n < len - 1 ){ msg_buf[n++] = 'a' + (id % 26); }
    msg_buf[n++] = '>';
    msg_buf[n] = 0;
    return n;
}

// queue one message via Caw_printf ("%s" + "\n\r") and note what happened
static void send_msg( uint32_t len )
{
    uint32_t id = next_id++;
    uint32_t n = make_msg( id, len );
    char copy[8192];
    memcpy( copy, msg_buf, n + 1 );
    uint32_t before = tm( TM_tx_dropped );
    uint32_t nested = dropped_bytes; // interrupts may drop their own meanwhile
    Caw_printf( "%s", copy );
    uint32_t d = tm( TM_tx_dropped ) - before - (dropped_bytes - nested);
    if( d ){
        CHECK( d == n + 2 ); // the whole message, newline included
        dropped_bytes += d;
    } else {
        CHECK( exp_n < MAX_MSGS );
        exp_id[exp_n]  = id;
        exp_len[exp_n] = n + 2;
        exp_n++;
    }
}

// the capture must be exactly the queued messages, whole and in order
static void check_msgs( void )
{
    size_t p = 0;
    int ok = 1;
    for( int i=0; i<exp_n && ok; i++ ){
        make_msg( exp_id[i], exp_len[i] - 2 );
        uint32_t n = exp_len[i];
        if( p + n > cap_len
         || memcmp( &cap[p], msg_buf, n - 2 )
         || memcmp( &cap[p + n - 2], "\n\r", 2 ) ){
            printf("  message %d (id %u) mismatch at byte %zu\n", i, exp_id[i], p);
            ok = 0;
        }
        p += n;
    }
    CHECK( ok );
    CHECK( p == cap_len );
}

static void msgs_reset( void ){ exp_n = 0; next_id = 0; dropped_bytes = 0; }


static void test_tx_backpressure( void )
{
    printf("tx: thread producer waits for a reading host\n");
    reset_all();
    msgs_reset();
    connect_now();
    idle_hook = hook_host_reads;
    for( int i=0; i<40; i++ ){ send_msg( 1000 ); } // 40 kB through a 4 kB ring
    for( int i=1; i<=300; i++ ){ send_msg( 2 + (i % 7) ); }
    idle_hook = NULL;
    host_drain();
    CHECK( tm( TM_tx_dropped ) == 0 );
    check_msgs();
    CHECK( tm( TM_tx_bytes ) == cap_len );
    CHECK( tx_max_len <= 1024 );
}

static int slow_count = 0;
static void hook_host_slow( void )
{
    now_ms++;
    if( ++slow_count % 15 == 0 ){ host_complete(); } // a completion every 15 ms
}

static void test_tx_slow_host( void )
{
    printf("tx: slow host (15 ms per transfer) still loses nothing\n");
    reset_all();
    msgs_reset();
    connect_now();
    idle_hook = hook_host_slow;
    for( int i=0; i<20; i++ ){ send_msg( 900 ); }
    idle_hook = NULL;
    host_drain();
    CHECK( tm( TM_tx_dropped ) == 0 );
    check_msgs();
}

static void hook_time_only( void ){ now_ms++; }

static void test_tx_no_reader( void )
{
    printf("tx: host stops reading: bounded wait, then whole messages dropped\n");
    reset_all();
    msgs_reset();
    connect_now();
    idle_hook = hook_time_only;
    send_msg( 1000 ); // goes on the wire, never completes
    CHECK( tx_submits == 1 );
    send_msg( 1000 );
    send_msg( 1000 );
    send_msg( 1000 ); // ring: 3006 of 4096 queued (in flight still counts)
    CHECK( tm( TM_tx_dropped ) == 0 );
    uint32_t t0 = now_ms;
    send_msg( 1500 ); // does not fit: waits TX_WAIT_MS without progress
    uint32_t waited = now_ms - t0;
    CHECK( waited >= 20 && waited <= 25 );
    CHECK( tm( TM_tx_dropped ) == 1502 );
    send_msg( 50 ); // fits: no wait
    // once the transfer has been out for 250 ms nobody is reading: no waiting
    now_ms = t0 + 300;
    t0 = now_ms;
    send_msg( 2000 );
    CHECK( now_ms - t0 <= 2 );
    CHECK( tm( TM_tx_dropped ) == 1502 + 2002 );
    idle_hook = NULL;
    // the host comes back: everything that was queued arrives whole
    host_drain();
    check_msgs();
    CHECK( tm( TM_tx_bytes ) + tm( TM_tx_dropped ) == 4*1002 + 1502 + 52 + 2002 );
}

static void test_tx_unconfigured( void )
{
    printf("tx: no host: never submits, never waits, drops whole\n");
    reset_all();
    msgs_reset();
    idle_hook = hook_time_only;
    uint32_t t0 = now_ms;
    for( int i=0; i<10; i++ ){ send_msg( 1000 ); }
    CHECK( now_ms - t0 <= 20 );
    CHECK( tx_submits == 0 );
    CHECK( exp_n == 4 ); // 4 x 1002 fit in 4096
    CHECK( tm( TM_tx_dropped ) == 6 * 1002 );
    idle_hook = NULL;
    connect_now();
    host_drain();
    check_msgs();
}

static void test_tx_too_big( void )
{
    printf("tx: a message larger than the ring is dropped whole\n");
    reset_all();
    connect_now();
    idle_hook = hook_host_reads;
    static char big[5000];
    memset( big, 'x', sizeof big - 1 );
    uint32_t t0 = now_ms;
    Caw_send_luachunk( big );
    CHECK( now_ms - t0 <= 1 );
    CHECK( tm( TM_tx_dropped ) == 4999 + 2 );
    idle_hook = NULL;
    host_drain();
    CHECK( cap_len == 0 );
}

static void isr_msg( uint32_t len )
{
    uint32_t ipsr = test_ipsr;
    test_ipsr = IRQ_DMA;
    send_msg( len );
    test_ipsr = ipsr;
}

static void test_tx_isr_producer( void )
{
    printf("tx: interrupt producer never waits or submits\n");
    reset_all();
    msgs_reset();
    connect_now();
    idle_hook = hook_time_only;
    uint32_t t0 = now_ms;
    for( int i=0; i<10; i++ ){ isr_msg( 1000 ); }
    CHECK( now_ms == t0 );
    CHECK( tx_submits == 0 );
    CHECK( exp_n == 4 );
    CHECK( tm( TM_tx_dropped ) == 6 * 1002 );
    idle_hook = NULL;
    host_drain(); // the main loop starts it
    check_msgs();
}

// random interleaving of thread and interrupt producers, a host reading at
// random, and interrupts landing inside thread-mode waits
static uint32_t rng = 12345;
static uint32_t rnd( void ){ rng = rng * 1103515245u + 12345u; return (rng >> 8) & 0xffffff; }

static void hook_random( void )
{
    now_ms++;
    uint32_t r = rnd() % 10;
    if( r < 6 ){ host_complete(); }
    else if( r < 8 ){ isr_msg( 4 + rnd() % 300 ); }
}

static void test_tx_random( void )
{
    printf("tx: random mix of producers and host reads\n");
    reset_all();
    msgs_reset();
    connect_now();
    for( int i=0; i<6000; i++ ){
        uint32_t r = rnd() % 100;
        if( r < 40 ){
            idle_hook = hook_random;
            send_msg( 4 + rnd() % ((rnd() % 4) ? 200 : 4500) );
            idle_hook = NULL;
        } else if( r < 60 ){
            isr_msg( 4 + rnd() % 300 );
        } else if( r < 85 ){
            host_complete();
        } else if( r < 95 ){
            Caw_send_queued();
        } else {
            now_ms += rnd() % 400; // sometimes long enough to look stalled
        }
    }
    host_drain();
    printf("  %d messages delivered (%zu bytes), %u bytes dropped\n"
          , exp_n, cap_len, dropped_bytes);
    CHECK( exp_n > 1000 && dropped_bytes > 0 );
    check_msgs();
    CHECK( tm( TM_tx_dropped ) == dropped_bytes );
    CHECK( tm( TM_tx_bytes ) == cap_len );
    CHECK( tx_max_len <= 1024 );
}

static void test_tx_stream( void )
{
    printf("tx: streams go out in order, zero-copy\n");
    reset_all();
    static uint8_t script[15000];
    for( size_t i=0; i<sizeof script; i++ ){ script[i] = ' ' + (i * 7) % 90; }
    src_lo = script;
    src_hi = script + sizeof script;
    src_hits = 0;
    connect_now();
    Caw_send_luachunk( "before" );
    Caw_stream_raw( script, sizeof script ); // ^^p
    Caw_send_luachunk( "after" );            // queued while the stream waits
    static char help[3000];
    memset( help, 'h', sizeof help - 1 );
    idle_hook = hook_host_reads;
    Caw_stream_constchar( help );            // waits for the first stream
    idle_hook = NULL;
    Caw_send_luachunk( "end" );
    host_drain();
    CHECK( tm( TM_tx_dropped ) == 0 );
    // the whole thing, in order
    static uint8_t want[40000];
    size_t w = 0;
    memcpy( &want[w], "before\n\r", 8 ); w += 8;
    memcpy( &want[w], script, sizeof script ); w += sizeof script;
    memcpy( &want[w], "after\n\r", 7 ); w += 7;
    memcpy( &want[w], help, strlen(help) ); w += strlen(help);
    memcpy( &want[w], "\n\r", 2 ); w += 2;
    memcpy( &want[w], "end\n\r", 5 ); w += 5;
    CHECK( cap_len == w && !memcmp( cap, want, w ) );
    CHECK( src_hits >= sizeof script / 1024 ); // sent from the source itself
    src_lo = src_hi = NULL;
    CHECK( tx_max_len <= 1024 );
    CHECK( USB_tx_is_ready() );
}

static void test_tx_stream_wait( void )
{
    printf("tx: Caw_stream_wait returns once the stream has gone\n");
    reset_all();
    static uint8_t script[6000];
    memset( script, 's', sizeof script );
    connect_now();
    Caw_stream_raw( script, sizeof script );
    idle_hook = hook_host_reads;
    Caw_stream_wait();
    idle_hook = NULL;
    CHECK( cap_len == sizeof script );
    // a host that does not read: bounded
    Caw_stream_raw( script, sizeof script );
    Caw_send_queued();
    idle_hook = hook_time_only;
    uint32_t t0 = now_ms;
    Caw_stream_wait();
    CHECK( now_ms - t0 <= 25 );
    idle_hook = NULL;
    host_drain();
    CHECK( cap_len == 2 * sizeof script );
}

static void test_tx_reconnect( void )
{
    printf("tx: disconnect mid-transfer: nothing lost\n");
    reset_all();
    msgs_reset();
    connect_now();
    for( int i=0; i<3; i++ ){ send_msg( 900 ); }
    Caw_send_queued();
    CHECK( tx_submits == 1 );
    usb_disconnect(); // the transfer on the wire is lost
    send_msg( 100 );
    Caw_send_queued();
    CHECK( tx_submits == 1 );
    connect_now();
    host_drain();
    check_msgs();
}

static void run_tx_tests( void )
{
    test_tx_holdoff();
    test_tx_formats();
    test_tx_is_ready();
    test_tx_backpressure();
    test_tx_slow_host();
    test_tx_no_reader();
    test_tx_unconfigured();
    test_tx_too_big();
    test_tx_isr_producer();
    test_tx_random();
    test_tx_stream();
    test_tx_stream_wait();
    test_tx_reconnect();
}

/////////////////////////////////////////////////////
// RX: the oracle
//
// Caw_try_receive and its helpers exactly as they were before the receive
// path was rebuilt (lib/caw.c at 2ff7e59), with every name prefixed o_ and
// fed by o_dequeue_LOCK from a list of packets, one packet per call.

#define O_RX_BUFFER 2048
static char o_reader[O_RX_BUFFER];
static int16_t o_pReader = 0;

static uint8_t  o_pkts[200000][CDC_DATA_FS_OUT_PACKET_SIZE + 4]; // zero padded
static uint32_t o_lens[200000];
static int      o_npkts = 0;
static int      o_next = 0;
static uint8_t  o_buf[CDC_DATA_FS_OUT_PACKET_SIZE + 4];
static int      o_overflows = 0;
static int      o_reset = 0;

static uint8_t o_dequeue_LOCK( uint8_t** buf, uint32_t* len )
{
    if( o_next >= o_npkts ){ return 0; }
    // the old receive buffer: the packet, then whatever stale bytes followed
    memset( o_buf, 0, sizeof o_buf );
    memcpy( o_buf, o_pkts[o_next], o_lens[o_next] );
    *buf = o_buf;
    *len = o_lens[o_next];
    o_next++;
    return 1;
}
static void o_dequeue_UNLOCK( void ){}
static void o_luachunk( char* s ){ (void)s; o_overflows++; }
#define o_count(c,n)
#define o_printf(...)
#define o_led_xor()

static C_cmd_t o_find_cmd( char* str, uint32_t len )
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

static uint8_t o_is_multiline( char* first_char, uint32_t len )
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

static uint8_t o_packet_complete( char* last_char )
{
    if( *last_char == '\0'
     || *last_char == '\n'
     || *last_char == '\r' ){ return 1; }
    return 0;
}

C_cmd_t o_try_receive( void )
{
    static uint8_t* buf;
    static uint32_t len;
    static uint8_t multiline = 0;
    if( o_reset ){ multiline = 0; o_reset = 0; } // test harness only

    C_cmd_t retcmd = C_none; // if nothing to dequeue do nothing

    if( o_dequeue_LOCK( &buf, &len ) ){
        retcmd = o_find_cmd( (char*)buf, len ); // check for a system command
        if( retcmd != C_none ){ goto exit; } // sys command received, so skip ahead
        if( *buf == '\e' ){ // escape key
            o_pReader = 0;    // clear buffer
            retcmd = C_none;  // no action
            goto exit;
        }
        if( o_is_multiline( (char*)buf, len ) ){
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
        if( (o_pReader + len) >= O_RX_BUFFER ){ // overflow protection
            o_pReader = 0;
            o_count( TM_rx_overflow, 1 );
            o_luachunk("!chunk too long!");
            o_printf("!chunk too long!\n");
            //multiline = 0; // FIXME can we know whether this is high / low?
            retcmd = C_none;
            goto exit;
        }
    // receive code for repl/flash
        memcpy( &(o_reader[o_pReader])
              , (char*)buf
              , len
              );
        o_pReader += len;
        if( !multiline
         && o_packet_complete( &o_reader[o_pReader-1] ) ){
            retcmd = C_repl;
            o_led_xor(); // blink status light
            goto exit;
        }
    exit:
        o_dequeue_UNLOCK();
    }
    return retcmd;
}

char* o_get_read( void )
{
    return o_reader;
}

uint32_t o_get_read_len( void )
{
    uint32_t len = o_pReader;
    o_pReader = 0;
    return len;
}

/////////////////////////////////////////////////////
// RX: what the REPL sees
//
// Both parsers drive the same model of main.c + repl.c. It logs every command,
// every chunk evaluated outside an upload, every finished upload's script,
// and every "!chunk too long!", so two runs can be compared byte for byte.

typedef struct{
    uint8_t* buf;
    size_t   len;
    size_t   cap;
} log_t;

static void log_put( log_t* l, char type, const void* data, size_t n )
{
    char head[32];
    int h = snprintf( head, sizeof head, "%c%zu:", type, n );
    if( l->len + h + n + 1 > l->cap ){
        l->cap = (l->len + h + n + 1) * 2;
        l->buf = realloc( l->buf, l->cap );
    }
    memcpy( &l->buf[l->len], head, h ); l->len += h;
    memcpy( &l->buf[l->len], data, n ); l->len += n;
    l->buf[l->len++] = '|';
}

typedef struct{
    int     upload;
    uint8_t script[1 << 17];
    size_t  slen;
    log_t   log;
} repl_t;

static repl_t m_old, m_new, m_want;

static void repl_reset( repl_t* m )
{
    m->upload = 0;
    m->slen = 0;
    m->log.len = 0;
}

static char cmd_char( C_cmd_t c )
{
    switch( c ){
        case C_boot:        return 'b';
        case C_startupload: return 's';
        case C_endupload:   return 'e';
        case C_flashupload: return 'w';
        case C_flashclear:  return 'c';
        case C_restart:     return 'r';
        case C_print:       return 'p';
        case C_version:     return 'v';
        case C_identity:    return 'i';
        case C_killlua:     return 'k';
        case C_loadFirst:   return 'f';
        default:            return '?';
    }
}

static void model( repl_t* m, C_cmd_t c, const char* data, uint32_t len )
{
    char cc;
    switch( c ){
        case C_none: return;
        case C_repl:
            if( m->upload ){
                CHECK( m->slen + len <= sizeof m->script );
                memcpy( &m->script[m->slen], data, len );
                m->slen += len;
            } else {
                log_put( &m->log, 'E', data, len );
            }
            return;
        case C_startupload:
            m->upload = 1;
            m->slen = 0;
            break;
        case C_endupload:
        case C_flashupload:
            if( m->upload ){ log_put( &m->log, 'S', m->script, m->slen ); }
            m->upload = 0;
            break;
        default: break;
    }
    cc = cmd_char( c );
    log_put( &m->log, 'C', &cc, 1 );
}

// expected results, by hand
static void want_eval( const char* s ){ model( &m_want, C_repl, s, strlen(s) ); }
static void want_cmd( C_cmd_t c ){ model( &m_want, c, NULL, 0 ); }
static void want_overflow( void ){ log_put( &m_want.log, 'O', "", 0 ); }

static int logs_equal( log_t* a, log_t* b, const char* what )
{
    if( a->len == b->len && !memcmp( a->buf, b->buf, a->len ) ){ return 1; }
    size_t i = 0;
    while( i < a->len && i < b->len && a->buf[i] == b->buf[i] ){ i++; }
    size_t from = (i > 60) ? i - 60 : 0;
    printf("  %s: logs differ at byte %zu (lengths %zu, %zu)\n", what, i, a->len, b->len);
    printf("    a: \"");
    for( size_t k=from; k<a->len && k<i+60; k++ ){
        uint8_t ch = a->buf[k];
        if( ch >= 32 && ch < 127 ){ putchar( ch ); } else { printf("\\x%02x", ch); }
    }
    printf("\"\n    b: \"");
    for( size_t k=from; k<b->len && k<i+60; k++ ){
        uint8_t ch = b->buf[k];
        if( ch >= 32 && ch < 127 ){ putchar( ch ); } else { printf("\\x%02x", ch); }
    }
    printf("\"\n");
    return 0;
}

/////////////////////////////////////////////////////
// RX: packets and how they arrive

#define MAX_PKTS 200000
static uint8_t  pk_data[MAX_PKTS][CDC_DATA_FS_OUT_PACKET_SIZE];
static uint32_t pk_len[MAX_PKTS];
static uint32_t pk_gap[MAX_PKTS]; // ms between the previous packet and this one
static int      pk_n = 0;

static void pk_reset( void ){ pk_n = 0; }

static void pk_add( const uint8_t* d, uint32_t len, uint32_t gap )
{
    CHECK( pk_n < MAX_PKTS && len <= CDC_DATA_FS_OUT_PACKET_SIZE );
    memcpy( pk_data[pk_n], d, len );
    pk_len[pk_n] = len;
    pk_gap[pk_n] = gap;
    pk_n++;
}

// one host write, cut into packets of `size` bytes (64: what the host does)
static void write_sized( const void* d, size_t len, uint32_t size, uint32_t gap_write, uint32_t gap_in )
{
    const uint8_t* p = d;
    int first = 1;
    while( len ){
        uint32_t n = (len > size) ? size : len;
        pk_add( p, n, first ? gap_write : gap_in );
        first = 0;
        p += n;
        len -= n;
    }
}
static void write_str( const char* s, uint32_t gap ){ write_sized( s, strlen(s), 64, gap, 0 ); }

// the old parser over the same packets
static void run_old( repl_t* m )
{
    o_pReader = 0;
    o_npkts = pk_n;
    for( int i=0; i<pk_n; i++ ){
        memset( o_pkts[i], 0, sizeof o_pkts[i] );
        memcpy( o_pkts[i], pk_data[i], pk_len[i] );
        o_lens[i] = pk_len[i];
    }
    o_next = 0;
    repl_reset( m );
    while( o_next < o_npkts ){
        int before = o_overflows;
        C_cmd_t c = o_try_receive();
        if( o_overflows != before ){ log_put( &m->log, 'O', "", 0 ); }
        if( c == C_repl ){
            char* b = o_get_read();
            uint32_t l = o_get_read_len();
            model( m, c, b, l );
        } else {
            model( m, c, NULL, 0 );
        }
    }
}

// the device: host sends while the endpoint is armed and the packet is due;
// the main loop calls Caw_try_receive; the clock moves while it has nothing
// to do (so packets with no gap arrive back to back)
static uint32_t rx_disarmed = 0; // times the device made the host wait
static void run_new( repl_t* m )
{
    repl_reset( m );
    int i = 0;
    int idle = 0;
    uint32_t due = now_ms + (pk_n ? pk_gap[0] : 0);
    uint32_t calls = 0;
    while( calls++ < 50000000 ){
        // the host is quicker than the main loop: up to 4 packets per pass
        for( int burst=0; burst<4 && i < pk_n && now_ms >= due; burst++ ){
            if( !rx_armed ){ rx_disarmed++; break; }
            host_send( pk_data[i], pk_len[i] );
            i++;
            if( i < pk_n ){ due = now_ms + pk_gap[i]; }
        }
        uint32_t before = tm( TM_rx_overflow );
        C_cmd_t c = Caw_try_receive();
        if( tm( TM_rx_overflow ) != before ){ log_put( &m->log, 'O', "", 0 ); }
        if( c == C_repl ){
            char* b = Caw_get_read();
            uint32_t l = Caw_get_read_len();
            model( m, c, b, l );
        } else {
            model( m, c, NULL, 0 );
        }
        if( c == C_none ){
            if( i >= pk_n && !USB_rx_pending() ){
                if( ++idle > 10 ){ break; }
            }
            now_ms++;
        } else {
            idle = 0;
        }
    }
    CHECK( i == pk_n );
}

// a fresh device and parser (the parser's state is file-local in caw.c,
// which this file includes)
static void rx_reset( void )
{
    reset_all();
    connect_now();
    pReader = 0;
    pkt_len = pkt_pos = 0;
    pkt_gap = 0;
    pkt_fresh = pkt_drop = 0;
    cmd_eol = 0;
    pkt_kept = 0;
    carets = carets_kept = carets_drop = 0;
    multiline = 0;
    upload = 0;
    pending = C_none;
    read_len = consumed = 0;
    rx_disarmed = 0;
}

// the old parser keeps `multiline` in a function static: give it a clean one
static void old_reset( void ){ o_reset = 1; }

/////////////////////////////////////////////////////
// RX: differential against the old parser, on input the old one handled

// text that is plain to both parsers: no "^^", no control characters, no
// leading ``` or ESC
static size_t gen_text( char* out, size_t len )
{
    static const char alphabet[] =
        "abcdefghijklmnopqrstuvwxyz ABCDEFG0123456789=+-*/()[]{}.,:;'\"`~#%&_<>?!^";
    for( size_t i=0; i<len; i++ ){
        char c = alphabet[rnd() % (sizeof alphabet - 1)];
        if( c == '^' && i && out[i-1] == '^' ){ c = 'x'; }
        if( c == '`' && i && out[i-1] == '`' ){ c = 'y'; } // no ``` anywhere
        out[i] = c;
    }
    // nor a "^^" made of two writes, which the old parser did not see
    if( len && out[0] == '^' ){ out[0] = 'z'; }
    if( len && out[len-1] == '^' ){ out[len-1] = 'z'; }
    return len;
}

static const char* line_end( void )
{
    switch( rnd() % 3 ){
        case 0:  return "\n";
        case 1:  return "\r\n";
        default: return "\r";
    }
}

// cut one write at random places, never right after a line ending (a packet
// that ends a line inside a write is the case the old parser got wrong)
static uint8_t is_term( char ch ){ return ch == '\n' || ch == '\r'; }
static void write_cut( const char* d, size_t len, uint32_t gap_write, uint32_t gap_in )
{
    int first = 1;
    while( len ){
        uint32_t n = 1 + rnd() % 64;
        if( n > len ){ n = len; }
        while( n < len && n > 1 && is_term( d[n-1] ) ){ n--; }
        while( n < len && n < 64 && is_term( d[n-1] ) ){ n++; }
        pk_add( (const uint8_t*)d, n, first ? gap_write : gap_in );
        first = 0;
        d += n;
        len -= n;
    }
}

// druid's upload: ^^s, 200 ms, each line + \r\n 1 ms apart (a 64-byte
// multiple gets an extra \n), 100 ms, ^^e or ^^w
static void druid_line( const char* s, size_t len, uint32_t gap, uint32_t gap_in )
{
    char b[512];
    memcpy( b, s, len );
    memcpy( &b[len], "\r\n", 2 );
    len += 2;
    if( len % 64 == 0 ){ b[len++] = '\n'; }
    write_sized( b, len, 64, gap, gap_in );
}

static void gen_druid_upload( size_t lines, int flash, uint32_t gap_in, char* script, size_t* slen )
{
    write_str( "^^s", 5 );
    *slen = 0;
    for( size_t l=0; l<lines; l++ ){
        char t[300];
        size_t n = (rnd() % 8) ? rnd() % 70 : rnd() % 250;
        gen_text( t, n );
        while( n && t[n-1] == ' ' ){ n--; } // druid strips the line
        druid_line( t, n, l ? 3 : 200, gap_in );
        memcpy( &script[*slen], t, n );
        *slen += n;
        memcpy( &script[*slen], "\r\n", 2 );
        *slen += 2;
        if( (n + 2) % 64 == 0 ){ script[(*slen)++] = '\n'; }
    }
    write_str( flash ? "^^w" : "^^e", 100 );
}

static const char cmd_letters[] = "bcrpvikfF";

// a random session of writes the old parser understood, every packet given
// time to be handled before the next (PACED ms between writes, gap_in between
// the packets of one write: the old parser never saw time at all)
#define PACED 5
static void gen_legacy_session( int writes, uint32_t gap_in )
{
    int ml = 0; // inside a ``` block
    for( int w=0; w<writes; w++ ){
        char t[600];
        size_t n;
        uint32_t r = rnd() % 100;
        if( r < 30 ){ // a line, cut by the host into 64s
            n = gen_text( t, rnd() % 150 );
            strcpy( &t[n], line_end() );
            if( strlen(t) % 64 == 1 && n % 64 == 63 ){ // "\r" | "\n": the old
                strcpy( &t[n], "\n" );                 // parser's failure case
            }
            write_sized( t, strlen(t), 64, PACED, gap_in );
        } else if( r < 45 ){ // several lines in one write, cut anywhere but after a line ending
            n = 0;
            int lines = 1 + rnd() % 6;
            for( int l=0; l<lines; l++ ){
                n += gen_text( &t[n], rnd() % 80 );
                const char* e = line_end();
                strcpy( &t[n], e );
                n += strlen(e);
            }
            write_cut( t, n, PACED, gap_in );
        } else if( r < 55 ){ // text with no line ending yet
            n = gen_text( t, 1 + rnd() % 40 );
            write_cut( t, n, PACED, gap_in );
        } else if( r < 70 ){ // a command alone, with or without a line ending
            t[0] = '^'; t[1] = '^';
            t[2] = cmd_letters[rnd() % (sizeof cmd_letters - 1)];
            t[3] = 0;
            if( rnd() % 2 ){ strcat( t, line_end() ); }
            write_str( t, PACED );
        } else if( r < 78 ){ // a fence alone, maybe with a first line
            switch( rnd() % 3 ){
                case 0: strcpy( t, "```" ); break;
                case 1: strcpy( t, "```\n" ); break;
                default:
                    strcpy( t, "```\n" );
                    n = gen_text( &t[4], rnd() % 40 );
                    strcpy( &t[4 + n], "\n" );
                    break;
            }
            write_str( t, PACED );
            ml ^= 1;
        } else if( r < 84 ){ // escape, outside a ``` block
            if( ml ){ continue; }
            switch( rnd() % 3 ){
                case 0:  write_str( "\x1b", PACED ); break;
                case 1:  write_str( "\x1b[A", PACED ); break;
                default: write_str( "\x1b[B", PACED ); break;
            }
        } else if( r < 88 ){ // an empty line
            write_str( line_end(), PACED );
        } else { // a druid upload
            if( ml ){ continue; }
            static char script[1 << 16];
            size_t slen;
            gen_druid_upload( 1 + rnd() % 30, rnd() % 2, gap_in, script, &slen );
        }
    }
}

static void test_rx_legacy_cases( void )
{
    printf("rx: fixed legacy-shaped sessions match the old parser\n");
    const char* cases[][8] = {
        { "print(1)\n", "x = 3\r\n", "print(x)\r", NULL },
        { "^^v", "^^i\n", "^^k\r\n", "print(2)\n", NULL },
        { "x = ", "^^v", "1\n", NULL },            // a command between halves
        { "```\n", "a = 1\n", "b = 2\n", "```\n", NULL },
        { "```", "a = 1\n", "```", NULL },
        { "```\nprint(5)\n", "```\n", NULL },
        { "abc", "\x1b", "print(3)\n", NULL },
        { "```\n", "^^p", "x=1\n", "```\n", NULL }, // a command inside a block
        { "\n", "\r", "\r\n", NULL },
        { NULL }
    };
    for( int k=0; cases[k][0]; k++ ){
        rx_reset();
        old_reset();
        pk_reset();
        for( int j=0; cases[k][j]; j++ ){ write_str( cases[k][j], PACED ); }
        run_old( &m_old );
        run_new( &m_new );
        char what[32];
        snprintf( what, sizeof what, "case %d", k );
        CHECK( logs_equal( &m_old.log, &m_new.log, what ) );
    }
}

// how many of each kind of entry a log holds
static void log_count( log_t* l, uint32_t counts[4] )
{
    size_t p = 0;
    while( p < l->len ){
        char type = l->buf[p++];
        size_t len = strtoul( (char*)&l->buf[p], NULL, 10 );
        while( l->buf[p] != ':' ){ p++; }
        p += 1 + len + 1;
        switch( type ){
            case 'E': counts[0]++; break;
            case 'C': counts[1]++; break;
            case 'S': counts[2]++; break;
            case 'O': counts[3]++; break;
        }
    }
}

static void test_rx_legacy_random( void )
{
    printf("rx: random legacy-shaped sessions match the old parser\n");
    int sessions = 3000;
    uint32_t counts[4] = {0};
    for( int k=0; k<sessions; k++ ){
        rx_reset();
        old_reset();
        pk_reset();
        gen_legacy_session( 40, (k % 2) ? PACED : 0 );
        run_old( &m_old );
        run_new( &m_new );
        char what[32];
        snprintf( what, sizeof what, "session %d", k );
        if( !logs_equal( &m_old.log, &m_new.log, what ) ){ CHECK( 0 ); break; }
        log_count( &m_new.log, counts );
    }
    printf("  %u chunks, %u commands, %u uploads, %u overflows\n"
          , counts[0], counts[1], counts[2], counts[3]);
}

static void test_rx_druid_upload( void )
{
    printf("rx: druid upload (paced) stores what it stored before\n");
    for( int k=0; k<40; k++ ){
        rx_reset();
        old_reset();
        pk_reset();
        static char script[1 << 16];
        size_t slen;
        gen_druid_upload( 200, k % 2, 0, script, &slen );
        run_old( &m_old );
        run_new( &m_new );
        CHECK( logs_equal( &m_old.log, &m_new.log, "druid upload" ) );
        repl_reset( &m_want );
        want_cmd( C_startupload );
        model( &m_want, C_repl, script, slen );
        want_cmd( (k % 2) ? C_flashupload : C_endupload );
        CHECK( logs_equal( &m_want.log, &m_new.log, "druid script" ) );
    }
}

/////////////////////////////////////////////////////
// RX: what is new

static void run_and_expect( const char* what )
{
    run_new( &m_new );
    CHECK( logs_equal( &m_want.log, &m_new.log, what ) );
}

// ^^s, a 15 kB script and ^^w in one write, cut every way
static void test_rx_upload_one_write( void )
{
    printf("rx: an upload in one write, at every packet size and many random cuts\n");
    static char script[16000];
    size_t slen = 0;
    while( slen < 15000 ){
        size_t n = rnd() % 90;
        gen_text( &script[slen], n );
        slen += n;
        memcpy( &script[slen], "\r\n", 2 );
        slen += 2;
    }
    memcpy( &script[slen], "end", 3 ); // unterminated last line
    slen += 3;
    static char w[16100];
    size_t wl = 0;
    memcpy( &w[wl], "^^s\n", 4 ); wl += 4;
    memcpy( &w[wl], script, slen ); wl += slen;
    memcpy( &w[wl], "^^w", 3 ); wl += 3;

    repl_reset( &m_want );
    want_cmd( C_startupload );
    model( &m_want, C_repl, script, slen );
    want_cmd( C_flashupload );

    int bad = 0;
    for( uint32_t size=1; size<=64 && !bad; size++ ){
        rx_reset();
        pk_reset();
        write_sized( w, wl, size, 0, 0 );
        run_new( &m_new );
        if( !logs_equal( &m_want.log, &m_new.log, "fixed size" ) ){
            printf("  packet size %u\n", size);
            bad = 1;
        }
    }
    CHECK( rx_disarmed > 0 ); // the host was made to wait
    for( int k=0; k<3000 && !bad; k++ ){
        rx_reset();
        pk_reset();
        size_t p = 0;
        while( p < wl ){
            uint32_t n = 1 + rnd() % 64;
            if( rnd() % 4 == 0 ){ n = 64; }
            if( n > wl - p ){ n = wl - p; }
            pk_add( (uint8_t*)&w[p], n, (rnd() % 8) ? 0 : rnd() % 3 ); // one write
            p += n;
        }
        run_new( &m_new );
        if( !logs_equal( &m_want.log, &m_new.log, "random cut" ) ){
            printf("  random cut %d\n", k);
            bad = 1;
        }
    }
    CHECK( !bad );
    // and \r\n after the ^^s
    rx_reset();
    pk_reset();
    write_str( "^^s\r\nx = 1\n^^e", 0 );
    repl_reset( &m_want );
    want_cmd( C_startupload );
    model( &m_want, C_repl, "x = 1\n", 6 );
    want_cmd( C_endupload );
    run_and_expect( "crlf after ^^s" );
}

static void test_rx_split_commands( void )
{
    printf("rx: commands split across packets\n");
    const char* splits[][4] = {
        { "^", "^v", NULL },
        { "^^", "v", NULL },
        { "^", "^", "v", NULL },
        { "print(1)\n^", "^i\n", NULL },
    };
    C_cmd_t cmds[] = { C_version, C_version, C_version, C_identity };
    for( int k=0; k<4; k++ ){
        rx_reset();
        pk_reset();
        for( int j=0; splits[k][j]; j++ ){ write_str( splits[k][j], 1 ); }
        repl_reset( &m_want );
        if( k == 3 ){ want_eval( "print(1)\n" ); }
        want_cmd( cmds[k] );
        run_and_expect( "split command" );
    }
    // carets that turn out to be text
    rx_reset();
    pk_reset();
    write_sized( "x = 2^", 6, 64, 0, 0 );
    write_sized( "3\n", 2, 64, 0, 0 );
    write_sized( "y = '^^", 7, 64, 3, 0 );
    write_sized( "'\n", 2, 64, 0, 0 );
    repl_reset( &m_want );
    want_eval( "x = 2^3\n" );
    want_eval( "y = '^^'\n" );
    run_and_expect( "held carets are text" );
    // and ESC after held carets clears them with the rest
    rx_reset();
    pk_reset();
    write_str( "abc^", 0 );
    write_str( "\x1b", 3 );
    write_str( "ok\n", 3 );
    repl_reset( &m_want );
    want_eval( "ok\n" );
    run_and_expect( "ESC after held caret" );
    // carets at the end of an ESC packet: a command if one follows, else gone
    rx_reset();
    pk_reset();
    write_str( "\x1b[A^", 0 );
    write_str( "^v", 0 );
    write_str( "\x1b^", 5 );
    write_str( "x\n", 5 );
    repl_reset( &m_want );
    want_cmd( C_version );
    want_eval( "x\n" );
    run_and_expect( "carets after ESC" );
}

static void test_rx_command_line_end( void )
{
    printf("rx: a command's line ending split off into the next packet\n");
    // the same write: dropped with the command
    rx_reset();
    pk_reset();
    pk_add( (uint8_t*)"^^v\r", 4, 0 );
    pk_add( (uint8_t*)"\nprint(1)\n", 10, 0 );
    pk_add( (uint8_t*)"^^i", 3, 5 );
    pk_add( (uint8_t*)"\r\nx\n", 4, 1 );
    repl_reset( &m_want );
    want_cmd( C_version );
    want_eval( "print(1)\n" );
    want_cmd( C_identity );
    want_eval( "x\n" );
    run_and_expect( "split line ending" );
    // a later write is not part of the command: druid's ^^s, 200 ms, then an
    // empty first line keeps that line
    rx_reset();
    pk_reset();
    write_str( "^^s", 0 );
    write_str( "\r\n", 200 );
    write_str( "x = 1\r\n", 1 );
    write_str( "^^e", 100 );
    repl_reset( &m_want );
    want_cmd( C_startupload );
    model( &m_want, C_repl, "\r\nx = 1\r\n", 9 );
    want_cmd( C_endupload );
    run_and_expect( "druid empty first line" );
}

static void test_rx_shared_packet( void )
{
    printf("rx: text sharing a packet with a command\n");
    rx_reset();
    pk_reset();
    write_str( "print(1)\n^^v\nprint(2)\n", 0 );
    write_str( "x=1^^i", 3 );
    write_str( "^^k junk to drop\r\ny = 2\n", 3 );
    write_str( "^^p rest of the packet is dropped", 3 );
    write_str( "z = 3\n", 3 );
    repl_reset( &m_want );
    want_eval( "print(1)\n" );
    want_cmd( C_version );
    want_eval( "print(2)\n" );
    // "x=1" was cut short by the command: discarded, as the old parser did
    want_cmd( C_identity );
    want_cmd( C_killlua );
    want_eval( "y = 2\n" );
    want_cmd( C_print );
    want_eval( "z = 3\n" );
    run_and_expect( "shared packet" );
    // inside an upload: text before and after
    rx_reset();
    pk_reset();
    write_str( "^^s\nline one\nlast^^e\nprint(9)\n", 0 );
    repl_reset( &m_want );
    want_cmd( C_startupload );
    model( &m_want, C_repl, "line one\nlast", 13 );
    want_cmd( C_endupload );
    want_eval( "print(9)\n" );
    run_and_expect( "upload in one packet" );
}

static void test_rx_continuation( void )
{
    printf("rx: a full packet ending a line waits for the rest of the write\n");
    char a[64], b[80];
    memset( a, 'a', 63 ); a[63] = '\n';
    // continued straight away: one chunk
    rx_reset();
    pk_reset();
    char w[100];
    memcpy( w, a, 64 );
    memcpy( &w[64], "b = 2\n", 6 );
    write_sized( w, 70, 64, 0, 0 );
    repl_reset( &m_want );
    memcpy( b, w, 70 ); b[70] = 0;
    want_eval( b );
    run_and_expect( "continued" );
    // continued 2 ms later: still the same write
    rx_reset();
    pk_reset();
    pk_add( (uint8_t*)a, 64, 0 );
    pk_add( (uint8_t*)"b = 2\n", 6, 2 );
    run_and_expect( "continued after 2ms" );
    // nothing follows: evaluated once more than CAW_CONTINUE_MS has passed
    rx_reset();
    host_send( (uint8_t*)a, 64 );
    uint32_t t0 = now_ms;
    CHECK( Caw_try_receive() == C_none );
    now_ms = t0 + 2;
    CHECK( Caw_try_receive() == C_none );
    now_ms = t0 + 3;
    CHECK( Caw_try_receive() == C_repl );
    CHECK( Caw_get_read_len() == 64 );
    // a packet queued behind it, but which came later, is a new write
    rx_reset();
    host_send( (uint8_t*)a, 64 );
    now_ms += 10; // the main loop was busy
    host_send( (uint8_t*)"b = 2\n", 6 );
    repl_reset( &m_new );
    for( int k=0; k<10; k++ ){
        C_cmd_t c = Caw_try_receive();
        if( c == C_repl ){ uint32_t l = Caw_get_read_len(); model( &m_new, c, Caw_get_read(), l ); }
    }
    repl_reset( &m_want );
    memcpy( b, a, 64 ); b[64] = 0;
    want_eval( b );
    want_eval( "b = 2\n" );
    CHECK( logs_equal( &m_want.log, &m_new.log, "late packet" ) );
    // a later write is separate
    rx_reset();
    pk_reset();
    pk_add( (uint8_t*)a, 64, 0 );
    pk_add( (uint8_t*)"b = 2\n", 6, 5 );
    repl_reset( &m_want );
    memcpy( b, a, 64 ); b[64] = 0;
    want_eval( b );
    want_eval( "b = 2\n" );
    run_and_expect( "separate writes" );
    // the next packet starts afresh: the waiting chunk goes first
    const char* afresh[] = { "\x1b", "```\n", "^^v" };
    for( int k=0; k<3; k++ ){
        rx_reset();
        pk_reset();
        pk_add( (uint8_t*)a, 64, 0 );
        pk_add( (uint8_t*)afresh[k], strlen(afresh[k]), 0 );
        repl_reset( &m_want );
        want_eval( b );
        if( k == 2 ){ want_cmd( C_version ); }
        run_and_expect( "waiting chunk then a fresh start" );
    }
    // druid's 64-byte line plus the extra \n it adds: one chunk
    rx_reset();
    pk_reset();
    char d[65];
    memset( d, 'd', 62 ); d[62] = '\r'; d[63] = '\n'; d[64] = '\n';
    write_sized( d, 65, 64, 0, 0 );
    repl_reset( &m_want );
    model( &m_want, C_repl, d, 65 );
    run_and_expect( "druid 64" );
    // and a multi-line write whose packet boundary falls after a newline
    rx_reset();
    pk_reset();
    char ml[200];
    size_t n = 0;
    memset( ml, ' ', 63 ); memcpy( ml, "input[1].change = function()", 28 ); ml[63] = '\n';
    n = 64;
    const char* rest = "  tell('change',1,1)\nend\ninput[1].mode('change',2,0.1,'rising')\n";
    memcpy( &ml[n], rest, strlen(rest) ); n += strlen(rest);
    write_sized( ml, n, 64, 0, 0 );
    repl_reset( &m_want );
    model( &m_want, C_repl, ml, n );
    run_and_expect( "multi-line write" );
}

static void test_rx_overflow( void )
{
    printf("rx: overflow evaluates up to the last line ending\n");
    static char w[6000];
    size_t n = 0;
    while( n < 5000 ){
        n += gen_text( &w[n], 10 + rnd() % 60 );
        w[n++] = '\n';
    }
    rx_reset();
    pk_reset();
    write_sized( w, n, 64, 0, 0 );
    run_new( &m_new );
    // the chunks, joined, are the write; each ends a line; none too long
    log_t* l = &m_new.log;
    size_t p = 0, got = 0;
    int chunks = 0, ok = 1;
    static char joined[6000];
    while( p < l->len && ok ){
        char type = l->buf[p++];
        size_t len = strtoul( (char*)&l->buf[p], NULL, 10 );
        while( l->buf[p] != ':' ){ p++; }
        p++;
        if( type != 'E' || len >= 2048 || l->buf[p + len - 1] != '\n' ){ ok = 0; }
        memcpy( &joined[got], &l->buf[p], len );
        got += len;
        p += len + 1;
        chunks++;
    }
    CHECK( ok );
    CHECK( chunks >= 3 );
    CHECK( got == n && !memcmp( joined, w, n ) );
    CHECK( tm( TM_rx_overflow ) == 0 );
    // no line ending at all: dropped, as before (and the same as the old parser)
    rx_reset();
    old_reset();
    pk_reset();
    memset( w, 'x', 3000 );
    write_sized( w, 3000, 64, 0, 0 );
    write_str( "print(1)\n", PACED );
    repl_reset( &m_want );
    want_overflow(); // the 32nd packet does not fit: it and the 31 before go
    memset( w, 'x', 952 ); // the rest of the write is kept
    strcpy( &w[952], "print(1)\n" );
    want_eval( w );
    run_and_expect( "overflow without a line ending" );
    CHECK( tm( TM_rx_overflow ) == 1 );
    run_old( &m_old );
    CHECK( logs_equal( &m_old.log, &m_new.log, "overflow vs old" ) );
}

static void test_rx_overflow_multiline( void )
{
    printf("rx: overflow inside a ``` block discards, as before\n");
    static char w[3000];
    rx_reset();
    old_reset();
    pk_reset();
    write_str( "```\n", PACED );
    for( int l=0; l<60; l++ ){
        size_t n = gen_text( w, 40 );
        strcpy( &w[n], "\n" );
        write_str( w, PACED );
    }
    write_str( "```\n", PACED );
    write_str( "print(1)\n", PACED );
    run_old( &m_old );
    run_new( &m_new );
    CHECK( logs_equal( &m_old.log, &m_new.log, "multiline overflow" ) );
    CHECK( tm( TM_rx_overflow ) == 1 );
}

static void test_rx_escape( void )
{
    printf("rx: ESC also clears an unclosed ``` block\n");
    rx_reset();
    pk_reset();
    write_str( "```\n", 3 );
    write_str( "a = 1\n", 3 );
    write_str( "\x1b", 3 ); // the closing fence was lost
    write_str( "print(2)\n", 3 );
    repl_reset( &m_want );
    want_eval( "print(2)\n" );
    run_and_expect( "escape" );
}

static void test_rx_ring( void )
{
    printf("rx: the endpoint NAKs while the ring is full, ZLPs ignored, reset on configure\n");
    rx_reset();
    uint8_t p[64];
    memset( p, 'x', 64 );
    int sent = 0;
    while( rx_armed && sent < 100 ){ host_send( p, 64 ); sent++; }
    CHECK( sent == 2048 / 66 ); // only while another full packet fits
    CHECK( !rx_armed );
    uint8_t got[64], gap;
    CHECK( USB_rx_dequeue( got, &gap ) == 64 );
    CHECK( rx_armed ); // room again
    host_send( p, 0 ); // zero-length
    CHECK( rx_armed );
    int n = 0;
    while( USB_rx_dequeue( got, &gap ) ){ n++; }
    CHECK( n == sent - 1 ); // nothing lost, no empty packet
    host_send( (uint8_t*)"abc", 3 );
    usb_disconnect();
    usb_configure();
    CHECK( !USB_rx_pending() );
}

static void run_rx_tests( void )
{
    test_rx_legacy_cases();
    test_rx_legacy_random();
    test_rx_druid_upload();
    test_rx_upload_one_write();
    test_rx_split_commands();
    test_rx_shared_packet();
    test_rx_continuation();
    test_rx_overflow();
    test_rx_overflow_multiline();
    test_rx_escape();
    test_rx_command_line_end();
    test_rx_ring();
}

int main( void )
{
    setvbuf( stdout, NULL, _IONBF, 0 );
    run_tx_tests();
    run_rx_tests();
    if( failures ){
        printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
