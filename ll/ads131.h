#pragma once

#include <stm32f7xx.h>
#include "interrupts.h" // ADC_IRQPriority

// Defs for MCLK pin, using hardware timer
#define TIMa                             TIM2
#define TIMa_CLK_ENABLE()                __HAL_RCC_TIM2_CLK_ENABLE()

#define TIMa_FORCE_RESET()               __HAL_RCC_TIM2_FORCE_RESET()
#define TIMa_RELEASE_RESET()             __HAL_RCC_TIM2_RELEASE_RESET()

#define TIMa_MCLK_GPIO_CLK_ENABLE()      __HAL_RCC_GPIOA_CLK_ENABLE()

#define TIMa_MCLK_PIN                    GPIO_PIN_3
#define TIMa_MCLK_GPIO_PORT              GPIOA
#define TIMa_MCLK_AF                     GPIO_AF1_TIM2 // this is PWM AF
#define TIMa_CHANNEL                     TIM_CHANNEL_4

// Definition for SPIa clock resources
#define SPIa                             SPI1
#define SPIa_CLK_ENABLE()                __HAL_RCC_SPI1_CLK_ENABLE()
#define SPIa_DMAx_CLK_ENABLE()           __HAL_RCC_DMA2_CLK_ENABLE()
#define SPIa_NSS_GPIO_CLK_ENABLE()       __HAL_RCC_GPIOA_CLK_ENABLE()
#define SPIa_SCK_GPIO_CLK_ENABLE()       __HAL_RCC_GPIOA_CLK_ENABLE()
#define SPIa_MISO_GPIO_CLK_ENABLE()      __HAL_RCC_GPIOA_CLK_ENABLE()
#define SPIa_MOSI_GPIO_CLK_ENABLE()      __HAL_RCC_GPIOA_CLK_ENABLE()

#define SPIa_FORCE_RESET()               __HAL_RCC_SPI1_FORCE_RESET()
#define SPIa_RELEASE_RESET()             __HAL_RCC_SPI1_RELEASE_RESET()

// Definition for SPIa Pins
#define SPIa_NSS_PIN                     GPIO_PIN_4
#define SPIa_NSS_GPIO_PORT               GPIOA
#define SPIa_SCK_PIN                     GPIO_PIN_5
#define SPIa_SCK_GPIO_PORT               GPIOA
#define SPIa_SCK_AF                      GPIO_AF5_SPI1
#define SPIa_MISO_PIN                    GPIO_PIN_6
#define SPIa_MISO_GPIO_PORT              GPIOA
#define SPIa_MISO_AF                     GPIO_AF5_SPI1
#define SPIa_MOSI_PIN                    GPIO_PIN_7
#define SPIa_MOSI_GPIO_PORT              GPIOA
#define SPIa_MOSI_AF                     GPIO_AF5_SPI1

#define SPIa_NRST_GPIO_CLK_ENABLE()      __HAL_RCC_GPIOC_CLK_ENABLE()
#define SPIa_NRST_PIN                    GPIO_PIN_4
#define SPIa_NRST_GPIO_PORT              GPIOC

// Definition for SPIa's DMA
#define SPIa_TX_DMA_CHANNEL              DMA_CHANNEL_3
#define SPIa_TX_DMA_STREAM               DMA2_Stream3
#define SPIa_RX_DMA_CHANNEL              DMA_CHANNEL_3
#define SPIa_RX_DMA_STREAM               DMA2_Stream0

// Definition for SPIa's NVIC
#define SPIa_IRQn                        SPI1_IRQn
#define SPIa_IRQHandler                  SPI1_IRQHandler
#define SPIa_DMA_TX_IRQn                 DMA2_Stream3_IRQn
#define SPIa_DMA_RX_IRQn                 DMA2_Stream0_IRQn
#define SPIa_DMA_TX_IRQHandler           DMA2_Stream3_IRQHandler
#define SPIa_DMA_RX_IRQHandler           DMA2_Stream0_IRQHandler

#define SPIa_IRQPriority             ADC_IRQPriority
#define SPIa_IRQSubPriority          2
#define SPIa_DMA_TX_IRQPriority      ADC_IRQPriority
#define SPIa_DMA_TX_IRQSubPriority   1
#define SPIa_DMA_RX_IRQPriority      ADC_IRQPriority
#define SPIa_DMA_RX_IRQSubPriority   0

// ADS131 commands
#define ADS_READY   0xFF02

#define ADS_NULL    0x0
#define ADS_RESET   0x0011
#define ADS_STANDBY 0x0022
#define ADS_WAKEUP  0x0033
#define ADS_LOCK    0x0555
#define ADS_UNLOCK  0x0655
//#define ADS_UNLOCK  0x5506

#define ADS_READ_REG  0x20
#define ADS_WRITE_REG 0x40

// ads131 status registers
#define ADS_STAT_1    0x02
#define ADS_STAT_P    0x03
#define ADS_STAT_N    0x04
#define ADS_STAT_S    0x05
#define ADS_ERROR_CNT 0x06
#define ADS_STAT_M2   0x07

// ads131 user config registers
#define ADS_A_SYS_CFG 0x0B
#define ADS_D_SYS_CFG 0x0C
#define ADS_CLK1      0x0D
#define ADS_CLK2      0x0E
#define ADS_ADC_ENA   0x0F

#define ADS_DATAWORDSIZE 0x2 // 16bit, pin M1 floats

// continuous acquisition
#define ADC_MCLK_DIV     18     // TIM2: 216MHz / 18 = 12MHz MCLK
#define ADC_FRAME_TICKS  4608   // TIM8: 216MHz / 4608 = 46875Hz frames
#define ADC_SAMPLE_RATE  46875.0f
#define ADC_RING_LEN     2048   // ~44ms per channel
#define ADC_RING_MASK    (ADC_RING_LEN-1)
extern volatile int16_t  adc_ring[2][ADC_RING_LEN];
extern volatile uint32_t adc_widx;
float ADC_RawToVolts( uint8_t channel, int16_t raw );
void ADC_GetStats( uint32_t* frames, uint32_t* resync, uint32_t* badstat, uint32_t* overrun, uint16_t* status );
void ADC_ShiftPhase( int ticks );

void ADC_Init( uint16_t bsize, uint8_t chan_count, int timer_ix );

//int32_t
uint16_t ADC_GetU16( uint8_t channel );
void ADC_UnpickleBlock( float*   unpickled
                      , uint16_t bsize
                      );
float ADC_GetValue( uint8_t channel );

void ADC_CalibrateScalar( uint8_t channel, float scale );
void ADC_CalibrateShift( uint8_t channel, float volts );

void SPIa_DMA_RX_IRQHandler(void);
void SPIa_DMA_TX_IRQHandler(void);
void SPIa_IRQHandler(void);

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi);
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi);
void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef *hspi);

void HAL_SPI_MspInit(SPI_HandleTypeDef *hspi);
void HAL_SPI_MspDeInit(SPI_HandleTypeDef *hspi);
