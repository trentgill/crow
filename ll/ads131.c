#include "ads131.h"

#include <stm32f7xx_hal.h>
#include <stdio.h>

#include "timers.h"
#include "debug_pin.h"

#define ADC_FRAMES     3 // status word, plus 2 channels
#define ADC_BUF_SIZE   (ADS_DATAWORDSIZE * ADC_FRAMES)
//#define ADC_BUF_SIZE   (ADC_FRAMES)

#define NSS_DELAY 10000
#define DELAY_usec(u) \
    do{ for( volatile int i=0; i<u; i++ ){;;} \
    } while(0);

SPI_HandleTypeDef adc_spi;
TIM_HandleTypeDef adc_tim;
TIM_OC_InitTypeDef tim_config;
uint16_t aRxBuffer[ADC_BUF_SIZE];
uint16_t aTxBuffer[ADC_BUF_SIZE];

#define ADC_COUNTT 2
uint8_t  adc_count = ADC_COUNTT;
uint32_t adc_samp_count = 64;

float adc_calibrated_scalar[ADC_COUNTT];
float adc_calibrated_shift[ADC_COUNTT];

int timer_index_ads;

void ADS_Init_Sequence(void);
void ADS_Reset_Device(void);
uint8_t ADS_IsReady( void );
uint16_t ADS_Cmd( uint16_t command );
uint16_t ADS_Reg( uint8_t reg_mask, uint8_t val );
void ADC_TxRx( uint16_t* aTxBuffer, uint16_t* aRxBuffer, uint32_t size );
void ADC_Rx( uint16_t* aRxBuffer, uint32_t size );
uint8_t _ADC_CheckErrors( uint16_t error_mask );
//uint8_t _ADC_CheckErrors( uint16_t expect, uint16_t error );

// background wait for NSS delay
static void ADC_UnpickleBlock_Cont( int unused );

#define ADC_U16_TO_V        ((float)(15.0 / 65535.0))

// continuous (audio-rate) acquisition, see ADC_Cont_* below
static void ADC_Cont_Start( void );
volatile int16_t  adc_ring[ADC_COUNTT][ADC_RING_LEN];
volatile uint32_t adc_widx = 0; // total frames written (wraps)
volatile uint32_t adc_frames_resync = 0;
volatile uint32_t adc_frames_badstat = 0;
volatile uint32_t adc_frames_overrun = 0;
static volatile int adc_cont_running = 0;

void ADC_Init( uint16_t bsize, uint8_t chan_count, int timer_ix )
{
    adc_count  = chan_count;
    adc_samp_count = bsize * chan_count;

    // Set the SPI parameters
    adc_spi.Instance               = SPIa;
    adc_spi.Init.Mode              = SPI_MODE_MASTER;
    adc_spi.Init.Direction         = SPI_DIRECTION_2LINES;
    adc_spi.Init.DataSize          = SPI_DATASIZE_16BIT;
    adc_spi.Init.CLKPolarity       = SPI_POLARITY_LOW;
    adc_spi.Init.CLKPhase          = SPI_PHASE_2EDGE;
    adc_spi.Init.NSS               = SPI_NSS_SOFT;
    adc_spi.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_128; // 1.2MHz
    adc_spi.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    adc_spi.Init.TIMode            = SPI_TIMODE_DISABLE;
    adc_spi.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    adc_spi.Init.CRCPolynomial     = 7;
    if(HAL_SPI_Init(&adc_spi) != HAL_OK){ printf("spi_init\n"); }


// 216/27 = 8MHz. then /2 and /2 in the ads clocks so 2MHz fMOD
    // thus 500nS per clock
// then we average OSR to 512 samples
    // so 256uS per clock
    // then we give ~600uS to reset & resettle
    

    //uint32_t prescaler = (uint32_t)((SystemCoreClock / 10000)-1);
    uint32_t period_value = ADC_MCLK_DIV-1; // 216MHz/18 = 12MHz MCLK
    adc_tim.Instance = TIMa;
    adc_tim.Init.Period = period_value;
    // adc_tim.Init.Prescaler = 3; //prescaler;
    adc_tim.Init.Prescaler = 0; //prescaler;
    adc_tim.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    adc_tim.Init.CounterMode = TIM_COUNTERMODE_UP;
    if(HAL_TIM_PWM_Init(&adc_tim) != HAL_OK){ printf("tim_init\n"); }

    tim_config.OCMode       = TIM_OCMODE_PWM1;
    tim_config.OCPolarity   = TIM_OCPOLARITY_HIGH;
    tim_config.OCFastMode   = TIM_OCFAST_DISABLE;
    tim_config.OCNPolarity  = TIM_OCNPOLARITY_HIGH;
    tim_config.OCNIdleState = TIM_OCNIDLESTATE_RESET;
    tim_config.OCIdleState  = TIM_OCIDLESTATE_RESET;
    tim_config.Pulse        = period_value/2;
    if(HAL_TIM_PWM_ConfigChannel(&adc_tim, &tim_config, TIMa_CHANNEL) != HAL_OK){
        printf("tim_config\n");
    }
    if(HAL_TIM_PWM_Start( &adc_tim, TIMa_CHANNEL ) != HAL_OK){
        printf("tim_st\n");
    }

    for( uint8_t i=0; i<ADC_BUF_SIZE; i++ ){
        aRxBuffer[i] = 0;
        aTxBuffer[i] = 0;
    }
    for( uint8_t i=0; i<ADC_COUNTT; i++ ){
        adc_calibrated_scalar[i] = ADC_U16_TO_V;
        adc_calibrated_shift[i] = 2.5;
    }

    // basically the ADC has a *resync* event every time bc the MCLK is not in phase with SPI
    // i tried for ages to get them aligned, but i think it requires 1) hardware change (to use I2S) or
    // 2) big architectural changes in the dsp block processing.

    // instead we have *increased* this delay
    // after NSS goes low, the *resync* error occurs, and the sinc3 filter is reset.
    // it continues sampling while it awaits the SPI transmission.
    // so we increase the delay time to the max, to be able to use the highest OSR ratio
    timer_index_ads = timer_ix;
    Timer_Set_Params( timer_index_ads, 0.00063 ); // 630uS (block is 666uS)
    Timer_Priority( timer_index_ads, ADC_IRQPriority );

    ADS_Init_Sequence();
    ADC_Cont_Start();
}

void ADS_Init_Sequence(void)
{
    ADS_Reset_Device();

    ADS_Cmd(ADS_UNLOCK);
    if( ADS_Cmd(ADS_NULL) != ADS_UNLOCK ){ printf("Can't Unlock\n"); }

    // CONFIGURE REGS HERE
    ADS_Reg(ADS_WRITE_REG | ADS_CLK1, 0x02); // fICLK = MCLK/2
    //printf( "%i\n",ADS_Cmd(ADS_NULL) ); // assert ADS_CLK1
        // 0x02 is /2 which is least divisions (fastest internal)
        // meaning slowest clkout from uc and least noise.
    // ADS_Reg(ADS_WRITE_REG | ADS_CLK2, 0x25); // fMOD = fICLK/2, OSR = 512
    // ADS_Reg(ADS_WRITE_REG | ADS_CLK2, 0x27); // fMOD = fICLK/2, OSR = 384

// weirdly, we choose the OSR value by increasing the multiplier until the DRDY error flag is set
    // we actualy *want* this error flag set, bc it infers the filter has settled
    // continuous mode: MCLK 12MHz -> fICLK 6MHz -> fMOD 3MHz -> OSR 64 -> 46875Hz
    // (= 216MHz / 4608, phase locked to the TIM8 frame clock below)
    ADS_Reg(ADS_WRITE_REG | ADS_CLK2, 0x2D); // fMOD = fICLK/2, OSR = 64
    // ADS_Reg(ADS_WRITE_REG | ADS_CLK2, 0x2a); // fMOD = fICLK/2, OSR = 192
    // 0x2* sets clockdiv to /2 (the least divisions, for slowest MCLK)
    // 0x*b was lowest (ie most oversampling) where -5v actually reached 0x8000
    // using 0x*c for safety
    ADS_Reg(ADS_WRITE_REG | ADS_ADC_ENA, 0x03); // contrasts with datasheet
        // 0x03 is a bitwise mask of enabled channels
        // confirmed that ADS_Reg response agrees
    ADS_Cmd(ADS_WAKEUP);

    ADS_Cmd(ADS_LOCK);
}
void ADS_Reset_Device(void)
{
    ADS_IsReady(); // send query
    if(ADS_IsReady()){ return; } // ready to go!

    // not ready: reset
    HAL_GPIO_WritePin( SPIa_NRST_GPIO_PORT, SPIa_NRST_PIN, 0 );
    HAL_Delay(5); // reset can take up to 4.5ms (p35)
    HAL_GPIO_WritePin( SPIa_NRST_GPIO_PORT, SPIa_NRST_PIN, 1 );
    HAL_Delay(5); // reset can take up to 4.5ms (p35)

    while(!ADS_IsReady()){} // needs timeout
    ADS_Cmd(ADS_NULL);
}
uint16_t ADS_Reg( uint8_t reg_mask, uint8_t val )
{
    uint16_t retval = 0;

    if( (reg_mask & ADS_READ_REG) == ADS_READ_REG ){ // read
        ADS_Cmd( ((uint16_t)reg_mask) << 8);         // send query
        retval = ADS_Cmd( ADS_NULL );                // get response
    } else { // write
        retval = ADS_Cmd( ((uint16_t)reg_mask) << 8 | val );
        // write success assertion
        uint16_t status = ADS_Cmd(ADS_NULL);
        uint16_t expect = ( ((reg_mask & 0x1F) | 0x20)<<8
                          | val
                          );
        if( status != expect ){
            printf("!WREG expect: %x\n", expect);
            printf("received: %x\n", status);
        }
    }
    return retval;
}

uint8_t ADS_IsReady( void )
{
    uint32_t* tx = (uint32_t*)aTxBuffer;
    uint32_t* rx = (uint32_t*)aRxBuffer;
    *tx++ = 0;
    *rx++ = 0;
    ADC_Rx( aRxBuffer, 2 );

    return (aRxBuffer[0] == ADS_READY);
}

uint16_t ADS_Cmd( uint16_t command )
{
    aTxBuffer[0] = command;
    aRxBuffer[0] = 0;
    ADC_TxRx( aTxBuffer, aRxBuffer, 1);

    return aRxBuffer[0];
}

static float last[2] = {0.0,0.0};
void ADC_UnpickleBlock( float*   unpickled
                      , uint16_t bsize
                      )
{
    // copy the most recent bsize frames from the continuous ring
    uint32_t w = adc_widx;
    float* unpick = unpickled;
    for( uint8_t j=0; j<adc_count; j++ ){
        float sc = adc_calibrated_scalar[j];
        float sh = adc_calibrated_shift[j];
        uint32_t r = w - bsize;
        for( uint16_t i=0; i<bsize; i++ ){
            *unpick++ = (float)adc_ring[j][(r+i) & ADC_RING_MASK] * sc + sh;
        }
        last[j] = unpick[-1];
    }
}

float ADC_RawToVolts( uint8_t channel, int16_t raw )
{
    return (float)raw * adc_calibrated_scalar[channel] + adc_calibrated_shift[channel];
}

static void ADC_UnpickleBlock_Cont( int unused )
{
    *aTxBuffer = 0x0000; // NULL command
    BLOCK_IRQS(
        if(HAL_SPI_TransmitReceive_DMA( &adc_spi
                                      , (uint8_t*)aTxBuffer
                                      , (uint8_t*)aRxBuffer
                                      , ADC_FRAMES
                                      ) != HAL_OK ){
            printf("spi_txrx_fail\n");
        }
    );
    Timer_Stop( timer_index_ads );
}

void ADC_CalibrateScalar( uint8_t channel, float scale )
{
    adc_calibrated_scalar[channel] = ADC_U16_TO_V * scale;
}

void ADC_CalibrateShift( uint8_t channel, float volts )
{
    adc_calibrated_shift[channel] = 2.5 + volts;
}

float ADC_GetValue( uint8_t channel )
{
    return last[!!channel]; // force 0/1
}

void ADC_Rx( uint16_t* aRxBuffer, uint32_t size )
{
    // pull !SYNC low
    HAL_GPIO_WritePin( SPIa_NSS_GPIO_PORT, SPIa_NSS_PIN, 0 );
    DELAY_usec(NSS_DELAY);
    BLOCK_IRQS(
        if(HAL_SPI_Receive_DMA( &adc_spi
                              , (uint8_t*)aRxBuffer
                              , size
                              ) != HAL_OK ){
            printf("spi_rx_fail\n");
        }
    );
    // just wait til it's done (for now)
    while (HAL_SPI_GetState(&adc_spi) != HAL_SPI_STATE_READY){;;}
}

void ADC_TxRx( uint16_t* aTxBuffer, uint16_t* aRxBuffer, uint32_t size )
{
    // pull !SYNC low
    HAL_GPIO_WritePin( SPIa_NSS_GPIO_PORT, SPIa_NSS_PIN, 0 );
    DELAY_usec(NSS_DELAY);
    BLOCK_IRQS(
        if(HAL_SPI_TransmitReceive_DMA( &adc_spi
                                      , (uint8_t*)aTxBuffer
                                      , (uint8_t*)aRxBuffer
                                      , size
                                      ) != HAL_OK ){
            printf("spi_txrx_fail\n");
        }
    );
    // just wait til it's done (for now)
    while (HAL_SPI_GetState(&adc_spi) != HAL_SPI_STATE_READY){;;}
}

uint8_t _ADC_CheckErrors( uint16_t error )
{
    uint8_t retval = 0;
    //error = error >> 8;
    if( error != 0 ){ // fault handler
        retval = 1;
        if( error & 0x2 ){} // Data ready fault
        if( error & 0x4 ){ // Resync fault
            //printf("sync\n");
        }
        if( error & 0x8 ){ // Watchdog timer
            printf("watchdog\n");
        }
        if( error & 0x10 ){ // ADC input fault
            //printf("adc_p: %i\n", ADS_Reg( ADS_READ_REG | ADS_STAT_P, 0 ));
            //printf("adc_n: %i\n", ADS_Reg( ADS_READ_REG | ADS_STAT_N, 0 ));
        }
        if( error & 0x20  ){ // SPI fault
            //printf("spi: %i\n", ADS_Reg( ADS_READ_REG | ADS_STAT_S, 0 ));
        }
        if( error & 0x40 ){ // Command fault
            //printf("ADS bad_cmd\n");
            return 2;
        }
    }
    return retval;
}
void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    printf("ads spi_error\n");
    // pull NSS high to cancel any ongoing transmission
    HAL_GPIO_WritePin( SPIa_NSS_GPIO_PORT, SPIa_NSS_PIN, 1 );
}

void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    //printf("txrx-cb\n");
    // signal end of transmission by pulling NSS high
    HAL_GPIO_WritePin( SPIa_NSS_GPIO_PORT, SPIa_NSS_PIN, 1 );
}

void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef *hspi)
{
    //printf("rx-cb\n");
    // signal end of transmission by pulling NSS high
    //_ADC_CheckErrors( aRxBuffer[0] );
    HAL_GPIO_WritePin( SPIa_NSS_GPIO_PORT, SPIa_NSS_PIN, 1 );
}

void HAL_SPI_MspInit(SPI_HandleTypeDef *hspi)
{
    static DMA_HandleTypeDef hdma_rx;
    static DMA_HandleTypeDef hdma_tx;

    GPIO_InitTypeDef  GPIO_InitStruct;

    SPIa_NSS_GPIO_CLK_ENABLE();
    SPIa_SCK_GPIO_CLK_ENABLE();
    SPIa_MISO_GPIO_CLK_ENABLE();
    SPIa_MOSI_GPIO_CLK_ENABLE();

    SPIa_NRST_GPIO_CLK_ENABLE();

    SPIa_CLK_ENABLE();

    SPIa_DMAx_CLK_ENABLE();

    // GPIO pins
    GPIO_InitStruct.Pin       = SPIa_SCK_PIN;
    GPIO_InitStruct.Speed     = GPIO_SPEED_FAST;
    GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Alternate = SPIa_SCK_AF;
    HAL_GPIO_Init(SPIa_SCK_GPIO_PORT, &GPIO_InitStruct);

    GPIO_InitStruct.Pin       = SPIa_MISO_PIN;
    GPIO_InitStruct.Alternate = SPIa_MISO_AF;
    HAL_GPIO_Init(SPIa_MISO_GPIO_PORT, &GPIO_InitStruct);

    GPIO_InitStruct.Pin       = SPIa_MOSI_PIN;
    GPIO_InitStruct.Alternate = SPIa_MOSI_AF;
    HAL_GPIO_Init(SPIa_MOSI_GPIO_PORT, &GPIO_InitStruct);

    // NSS & NRST handled manually
    GPIO_InitStruct.Pin       = SPIa_NSS_PIN;
    GPIO_InitStruct.Pull      = GPIO_PULLUP;
    GPIO_InitStruct.Mode      = GPIO_MODE_OUTPUT_PP;
    HAL_GPIO_Init(SPIa_NSS_GPIO_PORT, &GPIO_InitStruct);

    GPIO_InitStruct.Pin       = SPIa_NRST_PIN;
    GPIO_InitStruct.Pull      = GPIO_PULLUP;
    GPIO_InitStruct.Mode      = GPIO_MODE_OUTPUT_PP;
    HAL_GPIO_Init(SPIa_NRST_GPIO_PORT, &GPIO_InitStruct);

    // pull NSS high immediately (resting state)
    HAL_GPIO_WritePin( SPIa_NRST_GPIO_PORT, SPIa_NRST_PIN, 1 );
    HAL_GPIO_WritePin( SPIa_NSS_GPIO_PORT, SPIa_NSS_PIN, 1 );

    // DMA Streams
    hdma_tx.Instance                 = SPIa_TX_DMA_STREAM;

    hdma_tx.Init.Channel             = SPIa_TX_DMA_CHANNEL;
    hdma_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
    hdma_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_tx.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdma_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
    hdma_tx.Init.Mode                = DMA_NORMAL;
    hdma_tx.Init.Priority            = DMA_PRIORITY_LOW;
    hdma_tx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    hdma_tx.Init.FIFOThreshold       = DMA_FIFO_THRESHOLD_FULL;
    hdma_tx.Init.MemBurst            = DMA_MBURST_INC4;
    hdma_tx.Init.PeriphBurst         = DMA_PBURST_INC4;

    HAL_DMA_Init(&hdma_tx);

    // Associate SPI w/ DMA
    __HAL_LINKDMA(hspi, hdmatx, hdma_tx);

    // Reception:
    hdma_rx.Instance                 = SPIa_RX_DMA_STREAM;

    hdma_rx.Init.Channel             = SPIa_RX_DMA_CHANNEL;
    hdma_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdma_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_rx.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdma_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
    hdma_rx.Init.Mode                = DMA_NORMAL;
    hdma_rx.Init.Priority            = DMA_PRIORITY_LOW;
    hdma_rx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    hdma_rx.Init.FIFOThreshold       = DMA_FIFO_THRESHOLD_FULL;
    hdma_rx.Init.MemBurst            = DMA_MBURST_INC4;
    hdma_rx.Init.PeriphBurst         = DMA_PBURST_INC4;

    HAL_DMA_Init(&hdma_rx);

    // Associate SPI w/ DMA
    __HAL_LINKDMA(hspi, hdmarx, hdma_rx);


    // DMA Priority (should be below IO, but above main process)
    HAL_NVIC_SetPriority( SPIa_DMA_TX_IRQn
                        , SPIa_DMA_TX_IRQPriority
                        , SPIa_DMA_TX_IRQSubPriority
                        );
    HAL_NVIC_EnableIRQ( SPIa_DMA_TX_IRQn );

    HAL_NVIC_SetPriority( SPIa_DMA_RX_IRQn
                        , SPIa_DMA_RX_IRQPriority
                        , SPIa_DMA_RX_IRQSubPriority
                        );
    HAL_NVIC_EnableIRQ( SPIa_DMA_RX_IRQn );

    // Must be lower priority than the above DMA
    HAL_NVIC_SetPriority( SPIa_IRQn
                        , SPIa_IRQPriority
                        , SPIa_IRQSubPriority
                        );
    HAL_NVIC_EnableIRQ( SPIa_IRQn );
}

void HAL_SPI_MspDeInit(SPI_HandleTypeDef *hspi)
{
    static DMA_HandleTypeDef hdma_rx;
    static DMA_HandleTypeDef hdma_tx;

    SPIa_FORCE_RESET();
    SPIa_RELEASE_RESET();

    HAL_GPIO_DeInit(SPIa_SCK_GPIO_PORT, SPIa_SCK_PIN);
    HAL_GPIO_DeInit(SPIa_MISO_GPIO_PORT, SPIa_MISO_PIN);
    HAL_GPIO_DeInit(SPIa_MOSI_GPIO_PORT, SPIa_MOSI_PIN);
    HAL_GPIO_DeInit(SPIa_NSS_GPIO_PORT, SPIa_NSS_PIN);

    HAL_DMA_DeInit(&hdma_tx);
    HAL_DMA_DeInit(&hdma_rx);

    HAL_NVIC_DisableIRQ(SPIa_DMA_TX_IRQn);
    HAL_NVIC_DisableIRQ(SPIa_DMA_RX_IRQn);
    HAL_NVIC_DisableIRQ(SPIa_IRQn);
}

void HAL_TIM_PWM_MspInit(TIM_HandleTypeDef *htim)
{
    GPIO_InitTypeDef  GPIO_InitStruct;
    TIMa_MCLK_GPIO_CLK_ENABLE();

    TIMa_CLK_ENABLE();

    GPIO_InitStruct.Pin       = TIMa_MCLK_PIN;
    GPIO_InitStruct.Pull      = GPIO_PULLUP;
    GPIO_InitStruct.Speed     = GPIO_SPEED_FAST;
    //GPIO_InitStruct.Mode      = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Alternate = TIMa_MCLK_AF;
    HAL_GPIO_Init(TIMa_MCLK_GPIO_PORT, &GPIO_InitStruct);
}

void HAL_TIM_PWM_MspDeInit(TIM_HandleTypeDef *htim)
{
    TIMa_FORCE_RESET();
    TIMa_RELEASE_RESET();

    HAL_GPIO_DeInit(TIMa_MCLK_GPIO_PORT, TIMa_MCLK_PIN);
}

static void ADC_Cont_FrameDone( void );
void SPIa_DMA_RX_IRQHandler(void)
{
    if( adc_cont_running ){ ADC_Cont_FrameDone(); return; }
    HAL_DMA_IRQHandler(adc_spi.hdmarx);
}
void SPIa_DMA_TX_IRQHandler(void)
{
    if( adc_cont_running ){ // shouldn't fire (no TX interrupts enabled). clear all
        DMA2->LIFCR = 0x0F400000; // stream3 flags
        return;
    }
    HAL_DMA_IRQHandler(adc_spi.hdmatx);
}


//////////////////////////////////////////////////////////
// Continuous acquisition
//
// The ADS131A02 runs in synchronous-slave mode with DRDY tied to !CS. It
// expects a DRDY/!CS falling edge exactly once per data period (or a multiple
// of it), latched on its CLKIN (MCLK) edge with 10ns setup/hold. Any other
// edge flags F_RESYNC and resets the sinc3 filter.
//
// So the falling edge is generated in hardware: TIM8 (216MHz, period 4608 =
// one data period = 256 MCLK cycles of TIM2) raises a DMA request on update,
// and DMA2 Stream1 writes GPIOA->BSRR to pull PA4 (!CS) low. No interrupt
// latency is involved in the edge timing.
//
// TIM8 CC1 (a few hundred ns later) interrupts and starts the 3-word SPI frame
// by enabling the pre-configured SPI DMA streams. The RX-complete interrupt
// raises !CS, checks the status word and pushes both channels into adc_ring.

static volatile uint32_t cs_low_word  = (uint32_t)SPIa_NSS_PIN << 16; // BSRR reset
static volatile uint16_t cont_tx[ADC_FRAMES] = {0,0,0};
static volatile uint16_t cont_rx[ADC_FRAMES];
static volatile int      cont_busy = 0;

#define DMA_S0_FLAGS  0x0000003D // LIFCR stream0: TCIF|HTIF|TEIF|DMEIF|FEIF
#define DMA_S3_FLAGS  0x0F400000 // LIFCR stream3
#define DMA_S1_FLAGS  0x00000F40 // LIFCR stream1

static void ADC_Cont_Start( void )
{
    // faster SPI for audio-rate frames: 108MHz/8 = 13.5MHz (ADS max 25MHz)
    adc_spi.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
    HAL_SPI_Init(&adc_spi);

    SPI_TypeDef* spi = adc_spi.Instance;
    // flush any stale rx data
    while( spi->SR & SPI_SR_RXNE ){ (void)*(volatile uint16_t*)&spi->DR; }
    (void)spi->SR;

    DMA_Stream_TypeDef* rx = SPIa_RX_DMA_STREAM;
    DMA_Stream_TypeDef* tx = SPIa_TX_DMA_STREAM;
    rx->CR &= ~DMA_SxCR_EN; while( rx->CR & DMA_SxCR_EN ){}
    tx->CR &= ~DMA_SxCR_EN; while( tx->CR & DMA_SxCR_EN ){}
    DMA2->LIFCR = DMA_S0_FLAGS | DMA_S3_FLAGS;
    rx->PAR  = (uint32_t)&spi->DR;
    rx->M0AR = (uint32_t)cont_rx;
    rx->CR  &= ~(DMA_SxCR_HTIE | DMA_SxCR_TEIE | DMA_SxCR_DMEIE | DMA_SxCR_CIRC);
    rx->CR  |= DMA_SxCR_TCIE;
    rx->FCR &= ~DMA_SxFCR_FEIE;
    tx->PAR  = (uint32_t)&spi->DR;
    tx->M0AR = (uint32_t)cont_tx;
    tx->CR  &= ~(DMA_SxCR_HTIE | DMA_SxCR_TEIE | DMA_SxCR_DMEIE | DMA_SxCR_TCIE | DMA_SxCR_CIRC);
    tx->FCR &= ~DMA_SxFCR_FEIE;
    spi->CR2 |= SPI_CR2_RXDMAEN;
    spi->CR2 |= SPI_CR2_TXDMAEN;
    spi->CR1 |= SPI_CR1_SPE;

    HAL_GPIO_WritePin( SPIa_NSS_GPIO_PORT, SPIa_NSS_PIN, 1 ); // !CS high

    // DMA2 Stream1 ch7 = TIM8_UP : write BSRR to pull !CS low
    __HAL_RCC_DMA2_CLK_ENABLE();
    DMA_Stream_TypeDef* cs = DMA2_Stream1;
    cs->CR = 0; while( cs->CR & DMA_SxCR_EN ){}
    DMA2->LIFCR = DMA_S1_FLAGS;
    cs->PAR  = (uint32_t)&(SPIa_NSS_GPIO_PORT->BSRR);
    cs->M0AR = (uint32_t)&cs_low_word;
    cs->NDTR = 1;
    cs->FCR  = 0; // direct mode
    cs->CR   = (7u << DMA_SxCR_CHSEL_Pos)
             | DMA_SxCR_PL_1                  // high priority
             | DMA_SxCR_MSIZE_1 | DMA_SxCR_PSIZE_1 // 32bit
             | DMA_SxCR_CIRC
             | DMA_SxCR_DIR_0;                // memory -> peripheral
    cs->CR  |= DMA_SxCR_EN;

    // TIM8: frame clock at 216MHz/4608 = 46875Hz
    __HAL_RCC_TIM8_CLK_ENABLE();
    TIM8->CR1  = 0;
    TIM8->PSC  = 0;
    TIM8->ARR  = ADC_FRAME_TICKS - 1;
    TIM8->CCR1 = 64; // ~300ns after !CS falls, start the SPI frame
    TIM8->CCMR1 = 0; // frozen output compare (internal only)
    TIM8->DIER = TIM_DIER_UDE | TIM_DIER_CC1IE;
    TIM8->EGR  = TIM_EGR_UG;
    TIM8->SR   = 0;

    HAL_NVIC_SetPriority( TIM8_CC_IRQn, ADC_IRQPriority, 0 );
    HAL_NVIC_EnableIRQ( TIM8_CC_IRQn );

    adc_cont_running = 1;
    TIM8->CR1 |= TIM_CR1_CEN;
}

void TIM8_CC_IRQHandler( void )
{
    TIM8->SR = ~TIM_SR_CC1IF;
    if( cont_busy ){ adc_frames_overrun++; return; }
    cont_busy = 1;
    DMA_Stream_TypeDef* rx = SPIa_RX_DMA_STREAM;
    DMA_Stream_TypeDef* tx = SPIa_TX_DMA_STREAM;
    DMA2->LIFCR = DMA_S0_FLAGS | DMA_S3_FLAGS;
    rx->NDTR = ADC_FRAMES;
    tx->NDTR = ADC_FRAMES;
    rx->CR |= DMA_SxCR_EN;
    tx->CR |= DMA_SxCR_EN;
}

static void ADC_Cont_FrameDone( void )
{
    DMA2->LIFCR = DMA_S0_FLAGS | DMA_S3_FLAGS;
    // wait for the final SCLK to finish before releasing !CS
    SPI_TypeDef* spi = adc_spi.Instance;
    while( spi->SR & SPI_SR_BSY ){}
    SPIa_NSS_GPIO_PORT->BSRR = SPIa_NSS_PIN; // !CS high
    SPIa_TX_DMA_STREAM->CR &= ~DMA_SxCR_EN;

    uint16_t st = cont_rx[0];
    if( (st & 0xFF00) != 0x2200 ){
        adc_frames_badstat++;
    } else if( st & 0x0004 ){ // F_RESYNC
        adc_frames_resync++;
    }
    uint32_t w = adc_widx & ADC_RING_MASK;
    adc_ring[0][w] = (int16_t)cont_rx[1];
    adc_ring[1][w] = (int16_t)cont_rx[2];
    adc_widx++;
    cont_busy = 0;
}

void ADC_GetStats( uint32_t* frames, uint32_t* resync, uint32_t* badstat, uint32_t* overrun, uint16_t* status )
{
    *frames  = adc_widx;
    *resync  = adc_frames_resync;
    *badstat = adc_frames_badstat;
    *overrun = adc_frames_overrun;
    *status  = cont_rx[0];
}

void ADC_ShiftPhase( int ticks )
{
    // nudge the frame clock phase relative to MCLK (for experiments)
    BLOCK_IRQS(
        int c = (int)TIM8->CNT + ticks;
        while( c < 0 ){ c += ADC_FRAME_TICKS; }
        TIM8->CNT = (uint32_t)(c % ADC_FRAME_TICKS);
    );
}
void SPIa_IRQHandler(void)
{
    HAL_SPI_IRQHandler(&adc_spi);
}
