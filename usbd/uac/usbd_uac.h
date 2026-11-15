#pragma once

// USB Audio Class 1 function: 4 outputs from the host, 2 inputs to it, 16 bit
// 48kHz, asynchronous with explicit feedback. One of the functions the
// composite class (usbd_composite.c) carries in audio mode. The samples
// themselves live in lib/usb_audio.c.

#include <stdint.h>

#ifndef USBD_UAC_DESCRIPTOR_ONLY // tests/usb/uac_desc.py builds just the descriptor
#include "usbd_ioreq.h"

extern const USBD_ClassTypeDef USBD_UAC; // const: flash, not DTCM
#endif

// append the function's descriptors (IAD, audio control, both streaming
// interfaces) to a configuration descriptor. returns the bytes written
uint16_t USBD_UAC_Descriptor( uint8_t* buf );
#define USBD_UAC_DESC_SIZE 182
