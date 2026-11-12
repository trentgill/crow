#pragma once
// host stand-in for the ST device library types used by the CDC interface
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stm32f7xx_hal.h"

#define USBD_STATE_DEFAULT     0x01U
#define USBD_STATE_ADDRESSED   0x02U
#define USBD_STATE_CONFIGURED  0x03U
#define USBD_STATE_SUSPENDED   0x04U

typedef enum{ USBD_OK = 0U
            , USBD_BUSY
            , USBD_EMEM
            , USBD_FAIL
} USBD_StatusTypeDef;

typedef struct{
    volatile uint8_t dev_state;
    void*            pClassData;
} USBD_HandleTypeDef;

typedef struct{ int unused; } USBD_DescriptorsTypeDef;
