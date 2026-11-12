/**
  ******************************************************************************
  * @file    USB_Device/CDC_Standalone/Src/usbd_cdc_interface.c
  * @author  MCD Application Team
  * @version V1.2.1
  * @date    14-April-2017
  * @brief   Source file for USBD CDC interface
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; Copyright (c) 2017 STMicroelectronics International N.V. 
  * All rights reserved.</center></h2>
  *
  * Redistribution and use in source and binary forms, with or without 
  * modification, are permitted, provided that the following conditions are met:
  *
  * 1. Redistribution of source code must retain the above copyright notice, 
  *    this list of conditions and the following disclaimer.
  * 2. Redistributions in binary form must reproduce the above copyright notice,
  *    this list of conditions and the following disclaimer in the documentation
  *    and/or other materials provided with the distribution.
  * 3. Neither the name of STMicroelectronics nor the names of other 
  *    contributors to this software may be used to endorse or promote products 
  *    derived from this software without specific written permission.
  * 4. This software, including modifications and/or derivative works of this 
  *    software, must execute solely and exclusively on microcontroller or
  *    microprocessor devices manufactured by or for STMicroelectronics.
  * 5. Redistribution and use of this software other than as permitted under 
  *    this license is void and will automatically terminate your rights under 
  *    this license. 
  *
  * THIS SOFTWARE IS PROVIDED BY STMICROELECTRONICS AND CONTRIBUTORS "AS IS" 
  * AND ANY EXPRESS, IMPLIED OR STATUTORY WARRANTIES, INCLUDING, BUT NOT 
  * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A 
  * PARTICULAR PURPOSE AND NON-INFRINGEMENT OF THIRD PARTY INTELLECTUAL PROPERTY
  * RIGHTS ARE DISCLAIMED TO THE FULLEST EXTENT PERMITTED BY LAW. IN NO EVENT 
  * SHALL STMICROELECTRONICS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
  * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, 
  * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF 
  * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING 
  * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
  * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
  *
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "usbd_main.h"

#include "../ll/timers.h"
#include "../lib/telemetry.h"

// Private define
#define APP_RX_DATA_SIZE  256 // tmp buffer until the parser runs in main loop
#define APP_TX_DATA_SIZE  1024 // the only tx buffer
#define CONNECTION_DELAY  500 // millisecond delay before sending buffer after connect

static int8_t CDC_Itf_Init(void);
static int8_t CDC_Itf_DeInit(void);
static int8_t CDC_Itf_Control(uint8_t cmd, uint8_t* pbuf, uint16_t length);
static int8_t CDC_Itf_Receive(uint8_t* pbuf, uint32_t *Len);
static int8_t CDC_Itf_TransmitCplt(uint8_t *pbuf, uint32_t *Len, uint8_t epnum);

USBD_CDC_ItfTypeDef USBD_CDC_fops = { CDC_Itf_Init
                                    , CDC_Itf_DeInit
                                    , CDC_Itf_Control
                                    , CDC_Itf_Receive
                                    , CDC_Itf_TransmitCplt
                                    };

// Private vars
USBD_CDC_LineCodingTypeDef LineCoding = { 115200  // baud rate
                                        , 0x00    // stop bits-1
                                        , 0x00    // parity - none
                                        , 0x08    // nb. of bits 8
                                        };

static uint8_t UserRxBuffer[APP_RX_DATA_SIZE];
static uint8_t UserTxBuffer[APP_TX_DATA_SIZE];
static uint32_t UserTxDataLen  = 0;
static uint32_t UserRxDataLen  = 0;

// static TIM_HandleTypeDef  USBTimHandle;
int timer_index;

extern USBD_HandleTypeDef  USBD_Device;

// Private declarations

static void USB_Timer_Callback( int count );

/* Public functions ---------------------------------------------------------*/
void CDC_clear_buffers( void )
{
    for( int i=0; i<APP_RX_DATA_SIZE; i++ ){ UserRxBuffer[i] = 0; }
    for( int i=0; i<APP_TX_DATA_SIZE; i++ ){ UserTxBuffer[i] = 0; }
    UserTxDataLen  = 0;
    UserRxDataLen  = 0;
    USBD_CDC_SetRxBuffer(&USBD_Device, UserRxBuffer);
    USBD_CDC_SetTxBuffer(&USBD_Device, UserTxBuffer, 0);
}

#include "../ll/status_led.h" // increase LED blink speed when USB connected
int timerdelay = 0;
/* Private functions ---------------------------------------------------------*/
static int8_t CDC_Itf_Init(void)
{
    USBD_CDC_SetRxBuffer(&USBD_Device, UserRxBuffer);

    // set the timerdelay, so the TIM can be started but *not* send for
    // the first 100ms to solve ECHO issue on norns. see #137
    timerdelay = CONNECTION_DELAY / CDC_POLLING_INTERVAL;

    Timer_Set_Params( timer_index, CDC_POLLING_INTERVAL/1000.0 );
    Timer_Priority( timer_index, USB_IRQPriority );
    Timer_Start( timer_index, &USB_Timer_Callback );

    //TODO add lua callback when USB connects/disconnects
    //     also provide a flag to check that status
    printf("USB_Init\n");
    status_led_fast(LED_FAST);

    return (USBD_OK);
}

static int8_t CDC_Itf_DeInit(void)
{
    Timer_Stop( timer_index );
    printf("USB_DeInit\n");
    status_led_fast(LED_SLOW);
    return (USBD_OK);
}

static int8_t CDC_Itf_Control (uint8_t cmd, uint8_t* pbuf, uint16_t length)
{
    // Most of this is unimplemented!
    switch( cmd ){
        case CDC_SEND_ENCAPSULATED_COMMAND: printf("itf:send_cmd\n");     break;
        case CDC_GET_ENCAPSULATED_RESPONSE: printf("itf:get_response\n"); break;
        case CDC_SET_COMM_FEATURE:          printf("itf:set_feature\n");  break;
        case CDC_GET_COMM_FEATURE:          printf("itf:get_feature\n");  break;
        case CDC_CLEAR_COMM_FEATURE:        printf("itf:clr_feature\n");  break;
        case CDC_SET_LINE_CODING:
            LineCoding.bitrate    = (uint32_t)(  pbuf[0]
                                              | (pbuf[1] << 8)
                                              | (pbuf[2] << 16)
                                              | (pbuf[3] << 24)
                                              );
            LineCoding.format     = pbuf[4];
            LineCoding.paritytype = pbuf[5];
            LineCoding.datatype   = pbuf[6];
            break;
        case CDC_GET_LINE_CODING:
            pbuf[0] = (uint8_t)(LineCoding.bitrate);
            pbuf[1] = (uint8_t)(LineCoding.bitrate >> 8);
            pbuf[2] = (uint8_t)(LineCoding.bitrate >> 16);
            pbuf[3] = (uint8_t)(LineCoding.bitrate >> 24);
            pbuf[4] = LineCoding.format;
            pbuf[5] = LineCoding.paritytype;
            pbuf[6] = LineCoding.datatype;
            break;
        case CDC_SET_CONTROL_LINE_STATE: break;
        case CDC_SEND_BREAK:             printf("itf:send_brk\n");     break;
        default: printf("itf: default\n"); break;
    }
    return (USBD_OK);
}

// user call copies the data to the tx queue
void USB_tx_enqueue( uint8_t* buf, uint32_t len )
{
    // WARNING: UserTxDataLen having room doesn't necessarily mean
    // the buffer is junk data. There can still be an ongoing transfer
    // using the end of the buffer (usually just the last few bytes).
    // Check USB_tx_is_ready()==1 if you need to be sure no data is lost.
    // especially if *streaming* data to the CDC.
    if( (UserTxDataLen + len) >= APP_TX_DATA_SIZE ){
        Telemetry_count( TM_tx_dropped, len - (APP_TX_DATA_SIZE - UserTxDataLen) );
        len = APP_TX_DATA_SIZE - UserTxDataLen; // stop buffer overflow
    }
    Telemetry_count( TM_tx_bytes, len );
    if( len == 0 ){
        // FIXME? Likely means we're trying to TX when no usb device connected
        //printf("TxBuf full\n"); // TODO memcpy will still run (can rm this warning)
    }
    memcpy( &UserTxBuffer[UserTxDataLen]
          , buf
          , len
          );
    UserTxDataLen += len;
}

size_t USB_tx_space( void )
{
    return (size_t)(APP_TX_DATA_SIZE - UserTxDataLen);
}
int USB_tx_is_ready( void )
{
    USBD_CDC_HandleTypeDef *hcdc = USBD_Device.pClassData;
    return (UserTxDataLen == 0)  // ensure the bufer is empty
        && (hcdc->TxState == 0); // CDC has finished active tx
}

// interrupt sends out any queued data
static void USB_Timer_Callback( int count )
{
    // here we NOP the first 100ms of timer clicks
    // see PR #137. solves ECHO issue on norns.
    if( timerdelay ){ timerdelay--; return; }
    if( UserTxDataLen ){
        if( UserTxDataLen >= APP_TX_DATA_SIZE ){
            //printf("overflow %i\n",(int)UserTxDataLen);
            UserTxDataLen = APP_TX_DATA_SIZE;
        }
        USBD_CDC_SetTxBuffer( &USBD_Device
                            , UserTxBuffer
                            , UserTxDataLen
                            );
        int error = USBD_OK;
        if( (error = USBD_CDC_TransmitPacket(&USBD_Device)) ){
            // This means the buffer is full & hasn't been read
            //printf("CDC_tx failed %i\n", error);
        } else {
            UserTxDataLen = 0; // only clear data if no error
        }
    }
}


// RECEIVE
// on interrupt add to the queue
static int8_t CDC_Itf_Receive( uint8_t* buf, uint32_t *len )
{
    uint32_t length = *len;
    Telemetry_count( TM_rx_bytes, length );
    if( (UserRxDataLen + length) >= APP_RX_DATA_SIZE ){
        length = APP_RX_DATA_SIZE - UserRxDataLen; // stop buffer overflow
    }
    memcpy( &UserRxBuffer[UserRxDataLen]
          , buf
          , length
          );
    UserRxDataLen += length;
    return USBD_OK;
}

uint8_t USB_rx_dequeue_LOCK( uint8_t** buf, uint32_t* len )
{
    if( UserRxDataLen ){ // non-zero means data is present
        *buf = UserRxBuffer;
        *len = UserRxDataLen;
        UserRxDataLen = 0; // reset rx array
        return 1;
    }
    return 0;
}

// ONLY call this at the end of `if( USB_rx_dequeue_LOCK ){}`
void USB_rx_dequeue_UNLOCK( void )
{
    if( !UserRxDataLen ){ // zero means data has been processed
uint32_t old_primask = __get_PRIMASK();
__disable_irq();
        USBD_CDC_ReceivePacket(&USBD_Device); // Receive the next packet
__set_PRIMASK( old_primask );
    } else {
        printf("data in rx_queue means something's wrong.\n");
    }
}

static int8_t CDC_Itf_TransmitCplt(uint8_t *Buf, uint32_t *Len, uint8_t epnum)
{
  UNUSED(Buf);
  UNUSED(Len);
  UNUSED(epnum);

  return (0);
}

/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/
