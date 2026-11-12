#include "dac8565.h"

#include <stdio.h>
#include "stdlib.h" // malloc()
#include "debug_pin.h"

#include "adda.h"   // ADDA_BlockProcess()
#include "wrMath.h" // lim_f()
#include "../lib/telemetry.h"

#define DAC_BUFFER_COUNT 2 // ping-pong

I2S_HandleTypeDef dac_i2s;

// pointer to the malloc()d buffer from DAC_Init()
uint32_t  samp_count = 0;
uint32_t* samples = NULL;

#define DAC_ZERO_VOLTS      ((uint16_t)(((uint32_t)0xFFFF * 2)/3))
#define DAC_V_TO_U16        ((float)(65535.0 / 15.0))
#define DAC_CHANNELSS 4
float dac_calibrated_offset[DAC_CHANNELSS];
float dac_calibrated_scalar[DAC_CHANNELSS];

void DAC_Init( uint16_t bsize, uint8_t chan_count )
{
    // Create the sample buffer for DMA transfer
    samp_count = DAC_BUFFER_COUNT * bsize * chan_count;
    samples = malloc( sizeof(uint32_t) * samp_count );
    for( int i=0; i<samp_count; i++ ){ samples[i] = 0; } // unnecessary
    if(samples == NULL){ printf("DAC_buffer\n"); }

    for( int j=0; j<DAC_CHANNELSS; j++ ){
        dac_calibrated_offset[j] = 0.0;
        dac_calibrated_scalar[j] = DAC_V_TO_U16;
    }

    // Set the SPI parameters
    dac_i2s.Instance          = I2Sx;
    dac_i2s.Init.Mode         = I2S_MODE_MASTER_TX;
    dac_i2s.Init.Standard     = I2S_STANDARD_PCM_SHORT;
    dac_i2s.Init.DataFormat   = I2S_DATAFORMAT_24B;
    dac_i2s.Init.MCLKOutput   = I2S_MCLKOUTPUT_ENABLE;
    dac_i2s.Init.AudioFreq    = I2S_AUDIOFREQ_192K; // manipulates mclk to hit 48kHz on 4 chans
    dac_i2s.Init.CPOL         = I2S_CPOL_LOW;
    dac_i2s.Init.ClockSource  = I2S_CLOCK_PLL;

    if(HAL_I2S_Init(&dac_i2s) != HAL_OK){ printf("i2s_init\n"); }

    // NRST & NSS both high
    HAL_GPIO_WritePin( I2Sx_NRST_GPIO_PORT, I2Sx_NRST_PIN, 0 );
    volatile uint32_t t=10000; while(t){t--;}
    HAL_GPIO_WritePin( I2Sx_NRST_GPIO_PORT, I2Sx_NRST_PIN, 1 );
}

/* Initialize the DMA buffer to contain required metadata
 * Each channel is initialized with its relevant SPI command
 * The last channel latches all the new values to the outputs
 *
 * */
void DAC_Start(void)
{
    // prepare SPI packet metadata
    uint32_t* dbuf = samples;
    for( uint16_t i=0; i<samp_count; i++ ){
        uint8_t* addr_ptr = ((uint8_t*)dbuf) + 1;
        *addr_ptr = (i%4 == 3)
                        ? DAC8565_SET_AND_UPDATE | 3 << 1
                        : DAC8565_PREP_ONE | (i%4) << 1;
        dbuf++;
    }

    // set all channels to 0v (0xAAAA)
    dbuf = samples;
    for( uint16_t i=0; i<samp_count; i++ ){
        uint8_t* addr_ptr = ((uint8_t*)dbuf) + 0;
        *addr_ptr = 0xAA;
        addr_ptr += 3;
        *addr_ptr = 0xAA;
        dbuf++;
    }
    // begin i2s transmission
    int error;
    BLOCK_IRQS(
        error = HAL_I2S_Transmit_DMA( &dac_i2s
                            , (uint16_t*)samples
                            , samp_count
                            );
    );
    if(error){ printf("i2s failed to start\n"); }
}

void DAC_CalibrateScalar( uint8_t channel, float scale )
{
    dac_calibrated_scalar[channel] = DAC_V_TO_U16 * scale;
}

void DAC_CalibrateOffset( uint8_t channel, float volts )
{
    dac_calibrated_offset[channel] = volts;
}

int32_t lim_i32_u16( int32_t v )
{
    return (v > (int32_t)(uint16_t)0xFFFF) ? 0xFFFF : (v < (int32_t)0) ? 0 : v;
}

/* Does all the work converting a generic representation into serial packets
 * Convert floats (representing volts) to u16 representation
 * Interleave a block of each channel into a stream
 * */
void DAC_PickleBlock( uint32_t* dac_pickle_ptr
                    , float*    unpickled_data
                    , uint16_t  bsize
                    )
{
    for( uint8_t j=0; j<4; j++ ){
        add_vf_f( &(unpickled_data[j*bsize])
                , dac_calibrated_offset[j]
                , &(unpickled_data[j*bsize])
                , bsize
                );
    }
    for( uint8_t j=0; j<4; j++ ){
        mul_vf_f( &(unpickled_data[j*bsize])
                , dac_calibrated_scalar[j] // scale volts up to u16
                , bsize
                );
    }

    uint16_t usixteens[bsize * 4];
    uint16_t* usixp = usixteens;

    for( uint16_t i=0; i<bsize; i++ ){
        for( uint8_t j=0; j<4; j++ ){
            *usixp++ = (uint16_t)lim_i32_u16( DAC_ZERO_VOLTS
                                   //- (int32_t)(*u[j]++)
                                   - (int32_t)(unpickled_data[i+j*bsize])
                                 );
        }
    }

    uint8_t* insert_p = (uint8_t*)dac_pickle_ptr;
    usixp = usixteens;

    for( uint16_t i=0; i<(bsize*4); i++ ){
        *insert_p = *usixp>>8;
        insert_p += 3;
        *insert_p++ = *usixp++ & 0xFF;
    }
}



// This wraps the (un)pickling functions in ll/addac.c
void HAL_I2S_TxHalfCpltCallback( I2S_HandleTypeDef *hi2s )
{
    Debug_Pin_Set(1);
    uint32_t t = TM_CYCLES();
    ADDA_BlockProcess( samples );
    Telemetry_dsp( TM_CYCLES() - t );
    Debug_Pin_Set(0);
}
void HAL_I2S_TxCpltCallback( I2S_HandleTypeDef *hi2s )
{
    Debug_Pin_Set(1);
    uint32_t t = TM_CYCLES();
    ADDA_BlockProcess( &samples[samp_count/2] );
    Telemetry_dsp( TM_CYCLES() - t );
    Debug_Pin_Set(0);
}
void HAL_I2S_ErrorCallback( I2S_HandleTypeDef *hi2s )
{
    printf("i2s_tx_error\n");
}


// Driver Config
void HAL_I2S_MspInit(I2S_HandleTypeDef *hi2s)
{
    // I2S PLL Config
    RCC_PeriphCLKInitTypeDef RCC_ExCLKInitStruct;

    // PLLI2S_VCO = f(VCO clock) = f(PLLI2S clock input) × (PLLI2SN/PLLM)
    // I2SCLK = f(PLLI2S clock output) = f(VCO clock) / PLLI2SR
    RCC_ExCLKInitStruct.PeriphClockSelection = RCC_PERIPHCLK_I2S;
    RCC_ExCLKInitStruct.I2sClockSelection = RCC_I2SCLKSOURCE_PLLI2S;
    RCC_ExCLKInitStruct.PLLI2S.PLLI2SN = 344; // mul factor: 50~432
    RCC_ExCLKInitStruct.PLLI2S.PLLI2SR = 2; // div factor: 2~7
    HAL_RCCEx_PeriphCLKConfig(&RCC_ExCLKInitStruct);

    // SPI/I2S & DMA
    I2Sx_CLK_ENABLE();
    I2Sx_DMAx_CLK_ENABLE();

    // GPIO pins
    I2Sx_NSS_GPIO_CLK_ENABLE();
    I2Sx_SCK_GPIO_CLK_ENABLE();
    I2Sx_MOSI_GPIO_CLK_ENABLE();
    I2Sx_NRST_GPIO_CLK_ENABLE();

    GPIO_InitTypeDef  GPIO_InitStruct;

    GPIO_InitStruct.Pin       = I2Sx_SCK_PIN;
    GPIO_InitStruct.Speed     = GPIO_SPEED_FAST;
    GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Alternate = I2Sx_SCK_AF;
    HAL_GPIO_Init(I2Sx_SCK_GPIO_PORT, &GPIO_InitStruct);

    GPIO_InitStruct.Pin       = I2Sx_MOSI_PIN;
    GPIO_InitStruct.Alternate = I2Sx_MOSI_AF;
    HAL_GPIO_Init(I2Sx_MOSI_GPIO_PORT, &GPIO_InitStruct);

    GPIO_InitStruct.Pin       = I2Sx_NSS_PIN;
    GPIO_InitStruct.Alternate = I2Sx_NSS_AF;
    HAL_GPIO_Init(I2Sx_NSS_GPIO_PORT, &GPIO_InitStruct);

    // NRST handled manually
    GPIO_InitStruct.Pin       = I2Sx_NRST_PIN;
    GPIO_InitStruct.Pull      = GPIO_PULLUP;
    GPIO_InitStruct.Mode      = GPIO_MODE_OUTPUT_PP;
    HAL_GPIO_Init(I2Sx_NRST_GPIO_PORT, &GPIO_InitStruct);

    // DMA Streams
    static DMA_HandleTypeDef hdma_tx;

    hdma_tx.Instance                 = I2Sx_TX_DMA_STREAM;

    hdma_tx.Init.Channel             = I2Sx_TX_DMA_CHANNEL;
    hdma_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
    hdma_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_tx.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdma_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;
    hdma_tx.Init.Mode                = DMA_CIRCULAR;
    hdma_tx.Init.Priority            = DMA_PRIORITY_LOW;
    hdma_tx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    hdma_tx.Init.FIFOThreshold       = DMA_FIFO_THRESHOLD_FULL;
    hdma_tx.Init.MemBurst            = DMA_MBURST_INC4;
    hdma_tx.Init.PeriphBurst         = DMA_PBURST_INC4;

    HAL_DMA_Init(&hdma_tx);

    // Associate I2S w/ DMA
    __HAL_LINKDMA(hi2s, hdmatx, hdma_tx);

    // DMA Priority (should be below IO, but above main process)
    HAL_NVIC_SetPriority( I2Sx_DMA_TX_IRQn
                        , I2Sx_DMA_TX_IRQPriority
                        , I2Sx_DMA_TX_IRQSubPriority
                        );
    HAL_NVIC_EnableIRQ( I2Sx_DMA_TX_IRQn );

    // Must be lower priority than the above DMA
    HAL_NVIC_SetPriority( I2Sx_IRQn
                        , I2Sx_IRQPriority
                        , I2Sx_IRQSubPriority
                        );
    HAL_NVIC_EnableIRQ( I2Sx_IRQn );
}

void DAC_I2S_MspDeInit(I2S_HandleTypeDef *hi2s)
{

  static DMA_HandleTypeDef hdma_tx;

  I2Sx_FORCE_RESET();
  I2Sx_RELEASE_RESET();

  HAL_GPIO_DeInit(I2Sx_NSS_GPIO_PORT,  I2Sx_NSS_PIN);
  HAL_GPIO_DeInit(I2Sx_SCK_GPIO_PORT,  I2Sx_SCK_PIN);
  HAL_GPIO_DeInit(I2Sx_MOSI_GPIO_PORT, I2Sx_MOSI_PIN);

  HAL_DMA_DeInit(&hdma_tx);

  HAL_NVIC_DisableIRQ(I2Sx_DMA_TX_IRQn);
  HAL_NVIC_DisableIRQ(I2Sx_IRQn);
}

void I2Sx_DMA_TX_IRQHandler(void)
{
    HAL_DMA_IRQHandler(dac_i2s.hdmatx);
}
void I2Sx_IRQHandler(void)
{
    HAL_I2S_IRQHandler(&dac_i2s);
}
