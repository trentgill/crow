#pragma once

#include <stdint.h>

#include "../ll/adda.h" // IO_block_t

// USB Audio streams between the host and crow's jacks: 4 outputs from the
// host, 2 inputs to it. 16 bit, full scale +-1.0 = +-10V.
//
// Two sides meet here through a pair of rings:
//   the audio block (DAC DMA interrupt, priority 5) calls USB_Audio_BlockIn
//     and USB_Audio_BlockOut once per block;
//   the USB audio class (OTG interrupt, priority 6) calls the USB_Audio_Out*,
//     USB_Audio_In* and USB_Audio_Frame functions further down.
// Each ring has one writer and one reader, so neither side waits on the other.

// LIFECYCLE

void    USB_Audio_Init( void );            // once at boot, before USB starts
void    USB_Audio_Enable( uint8_t on );    // audio mode: every output streams
uint8_t USB_Audio_IsEnabled( void );

// which outputs the host stream drives. ch is 0..3. Lua takes a jack back with
// SetOutput(ch, 0) when output[n].action is assigned
void    USB_Audio_SetOutput( int ch, uint8_t usb );
uint8_t USB_Audio_GetOutput( int ch );

// THE AUDIO BLOCK. call both at the end of IO_BlockProcess, every block

void USB_Audio_BlockIn( IO_block_t* b );  // inputs -> host
void USB_Audio_BlockOut( IO_block_t* b ); // host -> outputs, for the jacks it owns
// test: the IN stream carries outputs 1 & 2 (after BlockOut) instead of the inputs
void USB_Audio_Loopback( uint8_t on );

// THE USB CLASS (usbd/uac/usbd_uac.c). OTG interrupt only

void     USB_Audio_OutStart( void );   // host selected the OUT stream
void     USB_Audio_OutStop( void );
void     USB_Audio_OutPacket( const uint8_t* buf, uint32_t len ); // a packet arrived
void     USB_Audio_InStart( void );    // host selected the IN stream
void     USB_Audio_InStop( void );
uint32_t USB_Audio_InPacket( uint8_t* buf ); // fill the next packet. returns bytes
void     USB_Audio_Frame( uint16_t frame );  // every SOF, with its frame number
uint32_t USB_Audio_Feedback( void );         // DAC rate for the host, 10.14 samples/frame
void     USB_Audio_IsoMissed( uint8_t in );  // an iso packet was lost in its frame

// STATS

typedef struct{
    uint32_t out_underruns;  // blocks the OUT ring ran dry: outputs held at 0V
    uint32_t out_overruns;   // packets dropped on a full ring, and latency resyncs
    uint32_t in_overruns;    // input blocks or a backlog dropped: host wasn't reading
    uint32_t in_underruns;   // IN packets padded with silence
    uint32_t iso_in_missed;  // IN packets the host didn't collect in their frame
    uint32_t iso_out_missed; // OUT packets that didn't arrive in their frame
    uint32_t out_packets;    // packets received
    uint32_t in_packets;     // packets queued to send
    uint32_t fb_rejected;    // rate windows thrown out as implausible
    uint32_t feedback;       // last value reported, 10.14 samples per frame
    uint32_t rate;           // measured DAC rate, 16.16 samples per frame
    uint16_t out_fill;       // frames in the OUT ring now
    uint16_t in_fill;        // frames in the IN ring now
    uint8_t  out_streaming;  // host has the OUT stream open
    uint8_t  out_playing;    // ..and the ring has filled: outputs follow it
    uint8_t  in_streaming;   // host has the IN stream open
} USB_Audio_Stats_t;

// counters only count in TELEMETRY builds. the rest is always live
void USB_Audio_Stats( USB_Audio_Stats_t* s );
void USB_Audio_StatsReset( void );
