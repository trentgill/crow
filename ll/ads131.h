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

typedef struct{
    uint8_t  mode;
    uint8_t  tim_period; // TIM2 ticks per CLKIN (216MHz / n)
    uint8_t  clk1, clk2, ena;
    uint32_t spi_br;     // SPI_BAUDRATEPRESCALER_*
    uint32_t wait_us;    // mode 0 read delay
    uint32_t period;     // mode 2 grid spacing, cpu cycles
    int32_t  offset;     // mode 2 grid offset, cpu cycles
    uint32_t margin;     // mode 2 lead time, cpu cycles
    uint32_t spb;        // mode 4 samples per block (8 or 32)
    uint32_t edge;       // mode 4 cs/drdy falling edge, cycles into each sample period
    uint32_t txcmd;      // mode 4 first word of every frame (0 = NULL)
} ADC_cfg_t;

typedef struct{
    uint32_t frames;
    uint32_t stat_bits[7]; // STAT_1 bit counts: 0 check,1 drdy,2 resync,3 wdt,4 adcin,5 spi,6 opc
    uint32_t bad_header;
    uint32_t last_status;
    uint32_t stat_m2;
    uint32_t busy, late, spin_max;
    uint32_t blk_last, blk_n, blk_min, blk_max;
    uint64_t blk_sum;
    uint32_t slips, ring_dist, last_other;
} ADC_dbg_t;

extern int16_t* adc_cap;   // malloc'd by adc_debug('capture')
extern uint32_t adc_cap_len;
extern volatile uint32_t adc_cap_n;
extern uint8_t adc_cap_ch;
extern ADC_cfg_t adc_cfg;
extern ADC_dbg_t adc_dbg;
void ADC_Reconfig( void );
void ADC_DebugReset( void );

void ADC_Init( uint16_t bsize, uint8_t chan_count, int timer_ix );

//int32_t
uint16_t ADC_GetU16( uint8_t channel );
void ADC_UnpickleBlock( float*   unpickled
                      , uint16_t bsize
                      );
float ADC_GetValue( uint8_t channel );

// average every ADC sample: drop `skip` samples, then sum `count` (max 65536)
void ADC_Average( uint32_t skip, uint32_t count );
// 0 while still summing, else the sample count, with the mean of each input in volts[0..1]
int ADC_AverageResult( float* volts );

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
