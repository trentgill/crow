#include "usbd_main.h"

#include "../ll/debug_usart.h"
#include "usbd_midi_interface.h"
#include "uac/usbd_uac.h"
#include "../lib/usb_audio.h" // USB_Audio_Init(), USB_Audio_Enable()

USBD_HandleTypeDef USBD_Device;

// time off the bus when changing mode: long enough that every host sees a
// disconnect rather than a glitch
#define MODE_SWITCH_OFF_MS 250

static uint8_t  audio_wanted = 0;
static uint8_t  switching = 0;
static uint32_t switch_tick = 0;

static void start( uint8_t audio )
{
    USBD_COMPOSITE_Build( audio );
    USB_Audio_Enable( audio );

    if( USBD_Init(&USBD_Device, &VCP_Desc, 0) != USBD_OK ){
        printf("Invalid Device handle\n");
    }
    if( USBD_RegisterClass(&USBD_Device, &USBD_COMPOSITE) != USBD_OK ){
        printf("Invalid class handle\n");
    }
    if( USBD_CDC_ACM_RegisterInterface(&USBD_Device, &USBD_CDC_fops) != USBD_OK ){
        printf("Failed to register serial\n");
    }
    if( USBD_MIDI_RegisterInterface(&USBD_Device, &USBD_MIDI_fops) != USBD_OK ){
        printf("Failed to register MIDI\n");
    }
    USBD_Start(&USBD_Device);
}

void USB_Init( void )
{
    USB_Audio_Init();
    start( audio_wanted );
}

void USB_DeInit(void)
{
}

void USB_set_audio_mode( uint8_t on )
{
    on = !!on;
    if( on == audio_wanted ){ return; }
    audio_wanted = on;
    if( !switching ){
        // leave the bus. the core de-inits the classes, which closes their
        // endpoints and lets each interface reset its state
        // the OTG interrupt is held off (not all interrupts: the HAL waits on
        // the tick while it disconnects) so no transfer completes half way
        HAL_NVIC_DisableIRQ(OTG_FS_IRQn);
        USBD_Stop(&USBD_Device);
        USBD_DeInit(&USBD_Device);
        switching = 1;
        switch_tick = HAL_GetTick();
    } // else: already off the bus, and USB_tick comes back in the new mode
}

uint8_t USB_get_audio_mode( void )
{
    return audio_wanted;
}

void USB_tick( uint32_t now_ms )
{
    if( switching && (now_ms - switch_tick) >= MODE_SWITCH_OFF_MS ){
        switching = 0;
        start( audio_wanted );
    }
    USB_midi_tick( now_ms );
}

extern PCD_HandleTypeDef hpcd;

#ifdef USE_USB_FS
void OTG_FS_IRQHandler(void)
#else
void OTG_HS_IRQHandler(void)
#endif
{
    HAL_PCD_IRQHandler(&hpcd);
}
