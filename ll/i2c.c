#include "i2c.h"

#include <stm32f7xx_hal.h>
#include <string.h>
#include <stdio.h>

#include "lib/caw.h" // Caw_send_luachunk (pullup suggestion)

////////////////////////////
// type definitions

// can probably replace with HAL state machine?
typedef enum{ OP_NULL
            , OP_ERROR
            , OP_LISTEN
            , OP_LEAD_TX
            , OP_LEAD_RX
            , OP_FOLLOW_TX
            , OP_FOLLOW_TX_READY
            , OP_FOLLOW_TX_WAIT // read request matched, answer not ready: SCL held
            , OP_FOLLOW_RX
            , OP_ABORT
} I2C_OP_t;

typedef struct{
    I2C_OP_t operation;
    uint8_t  address;
    uint8_t  size;
    uint8_t  command; // store the request
    uint8_t  data[I2C_MAX_CMD_BYTES];
    uint8_t  tx[I2C_MAX_CMD_BYTES]; // what we send as leader: the caller's buffer can change mid-send
} I2C_State_t;


///////////////////////////////////////
// global vars & objects

static I2C_HandleTypeDef i2c_handle;

static I2C_lead_callback_t   lead_response;
static I2C_follow_callback_t follow_action;
static I2C_request_callback_t follow_request;
static I2C_error_callback_t  error_action;

static I2C_State_t buf;
static uint8_t pullup_state = 1;

static uint32_t i2c_timings = I2C_TIMING_STABLE; // default timings

I2C_Stats_t i2c_stats;
static uint32_t busy_since = 0; // DWT cycles when we last left OP_LISTEN, 0 = listening

static void busy_start( void ){ if( !busy_since ){ busy_since = DWT->CYCCNT | 1; } }
static void busy_end( void ){
    if( busy_since ){
        uint32_t us = (DWT->CYCCNT - busy_since) / (SystemCoreClock / 1000000);
        if( us > i2c_stats.busy_max_us ){ i2c_stats.busy_max_us = us; }
        busy_since = 0;
    }
}

// Event trace, for finding out how the driver got stuck. The ring stops
// recording at the first reset so it holds the run-up to it. ii_trace() reads
// it; ii_trace('restart') clears it. It is not cleared at boot.
#ifdef TELEMETRY
#define TRACE_LEN 64
typedef struct{ uint32_t t; uint16_t isr; uint8_t ev, op, hal, arg; } I2C_Trace_t;
// in .noinit, so a trace survives a crash reset and can be read after it
__attribute__((section(".noinit"))) static I2C_Trace_t trace[TRACE_LEN];
__attribute__((section(".noinit"))) static volatile uint32_t trace_n;
__attribute__((section(".noinit"))) static volatile uint32_t trace_frozen;
static void TRACE( uint8_t ev, uint8_t arg )
{
    if( trace_frozen ){ return; }
    BLOCK_IRQS(
        I2C_Trace_t* e = &trace[trace_n++ % TRACE_LEN];
        e->t   = DWT->CYCCNT;
        e->isr = (uint16_t)(I2Cx->ISR & 0x7FFF) | ((I2Cx->ISR & I2C_ISR_DIR) ? 0x8000 : 0); // bit15: DIR, not BUSY
        e->ev  = ev;
        e->op  = buf.operation;
        e->hal = i2c_handle.State;
        e->arg = arg;
    );
}
int I2C_GetTrace( int ix, uint32_t* t, uint32_t* packed )
{
    uint32_t n = trace_n;
    uint32_t count = n < TRACE_LEN ? n : TRACE_LEN;
    if( ix >= (int)count ){ return 0; }
    I2C_Trace_t* e = &trace[(n - count + ix) % TRACE_LEN];
    *t = e->t;
    packed[0] = e->isr; packed[1] = e->ev; packed[2] = e->op; packed[3] = e->hal; packed[4] = e->arg;
    return 1;
}
void I2C_TraceRestart( void ){ trace_n = 0; trace_frozen = 0; }
#else
#define TRACE(ev, arg)
int I2C_GetTrace( int ix, uint32_t* t, uint32_t* packed ){ return 0; }
void I2C_TraceRestart( void ){}
#endif

void I2C_GetDiag( I2C_Diag_t* d )
{
    BLOCK_IRQS(
        d->isr       = I2Cx->ISR;
        d->cr1       = I2Cx->CR1;
        d->cr2       = I2Cx->CR2;
        d->hal_state = i2c_handle.State;
        d->hal_mode  = i2c_handle.Mode;
        d->hal_error = i2c_handle.ErrorCode;
        d->op        = buf.operation;
    );
    d->scl = !!(I2Cx_SCL_GPIO_PORT->IDR & I2Cx_SCL_PIN);
    d->sda = !!(I2Cx_SDA_GPIO_PORT->IDR & I2Cx_SDA_PIN);
}

void I2C_ResetStats( void )
{
    BLOCK_IRQS( memset( &i2c_stats, 0, sizeof(i2c_stats) ); );
}


//////////////////////////////
// public definitions

uint8_t I2C_Init( uint8_t               address
                , I2C_lead_callback_t   lead_callback
                , I2C_follow_callback_t follow_action_callback
                , I2C_request_callback_t follow_request_callback
                , I2C_error_callback_t  error_callback
                )
{
    uint8_t error = 0;

    lead_response  = lead_callback;
    follow_action  = follow_action_callback;
    follow_request = follow_request_callback;
    error_action   = error_callback;

    i2c_handle.Instance              = I2Cx;
    i2c_handle.Init.Timing           = i2c_timings;
    i2c_handle.Init.OwnAddress1      = address << 1; // correct MSB justification
    i2c_handle.Init.AddressingMode   = I2C_ADDRESSINGMODE_7BIT;
    i2c_handle.Init.DualAddressMode  = I2C_DUALADDRESS_DISABLE;
    i2c_handle.Init.GeneralCallMode  = I2C_GENERALCALL_DISABLE;
    i2c_handle.Init.NoStretchMode    = I2C_NOSTRETCH_DISABLE;

    if( HAL_I2C_Init( &i2c_handle ) != HAL_OK ){ error = 1; }

    buf.operation = OP_NULL;
    buf.address   = 0;
    buf.size      = 0;
    buf.command   = 0;
    memset( buf.data, 0, I2C_MAX_CMD_BYTES );

    HAL_I2C_ListenCpltCallback( &i2c_handle );
    return error;
}

void I2C_DeInit( void )
{
    HAL_I2C_DeInit( &i2c_handle );
}

// 0: slow mode
// 1: fast mode
// 2: classic mode (use old timings from v3.1)
// anything higher than 16 is treated as a raw timing value for experimentation
void I2C_SetTimings( uint32_t is_fast )
{
    I2C_DeInit();
    // update to custom speed, else fallback to stable timing
    if(is_fast > 0xF){
        printf("setting i2c timing to 0x%x\n\r", (unsigned int)is_fast);
        i2c_timings = is_fast;
    } else {
        switch(is_fast){ case 1:  i2c_timings = I2C_TIMING_SPEED; break;
                         case 2:  i2c_timings = I2C_TIMING_CLASSIC; break;
                         default: i2c_timings = I2C_TIMING_STABLE; break; }
    }
    // re-init the i2c driver in order to activate new timings.
    if( lead_response != NULL ){
        I2C_Init( I2C_GetAddress()
                , lead_response
                , follow_action
                , follow_request
                , error_action
                );
    }
}

uint8_t I2C_is_boot( void )
{
    uint8_t boot = 0;
    GPIO_InitTypeDef gpio;
    I2Cx_SCL_GPIO_CLK_ENABLE();
    gpio.Pin   = I2Cx_SCL_PIN;
    gpio.Mode  = GPIO_MODE_INPUT;
    gpio.Pull  = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init( I2Cx_SCL_GPIO_PORT, &gpio );

    if( !HAL_GPIO_ReadPin( I2Cx_SCL_GPIO_PORT, I2Cx_SCL_PIN ) ){
        boot = 1;
    }

    // set to OD to ensure no interaction with i2c line
    gpio.Mode = GPIO_MODE_AF_OD;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init( I2Cx_SCL_GPIO_PORT, &gpio );

    HAL_GPIO_DeInit( I2Cx_SCL_GPIO_PORT, I2Cx_SCL_PIN );
    return boot;
}

void HAL_I2C_MspInit( I2C_HandleTypeDef* h )
{
    GPIO_InitTypeDef gpio;
    I2Cx_SCL_GPIO_CLK_ENABLE();
    I2Cx_SDA_GPIO_CLK_ENABLE();
    I2Cx_CLK_ENABLE();

    gpio.Pin       = I2Cx_SCL_PIN
                   | I2Cx_SDA_PIN;
    gpio.Mode      = GPIO_MODE_AF_OD;
    gpio.Pull      = (pullup_state) ? GPIO_PULLUP : GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = I2Cx_SCL_SDA_AF;
    HAL_GPIO_Init( I2Cx_SCL_GPIO_PORT, &gpio );

    HAL_NVIC_SetPriority( I2Cx_ER_IRQn
                        , I2Cx_ER_Priority
                        , I2Cx_ER_SubPriority
                        );
    HAL_NVIC_EnableIRQ( I2Cx_ER_IRQn );

    HAL_NVIC_SetPriority( I2Cx_EV_IRQn
                        , I2Cx_EV_Priority
                        , I2Cx_EV_SubPriority
                        );
    HAL_NVIC_EnableIRQ( I2Cx_EV_IRQn );
}

void HAL_I2C_MspDeInit( I2C_HandleTypeDef* h )
{
    I2Cx_FORCE_RESET();
    I2Cx_RELEASE_RESET();

    HAL_NVIC_DisableIRQ( I2Cx_ER_IRQn );
    HAL_NVIC_DisableIRQ( I2Cx_EV_IRQn );

    HAL_GPIO_DeInit( I2Cx_SCL_GPIO_PORT, I2Cx_SCL_PIN );
    HAL_GPIO_DeInit( I2Cx_SDA_GPIO_PORT, I2Cx_SDA_PIN );
}

void I2C_SetPullups( uint8_t state )
{
    pullup_state = state;

    GPIO_InitTypeDef gpio;
    gpio.Pin       = I2Cx_SCL_PIN
                   | I2Cx_SDA_PIN;
    gpio.Mode      = GPIO_MODE_AF_OD;
    gpio.Pull      = (pullup_state) ? GPIO_PULLUP : GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = I2Cx_SCL_SDA_AF;

    HAL_GPIO_DeInit( I2Cx_SCL_GPIO_PORT, I2Cx_SCL_PIN );
    HAL_GPIO_DeInit( I2Cx_SDA_GPIO_PORT, I2Cx_SDA_PIN );
    HAL_GPIO_Init( I2Cx_SCL_GPIO_PORT, &gpio );
}

uint8_t I2C_GetPullups( void )
{
    return pullup_state;
}

uint8_t I2C_GetAddress( void )
{
    return i2c_handle.Init.OwnAddress1 >> 1;
}

void I2C_SetAddress( uint8_t address )
{
    i2c_handle.Init.OwnAddress1 = address << 1;
    I2C_DeInit();
    if( lead_response != NULL ){
        I2C_Init( address
                , lead_response
                , follow_action
                , follow_request
                , error_action
                );
    }
}


////////////////////////////
// leader set/get

// No single step of a transfer (leading, following, or waiting on the main loop
// for a query answer) should take this long. Past it the driver is stuck: the
// usual cause is the other end of a transfer going away part-way through.
#define I2C_STUCK_MS 100

static void delay_us( uint32_t us )
{
    uint32_t start = DWT->CYCCNT;
    uint32_t n = us * (SystemCoreClock / 1000000);
    while( DWT->CYCCNT - start < n ){;}
}

// Free a bus that a follower is holding. A device that lost track part-way
// through a byte keeps driving SDA low: clock SCL until it lets go (at most 9
// bits), then send STOP so every device returns to idle.
static void bus_clear( void )
{
    GPIO_InitTypeDef gpio;
    gpio.Pin   = I2Cx_SCL_PIN | I2Cx_SDA_PIN;
    gpio.Mode  = GPIO_MODE_OUTPUT_OD;
    gpio.Pull  = (pullup_state) ? GPIO_PULLUP : GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_WritePin( I2Cx_SCL_GPIO_PORT, I2Cx_SCL_PIN | I2Cx_SDA_PIN, GPIO_PIN_SET );
    HAL_GPIO_Init( I2Cx_SCL_GPIO_PORT, &gpio );
    delay_us( 10 );

    for( int i=0; i<9 && !HAL_GPIO_ReadPin( I2Cx_SDA_GPIO_PORT, I2Cx_SDA_PIN ); i++ ){
        HAL_GPIO_WritePin( I2Cx_SCL_GPIO_PORT, I2Cx_SCL_PIN, GPIO_PIN_RESET );
        delay_us( 5 );
        HAL_GPIO_WritePin( I2Cx_SCL_GPIO_PORT, I2Cx_SCL_PIN, GPIO_PIN_SET );
        for( int w=0; w<100 && !HAL_GPIO_ReadPin( I2Cx_SCL_GPIO_PORT, I2Cx_SCL_PIN ); w++ ){
            delay_us( 10 ); // a follower may stretch the clock
        }
        delay_us( 5 );
        i2c_stats.bus_clocks++;
    }
    // STOP: SDA rises while SCL is high
    HAL_GPIO_WritePin( I2Cx_SDA_GPIO_PORT, I2Cx_SDA_PIN, GPIO_PIN_RESET );
    delay_us( 5 );
    HAL_GPIO_WritePin( I2Cx_SDA_GPIO_PORT, I2Cx_SDA_PIN, GPIO_PIN_SET );
    delay_us( 5 );

    if( !HAL_GPIO_ReadPin( I2Cx_SCL_GPIO_PORT, I2Cx_SCL_PIN ) ){ i2c_stats.scl_held++; }
    if( !HAL_GPIO_ReadPin( I2Cx_SDA_GPIO_PORT, I2Cx_SDA_PIN ) ){ i2c_stats.sda_held++; }
    HAL_GPIO_DeInit( I2Cx_SCL_GPIO_PORT, I2Cx_SCL_PIN | I2Cx_SDA_PIN );
}

// Reset the peripheral, free the bus, and start listening again. Whatever
// transfer was in flight is lost.
static void I2C_Recover( void )
{
    TRACE( 17, 0 );
#ifdef TELEMETRY
    trace_frozen = 1;
#endif
    i2c_stats.recover++;
    HAL_NVIC_DisableIRQ( I2Cx_ER_IRQn ); // I2C_Init enables them again
    HAL_NVIC_DisableIRQ( I2Cx_EV_IRQn );
    I2C_Diag_t d;
    I2C_GetDiag( &d );
    i2c_stats.stuck_op    = d.op;
    i2c_stats.stuck_isr   = d.isr;
    i2c_stats.stuck_cr1   = d.cr1;
    i2c_stats.stuck_cr2   = d.cr2;
    i2c_stats.stuck_hal   = (d.hal_state << 8) | d.hal_mode;
    i2c_stats.stuck_lines = (d.scl << 1) | d.sda;
    uint8_t address = I2C_GetAddress();
    HAL_I2C_DeInit( &i2c_handle ); // resets I2C1 and releases the pins
    bus_clear();
    I2C_Init( address, lead_response, follow_action, follow_request, error_action );
}

int I2C_is_ready( void )
{
    static uint32_t since = 0;    // tick when the driver entered its current operation
    static I2C_OP_t since_op = OP_LISTEN;

    I2C_OP_t op = buf.operation;
    if( op == OP_LISTEN ){
        since_op = op;
        return 1;
    }
    uint32_t now = HAL_GetTick();
    if( op != since_op ){ // progress: restart the clock
        since_op = op;
        since = now;
    } else if( now - since > I2C_STUCK_MS ){
        i2c_stats.ready_timeout++;
        I2C_Recover();
        since_op = buf.operation;
    }
    return (buf.operation == OP_LISTEN);
}

// true when the peripheral has bus events the driver has not handled yet: an
// address match, a STOP, or a transfer in progress on the bus
static int bus_unsettled( void )
{
    return I2Cx->ISR & (I2C_ISR_BUSY | I2C_ISR_ADDR | I2C_ISR_STOPF);
}

int I2C_LeadTx( uint8_t  address
              , uint8_t* data
              , uint8_t  size
              )
{
    address <<= 1;
    int error = 0;
    if( size > I2C_MAX_CMD_BYTES ){ size = I2C_MAX_CMD_BYTES; }
    if( buf.operation == OP_LISTEN ){
        buf.address = address; // save in case of abort
        memcpy( buf.tx, data, size );
        BLOCK_IRQS(
            if( bus_unsettled() ){
                error |= 2; // means i2c bus was busy
                i2c_stats.start_busy++;
            } else if( HAL_I2C_DisableListen_IT( &i2c_handle ) != HAL_OK ){
                // a follower transfer began since the check above: leave it be
                error |= 1;
                i2c_stats.start_state++;
            } else if( HAL_I2C_Master_Transmit_IT( &i2c_handle
                    , address
                    , buf.tx
                    , size
                    ) != HAL_OK ){
                error |= 2; // means i2c bus was busy
                i2c_stats.start_busy++;
                HAL_I2C_ListenCpltCallback( &i2c_handle ); // re-enable listen
            } else { buf.operation = OP_LEAD_TX; i2c_stats.lead_tx++; busy_start(); }
            TRACE( 14, error );
        );
    } else {
        printf("can't leadTx because operation=%i\n",buf.operation);
        i2c_stats.start_state++;
        error = 1;
    }
    return error;
}

int I2C_LeadRx( uint8_t  address
              , uint8_t* data
              , uint8_t  size
              , uint8_t  rx_size
              )
{
    address <<= 1;
    int error = 0;
    if( size > I2C_MAX_CMD_BYTES ){ size = I2C_MAX_CMD_BYTES; }
    if( buf.operation == OP_LISTEN ){
        buf.address = address;
        buf.command = data[0];
        memcpy( buf.tx, data, size );
        BLOCK_IRQS(
            if( bus_unsettled() ){
                error |= 0x2;
                i2c_stats.start_busy++;
            } else if( HAL_I2C_DisableListen_IT( &i2c_handle ) != HAL_OK ){
                error |= 0x1;
                i2c_stats.start_state++;
            } else if( __HAL_I2C_GET_FLAG( &i2c_handle, I2C_FLAG_BUSY ) ){
                // NB: Explicitly check BUSY flag as Sequential_Transmit doesn't
                error |= 0x2;
                i2c_stats.start_busy++;
                HAL_I2C_ListenCpltCallback( &i2c_handle ); // re-enable listen
            } else if( HAL_I2C_Master_Sequential_Transmit_IT( &i2c_handle
                            , address
                            , buf.tx
                            , size
                            , I2C_FIRST_FRAME
                            ) != HAL_OK ){
                error |= 0x4;
                i2c_stats.start_busy++;
                HAL_I2C_ListenCpltCallback( &i2c_handle ); // re-enable listen
            } else {
                buf.size = rx_size; // after DisableListen: a follower transfer also uses it
                buf.operation = OP_LEAD_RX;
                i2c_stats.lead_rx++;
                busy_start();
            }
            TRACE( 15, error );
        );
    } else {
        printf("can't leadRx because operation=%i\n",buf.operation);
        i2c_stats.start_state++;
        error = 8;
    }
    return error;
}



////////////////////////////////////////////////
// HAL callbacks
#define I2C_NO_OPTION_FRAME     (0xFFFF0000U)
void I2Cx_EV_IRQHandler( void )
{
    TRACE( 1, 0 );
    // NB: nothing in this file's interrupt paths may printf or send over USB:
    // both share state (the heap, the USB stack) with the main loop.

    // Overload the HAL EV callback to handle early STOPF flag for variable length
    // i2c messagess. And (TODO) handle the arbitration-lost flag directly.

    if( __HAL_I2C_GET_FLAG( &i2c_handle, I2C_FLAG_STOPF ) ){
        if( buf.operation == OP_FOLLOW_RX ){ // End of variable length reception
        // HACK to make the EV Handler treat STOPF as a non-error rx
        // this whole runaround is probably bc shouldn't use sequential rxtx
            i2c_handle.XferCount = 0; // pretend we're done
            i2c_handle.XferOptions = I2C_NO_OPTION_FRAME;
            i2c_handle.State = HAL_I2C_STATE_BUSY_RX;
        }
    }
    if( __HAL_I2C_GET_FLAG( &i2c_handle, I2C_FLAG_ARLO ) ){
        // happens when multiple modules returning different data to TT
        // the losing module seems to cancel it's transmission
        // just need to check that rxing & txing are in a good state
        // also make sure any lib functions are cancelled
        i2c_stats.ev_arlo++;
    }
    HAL_I2C_EV_IRQHandler( &i2c_handle );
    // TODO EV handler will unset an Ack Failure flag but this seems to always
    // happen in some non-error cases. perhaps it makes sense to check here, after
    // we've run the callback and see if the flags remain?
    // perhaps all the flags are crushed by the event handler?
    if( __HAL_I2C_GET_FLAG( &i2c_handle, I2C_FLAG_AF ) ){
        //printf("TODO Ack failure\n");
    }
}


/////////////////////////////////////
// lead

// finished sending
void HAL_I2C_MasterTxCpltCallback( I2C_HandleTypeDef* h )
{
    TRACE( 11, 0 );
    if( buf.operation == OP_LEAD_RX ){
        BLOCK_IRQS(
            //if( HAL_I2C_Master_Receive_IT( &i2c_handle
            if( HAL_I2C_Master_Sequential_Receive_IT( &i2c_handle
                    , buf.address
                    , buf.data
                    , buf.size
                    , I2C_LAST_FRAME
                    ) != HAL_OK ){ printf("LeadRx failed\n"); }
        );
    } else { // buf.operation == OP_LEAD_TX
        i2c_stats.lead_tx_done++;
        HAL_I2C_ListenCpltCallback( &i2c_handle );
    }
}

// received response from follower
void HAL_I2C_MasterRxCpltCallback( I2C_HandleTypeDef* h )
{
    TRACE( 12, 0 );
    i2c_stats.lead_rx_done++;
    (*lead_response)( buf.address>>1
                    , buf.command
                    , buf.data
                    );
    HAL_I2C_ListenCpltCallback(h);
}


/////////////////////////////////////
// follow

// follower action/request received
void HAL_I2C_AddrCallback( I2C_HandleTypeDef* h
                         , uint8_t            TransferDirection
                         , uint16_t           AddrMatchCode
                         )
{
    TRACE( 3, TransferDirection );
    uint8_t error = 0;
    busy_start();
    if( TransferDirection ){ // Request data
        i2c_stats.follow_addr_tx++;
        // a repeated start straight after the leader wrote the request
        int fresh = (buf.operation == OP_FOLLOW_RX);
        int size = (*follow_request)( buf.data, fresh );
        if( size < 0 ){
            // ADDR stays set, so the peripheral holds SCL low until the main
            // loop has the answer and calls I2C_FollowRespond()
            i2c_stats.follow_defer++;
            TRACE( 5, 0 );
            buf.operation = OP_FOLLOW_TX_WAIT;
            return;
        }
        if( size < 1 ){ // never leave the leader waiting with nothing to send
            buf.data[0] = 0;
            size = 1;
        }
        buf.size = size;
        buf.operation = OP_FOLLOW_TX_READY;
        error = HAL_I2C_Slave_Sequential_Transmit_IT( &i2c_handle
                , buf.data
                , buf.size
                , I2C_LAST_FRAME
                );
        if( error ){ i2c_stats.follow_start_fail++; }
        else{ buf.operation = OP_FOLLOW_TX; }
    } else {
        i2c_stats.follow_addr_rx++;
        BLOCK_IRQS(
            error = HAL_I2C_Slave_Sequential_Receive_IT( &i2c_handle
                    , buf.data
                    , I2C_MAX_CMD_BYTES
                    , I2C_NEXT_FRAME
                    );
        );
        if( error ){ i2c_stats.follow_start_fail++; }
        else { buf.operation = OP_FOLLOW_RX; }
    }
}

int I2C_FollowRespond( uint8_t* data, int size )
{
    TRACE( 6, 0 );
    int error = 1;
    if( size > I2C_MAX_CMD_BYTES ){ size = I2C_MAX_CMD_BYTES; }
    if( size < 1 ){ data = (uint8_t*)"\0"; size = 1; } // never leave the leader waiting
    BLOCK_IRQS(
        if( buf.operation == OP_FOLLOW_TX_WAIT ){
            memcpy( buf.data, data, size );
            buf.size = size;
            if( HAL_I2C_Slave_Sequential_Transmit_IT( &i2c_handle
                    , buf.data
                    , buf.size
                    , I2C_LAST_FRAME
                    ) == HAL_OK ){
                buf.operation = OP_FOLLOW_TX;
                error = 0;
            }
        }
    );
    if( error ){ i2c_stats.follow_stale++; }
    return error;
}

void HAL_I2C_SlaveRxCpltCallback( I2C_HandleTypeDef* h )
{
    TRACE( 7, 0 );
    // triggered by STOPF flag in EV_IrqHandler
    i2c_stats.follow_rx_done++;
    (*follow_action)( buf.data );
    HAL_I2C_ListenCpltCallback( &i2c_handle );
}

void HAL_I2C_SlaveTxCpltCallback( I2C_HandleTypeDef* h )
{
    TRACE( 8, 0 );
    i2c_stats.follow_tx_done++;
    // The leader has NACKed our last byte but not yet sent STOP. Stay out of
    // OP_LISTEN until the STOP arrives (the HAL then calls ListenCplt): leading
    // before then left a stale STOPF that the master interrupt took as the end
    // of its own write, and the bus stalled.
}

// follower action/request completed
void HAL_I2C_ListenCpltCallback( I2C_HandleTypeDef* hi2c )
{
    TRACE( 9, 0 );
    BLOCK_IRQS(
        if( HAL_I2C_EnableListen_IT( &i2c_handle ) != HAL_OK ){
            i2c_stats.listen_fail++;
            TRACE( 10, hi2c->State );
        } else { buf.operation = OP_LISTEN; busy_end(); }
    );
}


////////////////////////////////////////////
// error handling

void I2Cx_ER_IRQHandler( void )
{
    TRACE( 2, 0 );
    buf.operation = OP_ERROR;
    HAL_I2C_ER_IRQHandler( &i2c_handle );
}

// i think this can only call with 3 errors:
// HAL_I2C_ERROR_BERR HAL_I2C_ERROR_OVR HAL_I2C_ERROR_ARLO
// but perhaps also
// HAL_I2C_ERROR_AF
// I2C_ITError
void HAL_I2C_ErrorCallback( I2C_HandleTypeDef* h )
{
    TRACE( 13, (uint8_t)h->ErrorCode );
    uint32_t e = h->ErrorCode;
    if( e & HAL_I2C_ERROR_AF   ){ i2c_stats.err_af++; }
    if( e & HAL_I2C_ERROR_ARLO ){ i2c_stats.err_arlo++; }
    if( e & HAL_I2C_ERROR_BERR ){ i2c_stats.err_berr++; }
    if( e & HAL_I2C_ERROR_OVR  ){ i2c_stats.err_ovr++; }
    if( e & ~(HAL_I2C_ERROR_AF | HAL_I2C_ERROR_ARLO
             | HAL_I2C_ERROR_BERR | HAL_I2C_ERROR_OVR) ){ i2c_stats.err_other++; }
    // printf("I2C_ERROR_");
    // switch( h->ErrorCode ){
    //     case HAL_I2C_ERROR_BERR: printf("BERR\n"); break;
    //     case HAL_I2C_ERROR_OVR:  printf("OVR\n"); break;
    //     case HAL_I2C_ERROR_ARLO: printf("ARLO\n"); break;
    //     case HAL_I2C_ERROR_AF:   printf("AF\n"); break;
    //     default: printf("unknown!\n"); break;
    // }
    if( h->ErrorCode == HAL_I2C_ERROR_AF ){
        (*error_action)( 0 );
    } else {
        if( h->ErrorCode == 2 ){
            (*error_action)( 1 );
        } else{
            (*error_action)( (int)h->ErrorCode );
        }
    }
    HAL_I2C_ListenCpltCallback( &i2c_handle ); // Re-enable Listen
}

void HAL_I2C_AbortCpltCallback( I2C_HandleTypeDef* h )
{
    TRACE( 18, 0 );
    i2c_stats.abort_done++;
    HAL_I2C_ListenCpltCallback( &i2c_handle );
}
