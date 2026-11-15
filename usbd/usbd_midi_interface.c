#include "usbd_midi_interface.h"

#include "usbd_main.h"
#include "../ll/interrupts.h" // BLOCK_IRQS
#include "../lib/midi.h"      // MIDI_receive()
#include "usbd_map.h"          // SRAM_NOINIT

// USB MIDI moves 4-byte event packets: a cable/code-index byte, then up to 3
// MIDI bytes. Both directions keep whole packets in a ring.
//
// RX: the OUT endpoint is re-armed only while a full USB packet (16 events)
// fits, so the host waits rather than losing messages.
// TX: messages queue as events and go out in chained IN transfers, like the
// serial transmit ring.
#define EV_SIZE       4
#define RX_EVENTS     128 // power of two
#define TX_EVENTS     128 // power of two
#define PKT_EVENTS    (MIDI_EPOUT_SIZE / EV_SIZE)
#define TX_STALL_MS   100 // nobody is reading: drop what's queued

SRAM_NOINIT static uint8_t rx_packet[MIDI_EPOUT_SIZE];
SRAM_NOINIT static uint8_t rx_ring[RX_EVENTS][EV_SIZE];
static volatile uint32_t rx_head = 0;
static volatile uint32_t rx_tail = 0;
static volatile uint8_t  rx_stalled = 0;

SRAM_NOINIT static uint8_t tx_ring[TX_EVENTS][EV_SIZE];
SRAM_NOINIT static uint8_t tx_packet[MIDI_EPIN_SIZE]; // the transfer on the wire
static volatile uint32_t tx_head = 0;
static volatile uint32_t tx_tail = 0;
static volatile uint32_t tx_in_flight = 0; // events in the transfer on the wire
static volatile uint32_t tx_started = 0;

static volatile uint8_t midi_open = 0;

extern USBD_HandleTypeDef USBD_Device;

static int8_t MIDI_Itf_Init( void );
static int8_t MIDI_Itf_DeInit( void );
static int8_t MIDI_Itf_Receive( uint8_t* buf, uint32_t* len );
static int8_t MIDI_Itf_TransmitCplt( uint8_t* buf, uint32_t* len, uint8_t epnum );

USBD_MIDI_ItfTypeDef USBD_MIDI_fops = { MIDI_Itf_Init
                                      , MIDI_Itf_DeInit
                                      , MIDI_Itf_Receive
                                      , MIDI_Itf_TransmitCplt
                                      };

// arm the OUT endpoint if a full packet of events fits, else leave it NAKing
// caller has interrupts blocked, or is the OTG interrupt
static void rx_arm_locked( void )
{
    if( (RX_EVENTS - (rx_head - rx_tail)) >= PKT_EVENTS ){
        rx_stalled = 0;
        USBD_MIDI_ReceivePacket( &USBD_Device );
    } else {
        rx_stalled = 1;
    }
}

static int8_t MIDI_Itf_Init( void )
{
    BLOCK_IRQS(
        rx_head = rx_tail = 0;
        tx_head = tx_tail = 0;
        tx_in_flight = 0;
        midi_open = 1;
        USBD_MIDI_SetRxBuffer( &USBD_Device, rx_packet );
        rx_arm_locked();
    );
    return USBD_OK;
}

static int8_t MIDI_Itf_DeInit( void )
{
    BLOCK_IRQS(
        midi_open = 0;
        rx_stalled = 0;
        tx_in_flight = 0;
    );
    return USBD_OK;
}

static int8_t MIDI_Itf_Receive( uint8_t* buf, uint32_t* len )
{
    for( uint32_t i=0; i+EV_SIZE <= *len; i += EV_SIZE ){
        if( (buf[i] & 0x0F) == 0 ){ continue; } // CIN 0: reserved / padding
        uint8_t* ev = rx_ring[rx_head & (RX_EVENTS-1)];
        ev[0] = buf[i]; ev[1] = buf[i+1]; ev[2] = buf[i+2]; ev[3] = buf[i+3];
        rx_head++; // room was checked before the endpoint was armed
    }
    rx_arm_locked();
    return USBD_OK;
}

// TRANSMIT

// caller has interrupts blocked, or is the OTG interrupt
static void tx_kick_locked( void )
{
    if( tx_in_flight || !midi_open
     || USBD_Device.dev_state != USBD_STATE_CONFIGURED ){ return; }
    uint32_t n = tx_head - tx_tail;
    if( n == 0 ){ return; }
    if( n > MIDI_EPIN_SIZE/EV_SIZE ){ n = MIDI_EPIN_SIZE/EV_SIZE; }
    for( uint32_t i=0; i<n; i++ ){
        memcpy( &tx_packet[i*EV_SIZE], tx_ring[(tx_tail + i) & (TX_EVENTS-1)], EV_SIZE );
    }
    tx_in_flight = n;
    tx_started = HAL_GetTick();
    USBD_MIDI_SetTxBuffer( &USBD_Device, tx_packet, n*EV_SIZE );
    if( USBD_MIDI_TransmitPacket( &USBD_Device ) != USBD_OK ){
        tx_in_flight = 0;
    }
}

static int8_t MIDI_Itf_TransmitCplt( uint8_t* buf, uint32_t* len, uint8_t epnum )
{
    UNUSED(buf); UNUSED(len); UNUSED(epnum);
    BLOCK_IRQS(
        tx_tail += tx_in_flight;
        tx_in_flight = 0;
        tx_kick_locked();
    );
    return USBD_OK;
}

// code index number for a message, from its status byte
static uint8_t cin_of( uint8_t status, uint8_t len )
{
    if( status >= 0xF8 ){ return 0x0F; } // realtime: single byte
    if( status < 0xF0 ){ return status >> 4; } // channel voice
    switch( status ){
        case 0xF1: case 0xF3: return 0x02; // 2-byte system common
        case 0xF2:            return 0x03; // 3-byte system common
        default:              return (len == 1) ? 0x05 : 0x0F;
    }
}

uint8_t USB_midi_send( const uint8_t* msg, uint8_t len )
{
    if( len == 0 || len > 3 || !(msg[0] & 0x80) ){ return 0; }
    uint8_t queued = 0;
    BLOCK_IRQS(
        if( midi_open && (tx_head - tx_tail) < TX_EVENTS ){
            uint8_t* ev = tx_ring[tx_head & (TX_EVENTS-1)];
            ev[0] = cin_of( msg[0], len ); // cable 0
            ev[1] = msg[0];
            ev[2] = (len > 1) ? msg[1] : 0;
            ev[3] = (len > 2) ? msg[2] : 0;
            tx_head++;
            queued = 1;
        }
        // only thread mode starts a transfer: an interrupt producer must not
        // preempt the OTG interrupt half way through the endpoint bookkeeping
        if( queued && __get_IPSR() == 0 ){ tx_kick_locked(); }
    );
    return queued;
}

// bytes in a message by its code index number
static const uint8_t cin_len[16] = { 0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1 };

// messages handled per call, so a burst can't starve the rest of the main loop
#define PROCESS_MAX 16

void USB_midi_process( void )
{
    uint8_t ev[EV_SIZE];
    for( int n=0; n<PROCESS_MAX; n++ ){
        uint8_t got = 0;
        BLOCK_IRQS(
            if( rx_head != rx_tail ){
                memcpy( ev, rx_ring[rx_tail & (RX_EVENTS-1)], EV_SIZE );
                rx_tail++;
                got = 1;
            }
            if( rx_stalled && midi_open ){ rx_arm_locked(); }
        );
        if( !got ){ break; }
        uint8_t len = cin_len[ev[0] & 0x0F];
        if( len ){ MIDI_receive( &ev[1], len ); }
    }
}

void USB_midi_tick( uint32_t now_ms )
{
    BLOCK_IRQS(
        if( tx_in_flight && (now_ms - tx_started) >= TX_STALL_MS ){
            // no host is reading (Linux only polls while a port is open).
            // the transfer on the wire stays, but stale messages queued behind
            // it are dropped, so an app that opens the port later doesn't get
            // a burst of old notes
            tx_head = tx_tail + tx_in_flight;
        }
        tx_kick_locked();
    );
}
