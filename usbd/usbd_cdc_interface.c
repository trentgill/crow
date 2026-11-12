/**
  ******************************************************************************
  * @file    USB_Device/CDC_Standalone/Src/usbd_cdc_interface.c
  * @author  MCD Application Team
  * @version V1.2.1
  * @date    14-April-2017
  * @brief   Source file for USBD CDC interface
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; Copyright (c) 2017 STMicroelectronics International N.V. 
  * All rights reserved.</center></h2>
  *
  * Redistribution and use in source and binary forms, with or without 
  * modification, are permitted, provided that the following conditions are met:
  *
  * 1. Redistribution of source code must retain the above copyright notice, 
  *    this list of conditions and the following disclaimer.
  * 2. Redistributions in binary form must reproduce the above copyright notice,
  *    this list of conditions and the following disclaimer in the documentation
  *    and/or other materials provided with the distribution.
  * 3. Neither the name of STMicroelectronics nor the names of other 
  *    contributors to this software may be used to endorse or promote products 
  *    derived from this software without specific written permission.
  * 4. This software, including modifications and/or derivative works of this 
  *    software, must execute solely and exclusively on microcontroller or
  *    microprocessor devices manufactured by or for STMicroelectronics.
  * 5. Redistribution and use of this software other than as permitted under 
  *    this license is void and will automatically terminate your rights under 
  *    this license. 
  *
  * THIS SOFTWARE IS PROVIDED BY STMICROELECTRONICS AND CONTRIBUTORS "AS IS" 
  * AND ANY EXPRESS, IMPLIED OR STATUTORY WARRANTIES, INCLUDING, BUT NOT 
  * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A 
  * PARTICULAR PURPOSE AND NON-INFRINGEMENT OF THIRD PARTY INTELLECTUAL PROPERTY
  * RIGHTS ARE DISCLAIMED TO THE FULLEST EXTENT PERMITTED BY LAW. IN NO EVENT 
  * SHALL STMICROELECTRONICS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
  * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, 
  * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF 
  * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING 
  * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
  * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
  *
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "usbd_main.h"

#include "../lib/telemetry.h"
#include "../lib/lua_watchdog.h" // Lua_watchdog_exempt_end()

// Private define
#define CONNECTION_DELAY  500 // millisecond delay before sending buffer after connect

// RX is a ring of packets, each stored as a length byte, a gap byte (ms since
// the packet before it, or since the endpoint was re-armed after a stall: tells
// the parser which packets belong to one host write) and its bytes, so the
// parser still sees where every packet ended. The OUT endpoint is only re-armed
// while another full packet fits: otherwise the controller NAKs and the host's
// write waits, until the main loop has taken enough out.
#define RX_RING_SIZE      2048 // power of two: the indices below run freely
#define RX_PACKET         CDC_DATA_FS_OUT_PACKET_SIZE

// TX is one byte ring drained by a chain of IN transfers: each transfer's
// completion (CDC_Itf_TransmitCplt) starts the next, and data queued while the
// transmitter is idle is started by its producer, or by the main loop.
#define TX_RING_SIZE      4096 // power of two: the indices below run freely
#define TX_CHUNK_MAX      1024 // largest single IN transfer
// An IN transfer outstanding this long has nobody reading it. Linux does not
// poll the endpoint while the port is closed, so this is the only sign we get.
#define TX_STALL_MS        250
// A thread-mode producer facing a full ring waits for a host that is reading,
// but gives up after this long without a transfer completing.
#define TX_WAIT_MS          20

static int8_t CDC_Itf_Init(void);
static int8_t CDC_Itf_DeInit(void);
static int8_t CDC_Itf_Control(uint8_t cmd, uint8_t* pbuf, uint16_t length);
static int8_t CDC_Itf_Receive(uint8_t* pbuf, uint32_t *Len);
static int8_t CDC_Itf_TransmitCplt(uint8_t *pbuf, uint32_t *Len, uint8_t epnum);

USBD_CDC_ItfTypeDef USBD_CDC_fops = { CDC_Itf_Init
                                    , CDC_Itf_DeInit
                                    , CDC_Itf_Control
                                    , CDC_Itf_Receive
                                    , CDC_Itf_TransmitCplt
                                    };

// Private vars
USBD_CDC_LineCodingTypeDef LineCoding = { 115200  // baud rate
                                        , 0x00    // stop bits-1
                                        , 0x00    // parity - none
                                        , 0x08    // nb. of bits 8
                                        };

static uint8_t UserRxBuffer[RX_PACKET]; // where the endpoint receives

// Single producer (CDC_Itf_Receive, in the OTG interrupt), single consumer
// (USB_rx_dequeue, main loop, which blocks interrupts while it takes a packet).
static uint8_t           rx_ring[RX_RING_SIZE];
static volatile uint32_t rx_head = 0;     // where the next packet is written
static volatile uint32_t rx_tail = 0;     // the oldest packet
static volatile uint8_t  rx_stalled = 0;  // endpoint left un-armed: ring was full
static volatile uint32_t rx_last_tick = 0; // last arrival, or re-arm after a stall

// The TX state is shared by producers (thread mode, and interrupts up to the
// audio DMA at priority 5) and the consumer in the OTG interrupt (priority 6),
// so anything more than a single read of it happens inside BLOCK_IRQS.
// Transfers are only submitted from thread mode with interrupts blocked, or
// from the OTG interrupt itself: nothing can then preempt the HAL's unlocked
// read-modify-writes of DIEPEMPMSK, which is what used to leave TxState stuck.
static uint8_t           tx_ring[TX_RING_SIZE];
static volatile uint32_t tx_head = 0;      // where the next byte is written
static volatile uint32_t tx_tail = 0;      // oldest byte not yet delivered
static volatile uint32_t tx_in_flight = 0; // length of the transfer on the wire
static volatile uint8_t  tx_in_flight_stream = 0; // ..which came from the stream
static volatile uint32_t tx_started = 0;   // tick that transfer was submitted
static volatile uint32_t tx_progress = 0;  // counts completed transfers
static volatile uint8_t  tx_holdoff = 0;   // until CONNECTION_DELAY after configure
static volatile uint32_t tx_connect_tick = 0;

// One zero-copy stream of constant data. It goes out after the ring bytes
// queued before it (up to tx_stream_mark) and before any queued after it.
static volatile uint8_t        tx_streaming = 0;
static volatile uint32_t       tx_stream_mark = 0;
static const uint8_t* volatile tx_stream_ptr = NULL;
static volatile uint32_t       tx_stream_len = 0;

extern USBD_HandleTypeDef  USBD_Device;

/* Public functions ---------------------------------------------------------*/
void CDC_clear_buffers( void )
{
    BLOCK_IRQS(
        rx_head = 0;
        rx_tail = 0;
        tx_head = 0;
        tx_tail = 0;
        tx_streaming = 0;
        tx_stream_len = 0;
    );
}

#include "../ll/status_led.h" // increase LED blink speed when USB connected
/* Private functions ---------------------------------------------------------*/
static int8_t CDC_Itf_Init(void)
{
    USBD_CDC_SetRxBuffer(&USBD_Device, UserRxBuffer);

    // nothing is sent for CONNECTION_DELAY after the host configures us.
    // solves ECHO issue on norns. see #137
    // the ring itself is kept: output queued before any host was listening
    // (eg. the boot text after a software reset) goes out once one reads.
    BLOCK_IRQS(
        // a new host starts with an empty receive ring. the class arms the
        // endpoint itself straight after this returns
        rx_head = 0;
        rx_tail = 0;
        rx_stalled = 0;
        rx_last_tick = HAL_GetTick();

        tx_connect_tick = HAL_GetTick();
        tx_holdoff = 1;
        tx_in_flight = 0; // a fresh class handle has no transfer outstanding
        tx_in_flight_stream = 0;
    );

    //TODO add lua callback when USB connects/disconnects
    //     also provide a flag to check that status
    printf("USB_Init\n");
    status_led_fast(LED_FAST);

    return (USBD_OK);
}

static int8_t CDC_Itf_DeInit(void)
{
    // the endpoint is closed, so a transfer still on the wire will never
    // complete. its bytes stay queued and are sent on the next configure.
    BLOCK_IRQS(
        rx_stalled = 0; // the endpoint is closed. Init arms it again
        tx_in_flight = 0;
        tx_in_flight_stream = 0;
    );
    printf("USB_DeInit\n");
    status_led_fast(LED_SLOW);
    return (USBD_OK);
}

static int8_t CDC_Itf_Control (uint8_t cmd, uint8_t* pbuf, uint16_t length)
{
    // Most of this is unimplemented!
    switch( cmd ){
        case CDC_SEND_ENCAPSULATED_COMMAND: printf("itf:send_cmd\n");     break;
        case CDC_GET_ENCAPSULATED_RESPONSE: printf("itf:get_response\n"); break;
        case CDC_SET_COMM_FEATURE:          printf("itf:set_feature\n");  break;
        case CDC_GET_COMM_FEATURE:          printf("itf:get_feature\n");  break;
        case CDC_CLEAR_COMM_FEATURE:        printf("itf:clr_feature\n");  break;
        case CDC_SET_LINE_CODING:
            LineCoding.bitrate    = (uint32_t)(  pbuf[0]
                                              | (pbuf[1] << 8)
                                              | (pbuf[2] << 16)
                                              | (pbuf[3] << 24)
                                              );
            LineCoding.format     = pbuf[4];
            LineCoding.paritytype = pbuf[5];
            LineCoding.datatype   = pbuf[6];
            break;
        case CDC_GET_LINE_CODING:
            pbuf[0] = (uint8_t)(LineCoding.bitrate);
            pbuf[1] = (uint8_t)(LineCoding.bitrate >> 8);
            pbuf[2] = (uint8_t)(LineCoding.bitrate >> 16);
            pbuf[3] = (uint8_t)(LineCoding.bitrate >> 24);
            pbuf[4] = LineCoding.format;
            pbuf[5] = LineCoding.paritytype;
            pbuf[6] = LineCoding.datatype;
            break;
        case CDC_SET_CONTROL_LINE_STATE: break;
        case CDC_SEND_BREAK:             printf("itf:send_brk\n");     break;
        default: printf("itf: default\n"); break;
    }
    return (USBD_OK);
}

// TRANSMIT

// a transfer may be submitted: configured, and past the hold-off
// caller has interrupts blocked, or is the OTG interrupt
static uint8_t tx_link_ready( void )
{
    if( USBD_Device.pClassData == NULL
     || USBD_Device.dev_state != USBD_STATE_CONFIGURED ){ return 0; }
    if( tx_holdoff ){
        if( (HAL_GetTick() - tx_connect_tick) < CONNECTION_DELAY ){ return 0; }
        tx_holdoff = 0;
    }
    return 1;
}

// a host is reading, so a full ring will soon have room
static uint8_t tx_draining( void )
{
    uint8_t draining = 0;
    BLOCK_IRQS(
        draining = tx_link_ready()
                && !( tx_in_flight
                   && (HAL_GetTick() - tx_started) >= TX_STALL_MS );
    );
    return draining;
}

// only thread mode with interrupts enabled may wait: the OTG interrupt has to
// be able to run to make room
static uint8_t tx_can_wait( void )
{
    return (__get_IPSR() == 0) && (__get_PRIMASK() == 0);
}

// start the next transfer if the transmitter is idle
// caller has interrupts blocked, or is the OTG interrupt
static void tx_kick_locked( void )
{
    if( tx_in_flight || !tx_link_ready() ){ return; }

    const uint8_t* src = NULL;
    uint32_t len = 0;
    uint8_t from_stream = 0;
    uint32_t end = tx_head;
    if( tx_streaming ){
        if( tx_tail == tx_stream_mark ){ // everything queued ahead of it is out
            src = tx_stream_ptr;
            len = (tx_stream_len > TX_CHUNK_MAX) ? TX_CHUNK_MAX : tx_stream_len;
            from_stream = 1;
        } else {
            end = tx_stream_mark; // only what was queued before the stream
        }
    }
    if( !from_stream ){
        uint32_t count = end - tx_tail;
        uint32_t ix    = tx_tail & (TX_RING_SIZE-1);
        len = TX_RING_SIZE - ix; // contiguous up to the end of the ring
        if( len > count ){ len = count; }
        if( len > TX_CHUNK_MAX ){ len = TX_CHUNK_MAX; }
        src = &tx_ring[ix];
    }
    if( len == 0 ){ return; }

    // claimed before it is submitted: the completion can arrive as soon as the
    // transfer exists, and must find the bookkeeping already in place
    tx_in_flight = len;
    tx_in_flight_stream = from_stream;
    tx_started = HAL_GetTick();
    USBD_CDC_SetTxBuffer( &USBD_Device, (uint8_t*)src, len );
    if( USBD_CDC_TransmitPacket( &USBD_Device ) != USBD_OK ){
        tx_in_flight = 0; // nothing went out, so nothing will complete
        tx_in_flight_stream = 0;
    }
}

// start a transfer if idle. does nothing in interrupt context: an interrupt
// producer leaves it to the main loop (or the completion chain), so it can
// never preempt the OTG interrupt in the middle of the endpoint bookkeeping
void USB_tx_kick( void )
{
    if( __get_IPSR() != 0 ){ return; }
    BLOCK_IRQS(
        tx_kick_locked();
    );
}

static void tx_copy_locked( const uint8_t* buf, uint32_t len )
{
    while( len ){
        uint32_t ix = tx_head & (TX_RING_SIZE-1);
        uint32_t n  = TX_RING_SIZE - ix;
        if( n > len ){ n = len; }
        memcpy( &tx_ring[ix], buf, n );
        tx_head += n;
        buf     += n;
        len     -= n;
    }
}

// wait helper for thread-mode producers. returns 0 when waiting is over:
// not allowed, nobody reading, or TX_WAIT_MS without a transfer completing
static uint8_t tx_wait( uint32_t* progress, uint32_t* since )
{
    if( !tx_can_wait() || !tx_draining() ){ return 0; }
    USB_tx_kick();
    uint32_t now = HAL_GetTick();
    if( tx_progress != *progress ){ // a transfer completed: room is coming
        *progress = tx_progress;
        *since = now;
    } else if( (now - *since) >= TX_WAIT_MS ){ return 0; }
    return 1;
}

// a script made to wait for the host is not charged for that time
static void tx_waited( uint32_t since_tick )
{
    if( __get_IPSR() == 0 ){ Lua_watchdog_exempt_end( since_tick ); }
}

// producers in thread mode and in interrupts both count
static void tx_count( uint8_t queued, uint32_t len )
{
    BLOCK_IRQS(
        Telemetry_count( (queued ? TM_tx_bytes : TM_tx_dropped), len );
    );
}

// Queue one message made of up to three pieces (unused ones NULL, 0).
// It goes into the ring whole or not at all: when there is no room, a
// thread-mode caller waits for a host that is reading; otherwise the whole
// message is dropped and counted.
void USB_tx_enqueue( const uint8_t* lead, uint32_t lead_len
                   , const uint8_t* buf,  uint32_t len
                   , const uint8_t* tail, uint32_t tail_len )
{
    uint32_t total = lead_len + len + tail_len;
    if( total == 0 ){ return; }
    uint8_t  queued   = 0;
    uint32_t progress = tx_progress;
    uint32_t since    = HAL_GetTick();
    uint32_t entered  = since;
    while(1){
        BLOCK_IRQS(
            if( (TX_RING_SIZE - (tx_head - tx_tail)) >= total ){
                tx_copy_locked( lead, lead_len );
                tx_copy_locked( buf, len );
                tx_copy_locked( tail, tail_len );
                queued = 1;
            }
        );
        if( queued
         || total > TX_RING_SIZE // can never fit
         || !tx_wait( &progress, &since ) ){ break; }
    }
    tx_waited( entered );
    tx_count( queued, total );
    if( queued ){ USB_tx_kick(); }
}

// Send `len` bytes straight from `buf`, which must stay valid and unchanged
// until they have gone (flash, string constants). They follow everything
// already queued; `tail` is copied into the ring right behind them, ahead of
// anything queued later. One stream at a time: another waits for it the way a
// message waits for room, or is dropped whole.
void USB_tx_stream( const uint8_t* buf, uint32_t len
                  , const uint8_t* tail, uint32_t tail_len )
{
    if( len == 0 ){
        USB_tx_enqueue( NULL, 0, tail, tail_len, NULL, 0 );
        return;
    }
    uint8_t  queued   = 0;
    uint32_t progress = tx_progress;
    uint32_t since    = HAL_GetTick();
    uint32_t entered  = since;
    while(1){
        BLOCK_IRQS(
            if( !tx_streaming
             && (TX_RING_SIZE - (tx_head - tx_tail)) >= tail_len ){
                tx_stream_mark = tx_head;
                tx_copy_locked( tail, tail_len );
                tx_stream_ptr = buf;
                tx_stream_len = len;
                tx_streaming  = 1;
                queued = 1;
            }
        );
        if( queued
         || !tx_wait( &progress, &since ) ){ break; }
    }
    tx_waited( entered );
    tx_count( queued, len + tail_len );
    if( queued ){ USB_tx_kick(); }
}

// thread mode: wait for a running stream to finish, while the host reads it
void USB_tx_stream_wait( void )
{
    uint32_t progress = tx_progress;
    uint32_t since    = HAL_GetTick();
    uint32_t entered  = since;
    while( tx_streaming
        && tx_wait( &progress, &since ) ){}
    tx_waited( entered );
}

int USB_tx_is_ready( void )
{
    int ready = 0;
    BLOCK_IRQS(
        ready = (tx_head == tx_tail) // nothing queued
             && !tx_streaming        // nor streaming
             && !tx_in_flight;       // and no transfer outstanding
    );
    return ready;
}

static int8_t CDC_Itf_TransmitCplt(uint8_t *Buf, uint32_t *Len, uint8_t epnum)
{
    UNUSED(Buf);
    UNUSED(Len);
    UNUSED(epnum);

    BLOCK_IRQS(
        if( tx_in_flight ){ // delivered: release its bytes
            if( tx_in_flight_stream ){
                tx_stream_ptr += tx_in_flight;
                tx_stream_len -= tx_in_flight;
                if( tx_stream_len == 0 ){ tx_streaming = 0; }
            } else {
                tx_tail += tx_in_flight;
            }
            tx_in_flight = 0;
            tx_in_flight_stream = 0;
            tx_progress++;
        }
        tx_kick_locked(); // and start the next
    );
    return (0);
}

// RECEIVE

// arm the OUT endpoint if another full packet fits, else leave it NAKing
// caller has interrupts blocked, or is the OTG interrupt
static void rx_arm_locked( void )
{
    if( (RX_RING_SIZE - (rx_head - rx_tail)) >= 2 + RX_PACKET ){
        rx_stalled = 0;
        USBD_CDC_ReceivePacket( &USBD_Device );
    } else {
        rx_stalled = 1;
    }
}

// on interrupt add the packet to the ring
static int8_t CDC_Itf_Receive( uint8_t* buf, uint32_t *len )
{
    uint32_t length = *len;
    Telemetry_count( TM_rx_bytes, length );
    if( length > RX_PACKET ){ length = RX_PACKET; } // not at full speed
    if( length ){ // zero-length packets carry nothing
        // there is room: the endpoint was only armed with a packet's worth free
        uint32_t now = HAL_GetTick();
        uint32_t gap = now - rx_last_tick;
        rx_ring[rx_head & (RX_RING_SIZE-1)] = (uint8_t)length;
        rx_ring[(rx_head + 1) & (RX_RING_SIZE-1)] = (gap > 255) ? 255 : (uint8_t)gap;
        for( uint32_t i=0; i<length; i++ ){
            rx_ring[(rx_head + 2 + i) & (RX_RING_SIZE-1)] = buf[i];
        }
        rx_head += 2 + length;
        rx_last_tick = now;
    }
    rx_arm_locked();
    return USBD_OK;
}

// copy the oldest packet into buf (RX_PACKET bytes), and the ms between its
// arrival and the one before into *gap. returns its length, 0 if none
uint32_t USB_rx_dequeue( uint8_t* buf, uint8_t* gap )
{
    uint32_t len = 0;
    BLOCK_IRQS(
        if( rx_head != rx_tail ){
            len  = rx_ring[rx_tail & (RX_RING_SIZE-1)];
            *gap = rx_ring[(rx_tail + 1) & (RX_RING_SIZE-1)];
            for( uint32_t i=0; i<len; i++ ){
                buf[i] = rx_ring[(rx_tail + 2 + i) & (RX_RING_SIZE-1)];
            }
            rx_tail += 2 + len;
        }
    );
    USB_rx_rearm();
    return len;
}

// let the host send again once a full packet fits
void USB_rx_rearm( void )
{
    BLOCK_IRQS(
        if( rx_stalled && USBD_Device.pClassData != NULL ){
            rx_arm_locked();
            // the host could not send until now: count the wait from here
            if( !rx_stalled ){ rx_last_tick = HAL_GetTick(); }
        }
    );
}

// a packet is waiting
uint8_t USB_rx_pending( void )
{
    return rx_head != rx_tail;
}

// tick of the last packet's arrival (or of re-arming after a stall)
uint32_t USB_rx_last_tick( void )
{
    return rx_last_tick;
}

/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/
