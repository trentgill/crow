#pragma once

// USB MIDI 1.0 function: one cable, one MIDI IN and one MIDI OUT jack.
// Started from atrium's AL94 MIDI class. The descriptor now carries the Audio
// Control interface the MIDI 1.0 spec puts ahead of MIDI Streaming, so it can
// sit beside serial (and audio) in a composite device.

#include "usbd_ioreq.h"

#define MIDI_EPIN_SIZE      0x40
#define MIDI_EPOUT_SIZE     0x40

typedef enum{ MIDI_IDLE = 0
            , MIDI_BUSY
} MIDI_StateTypeDef;

typedef struct _USBD_MIDI_Itf{
    int8_t (*Init)(void);
    int8_t (*DeInit)(void);
    int8_t (*Receive)(uint8_t *Buf, uint32_t *Len);
    int8_t (*TransmitCplt)(uint8_t *Buf, uint32_t *Len, uint8_t epnum);
} USBD_MIDI_ItfTypeDef;

typedef struct{
    volatile MIDI_StateTypeDef state;
    uint8_t* RxBuffer;
    uint32_t RxLength;
    uint8_t* TxBuffer;
    uint32_t TxLength;
} USBD_MIDI_HandleTypeDef;

extern USBD_ClassTypeDef USBD_MIDI;

// append the function's descriptors (AC, MS, jacks, endpoints) at buf.
// returns the number of bytes written
uint16_t USBD_MIDI_Descriptor( uint8_t* buf );

uint8_t USBD_MIDI_RegisterInterface(USBD_HandleTypeDef *pdev, USBD_MIDI_ItfTypeDef *fops);
uint8_t USBD_MIDI_SetRxBuffer(USBD_HandleTypeDef *pdev, uint8_t* pbuf);
uint8_t USBD_MIDI_SetTxBuffer(USBD_HandleTypeDef *pdev, uint8_t* pbuf, uint32_t length);
uint8_t USBD_MIDI_ReceivePacket(USBD_HandleTypeDef *pdev);
uint8_t USBD_MIDI_TransmitPacket(USBD_HandleTypeDef *pdev);
