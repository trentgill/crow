#include "usbd_composite.h"

#include "usbd_ctlreq.h"
#include "usbd_map.h"
#include "cdc_acm/usbd_cdc_acm.h"
#include "midi/usbd_midi.h"
#include "uac/usbd_uac.h"

static uint8_t COMP_Init( USBD_HandleTypeDef* pdev, uint8_t cfgidx );
static uint8_t COMP_DeInit( USBD_HandleTypeDef* pdev, uint8_t cfgidx );
static uint8_t COMP_Setup( USBD_HandleTypeDef* pdev, USBD_SetupReqTypedef* req );
static uint8_t COMP_EP0_TxSent( USBD_HandleTypeDef* pdev );
static uint8_t COMP_EP0_RxReady( USBD_HandleTypeDef* pdev );
static uint8_t COMP_DataIn( USBD_HandleTypeDef* pdev, uint8_t epnum );
static uint8_t COMP_DataOut( USBD_HandleTypeDef* pdev, uint8_t epnum );
static uint8_t COMP_SOF( USBD_HandleTypeDef* pdev );
static uint8_t COMP_IsoINIncomplete( USBD_HandleTypeDef* pdev, uint8_t epnum );
static uint8_t COMP_IsoOUTIncomplete( USBD_HandleTypeDef* pdev, uint8_t epnum );
static uint8_t* COMP_GetCfgDesc( uint16_t* length );
static uint8_t* COMP_GetDeviceQualifierDesc( uint16_t* length );
static uint8_t* COMP_GetUsrStrDesc( USBD_HandleTypeDef* pdev, uint8_t index, uint16_t* length );

USBD_ClassTypeDef USBD_COMPOSITE = { COMP_Init
                                   , COMP_DeInit
                                   , COMP_Setup
                                   , COMP_EP0_TxSent
                                   , COMP_EP0_RxReady
                                   , COMP_DataIn
                                   , COMP_DataOut
                                   , COMP_SOF
                                   , COMP_IsoINIncomplete
                                   , COMP_IsoOUTIncomplete
                                   , COMP_GetCfgDesc // HS: never used, FS only
                                   , COMP_GetCfgDesc
                                   , COMP_GetCfgDesc
                                   , COMP_GetDeviceQualifierDesc
                                   , COMP_GetUsrStrDesc
                                   };

static uint8_t audio_mode = 0;

// DESCRIPTORS

#define CFG_DESC_MAX 512 // audio mode is ~370 bytes
SRAM_NOINIT __ALIGN_BEGIN static uint8_t cfg_desc[CFG_DESC_MAX] __ALIGN_END;
static uint16_t cfg_len = 0;

// crow's serial port exactly as it was, behind an IAD so hosts bind interfaces
// 0 and 1 to one CDC ACM driver
static const uint8_t cdc_desc[] =
{ // interface association
  0x08, 0x0B, ITF_CDC_CMD, 0x02, 0x02, 0x02, 0x01, 0x00
  // communication interface, one interrupt endpoint
, 0x09, USB_DESC_TYPE_INTERFACE, ITF_CDC_CMD, 0x00, 0x01, 0x02, 0x02, 0x01, 0x00
, 0x05, 0x24, 0x00, 0x10, 0x01             // header, CDC 1.10
, 0x05, 0x24, 0x01, 0x00, ITF_CDC_DATA     // call management
, 0x04, 0x24, 0x02, 0x02                   // ACM: line coding & serial state
, 0x05, 0x24, 0x06, ITF_CDC_CMD, ITF_CDC_DATA // union
, 0x07, USB_DESC_TYPE_ENDPOINT, EP_CDC_CMD, 0x03
      , LOBYTE(CDC_CMD_PACKET_SIZE), HIBYTE(CDC_CMD_PACKET_SIZE), CDC_FS_BINTERVAL
  // data interface, bulk OUT & IN
, 0x09, USB_DESC_TYPE_INTERFACE, ITF_CDC_DATA, 0x00, 0x02, 0x0A, 0x00, 0x00, 0x00
, 0x07, USB_DESC_TYPE_ENDPOINT, EP_CDC_OUT, 0x02
      , LOBYTE(CDC_DATA_FS_MAX_PACKET_SIZE), HIBYTE(CDC_DATA_FS_MAX_PACKET_SIZE), 0x00
, 0x07, USB_DESC_TYPE_ENDPOINT, EP_CDC_IN, 0x02
      , LOBYTE(CDC_DATA_FS_MAX_PACKET_SIZE), HIBYTE(CDC_DATA_FS_MAX_PACKET_SIZE), 0x00
};

void USBD_COMPOSITE_Build( uint8_t audio )
{
    audio_mode = audio;

    // the vendored CDC ACM class reads its endpoints & interfaces from these
    CDC_IN_EP[0]        = EP_CDC_IN;
    CDC_OUT_EP[0]       = EP_CDC_OUT;
    CDC_CMD_EP[0]       = EP_CDC_CMD;
    CDC_CMD_ITF_NBR[0]  = ITF_CDC_CMD;
    CDC_COM_ITF_NBR[0]  = ITF_CDC_DATA;
    CDC_STR_DESC_IDX[0] = 0;

    uint8_t* p = &cfg_desc[9];
    memcpy( p, cdc_desc, sizeof(cdc_desc) ); p += sizeof(cdc_desc);
    p += USBD_MIDI_Descriptor( p );
    if( audio ){ p += USBD_UAC_Descriptor( p ); }
    cfg_len = (uint16_t)(p - cfg_desc);

    uint8_t* c = cfg_desc;
    c[0] = 0x09;
    c[1] = USB_DESC_TYPE_CONFIGURATION;
    c[2] = LOBYTE(cfg_len);
    c[3] = HIBYTE(cfg_len);
    c[4] = audio ? ITF_COUNT_AUDIO : ITF_COUNT_DEFAULT;
    c[5] = 0x01; // bConfigurationValue
    c[6] = 0x00; // iConfiguration
    c[7] = 0xC0; // self powered
    c[8] = USBD_MAX_POWER;
}

uint8_t USBD_COMPOSITE_IsAudio( void )
{
    return audio_mode;
}

static uint8_t* COMP_GetCfgDesc( uint16_t* length )
{
    *length = cfg_len;
    return cfg_desc;
}

__ALIGN_BEGIN static uint8_t qualifier_desc[USB_LEN_DEV_QUALIFIER_DESC] __ALIGN_END =
{ USB_LEN_DEV_QUALIFIER_DESC
, USB_DESC_TYPE_DEVICE_QUALIFIER
, 0x00, 0x02 // USB 2.00
, 0xEF, 0x02, 0x01 // miscellaneous: interface association
, 0x40
, 0x01
, 0x00
};

static uint8_t* COMP_GetDeviceQualifierDesc( uint16_t* length )
{
    *length = sizeof(qualifier_desc);
    return qualifier_desc;
}

static uint8_t* COMP_GetUsrStrDesc( USBD_HandleTypeDef* pdev, uint8_t index, uint16_t* length )
{
    UNUSED(pdev);
    static uint8_t str_desc[64];
    const char* s = NULL;
    switch( index ){
        case STR_IDX_MIDI:      s = "crow MIDI"; break;
        case STR_IDX_AUDIO:     s = "crow audio"; break;
        case STR_IDX_AUDIO_OUT: s = "crow outputs"; break;
        case STR_IDX_AUDIO_IN:  s = "crow inputs"; break;
        default: return NULL;
    }
    USBD_GetString( (uint8_t*)s, str_desc, length );
    return str_desc;
}

// DISPATCH

typedef enum{ F_NONE, F_CDC, F_MIDI, F_UAC } Function_t;

static Function_t itf_function( uint8_t itf )
{
    switch( itf ){
        case ITF_CDC_CMD: case ITF_CDC_DATA: return F_CDC;
        case ITF_MIDI_AC: case ITF_MIDI_MS:  return F_MIDI;
        case ITF_AUDIO_AC: case ITF_AUDIO_OUT: case ITF_AUDIO_IN:
            return audio_mode ? F_UAC : F_NONE;
        default: return F_NONE;
    }
}

// epnum without the direction bit
static Function_t ep_function( uint8_t epnum, uint8_t in )
{
    if( in ){
        switch( epnum ){
            case (EP_CDC_IN & 0x7F): case (EP_CDC_CMD & 0x7F): return F_CDC;
            case (EP_MIDI_IN & 0x7F): return F_MIDI;
            case (EP_AUDIO_IN & 0x7F): case (EP_AUDIO_FB & 0x7F):
                return audio_mode ? F_UAC : F_NONE;
            default: return F_NONE;
        }
    } else {
        switch( epnum ){
            case EP_CDC_OUT:   return F_CDC;
            case EP_MIDI_OUT:  return F_MIDI;
            case EP_AUDIO_OUT: return audio_mode ? F_UAC : F_NONE;
            default: return F_NONE;
        }
    }
}

static const USBD_ClassTypeDef* class_of( Function_t f )
{
    switch( f ){
        case F_CDC:  return &USBD_CDC_ACM;
        case F_MIDI: return &USBD_MIDI;
        case F_UAC:  return &USBD_UAC;
        default:     return NULL;
    }
}

static uint8_t COMP_Init( USBD_HandleTypeDef* pdev, uint8_t cfgidx )
{
    USBD_CDC_ACM.Init( pdev, cfgidx );
    USBD_MIDI.Init( pdev, cfgidx );
    if( audio_mode ){ USBD_UAC.Init( pdev, cfgidx ); }
    return USBD_OK;
}

static uint8_t COMP_DeInit( USBD_HandleTypeDef* pdev, uint8_t cfgidx )
{
    USBD_CDC_ACM.DeInit( pdev, cfgidx );
    USBD_MIDI.DeInit( pdev, cfgidx );
    if( audio_mode ){ USBD_UAC.DeInit( pdev, cfgidx ); }
    return USBD_OK;
}

static uint8_t COMP_Setup( USBD_HandleTypeDef* pdev, USBD_SetupReqTypedef* req )
{
    Function_t f = F_NONE;
    switch( req->bmRequest & USB_REQ_RECIPIENT_MASK ){
        case USB_REQ_RECIPIENT_INTERFACE:
            f = itf_function( LOBYTE(req->wIndex) );
            break;
        case USB_REQ_RECIPIENT_ENDPOINT:
            f = ep_function( LOBYTE(req->wIndex) & 0x7F, LOBYTE(req->wIndex) & 0x80 );
            break;
        default: break;
    }
    const USBD_ClassTypeDef* c = class_of( f );
    if( c == NULL ){
        // the core has already answered a standard endpoint request (it calls
        // here after CLEAR_FEATURE), so only stall the ones still waiting
        if( (req->bmRequest & USB_REQ_RECIPIENT_MASK) == USB_REQ_RECIPIENT_ENDPOINT
         && (req->bmRequest & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_STANDARD ){
            return USBD_OK;
        }
        USBD_CtlError( pdev, req );
        return USBD_FAIL;
    }
    return c->Setup( pdev, req );
}

// control transfers finishing go to every function that can start one: each
// only acts on a transfer it started
static uint8_t COMP_EP0_TxSent( USBD_HandleTypeDef* pdev )
{
    if( audio_mode && USBD_UAC.EP0_TxSent ){ USBD_UAC.EP0_TxSent( pdev ); }
    return USBD_OK;
}

static uint8_t COMP_EP0_RxReady( USBD_HandleTypeDef* pdev )
{
    USBD_CDC_ACM.EP0_RxReady( pdev );
    if( audio_mode && USBD_UAC.EP0_RxReady ){ USBD_UAC.EP0_RxReady( pdev ); }
    return USBD_OK;
}

static uint8_t COMP_DataIn( USBD_HandleTypeDef* pdev, uint8_t epnum )
{
    const USBD_ClassTypeDef* c = class_of( ep_function( epnum & 0x7F, 1 ) );
    return c ? c->DataIn( pdev, epnum & 0x7F ) : USBD_FAIL;
}

static uint8_t COMP_DataOut( USBD_HandleTypeDef* pdev, uint8_t epnum )
{
    const USBD_ClassTypeDef* c = class_of( ep_function( epnum, 0 ) );
    return c ? c->DataOut( pdev, epnum ) : USBD_FAIL;
}

static uint8_t COMP_SOF( USBD_HandleTypeDef* pdev )
{
    if( audio_mode && USBD_UAC.SOF ){ USBD_UAC.SOF( pdev ); }
    return USBD_OK;
}

static uint8_t COMP_IsoINIncomplete( USBD_HandleTypeDef* pdev, uint8_t epnum )
{
    if( audio_mode && USBD_UAC.IsoINIncomplete ){ USBD_UAC.IsoINIncomplete( pdev, epnum ); }
    return USBD_OK;
}

static uint8_t COMP_IsoOUTIncomplete( USBD_HandleTypeDef* pdev, uint8_t epnum )
{
    if( audio_mode && USBD_UAC.IsoOUTIncomplete ){ USBD_UAC.IsoOUTIncomplete( pdev, epnum ); }
    return USBD_OK;
}
