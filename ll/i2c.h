#pragma once

#include <stm32f7xx.h>
#include <stdbool.h>
#include "interrupts.h" // I2C_Priority

//////////////////////////////////////////
// hardware configuration


// these are from reference manual (p1232) but they do not give correct values
// I2C_TIMINGR register
// original v4.0.1 timings
// #define I2C_TIMING_STABLE  0xD0421C0C // runs at 55kHz
// #define I2C_TIMING_SPEED   0xA0420B07 // runs at 320kHz
// new v4.0.2 after much research of i2c & setting up a high-cap setup
// v4.0.2 values were tuned with I2C1 clocked from a 108MHz PCLK1, which is
// over the 54MHz APB1 rating. these are the same SCL low/high, setup and hold
// times re-expressed for 54MHz: SCL low and high within ~1%, setup and hold
// rounded up. counts below are in kernel clock cycles (field + 1)
//   was 0xA0411626: presc 11 -> 5, scll 39 -> 43, sclh 23 -> 25, scldel 5 -> 6
#define I2C_TIMING_STABLE  0x4051182A // 97~148kHz depending on bus capacitance
//   was 0x60440B13: presc 7 -> 3, scll 20 -> 23, sclh 12 -> 14, sdadel 4 -> 5, scldel 5 -> 6
#define I2C_TIMING_SPEED   0x20550D16 // 165~350kHz depending on bus capacitance
//   was 0xB042080B: presc 12 -> 6, the rest unchanged (exact)
#define I2C_TIMING_CLASSIC 0x5042080B // old timings for regression testing
  // 157~320kHz for classic mode.

#define I2Cx                            I2C1
#define I2Cx_CLK_ENABLE()               __HAL_RCC_I2C1_CLK_ENABLE()
#define I2Cx_SDA_GPIO_CLK_ENABLE()      __HAL_RCC_GPIOB_CLK_ENABLE()
#define I2Cx_SCL_GPIO_CLK_ENABLE()      __HAL_RCC_GPIOB_CLK_ENABLE()

#define I2Cx_FORCE_RESET()              __HAL_RCC_I2C1_FORCE_RESET()
#define I2Cx_RELEASE_RESET()            __HAL_RCC_I2C1_RELEASE_RESET()

#define I2Cx_SCL_PIN                    GPIO_PIN_6
#define I2Cx_SCL_GPIO_PORT              GPIOB
#define I2Cx_SDA_PIN                    GPIO_PIN_7
#define I2Cx_SDA_GPIO_PORT              GPIOB
#define I2Cx_SCL_SDA_AF                 GPIO_AF4_I2C1

#define I2Cx_EV_IRQn                    I2C1_EV_IRQn
#define I2Cx_ER_IRQn                    I2C1_ER_IRQn
#define I2Cx_EV_IRQHandler              I2C1_EV_IRQHandler
#define I2Cx_ER_IRQHandler              I2C1_ER_IRQHandler

#define I2Cx_ER_Priority                I2C_Priority
#define I2Cx_ER_SubPriority             3
#define I2Cx_EV_Priority                I2C_Priority
#define I2Cx_EV_SubPriority             4


///////////////////////////
// queue setup
#define I2C_MAX_CMD_BYTES 10
#define I2C_BUFFER_LEN    32


///////////////////////////
// counters, for ii.stats(). written from the i2c interrupts and from ii.c
typedef struct{
    // leading
    uint32_t lead_tx;        // write transfers started
    uint32_t lead_rx;        // query transfers started
    uint32_t lead_tx_done;   // write completed (STOP sent, all bytes ACKed)
    uint32_t lead_rx_done;   // query answer received
    uint32_t start_busy;     // could not start: bus busy
    uint32_t start_state;    // could not start: driver not listening
    uint32_t ready_timeout;  // driver stuck for I2C_STUCK_MS: reset and recovered
    uint32_t recover;        // peripheral resets
    uint32_t bus_clocks;     // SCL pulses sent to free a held SDA
    uint32_t scl_held;       // SCL still low after recovery (another device holds it)
    uint32_t sda_held;       // SDA still low after recovery
    // driver state just before the last reset
    uint32_t stuck_op, stuck_isr, stuck_cr1, stuck_cr2, stuck_hal, stuck_lines;
    uint32_t abort_done;
    // following
    uint32_t follow_addr_rx; // our address matched, leader writing
    uint32_t follow_addr_tx; // our address matched, leader reading
    uint32_t follow_rx_done;
    uint32_t follow_tx_done;
    uint32_t follow_defer;   // answer deferred to the main loop (SCL held)
    uint32_t follow_stale;   // deferred answer ready after the leader gave up
    uint32_t follow_start_fail; // HAL refused to start a follower transfer
    // errors (HAL ErrorCode bits, each counted)
    uint32_t err_af;
    uint32_t err_arlo;
    uint32_t err_berr;
    uint32_t err_ovr;
    uint32_t err_other;
    uint32_t ev_arlo;        // ARLO seen in the event interrupt
    uint32_t listen_fail;    // could not re-enable listen
    // ii layer
    uint32_t q_full;         // leader queue full, message dropped
    uint32_t retry;
    uint32_t abandon;        // dropped after a failed retry
    uint32_t fq_full;        // follower queue full, message dropped
    // timing
    uint32_t busy_max_us;    // longest time away from listening
} I2C_Stats_t;

extern I2C_Stats_t i2c_stats;
void I2C_ResetStats( void );

// a snapshot of the peripheral and driver state, for debugging a stuck bus
typedef struct{
    uint32_t isr, cr1, cr2;
    uint8_t  hal_state, hal_mode, op, scl, sda;
    uint32_t hal_error;
} I2C_Diag_t;
void I2C_GetDiag( I2C_Diag_t* d );

// event trace (TELEMETRY builds): entry ix, oldest first. 0 past the end
int I2C_GetTrace( int ix, uint32_t* t, uint32_t* packed );
void I2C_TraceRestart( void );


///////////////////////////////////////
// fnptr typedefs for low-level
typedef void (*I2C_lead_callback_t)( uint8_t address, uint8_t command, uint8_t* rx_data );
typedef int (*I2C_follow_callback_t)( uint8_t* pdata );
// a leader is reading from us. `fresh` is true when pdata holds the request it
// wrote just before (repeated start), false when the request came in an earlier
// transfer. return the number of bytes to send, or -1 to answer later with
// I2C_FollowRespond() (SCL is held low until then)
typedef int (*I2C_request_callback_t)( uint8_t* pdata, int fresh );
typedef void (*I2C_error_callback_t)( int error_code );


/////////////////
// setup
uint8_t I2C_Init( uint8_t               address
                , I2C_lead_callback_t   lead_callback
                , I2C_follow_callback_t follow_action_callback
                , I2C_request_callback_t follow_request_callback
                , I2C_error_callback_t  error_callback );
void I2C_DeInit( void );

uint8_t I2C_is_boot( void );

void I2C_SetPullups( uint8_t state );
uint8_t I2C_GetPullups( void );

void I2C_SetTimings( uint32_t mask );

uint8_t I2C_GetAddress( void );
void I2C_SetAddress( uint8_t address );


// send the answer to a request that I2C_request_callback_t deferred.
// returns 0 if sent, 1 if no leader is waiting for it any more
int I2C_FollowRespond( uint8_t* data, int size );


/////////////////////////
// lead the bus
int I2C_is_ready( void );
int I2C_LeadTx( uint8_t  address
              , uint8_t* data
              , uint8_t  size
              );
int I2C_LeadRx( uint8_t  address
              , uint8_t* data
              , uint8_t  size
              , uint8_t  rx_size
              );
