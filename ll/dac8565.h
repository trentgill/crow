#pragma once

#include <stm32f7xx.h>
#include "interrupts.h" // DAC_IRQPriority

// Defn for I2S
#define I2Sx                             SPI2
#define I2Sx_CLK_ENABLE()                __HAL_RCC_SPI2_CLK_ENABLE()
#define I2Sx_DMAx_CLK_ENABLE()           __HAL_RCC_DMA1_CLK_ENABLE()
#define I2Sx_NSS_GPIO_CLK_ENABLE()       __HAL_RCC_GPIOB_CLK_ENABLE()
#define I2Sx_SCK_GPIO_CLK_ENABLE()       __HAL_RCC_GPIOB_CLK_ENABLE()
#define I2Sx_MOSI_GPIO_CLK_ENABLE()      __HAL_RCC_GPIOB_CLK_ENABLE()
#define I2Sx_NRST_GPIO_CLK_ENABLE()      __HAL_RCC_GPIOB_CLK_ENABLE()

#define I2Sx_FORCE_RESET()               __HAL_RCC_SPI2_FORCE_RESET()
#define I2Sx_RELEASE_RESET()             __HAL_RCC_SPI2_RELEASE_RESET()

// Definition for I2Sx Pins
#define I2Sx_NSS_PIN                     GPIO_PIN_12
#define I2Sx_NSS_GPIO_PORT               GPIOB
#define I2Sx_NSS_AF                      GPIO_AF5_SPI2
#define I2Sx_SCK_PIN                     GPIO_PIN_13
#define I2Sx_SCK_GPIO_PORT               GPIOB
#define I2Sx_SCK_AF                      GPIO_AF5_SPI2
#define I2Sx_MOSI_PIN                    GPIO_PIN_15
#define I2Sx_MOSI_GPIO_PORT              GPIOB
#define I2Sx_MOSI_AF                     GPIO_AF5_SPI2
#define I2Sx_NRST_PIN                    GPIO_PIN_14
#define I2Sx_NRST_GPIO_PORT              GPIOB

// Definition for SPId's DMA
#define I2Sx_TX_DMA_CHANNEL              DMA_CHANNEL_0
#define I2Sx_TX_DMA_STREAM               DMA1_Stream4

// Definition for SPId's NVIC
#define I2Sx_IRQn                        SPI2_IRQn
#define I2Sx_IRQHandler                  SPI2_IRQHandler
#define I2Sx_DMA_TX_IRQn                 DMA1_Stream4_IRQn
#define I2Sx_DMA_TX_IRQHandler           DMA1_Stream4_IRQHandler

#define I2Sx_IRQPriority             DAC_IRQPriority
#define I2Sx_IRQSubPriority          2
#define I2Sx_DMA_TX_IRQPriority      DAC_IRQPriority
#define I2Sx_DMA_TX_IRQSubPriority   1

// dac8565 defines
#define DAC8565_SET_ONE        ((uint8_t)0b00010000)
#define DAC8565_PREP_ONE       ((uint8_t)0x0)
#define DAC8565_SET_AND_UPDATE ((uint8_t)0b00100000)
#define DAC8565_SET_ALL        ((uint8_t)0b00110100)
#define DAC8565_REFRESH_ALL    ((uint8_t)0b00110000)

void DAC_Init( uint16_t bsize, uint8_t chan_count );
void DAC_Start(void);

uint16_t DAC_GetFrame( void ); // sample frame the DMA is reading: 0 .. 2*bsize-1

void DAC_CalibrateScalar( uint8_t channel, float scale );
void DAC_CalibrateOffset( uint8_t channel, float volts );
void DAC_PickleBlock( uint32_t* dac_pickle_ptr
                    , float*    unpickled_data
                    , uint16_t  bsize
                    );

void I2Sx_DMA_TX_IRQHandler(void);
void I2Sx_IRQHandler(void);

void HAL_I2S_TxCpltCallback( I2S_HandleTypeDef *hi2s );
void HAL_I2S_TxHalfCpltCallback( I2S_HandleTypeDef *hi2s );
void HAL_I2S_ErrorCallback( I2S_HandleTypeDef *hi2s );

void DAC_I2S_MspInit(I2S_HandleTypeDef *hi2s);
void DAC_I2S_MspDeInit(I2S_HandleTypeDef *hi2s);
