#include "usbd_midi.h"

#include "usbd_ctlreq.h"
#include "../usbd_map.h"

// jack ids
#define JACK_IN_EMB   0x01 // host -> crow: the OUT endpoint feeds it
#define JACK_IN_EXT   0x02 // crow's own events, as if from an external port
#define JACK_OUT_EMB  0x03 // crow -> host: drives the IN endpoint
#define JACK_OUT_EXT  0x04 // where host messages are "played"

static uint8_t USBD_MIDI_Init(USBD_HandleTypeDef *pdev, uint8_t cfgidx);
static uint8_t USBD_MIDI_DeInit(USBD_HandleTypeDef *pdev, uint8_t cfgidx);
static uint8_t USBD_MIDI_Setup(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req);
static uint8_t USBD_MIDI_DataIn(USBD_HandleTypeDef *pdev, uint8_t epnum);
static uint8_t USBD_MIDI_DataOut(USBD_HandleTypeDef *pdev, uint8_t epnum);

USBD_ClassTypeDef USBD_MIDI = { USBD_MIDI_Init
                              , USBD_MIDI_DeInit
                              , USBD_MIDI_Setup
                              , NULL // EP0_TxSent
                              , NULL // EP0_RxReady
                              , USBD_MIDI_DataIn
                              , USBD_MIDI_DataOut
                              , NULL // SOF
                              , NULL // IsoINIncomplete
                              , NULL // IsoOUTIncomplete
                              , NULL, NULL, NULL, NULL // descriptors come from the composite
#if (USBD_SUPPORT_USER_STRING_DESC == 1U)
                              , NULL
#endif
                              };

static USBD_MIDI_HandleTypeDef hmidi;

// the class-specific MIDI Streaming descriptors, header through the endpoints
#define MS_TOTAL (7 + 6 + 6 + 9 + 9 + 9 + 5 + 9 + 5)

static const uint8_t midi_desc[] =
{ // audio control, no endpoints. no IAD: hosts group an audio function by the
  // AC header's interface list (the same layout TinyUSB's MIDI uses)
  0x09, USB_DESC_TYPE_INTERFACE, ITF_MIDI_AC, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00
  // AC header: ADC 1.00, total 9, one streaming interface
, 0x09, 0x24, 0x01, 0x00, 0x01, 0x09, 0x00, 0x01, ITF_MIDI_MS
  // MIDI streaming, 2 endpoints
, 0x09, USB_DESC_TYPE_INTERFACE, ITF_MIDI_MS, 0x00, 0x02, 0x01, 0x03, 0x00, STR_IDX_MIDI
  // MS header: MIDI 1.00
, 0x07, 0x24, 0x01, 0x00, 0x01, LOBYTE(MS_TOTAL), HIBYTE(MS_TOTAL)
  // jacks
, 0x06, 0x24, 0x02, 0x01, JACK_IN_EMB, 0x00
, 0x06, 0x24, 0x02, 0x02, JACK_IN_EXT, 0x00
, 0x09, 0x24, 0x03, 0x01, JACK_OUT_EMB, 0x01, JACK_IN_EXT, 0x01, 0x00
, 0x09, 0x24, 0x03, 0x02, JACK_OUT_EXT, 0x01, JACK_IN_EMB, 0x01, 0x00
  // bulk OUT, feeding the embedded IN jack
, 0x09, USB_DESC_TYPE_ENDPOINT, EP_MIDI_OUT, 0x02, LOBYTE(MIDI_EPOUT_SIZE), HIBYTE(MIDI_EPOUT_SIZE), 0x00, 0x00, 0x00
, 0x05, 0x25, 0x01, 0x01, JACK_IN_EMB
  // bulk IN, from the embedded OUT jack
, 0x09, USB_DESC_TYPE_ENDPOINT, EP_MIDI_IN, 0x02, LOBYTE(MIDI_EPIN_SIZE), HIBYTE(MIDI_EPIN_SIZE), 0x00, 0x00, 0x00
, 0x05, 0x25, 0x01, 0x01, JACK_OUT_EMB
};

uint16_t USBD_MIDI_Descriptor( uint8_t* buf )
{
    memcpy( buf, midi_desc, sizeof(midi_desc) );
    return sizeof(midi_desc);
}

static uint8_t USBD_MIDI_Init(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
    UNUSED(cfgidx);
    USBD_LL_OpenEP( pdev, EP_MIDI_IN, USBD_EP_TYPE_BULK, MIDI_EPIN_SIZE );
    pdev->ep_in[EP_MIDI_IN & 0xFU].is_used = 1U;
    USBD_LL_OpenEP( pdev, EP_MIDI_OUT, USBD_EP_TYPE_BULK, MIDI_EPOUT_SIZE );
    pdev->ep_out[EP_MIDI_OUT & 0xFU].is_used = 1U;

    hmidi.state = MIDI_IDLE;
    hmidi.RxBuffer = NULL;
    // the interface sets the receive buffer & arms the endpoint
    ((USBD_MIDI_ItfTypeDef*)pdev->pUserData_MIDI)->Init();
    return USBD_OK;
}

static uint8_t USBD_MIDI_DeInit(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
    UNUSED(cfgidx);
    USBD_LL_CloseEP( pdev, EP_MIDI_IN );
    pdev->ep_in[EP_MIDI_IN & 0xFU].is_used = 0U;
    USBD_LL_CloseEP( pdev, EP_MIDI_OUT );
    pdev->ep_out[EP_MIDI_OUT & 0xFU].is_used = 0U;
    hmidi.state = MIDI_IDLE;
    ((USBD_MIDI_ItfTypeDef*)pdev->pUserData_MIDI)->DeInit();
    return USBD_OK;
}

// both interfaces have a single alternate setting and no class requests
static uint8_t USBD_MIDI_Setup(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req)
{
    static uint8_t ifalt = 0;
    static uint16_t status = 0;
    if( (req->bmRequest & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_STANDARD ){
        switch( req->bRequest ){
            case USB_REQ_GET_STATUS:
                USBD_CtlSendData( pdev, (uint8_t*)&status, 2 );
                return USBD_OK;
            case USB_REQ_GET_INTERFACE:
                USBD_CtlSendData( pdev, &ifalt, 1 );
                return USBD_OK;
            case USB_REQ_SET_INTERFACE:
                if( LOBYTE(req->wValue) == 0 ){ return USBD_OK; }
                break;
            case USB_REQ_CLEAR_FEATURE:
                return USBD_OK;
            default: break;
        }
    }
    USBD_CtlError( pdev, req );
    return USBD_FAIL;
}

static uint8_t USBD_MIDI_DataIn(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
    PCD_HandleTypeDef* hpcd = pdev->pData;
    if( (pdev->ep_in[epnum].total_length > 0U)
     && ((pdev->ep_in[epnum].total_length % hpcd->IN_ep[epnum].maxpacket) == 0U) ){
        // a transfer that filled its last packet ends with a zero-length one
        pdev->ep_in[epnum].total_length = 0U;
        USBD_LL_Transmit( pdev, epnum, NULL, 0U );
    } else {
        hmidi.state = MIDI_IDLE;
        USBD_MIDI_ItfTypeDef* itf = pdev->pUserData_MIDI;
        if( itf->TransmitCplt != NULL ){
            itf->TransmitCplt( hmidi.TxBuffer, &hmidi.TxLength, epnum );
        }
    }
    return USBD_OK;
}

static uint8_t USBD_MIDI_DataOut(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
    hmidi.RxLength = USBD_LL_GetRxDataSize( pdev, epnum );
    ((USBD_MIDI_ItfTypeDef*)pdev->pUserData_MIDI)->Receive( hmidi.RxBuffer, &hmidi.RxLength );
    return USBD_OK;
}

uint8_t USBD_MIDI_RegisterInterface(USBD_HandleTypeDef *pdev, USBD_MIDI_ItfTypeDef *fops)
{
    if( fops == NULL ){ return USBD_FAIL; }
    pdev->pUserData_MIDI = fops;
    return USBD_OK;
}

uint8_t USBD_MIDI_SetRxBuffer(USBD_HandleTypeDef *pdev, uint8_t* pbuf)
{
    UNUSED(pdev);
    hmidi.RxBuffer = pbuf;
    return USBD_OK;
}

uint8_t USBD_MIDI_SetTxBuffer(USBD_HandleTypeDef *pdev, uint8_t* pbuf, uint32_t length)
{
    UNUSED(pdev);
    hmidi.TxBuffer = pbuf;
    hmidi.TxLength = length;
    return USBD_OK;
}

uint8_t USBD_MIDI_ReceivePacket(USBD_HandleTypeDef *pdev)
{
    return USBD_LL_PrepareReceive( pdev, EP_MIDI_OUT, hmidi.RxBuffer, MIDI_EPOUT_SIZE );
}

uint8_t USBD_MIDI_TransmitPacket(USBD_HandleTypeDef *pdev)
{
    if( hmidi.state != MIDI_IDLE ){ return USBD_BUSY; }
    hmidi.state = MIDI_BUSY;
    pdev->ep_in[EP_MIDI_IN & 0xFU].total_length = hmidi.TxLength;
    USBD_LL_Transmit( pdev, EP_MIDI_IN, hmidi.TxBuffer, hmidi.TxLength );
    return USBD_OK;
}
