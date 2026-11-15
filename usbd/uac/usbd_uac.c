#include "usbd_uac.h"

#include <string.h>

#include "usbd_map.h"

// USB Audio 1.0, full speed
//
// Host -> crow: an asynchronous isochronous OUT endpoint carries 4 channels.
// crow's DAC runs from its own clock (47.992kHz), so a feedback endpoint tells
// the host how many samples to send each frame (lib/usb_audio.c measures it).
// crow -> host: an asynchronous isochronous IN endpoint carries the 2 inputs,
// 47..49 frames per packet, at the same clock.
//
// No feature unit: the host has no volume control over CV.

// DESCRIPTOR

#define DT_INTERFACE        0x04
#define DT_ENDPOINT         0x05
#define DT_IAD              0x0B
#define DT_CS_INTERFACE     0x24
#define DT_CS_ENDPOINT      0x25

#define AUDIO_CLASS         0x01
#define SUBCLASS_CONTROL    0x01
#define SUBCLASS_STREAMING  0x02

#define AC_HEADER           0x01
#define AC_INPUT_TERMINAL   0x02
#define AC_OUTPUT_TERMINAL  0x03
#define AS_GENERAL          0x01
#define AS_FORMAT_TYPE      0x02
#define EP_GENERAL          0x01
#define FORMAT_TYPE_I       0x01
#define FORMAT_PCM          0x0001

#define TT_USB_STREAMING    0x0101
#define TT_LINE_CONNECTOR   0x0603 // external jacks

// Terminal ids. Each path is a pair of terminals, joined directly
#define TID_OUT_USB         1 // host stream in..
#define TID_OUT_JACKS       2 // ..to the outputs
#define TID_IN_JACKS        3 // the inputs..
#define TID_IN_USB          4 // ..out to the host

// Channel positions. The outputs are 4 CV jacks, not speakers: with no
// position bits Linux reports them as unknown positions (PipeWire: AUX0..3)
// and macOS as plain channels 1..4, so no host upmixes stereo into the "rear"
// pair or downmixes them. Windows takes it as direct-out 4ch. A surround mask
// (0x0033, quad) would invite all of those mixes onto CV. The inputs are
// marked left/right: every host and app knows what to do with a stereo
// capture device, and nothing remixes a capture stream.
#define OUT_CHANNEL_CONFIG  0x0000
#define IN_CHANNEL_CONFIG   0x0003

// the host polls the feedback every 2^FB_REFRESH ms. the value moves slowly
// (lib/usb_audio.c), so 4ms is plenty, and short enough that it rarely sits
// unread in its FIFO (see IsoINIncomplete)
#define FB_REFRESH          2

#define AC_HEADER_LEN       (8 + 2) // + 1 per streaming interface
#define IT_LEN              12
#define OT_LEN              9
#define AC_TOTAL            (AC_HEADER_LEN + 2*IT_LEN + 2*OT_LEN)

#define W(x)  (uint8_t)((x) & 0xFF), (uint8_t)(((x) >> 8) & 0xFF)
#define T(x)  W(x), (uint8_t)(((x) >> 16) & 0xFF)

static const uint8_t uac_desc[] = {
    // Interface Association: the three interfaces are one function
    0x08, DT_IAD
    , ITF_AUDIO_AC          // bFirstInterface
    , 3                     // bInterfaceCount
    , AUDIO_CLASS, 0x00, 0x00
    , STR_IDX_AUDIO         // iFunction

    // AUDIO CONTROL
    , 0x09, DT_INTERFACE
    , ITF_AUDIO_AC, 0x00    // bInterfaceNumber, bAlternateSetting
    , 0                     // bNumEndpoints
    , AUDIO_CLASS, SUBCLASS_CONTROL, 0x00
    , STR_IDX_AUDIO

    , AC_HEADER_LEN, DT_CS_INTERFACE, AC_HEADER
    , W(0x0100)             // bcdADC 1.0
    , W(AC_TOTAL)           // this header and the terminals after it
    , 2                     // bInCollection
    , ITF_AUDIO_OUT, ITF_AUDIO_IN

    , IT_LEN, DT_CS_INTERFACE, AC_INPUT_TERMINAL
    , TID_OUT_USB
    , W(TT_USB_STREAMING)
    , 0x00                  // bAssocTerminal
    , AUDIO_OUT_CHANNELS
    , W(OUT_CHANNEL_CONFIG)
    , 0x00, 0x00            // iChannelNames, iTerminal

    , OT_LEN, DT_CS_INTERFACE, AC_OUTPUT_TERMINAL
    , TID_OUT_JACKS
    , W(TT_LINE_CONNECTOR)
    , 0x00                  // bAssocTerminal
    , TID_OUT_USB           // bSourceID
    , STR_IDX_AUDIO_OUT     // iTerminal

    , IT_LEN, DT_CS_INTERFACE, AC_INPUT_TERMINAL
    , TID_IN_JACKS
    , W(TT_LINE_CONNECTOR)
    , 0x00
    , AUDIO_IN_CHANNELS
    , W(IN_CHANNEL_CONFIG)
    , 0x00, STR_IDX_AUDIO_IN

    , OT_LEN, DT_CS_INTERFACE, AC_OUTPUT_TERMINAL
    , TID_IN_USB
    , W(TT_USB_STREAMING)
    , 0x00
    , TID_IN_JACKS
    , 0x00

    // OUTPUTS STREAM. alt 0 has no bandwidth: the host's default
    , 0x09, DT_INTERFACE
    , ITF_AUDIO_OUT, 0x00, 0
    , AUDIO_CLASS, SUBCLASS_STREAMING, 0x00
    , STR_IDX_AUDIO_OUT

    // alt 1: streaming, data endpoint then its feedback endpoint (Linux
    // expects the feedback second)
    , 0x09, DT_INTERFACE
    , ITF_AUDIO_OUT, 0x01, 2
    , AUDIO_CLASS, SUBCLASS_STREAMING, 0x00
    , STR_IDX_AUDIO_OUT

    , 0x07, DT_CS_INTERFACE, AS_GENERAL
    , TID_OUT_USB           // bTerminalLink
    , 3                     // bDelay: ~2.7ms in the ring
    , W(FORMAT_PCM)

    , 0x0B, DT_CS_INTERFACE, AS_FORMAT_TYPE, FORMAT_TYPE_I
    , AUDIO_OUT_CHANNELS
    , AUDIO_BYTES           // bSubframeSize
    , AUDIO_BYTES * 8       // bBitResolution
    , 1                     // bSamFreqType: one discrete rate
    , T(AUDIO_RATE_NOMINAL)

    , 0x09, DT_ENDPOINT
    , EP_AUDIO_OUT
    , 0x05                  // isochronous, asynchronous, data
    , W(AUDIO_OUT_PACKET)
    , 1                     // bInterval: every frame
    , 0                     // bRefresh
    , EP_AUDIO_FB           // bSynchAddress

    , 0x07, DT_CS_ENDPOINT, EP_GENERAL
    , 0x01                  // bmAttributes: sampling frequency control
    , 0x00, W(0)            // bLockDelayUnits, wLockDelay

    , 0x09, DT_ENDPOINT
    , EP_AUDIO_FB
    , 0x11                  // isochronous, no sync, feedback
    , W(AUDIO_FB_PACKET)
    , 1                     // bInterval
    , FB_REFRESH            // bRefresh
    , 0                     // bSynchAddress

    // INPUTS STREAM
    , 0x09, DT_INTERFACE
    , ITF_AUDIO_IN, 0x00, 0
    , AUDIO_CLASS, SUBCLASS_STREAMING, 0x00
    , STR_IDX_AUDIO_IN

    , 0x09, DT_INTERFACE
    , ITF_AUDIO_IN, 0x01, 1
    , AUDIO_CLASS, SUBCLASS_STREAMING, 0x00
    , STR_IDX_AUDIO_IN

    , 0x07, DT_CS_INTERFACE, AS_GENERAL
    , TID_IN_USB
    , 2                     // bDelay: ~1.7ms in the ring
    , W(FORMAT_PCM)

    , 0x0B, DT_CS_INTERFACE, AS_FORMAT_TYPE, FORMAT_TYPE_I
    , AUDIO_IN_CHANNELS
    , AUDIO_BYTES
    , AUDIO_BYTES * 8
    , 1
    , T(AUDIO_RATE_NOMINAL)

    , 0x09, DT_ENDPOINT
    , EP_AUDIO_IN
    , 0x05                  // isochronous, asynchronous, data
    , W(AUDIO_IN_PACKET)
    , 1
    , 0
    , 0

    , 0x07, DT_CS_ENDPOINT, EP_GENERAL
    , 0x01                  // sampling frequency control
    , 0x00, W(0)
};

_Static_assert( sizeof(uac_desc) == USBD_UAC_DESC_SIZE, "USBD_UAC_DESC_SIZE" );

uint16_t USBD_UAC_Descriptor( uint8_t* buf )
{
    memcpy( buf, uac_desc, sizeof(uac_desc) );
    return sizeof(uac_desc);
}


#ifndef USBD_UAC_DESCRIPTOR_ONLY

#include "usbd_ctlreq.h"
#include "../../lib/usb_audio.h"

// THE CLASS
//
// Everything here runs in the OTG interrupt, so nothing in it races anything
// else in it. The audio block (higher priority) only meets it in the rings.
//
// How the iso endpoints are kept fed on the F7 OTG core, through HAL V1.2.8:
// - an iso transfer is aimed at one frame, the frame after the one it was
//   started in (USB_EPStartXfer sets odd/even from the current frame number).
// - IN: the HAL writes the whole packet into the endpoint's FIFO at once
//   (no TXFE interrupt for iso). So the next packet is started from DataIn,
//   in the frame its predecessor went out, aimed at the next. If the host
//   didn't collect it, the core raises incomplete-iso-IN at the end of that
//   frame: the endpoint is stopped, its FIFO flushed and a fresh packet
//   started for the frame after (the stale one is dropped, so latency from
//   the jacks never grows). The feedback endpoint runs the same way: the
//   host polls it only every 2^FB_REFRESH frames, so it's re-aimed every
//   frame in between. SOF starts anything left idle.
// - OUT: re-armed from DataOut, aimed at the next frame. If the host's packet
//   doesn't come (stream not started yet, or the re-arm missed its frame),
//   incomplete-iso-OUT at the end of the frame re-aims it at the next one,
//   and lib/usb_audio.c patches the gap (USB_Audio_IsoMissed).
// - HAL V1.2.8 passes epnum 0 for both incomplete interrupts, so the
//   handlers check every audio endpoint themselves.

#define REQ_SET_CUR         0x01
#define REQ_GET_CUR         0x81
#define REQ_GET_MIN         0x82
#define REQ_GET_MAX         0x83
#define CS_SAMPLING_FREQ    0x01

#define DOEPCTL_EONUM       (1UL << 16) // OUT: frame parity armed for (unnamed in CMSIS)
#define EP_STOP_SPINS       200         // register polls before giving up on the core

static uint8_t UAC_Init( USBD_HandleTypeDef* pdev, uint8_t cfgidx );
static uint8_t UAC_DeInit( USBD_HandleTypeDef* pdev, uint8_t cfgidx );
static uint8_t UAC_Setup( USBD_HandleTypeDef* pdev, USBD_SetupReqTypedef* req );
static uint8_t UAC_EP0_RxReady( USBD_HandleTypeDef* pdev );
static uint8_t UAC_DataIn( USBD_HandleTypeDef* pdev, uint8_t epnum );
static uint8_t UAC_DataOut( USBD_HandleTypeDef* pdev, uint8_t epnum );
static uint8_t UAC_SOF( USBD_HandleTypeDef* pdev );
static uint8_t UAC_IsoINIncomplete( USBD_HandleTypeDef* pdev, uint8_t epnum );
static uint8_t UAC_IsoOUTIncomplete( USBD_HandleTypeDef* pdev, uint8_t epnum );

const USBD_ClassTypeDef USBD_UAC = { UAC_Init
                                   , UAC_DeInit
                                   , UAC_Setup
                                   , NULL // EP0_TxSent
                                   , UAC_EP0_RxReady
                                   , UAC_DataIn
                                   , UAC_DataOut
                                   , UAC_SOF
                                   , UAC_IsoINIncomplete
                                   , UAC_IsoOUTIncomplete
                                   , NULL, NULL, NULL, NULL // descriptors: the composite's
#if (USBD_SUPPORT_USER_STRING_DESC == 1U)
                                   , NULL
#endif
                                   };

// packet buffers in SRAM (DTCM is full). contents need no init
static uint8_t out_packet[AUDIO_OUT_PACKET] SRAM_NOINIT __attribute__((aligned(4)));
static uint8_t in_packet[AUDIO_IN_PACKET]   SRAM_NOINIT __attribute__((aligned(4)));
static uint8_t fb_packet[4]                 SRAM_NOINIT __attribute__((aligned(4)));

// a few bytes of DTCM: zero at boot, before the first USB reset reaches DeInit
static uint8_t  alt_out    = 0; // alternate settings the host chose
static uint8_t  alt_in     = 0;
static uint8_t  out_seen   = 0; // a packet has arrived since alt 1
static uint8_t  in_seen    = 0; // a packet has been collected since alt 1
static uint8_t  ep0_rx     = 0; // a SET_CUR data stage is ours
static uint8_t  ep0_buf[4];     // control data, both ways


// ENDPOINT PLUMBING

static inline uint32_t otg_base( USBD_HandleTypeDef* pdev )
{
    return (uint32_t)((PCD_HandleTypeDef*)pdev->pData)->Instance;
}

static uint32_t frame_now( USBD_HandleTypeDef* pdev )
{
    uint32_t USBx_BASE = otg_base( pdev );
    return (USBx_DEVICE->DSTS & USB_OTG_DSTS_FNSOF) >> USB_OTG_DSTS_FNSOF_Pos;
}

static uint8_t in_busy( USBD_HandleTypeDef* pdev, uint8_t ep )
{
    uint32_t USBx_BASE = otg_base( pdev );
    return (USBx_INEP(ep & 0x7F)->DIEPCTL & USB_OTG_DIEPCTL_EPENA) ? 1 : 0;
}

// Stop an iso IN endpoint and empty its FIFO (RM0431 "disabling an IN
// endpoint"). Returns 1 if its packet went out after all: the transfer
// completed while we were stopping it, and DataIn will follow.
static uint8_t in_stop( USBD_HandleTypeDef* pdev, uint8_t ep )
{
    USB_OTG_GlobalTypeDef* USBx = ((PCD_HandleTypeDef*)pdev->pData)->Instance;
    uint32_t USBx_BASE = (uint32_t)USBx;
    uint32_t n = ep & 0x7F;
    if( USBx_INEP(n)->DIEPCTL & USB_OTG_DIEPCTL_EPENA ){
        USBx_INEP(n)->DIEPCTL |= USB_OTG_DIEPCTL_SNAK;
        for( int i=0; i<EP_STOP_SPINS; i++ ){
            if( USBx_INEP(n)->DIEPINT & USB_OTG_DIEPINT_INEPNE ){ break; }
        }
        if( USBx_INEP(n)->DIEPCTL & USB_OTG_DIEPCTL_EPENA ){
            USBx_INEP(n)->DIEPCTL |= USB_OTG_DIEPCTL_EPDIS | USB_OTG_DIEPCTL_SNAK;
            for( int i=0; i<EP_STOP_SPINS; i++ ){
                if( USBx_INEP(n)->DIEPINT & USB_OTG_DIEPINT_EPDISD ){ break; }
            }
        }
        USBx_INEP(n)->DIEPINT = USB_OTG_DIEPINT_EPDISD | USB_OTG_DIEPINT_INEPNE;
    }
    // direct, not USBD_LL_FlushEP: the HAL's version gives up if the PCD lock
    // is held, and a packet left in the FIFO would sit in front of the next
    USB_FlushTxFifo( USBx, n );
    return (USBx_INEP(n)->DIEPINT & USB_OTG_DIEPINT_XFRC) ? 1 : 0;
}

// Disable the iso OUT endpoint the way the reference manual asks: under a
// global OUT NAK. Setting EPDIS without it (what the HAL's close does) never
// completes, and the endpoint is left EPENA|EPDIS: the next alt 1 then finds
// it "armed" and nothing is ever received. Linux sets alt 0 then alt 1 again
// when a stream is prepared, so this happens on the first play.
static void out_stop( USBD_HandleTypeDef* pdev )
{
    USB_OTG_GlobalTypeDef* USBx = ((PCD_HandleTypeDef*)pdev->pData)->Instance;
    uint32_t USBx_BASE = (uint32_t)USBx;
    uint32_t n = EP_AUDIO_OUT & 0x7F;
    if( !(USBx_OUTEP(n)->DOEPCTL & USB_OTG_DOEPCTL_EPENA) ){ return; }
    USBx_DEVICE->DCTL |= USB_OTG_DCTL_SGONAK;
    for( int i=0; i<EP_STOP_SPINS; i++ ){
        if( USBx->GINTSTS & USB_OTG_GINTSTS_BOUTNAKEFF ){ break; }
    }
    USBx_OUTEP(n)->DOEPCTL |= USB_OTG_DOEPCTL_EPDIS | USB_OTG_DOEPCTL_SNAK;
    for( int i=0; i<EP_STOP_SPINS; i++ ){
        if( USBx_OUTEP(n)->DOEPINT & USB_OTG_DOEPINT_EPDISD ){ break; }
    }
    USBx_OUTEP(n)->DOEPINT = USB_OTG_DOEPINT_EPDISD;
    USBx_DEVICE->DCTL |= USB_OTG_DCTL_CGONAK;
}

// Called at the end of a frame the host didn't collect something in. An
// endpoint armed for this frame (parity matches) missed it: stop it so it can
// be armed again for the next. One armed for the next frame is left alone.
static uint8_t in_missed( USBD_HandleTypeDef* pdev, uint8_t ep, uint32_t frame )
{
    uint32_t USBx_BASE = otg_base( pdev );
    uint32_t ctl = USBx_INEP(ep & 0x7F)->DIEPCTL;
    if( !(ctl & USB_OTG_DIEPCTL_EPENA) ){ return 0; } // idle: SOF arms it
    if( ((ctl & USB_OTG_DIEPCTL_EONUM_DPID) ? 1 : 0) != (frame & 1) ){ return 0; }
    return !in_stop( pdev, ep );
}

static void fb_arm( USBD_HandleTypeDef* pdev )
{
    if( in_busy( pdev, EP_AUDIO_FB ) ){ return; }
    uint32_t fb = USB_Audio_Feedback(); // 10.14, little endian, 3 bytes
    fb_packet[0] = fb & 0xFF;
    fb_packet[1] = (fb >> 8) & 0xFF;
    fb_packet[2] = (fb >> 16) & 0xFF;
    USBD_LL_Transmit( pdev, EP_AUDIO_FB, fb_packet, AUDIO_FB_PACKET );
}

static void in_arm( USBD_HandleTypeDef* pdev )
{
    if( in_busy( pdev, EP_AUDIO_IN ) ){ return; }
    uint32_t len = USB_Audio_InPacket( in_packet );
    USBD_LL_Transmit( pdev, EP_AUDIO_IN, in_packet, len );
}

static void out_arm( USBD_HandleTypeDef* pdev )
{
    uint32_t USBx_BASE = otg_base( pdev );
    if( USBx_OUTEP(EP_AUDIO_OUT & 0x7F)->DOEPCTL & USB_OTG_DOEPCTL_EPENA ){ return; }
    USBD_LL_PrepareReceive( pdev, EP_AUDIO_OUT, out_packet, AUDIO_OUT_PACKET );
}

static void out_open( USBD_HandleTypeDef* pdev )
{
    USBD_LL_OpenEP( pdev, EP_AUDIO_OUT, USBD_EP_TYPE_ISOC, AUDIO_OUT_PACKET );
    pdev->ep_out[EP_AUDIO_OUT & 0xF].is_used = 1;
    pdev->ep_out[EP_AUDIO_OUT & 0xF].maxpacket = AUDIO_OUT_PACKET;
    USBD_LL_OpenEP( pdev, EP_AUDIO_FB, USBD_EP_TYPE_ISOC, AUDIO_FB_PACKET );
    pdev->ep_in[EP_AUDIO_FB & 0xF].is_used = 1;
    pdev->ep_in[EP_AUDIO_FB & 0xF].maxpacket = AUDIO_FB_PACKET;
    in_stop( pdev, EP_AUDIO_FB ); // empty FIFO before the first packet
    out_stop( pdev ); // nothing may be left armed (or stuck disabling)
    out_seen = 0;
    USB_Audio_OutStart();
    out_arm( pdev );
    fb_arm( pdev );
}

static void out_close( USBD_HandleTypeDef* pdev )
{
    USB_Audio_OutStop();
    in_stop( pdev, EP_AUDIO_FB );
    USBD_LL_CloseEP( pdev, EP_AUDIO_FB );
    pdev->ep_in[EP_AUDIO_FB & 0xF].is_used = 0;
    out_stop( pdev );
    USBD_LL_CloseEP( pdev, EP_AUDIO_OUT );
    pdev->ep_out[EP_AUDIO_OUT & 0xF].is_used = 0;
}

static void in_open( USBD_HandleTypeDef* pdev )
{
    USBD_LL_OpenEP( pdev, EP_AUDIO_IN, USBD_EP_TYPE_ISOC, AUDIO_IN_PACKET );
    pdev->ep_in[EP_AUDIO_IN & 0xF].is_used = 1;
    pdev->ep_in[EP_AUDIO_IN & 0xF].maxpacket = AUDIO_IN_PACKET;
    in_stop( pdev, EP_AUDIO_IN );
    in_seen = 0;
    USB_Audio_InStart();
    in_arm( pdev );
}

static void in_close( USBD_HandleTypeDef* pdev )
{
    USB_Audio_InStop();
    in_stop( pdev, EP_AUDIO_IN );
    USBD_LL_CloseEP( pdev, EP_AUDIO_IN );
    pdev->ep_in[EP_AUDIO_IN & 0xF].is_used = 0;
}


// CLASS CALLBACKS

// SET_CONFIGURATION. nothing opens until the host picks alt 1
static uint8_t UAC_Init( USBD_HandleTypeDef* pdev, uint8_t cfgidx )
{
    UNUSED(cfgidx);
    // the incomplete-iso interrupts fire at the end of the periodic part of
    // the frame. the HAL puts that at 80%, but behind a hub's transaction
    // translator the host's iso transfers can run until 90%: at 80% we'd stop
    // an endpoint whose token was still coming, every frame. 95% is after all
    // of them, and still leaves 50us to re-arm before the next SOF (a late
    // re-arm costs that one frame, see in_missed)
    uint32_t USBx_BASE = otg_base( pdev );
    USBx_DEVICE->DCFG = (USBx_DEVICE->DCFG & ~USB_OTG_DCFG_PFIVL)
                      | (DCFG_FRAME_INTERVAL_95 << USB_OTG_DCFG_PFIVL_Pos);
    alt_out = 0;
    alt_in  = 0;
    ep0_rx  = 0;
    USB_Audio_OutStop();
    USB_Audio_InStop();
    return USBD_OK;
}

// also on bus reset and disconnect
static uint8_t UAC_DeInit( USBD_HandleTypeDef* pdev, uint8_t cfgidx )
{
    UNUSED(cfgidx);
    if( alt_out ){ out_close( pdev ); }
    if( alt_in ){  in_close( pdev ); }
    alt_out = 0;
    alt_in  = 0;
    ep0_rx  = 0;
    USB_Audio_OutStop();
    USB_Audio_InStop();
    return USBD_OK;
}

static uint8_t set_interface( USBD_HandleTypeDef* pdev, uint8_t itf, uint8_t alt )
{
    if( alt > 1 ){ return USBD_FAIL; }
    switch( itf ){
        case ITF_AUDIO_AC:
            return (alt == 0) ? USBD_OK : USBD_FAIL;
        case ITF_AUDIO_OUT:
            // a repeated alt 1 restarts the stream: hosts use it to resync
            if( alt_out ){ out_close( pdev ); }
            alt_out = alt;
            if( alt ){ out_open( pdev ); }
            return USBD_OK;
        case ITF_AUDIO_IN:
            if( alt_in ){ in_close( pdev ); }
            alt_in = alt;
            if( alt ){ in_open( pdev ); }
            return USBD_OK;
        default:
            return USBD_FAIL;
    }
}

// Sampling frequency control on either streaming endpoint. There's one rate:
// SET_CUR is accepted and changes nothing, the GETs all report 48kHz.
static uint8_t ep_request( USBD_HandleTypeDef* pdev, USBD_SetupReqTypedef* req )
{
    uint8_t ep = LOBYTE(req->wIndex);
    if( (ep != EP_AUDIO_OUT && ep != EP_AUDIO_IN)
     || HIBYTE(req->wValue) != CS_SAMPLING_FREQ
     || req->wLength != 3 ){
        return USBD_FAIL;
    }
    switch( req->bRequest ){
        case REQ_SET_CUR:
            ep0_rx = 1;
            USBD_CtlPrepareRx( pdev, ep0_buf, 3 );
            return USBD_OK;
        case REQ_GET_CUR:
        case REQ_GET_MIN:
        case REQ_GET_MAX:
            ep0_buf[0] = AUDIO_RATE_NOMINAL & 0xFF;
            ep0_buf[1] = (AUDIO_RATE_NOMINAL >> 8) & 0xFF;
            ep0_buf[2] = (AUDIO_RATE_NOMINAL >> 16) & 0xFF;
            USBD_CtlSendData( pdev, ep0_buf, 3 );
            return USBD_OK;
        default:
            return USBD_FAIL;
    }
}

// The composite sends: requests to ITF_AUDIO_*, standard ones included, and
// requests to EP_AUDIO_*. The core sends the status stage of no-data
// interface requests itself when this returns OK.
static uint8_t UAC_Setup( USBD_HandleTypeDef* pdev, USBD_SetupReqTypedef* req )
{
    uint8_t recipient = req->bmRequest & USB_REQ_RECIPIENT_MASK;
    uint8_t ret = USBD_FAIL;
    ep0_rx = 0;
    switch( req->bmRequest & USB_REQ_TYPE_MASK ){
        case USB_REQ_TYPE_STANDARD:
            if( recipient == USB_REQ_RECIPIENT_ENDPOINT ){
                return USBD_OK; // CLEAR_FEATURE: the core has answered it already
            }
            switch( req->bRequest ){
                case USB_REQ_GET_STATUS:
                    ep0_buf[0] = 0;
                    ep0_buf[1] = 0;
                    USBD_CtlSendData( pdev, ep0_buf, 2 );
                    return USBD_OK;
                case USB_REQ_GET_INTERFACE:
                    ep0_buf[0] = (LOBYTE(req->wIndex) == ITF_AUDIO_OUT) ? alt_out
                               : (LOBYTE(req->wIndex) == ITF_AUDIO_IN)  ? alt_in
                               : 0;
                    USBD_CtlSendData( pdev, ep0_buf, 1 );
                    return USBD_OK;
                case USB_REQ_SET_INTERFACE:
                    if( pdev->dev_state != USBD_STATE_CONFIGURED ){ break; }
                    ret = set_interface( pdev, LOBYTE(req->wIndex), LOBYTE(req->wValue) );
                    break;
                default: break;
            }
            break;
        case USB_REQ_TYPE_CLASS:
            // the control interface has no controls: only the endpoints do
            if( recipient == USB_REQ_RECIPIENT_ENDPOINT ){
                ret = ep_request( pdev, req );
            }
            break;
        default: break;
    }
    if( ret != USBD_OK ){ USBD_CtlError( pdev, req ); }
    return ret;
}

// every function hears every EP0 data stage: only take ours
static uint8_t UAC_EP0_RxReady( USBD_HandleTypeDef* pdev )
{
    UNUSED(pdev);
    if( ep0_rx ){
        ep0_rx = 0; // the new rate is in ep0_buf. there's only 48kHz to have
    }
    return USBD_OK;
}

static uint8_t UAC_DataIn( USBD_HandleTypeDef* pdev, uint8_t epnum )
{
    // went out this frame: the next one goes in the next frame
    if( epnum == (EP_AUDIO_FB & 0x7F) && alt_out ){
        fb_arm( pdev );
    } else if( epnum == (EP_AUDIO_IN & 0x7F) && alt_in ){
        in_seen = 1;
        in_arm( pdev );
    }
    return USBD_OK;
}

static uint8_t UAC_DataOut( USBD_HandleTypeDef* pdev, uint8_t epnum )
{
    if( epnum != (EP_AUDIO_OUT & 0x7F) || !alt_out ){ return USBD_OK; }
    out_seen = 1;
    USB_Audio_OutPacket( out_packet, USBD_LL_GetRxDataSize( pdev, epnum ) );
    out_arm( pdev ); // for the next frame
    return USBD_OK;
}

static uint8_t UAC_SOF( USBD_HandleTypeDef* pdev )
{
    USB_Audio_Frame( (uint16_t)frame_now( pdev ) ); // runs whether streaming or not
    // anything idle gets armed for the next frame. normally nothing is: the
    // completion and incomplete handlers keep them going
    if( alt_out ){
        out_arm( pdev );
        fb_arm( pdev );
    }
    if( alt_in ){ in_arm( pdev ); }
    return USBD_OK;
}

static uint8_t UAC_IsoINIncomplete( USBD_HandleTypeDef* pdev, uint8_t epnum )
{
    UNUSED(epnum); // always 0 from this HAL
    uint32_t frame = frame_now( pdev );
    if( alt_out && in_missed( pdev, EP_AUDIO_FB, frame ) ){
        fb_arm( pdev ); // with a fresh value. most frames the host isn't asking
    }
    if( alt_in && in_missed( pdev, EP_AUDIO_IN, frame ) ){
        if( in_seen ){ USB_Audio_IsoMissed( 1 ); }
        in_arm( pdev ); // a fresh packet: the stale one is dropped
    }
    return USBD_OK;
}

static uint8_t UAC_IsoOUTIncomplete( USBD_HandleTypeDef* pdev, uint8_t epnum )
{
    UNUSED(epnum); // always 0 from this HAL
    if( !alt_out ){ return USBD_OK; }
    uint32_t USBx_BASE = otg_base( pdev );
    __IO uint32_t* ctl = &USBx_OUTEP(EP_AUDIO_OUT & 0x7F)->DOEPCTL;
    if( !(*ctl & USB_OTG_DOEPCTL_EPENA) ){ // not armed at all
        out_arm( pdev );
        return USBD_OK;
    }
    uint32_t odd = frame_now( pdev ) & 1;
    if( ((*ctl & DOEPCTL_EONUM) ? 1 : 0) == odd ){
        // armed for this frame, which is ending empty: wait on the next one
        if( out_seen ){ USB_Audio_IsoMissed( 0 ); }
        *ctl |= odd ? USB_OTG_DOEPCTL_SD0PID_SEVNFRM : USB_OTG_DOEPCTL_SODDFRM;
    }
    return USBD_OK;
}

#endif // USBD_UAC_DESCRIPTOR_ONLY
