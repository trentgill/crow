#pragma once

#include "stm32f7xx_hal.h"
#include "usbd_core.h"
#include "usbd_desc.h"
#include "usbd_composite.h"
#include "cdc_acm/usbd_cdc_acm.h"
#include "usbd_cdc_interface.h"

#define USE_USB_FS

void USB_Init(void);
void USB_DeInit(void);

// Switch audio mode on or off. crow leaves the bus and comes back as the other
// device, so the host sees it unplugged and replugged. Returns at once: the
// switch happens over the next ~250ms in USB_tick.
void USB_set_audio_mode( uint8_t on );
uint8_t USB_get_audio_mode( void ); // the mode asked for (may still be switching)

// call each ms from the main loop
void USB_tick( uint32_t now_ms );

#ifdef USE_USB_FS
void OTG_FS_IRQHandler(void);
#else
void OTG_HS_IRQHandler(void);
#endif
