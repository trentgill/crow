#include "debug_usart.h"

#include <string.h>

#include "str_buffer.h"

USART_HandleTypeDef handusart;
str_buffer_t* str_buf;

#ifdef RELEASE
void Debug_USART_Init(void){ return; }
void U_PrintNow(void){ return; }
void U_Print(char* s, int len){ return; }
#endif // RELEASE

#ifndef RELEASE
void Debug_USART_Init( void )
{
#ifndef TRACE // ie using USART to debug
    handusart.Instance = DBG_USARTx;

    handusart.Init.BaudRate   = DBG_USART_baud;
    handusart.Init.WordLength = USART_WORDLENGTH_8B;
    handusart.Init.StopBits   = USART_STOPBITS_1;
    handusart.Init.Parity     = USART_PARITY_NONE;
    handusart.Init.Mode       = USART_MODE_TX;

    HAL_USART_Init( &handusart );
#endif // TRACE
    str_buf = str_buffer_init( 511 ); // fifo for DMA buffer
}

void Debug_USART_DeInit(void)
{
#ifndef TRACE
    HAL_USART_DeInit( &handusart );
#endif // TRACE
    str_buffer_deinit( str_buf );
}

// LOW LEVEL USART HARDWARE CONFIGURATION FUNCTION
void HAL_USART_MspInit(USART_HandleTypeDef *hu )
{
    if(hu == &handusart){
        static DMA_HandleTypeDef hdma_tx;

        DBG_USART_USART_RCC();
        DBG_USART_GPIO_RCC();
        DBG_DMAx_CLK_ENABLE();

        GPIO_InitTypeDef gpio;
        gpio.Pin       = DBG_USART_TXPIN;
        gpio.Mode      = GPIO_MODE_AF_PP;
        gpio.Pull      = GPIO_PULLUP;
        gpio.Speed     = GPIO_SPEED_FREQ_HIGH;
        gpio.Alternate = DBG_USART_AF;
        HAL_GPIO_Init( DBG_USART_GPIO, &gpio );

        // Configure DMA
        hdma_tx.Instance                 = USARTx_TX_DMA_STREAM;
        hdma_tx.Init.Channel             = USARTx_TX_DMA_CHANNEL;
        hdma_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
        hdma_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
        hdma_tx.Init.MemInc              = DMA_MINC_ENABLE;
        hdma_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
        hdma_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
        hdma_tx.Init.Mode                = DMA_NORMAL;
        hdma_tx.Init.Priority            = DMA_PRIORITY_LOW;
        HAL_DMA_Init( &hdma_tx );
        __HAL_LINKDMA( hu, hdmatx, hdma_tx ); // Associate DMA to USART handle

        // Configure NVIC for DMA
        HAL_NVIC_SetPriority( USARTx_DMA_TX_IRQn
                            , USARTx_DMA_IRQPriority
                            , USARTx_DMA_TXIRQSubPriority
                            );
        HAL_NVIC_EnableIRQ( USARTx_DMA_TX_IRQn );

        HAL_NVIC_SetPriority( USARTx_IRQn
                            , USARTx_IRQSubPriority
                            , USARTx_IRQSubPriority
                            );
        HAL_NVIC_EnableIRQ( USARTx_IRQn );
    }
}

void USARTx_DMA_TX_IRQHandler( void )
{
    HAL_DMA_IRQHandler( handusart.hdmatx );
}

void HAL_USART_TxCpltCallback( USART_HandleTypeDef *husart )
{
}

void HAL_USART_ErrorCallback( USART_HandleTypeDef *husart )
{
    printf( "errorcode %i\n", (int)husart->ErrorCode );
}
void USARTx_IRQHandler( void )
{
    HAL_USART_IRQHandler( &handusart );
}

// Communication Functions
void U_PrintNow( void )
{
#ifndef TRACE
    if( HAL_USART_GetState( &handusart ) == HAL_USART_STATE_READY
     && !str_buffer_empty( str_buf ) ){
        BLOCK_IRQS(
            uint16_t str_len = str_buffer_len( str_buf );
            uint8_t* str = (uint8_t*)str_buffer_dequeue( str_buf, str_len );
            // the fifo is on the heap, which is cached: push it out to RAM for the DMA
            uint32_t line = (uint32_t)str & ~31u;
            SCB_CleanDCache_by_Addr( (uint32_t*)line, (int32_t)((uint32_t)str + str_len - line) );
            HAL_USART_Transmit_DMA( &handusart, str, str_len );
        );
    }
#endif // TRACE
}

void U_Print(char* s, int len)
{
    char str[len+2];
    memcpy( str, s, len );
    str[len] = '\r';
    str[len+1] = '\0';
    BLOCK_IRQS(
        str_buffer_enqueue( str_buf, str );
    );
}
#endif // DEBUG
