#pragma once

// crow's composite USB device: serial + MIDI, and in audio mode serial + MIDI
// + USB Audio. One class for the core, which builds the configuration
// descriptor for the current mode and hands each request, endpoint and frame
// event to the function it belongs to. Interface and endpoint numbers are in
// usbd_map.h.

#include "usbd_ioreq.h"

extern USBD_ClassTypeDef USBD_COMPOSITE;

// build the configuration descriptor for a mode. call before USBD_Start
void USBD_COMPOSITE_Build( uint8_t audio );

// the mode the current descriptor describes
uint8_t USBD_COMPOSITE_IsAudio( void );
