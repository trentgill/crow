#include "ads131.h"

#include <stm32f7xx_hal.h>
#include <stdio.h>
#include <string.h>

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

// -------- sync experiments (adc-sync branch) --------
// mode 0: legacy. CS/DRDY low at block start, read after wait_us (resyncs every block)
// mode 1: software edge at block start, read straight away
// mode 2: edge placed on a DWT-cycle grid (t0 + n*period + offset), read straight away
ADC_cfg_t adc_cfg = { .mode       = 4
                    , .tim_period = 18   // 12MHz CLKIN: a 32-sample block is 8192 CLKIN cycles
                    , .clk1       = 0x02 // fICLK = CLKIN/2
                    , .clk2       = 0x2D // fMOD = fICLK/2 = 3MHz, OSR 64 -> 46.875kHz
                    , .ena        = 0x03
                    , .spi_br     = SPI_BAUDRATEPRESCALER_16 // 13.5MHz
                    , .wait_us    = 630
                    , .period     = 147456
                    , .offset     = 0
                    , .margin     = 4000
                    , .spb        = 32
                    , .edge       = 1016 // 8 cycles after the CLKIN edge that samples DRDY: DMA can only be late
                    , .txcmd      = 0
                    };
ADC_dbg_t adc_dbg;
static volatile uint8_t adc_paused = 0;
static volatile uint8_t adc_streaming = 0;
static uint8_t  grid_started = 0;
static uint32_t grid_target  = 0;
static int32_t  grid_offset_applied = 0;

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
static uint8_t m4_running;
static void m4_unpickle( float* unpickled, uint16_t bsize );
static void m4_start( void );

#define ADC_U16_TO_V        ((float)(15.0 / 65535.0))

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
    adc_spi.Init.BaudRatePrescaler = adc_cfg.spi_br; // /128 -> 1.69MHz (PCLK2 216MHz)
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
    uint32_t period_value = adc_cfg.tim_period - 1; // 27 -> 8MHz
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
    tim_config.Pulse        = adc_cfg.tim_period/2;
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
    Timer_Set_Params( timer_index_ads, adc_cfg.wait_us * 1e-6 ); // 630uS (block is 682uS)
    Timer_Priority( timer_index_ads, ADC_IRQPriority );

    // cycle counter is the timebase for the grid-aligned edge
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->LAR = 0xC5ACCE55;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    ADS_Init_Sequence();
    if( adc_cfg.mode == 4 ){
        m4_start();
    } else {
        adc_streaming = 1;
    }
}

void ADS_Init_Sequence(void)
{
    ADS_Reset_Device();

    ADS_Cmd(ADS_UNLOCK);
    if( ADS_Cmd(ADS_NULL) != ADS_UNLOCK ){ printf("Can't Unlock\n"); }

    adc_dbg.stat_m2 = ADS_Reg(ADS_READ_REG | ADS_STAT_M2, 0);
    ADS_Reg(ADS_READ_REG | ADS_STAT_S, 0); // clears the F_SPI left over from power-up

    // CONFIGURE REGS HERE
    ADS_Reg(ADS_WRITE_REG | ADS_CLK1, adc_cfg.clk1); // 0x02: fICLK = MCLK/2
    //printf( "%i\n",ADS_Cmd(ADS_NULL) ); // assert ADS_CLK1
        // 0x02 is /2 which is least divisions (fastest internal)
        // meaning slowest clkout from uc and least noise.
    // ADS_Reg(ADS_WRITE_REG | ADS_CLK2, 0x25); // fMOD = fICLK/2, OSR = 512
    // ADS_Reg(ADS_WRITE_REG | ADS_CLK2, 0x27); // fMOD = fICLK/2, OSR = 384

// weirdly, we choose the OSR value by increasing the multiplier until the DRDY error flag is set
    // we actualy *want* this error flag set, bc it infers the filter has settled
    ADS_Reg(ADS_WRITE_REG | ADS_CLK2, adc_cfg.clk2); // 0x28: fMOD = fICLK/2, OSR = 256
    // ADS_Reg(ADS_WRITE_REG | ADS_CLK2, 0x2a); // fMOD = fICLK/2, OSR = 192
    // 0x2* sets clockdiv to /2 (the least divisions, for slowest MCLK)
    // 0x*b was lowest (ie most oversampling) where -5v actually reached 0x8000
    // using 0x*c for safety
    ADS_Reg(ADS_WRITE_REG | ADS_ADC_ENA, adc_cfg.ena); // 0x03 contrasts with datasheet
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
    if( m4_running ){
        uint32_t now = DWT->CYCCNT;
        if( adc_dbg.blk_last ){
            uint32_t d = now - adc_dbg.blk_last;
            if( adc_dbg.blk_n == 0 || d < adc_dbg.blk_min ){ adc_dbg.blk_min = d; }
            if( d > adc_dbg.blk_max ){ adc_dbg.blk_max = d; }
            adc_dbg.blk_sum += d;
            adc_dbg.blk_n++;
        }
        adc_dbg.blk_last = now;
        m4_unpickle( unpickled, bsize );
        return;
    }
    float* unpick = unpickled;
    // Return current buf
    for( uint8_t j=0; j<adc_count; j++ ){
        // cast to signed -> cast to float -> scale -> shift
        if( j == adc_cap_ch && adc_cap_n < adc_cap_len ){ adc_cap[adc_cap_n++] = ((int16_t*)aRxBuffer)[j+1]; }
        float once = ((float)((int16_t*)aRxBuffer)[j+1]) // +1 past status byte
                        * adc_calibrated_scalar[j]
                        + adc_calibrated_shift[j];
        for( uint16_t i=0; i<bsize; i++ ){
            *unpick++ = once; // copy single value into whole block
        }
        last[j] = once;
    }

    // block period, measured on the cycle counter
    uint32_t now = DWT->CYCCNT;
    if( adc_dbg.blk_last ){
        uint32_t d = now - adc_dbg.blk_last;
        if( adc_dbg.blk_n == 0 || d < adc_dbg.blk_min ){ adc_dbg.blk_min = d; }
        if( d > adc_dbg.blk_max ){ adc_dbg.blk_max = d; }
        adc_dbg.blk_sum += d;
        adc_dbg.blk_n++;
    }
    adc_dbg.blk_last = now;

    if( adc_paused ){ return; }

    // Request next buffer
    if (HAL_SPI_GetState(&adc_spi) != HAL_SPI_STATE_READY){
        adc_dbg.busy++;
        return;
    }
    switch( adc_cfg.mode ){
        case 0: // legacy: resync now, read once the filter has settled
            HAL_GPIO_WritePin( SPIa_NSS_GPIO_PORT, SPIa_NSS_PIN, 0 );
            Timer_Start( timer_index_ads, &ADC_UnpickleBlock_Cont );
            break;
        case 1: // software edge, read immediately
            SPIa_NSS_GPIO_PORT->BSRR = (uint32_t)SPIa_NSS_PIN << 16;
            ADC_UnpickleBlock_Cont( 0 );
            break;
        case 2: { // edge on the cycle-counter grid
            if( !grid_started ){
                grid_started = 1;
                grid_target = now + adc_cfg.margin;
                grid_offset_applied = adc_cfg.offset;
            } else {
                grid_target += adc_cfg.period + (adc_cfg.offset - grid_offset_applied);
                grid_offset_applied = adc_cfg.offset;
            }
            int32_t ahead = (int32_t)(grid_target - DWT->CYCCNT);
            if( ahead < 0 || ahead > (int32_t)(2*adc_cfg.margin) ){
                adc_dbg.late++; // missed (or wildly early): restart the grid here
                grid_target = DWT->CYCCNT + adc_cfg.margin;
            }
            uint32_t prim = __get_PRIMASK();
            __disable_irq();
            while( (int32_t)(DWT->CYCCNT - grid_target) < 0 ){;}
            SPIa_NSS_GPIO_PORT->BSRR = (uint32_t)SPIa_NSS_PIN << 16;
            __set_PRIMASK( prim );
            uint32_t spin = DWT->CYCCNT - now;
            if( spin > adc_dbg.spin_max ){ adc_dbg.spin_max = spin; }
            ADC_UnpickleBlock_Cont( 0 );
            break; }
    }
}

// ---------------- mode 4: timer + DMA streaming ----------------
// TIM8 is slaved to TIM2 (ITR1) so it starts on the same tick: its period is a
// whole number of CLKIN cycles, so every edge lands on the same CLKIN phase.
// Per sample period (counter 0..ARR):
//   UP   -> DMA2 S1 ch7 : GPIOA->BSRR = PA4 high (end previous frame)
//   CC1  -> DMA2 S2 ch7 : GPIOA->BSRR = PA4 low  (cs + drdy falling edge)
//   CC2,3,4 -> DMA2 S3,S4,S7 ch7 : SPI1->DR = word (16 SCLKs each)
//   SPI1 RX -> DMA2 S0 ch3 circular into rx_ring
// No CPU per sample. The block ISR reads the ring 32 samples behind the writer.
#define RING_SAMPS 64
#define RING_WORDS (RING_SAMPS * ADC_FRAMES)
static volatile uint16_t rx_ring[RING_WORDS];
static uint32_t bsrr_lo = (uint32_t)SPIa_NSS_PIN << 16;
static uint32_t bsrr_hi = (uint32_t)SPIa_NSS_PIN;
volatile uint16_t tx_word0 = 0;
static uint16_t tx_zero = 0;
static uint32_t m4_rd = 0;     // next sample to consume
static uint8_t  m4_primed = 0;
static float    m4_prev[2];
int16_t* adc_cap = NULL;
uint32_t adc_cap_len = 0;
volatile uint32_t adc_cap_n = 0; // == len: idle
uint8_t  adc_cap_ch = 0;

#define DMA_CR(ch, dir, circ, minc, psize, msize, pl) \
    ( ((uint32_t)(ch) << 25) | ((pl) << 16) | ((msize) << 13) | ((psize) << 11) \
    | ((minc) << 10) | ((circ) << 8) | ((dir) << 6) )

static void m4_stream( DMA_Stream_TypeDef* st, uint32_t cr, volatile void* par
                     , volatile void* mar, uint32_t n )
{
    st->CR &= ~DMA_SxCR_EN;
    while( st->CR & DMA_SxCR_EN ){;}
    st->CR   = cr;
    st->PAR  = (uint32_t)par;
    st->M0AR = (uint32_t)mar;
    st->NDTR = n;
    st->FCR  = 0; // direct mode
}

static void m4_clear_flags( void )
{
    DMA2->LIFCR = 0x0F7D0F7D; // streams 0..3
    DMA2->HIFCR = 0x0F7D0F7D; // streams 4..7
}

static void m4_stop( void )
{
    if( !m4_running ){ return; }
    m4_running = 0;
    TIM8->CR1 &= ~TIM_CR1_CEN;
    TIM8->DIER = 0;
    TIM8->SMCR = 0;
    DMA_Stream_TypeDef* sts[] = { DMA2_Stream0, DMA2_Stream1, DMA2_Stream2
                                , DMA2_Stream3, DMA2_Stream4, DMA2_Stream7 };
    for( int i=0; i<6; i++ ){
        sts[i]->CR &= ~DMA_SxCR_EN;
        while( sts[i]->CR & DMA_SxCR_EN ){;}
    }
    m4_clear_flags();
    TIM2->CR2 &= ~TIM_CR2_MMS;
    // the timer may have stopped mid-frame: wait it out, then reset the
    // peripheral and let HAL rebuild spi + its dma streams from scratch
    for( volatile int i=0; i<2000 && (SPIa->SR & SPI_SR_BSY); i++ ){;}
    SPIa_FORCE_RESET();
    SPIa_RELEASE_RESET();
    adc_spi.State = HAL_SPI_STATE_RESET;
    HAL_SPI_Init( &adc_spi ); // -> HAL_SPI_MspInit: gpio, dma, nvic
    SPIa_NSS_GPIO_PORT->BSRR = bsrr_hi;
}

static void m4_start( void )
{
    uint32_t period = adc_cfg.period / adc_cfg.spb; // cpu cycles per sample
    __HAL_RCC_TIM8_CLK_ENABLE();
    __HAL_RCC_DMA2_CLK_ENABLE();

    // spi: rx by dma, tx by timer writes to DR
    SPIa->CR1 &= ~SPI_CR1_SPE;
    while( SPIa->SR & SPI_SR_FRLVL ){ (void)*(volatile uint16_t*)&SPIa->DR; }
    SPIa->CR2 = (SPIa->CR2 & ~SPI_CR2_TXDMAEN) | SPI_CR2_RXDMAEN;

    for( int i=0; i<RING_WORDS; i++ ){ rx_ring[i] = 0; }
    tx_word0 = adc_cfg.txcmd;

    // streams: (ch, dir 0=p2m 1=m2p, circ, minc, psize, msize, prio)
    m4_stream( DMA2_Stream0, DMA_CR(3,0,1,1,1,1,3), &SPIa->DR, rx_ring, RING_WORDS );
    m4_stream( DMA2_Stream1, DMA_CR(7,1,1,0,2,2,2), &GPIOA->BSRR, &bsrr_hi, 1 );
    m4_stream( DMA2_Stream2, DMA_CR(7,1,1,0,2,2,3), &GPIOA->BSRR, &bsrr_lo, 1 );
    m4_stream( DMA2_Stream3, DMA_CR(7,1,1,0,1,1,2), &SPIa->DR, &tx_word0, 1 );
    m4_stream( DMA2_Stream4, DMA_CR(7,1,1,0,1,1,2), &SPIa->DR, &tx_zero, 1 );
    m4_stream( DMA2_Stream7, DMA_CR(7,1,1,0,1,1,2), &SPIa->DR, &tx_zero, 1 );
    m4_clear_flags();
    DMA2_Stream0->CR |= DMA_SxCR_EN;
    SPIa->CR1 |= SPI_CR1_SPE;

    // tim8: same 216MHz clock as tim2
    uint32_t word = 16 * (2u << ((adc_cfg.spi_br >> 3) & 7)); // cycles per 16-bit word
    TIM8->CR1  = 0;
    TIM8->PSC  = 0;
    TIM8->ARR  = period - 1;
    TIM8->CCMR1 = 0; TIM8->CCMR2 = 0; // frozen: compare match flags only
    TIM8->CCR1 = adc_cfg.edge;
    TIM8->CCR2 = adc_cfg.edge + 64;
    TIM8->CCR3 = adc_cfg.edge + 64 + word + 32;
    TIM8->CCR4 = adc_cfg.edge + 64 + 2*(word + 32);
    TIM8->CNT  = 0;
    TIM8->EGR  = TIM_EGR_UG;         // load PSC/ARR
    TIM8->SR   = 0;
    DMA2_Stream1->CR |= DMA_SxCR_EN;
    DMA2_Stream2->CR |= DMA_SxCR_EN;
    DMA2_Stream3->CR |= DMA_SxCR_EN;
    DMA2_Stream4->CR |= DMA_SxCR_EN;
    DMA2_Stream7->CR |= DMA_SxCR_EN;
    TIM8->DIER = TIM_DIER_UDE | TIM_DIER_CC1DE | TIM_DIER_CC2DE | TIM_DIER_CC3DE | TIM_DIER_CC4DE;
    TIM8->SMCR = (1 << TIM_SMCR_TS_Pos) | (6 << TIM_SMCR_SMS_Pos); // trigger on ITR1 (TIM2)

    // restart tim2 so tim8 starts on the same tick
    BLOCK_IRQS(
        TIM2->CR1 &= ~TIM_CR1_CEN;
        TIM2->CR2 = (TIM2->CR2 & ~TIM_CR2_MMS) | (1 << TIM_CR2_MMS_Pos); // TRGO = enable
        TIM2->CNT = 0;
        TIM2->CR1 |= TIM_CR1_CEN;
    );
    m4_primed = 0;
    m4_running = 1;
}

static inline float m4_volts( int16_t raw, int ch ){
    return (float)raw * adc_calibrated_scalar[ch] + adc_calibrated_shift[ch];
}

// fill the block from the ring. called from the block ISR.
static void m4_unpickle( float* unpickled, uint16_t bsize )
{
    uint32_t wr = (RING_WORDS - DMA2_Stream0->NDTR) / ADC_FRAMES; // samples complete (mod ring)
    uint32_t spb = adc_cfg.spb;
    uint32_t lag = spb + spb/2;
    if( m4_primed < 3 ){ // first blocks: one STAT_S read clears F_SPI from the restart
        if( m4_primed == 0 ){ m4_rd = (wr + RING_SAMPS - lag) % RING_SAMPS; }
        tx_word0 = (m4_primed == 1) ? 0x2500 : adc_cfg.txcmd;
        m4_primed++;
    }
    uint32_t dist = (wr + RING_SAMPS - m4_rd) % RING_SAMPS;
    adc_dbg.ring_dist = dist;
    if( dist < spb + 2 || dist > lag + spb/2 + 2 ){ // re-centre
        adc_dbg.slips++;
        m4_rd = (wr + RING_SAMPS - lag) % RING_SAMPS;
    }
    uint16_t stor = 0;
    float* out0 = unpickled;
    float* out1 = unpickled + bsize;
    uint32_t r = m4_rd;
    if( spb == bsize ){
        for( uint32_t i=0; i<spb; i++ ){
            volatile uint16_t* f = &rx_ring[r * ADC_FRAMES];
            stor |= f[0];
            if( (f[0] & 0xFF00) != 0x2200 ){ adc_dbg.last_other = f[0]; }
            out0[i] = m4_volts( (int16_t)f[1], 0 );
            out1[i] = m4_volts( (int16_t)f[2], 1 );
            if( adc_cap_n < adc_cap_len ){ adc_cap[adc_cap_n++] = (int16_t)f[1+adc_cap_ch]; }
            if( ++r == RING_SAMPS ){ r = 0; }
        }
    } else { // linear interpolation up to bsize
        uint32_t k = bsize / spb;
        float ik = 1.0f / (float)k;
        for( uint32_t i=0; i<spb; i++ ){
            volatile uint16_t* f = &rx_ring[r * ADC_FRAMES];
            stor |= f[0];
            if( (f[0] & 0xFF00) != 0x2200 ){ adc_dbg.last_other = f[0]; }
            if( adc_cap_n < adc_cap_len ){ adc_cap[adc_cap_n++] = (int16_t)f[1+adc_cap_ch]; }
            float a = m4_volts( (int16_t)f[1], 0 );
            float b = m4_volts( (int16_t)f[2], 1 );
            float da = (a - m4_prev[0]) * ik;
            float db = (b - m4_prev[1]) * ik;
            for( uint32_t j=0; j<k; j++ ){
                m4_prev[0] += da; *out0++ = m4_prev[0];
                m4_prev[1] += db; *out1++ = m4_prev[1];
            }
            m4_prev[0] = a; m4_prev[1] = b;
            if( ++r == RING_SAMPS ){ r = 0; }
        }
        out0 = unpickled; out1 = unpickled + bsize;
    }
    m4_rd = r;
    last[0] = out0[bsize-1];
    last[1] = out1[bsize-1];
    adc_dbg.frames += spb;
    if( stor & 0x7F ){ // count flags per sample only when any were set
        uint32_t q = (r + RING_SAMPS - spb) % RING_SAMPS;
        for( uint32_t i=0; i<spb; i++ ){
            uint16_t st = rx_ring[q * ADC_FRAMES];
            for( int b=0; b<7; b++ ){ if( st & (1<<b) ){ adc_dbg.stat_bits[b]++; } }
            adc_dbg.last_status = st;
            if( ++q == RING_SAMPS ){ q = 0; }
        }
    }
}

// called from the main loop (lua). blocks for ~20ms.
void ADC_Reconfig( void )
{
    m4_stop();
    adc_paused = 1;
    adc_streaming = 0;
    Timer_Stop( timer_index_ads );
    while( HAL_SPI_GetState(&adc_spi) != HAL_SPI_STATE_READY ){;;}
    HAL_GPIO_WritePin( SPIa_NSS_GPIO_PORT, SPIa_NSS_PIN, 1 );

    // master clock
    __HAL_TIM_SET_AUTORELOAD( &adc_tim, adc_cfg.tim_period - 1 );
    __HAL_TIM_SET_COMPARE( &adc_tim, TIMa_CHANNEL, adc_cfg.tim_period/2 );
    __HAL_TIM_SET_COUNTER( &adc_tim, 0 );

    // spi rate
    __HAL_SPI_DISABLE( &adc_spi );
    MODIFY_REG( adc_spi.Instance->CR1, SPI_CR1_BR, adc_cfg.spi_br );
    adc_spi.Init.BaudRatePrescaler = adc_cfg.spi_br;

    Timer_Set_Params( timer_index_ads, adc_cfg.wait_us * 1e-6 );

    // force a hardware reset so the new clocks + regs take
    HAL_GPIO_WritePin( SPIa_NRST_GPIO_PORT, SPIa_NRST_PIN, 0 );
    HAL_Delay(2);
    HAL_GPIO_WritePin( SPIa_NRST_GPIO_PORT, SPIa_NRST_PIN, 1 );
    HAL_Delay(6);
    ADS_Init_Sequence();

    grid_started = 0;
    ADC_DebugReset();
    if( adc_cfg.mode == 4 ){
        m4_start();
    } else {
        adc_streaming = 1;
    }
    adc_paused = 0;
}

void ADC_DebugReset( void )
{
    uint32_t m2 = adc_dbg.stat_m2;
    BLOCK_IRQS(
        memset( &adc_dbg, 0, sizeof(adc_dbg) );
        adc_dbg.stat_m2 = m2;
    );
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
    // signal end of transmission by pulling NSS high
    SPIa_NSS_GPIO_PORT->BSRR = SPIa_NSS_PIN;
    if( adc_streaming ){
        uint16_t st = aRxBuffer[0];
        adc_dbg.frames++;
        adc_dbg.last_status = st;
        if( (st & 0xFF00) != 0x2200 ){ adc_dbg.bad_header++; }
        else {
            for( int b=0; b<7; b++ ){
                if( st & (1<<b) ){ adc_dbg.stat_bits[b]++; }
            }
        }
    }
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

void SPIa_DMA_RX_IRQHandler(void)
{
    HAL_DMA_IRQHandler(adc_spi.hdmarx);
}
void SPIa_DMA_TX_IRQHandler(void)
{
    HAL_DMA_IRQHandler(adc_spi.hdmatx);
}
void SPIa_IRQHandler(void)
{
    HAL_SPI_IRQHandler(&adc_spi);
}
