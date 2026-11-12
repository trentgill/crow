// Host-side tests for the USB serial transport: the real lib/caw.c and
// usbd/usbd_cdc_interface.c, compiled against tests/host/stub/ with a fake
// USB device library, a fake clock and a fake host on the far end.
// Build and run with tests/host/run_caw.sh.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../../lib/caw.h"
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

int main( void )
{
    setvbuf( stdout, NULL, _IONBF, 0 );
    run_tx_tests();
    if( failures ){
        printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
