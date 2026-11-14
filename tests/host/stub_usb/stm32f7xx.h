#pragma once
// host stand-in for the device header: just enough for lib/caw.c and
// usbd/usbd_cdc_interface.c. the interrupt state is owned by caw_test.c.

#include <stdint.h>
#include <stddef.h>

extern uint32_t test_ipsr;    // 0 = thread mode, else the active exception
extern uint32_t test_primask; // 1 = interrupts blocked

static inline uint32_t __get_IPSR( void ){ return test_ipsr; }
static inline uint32_t __get_PRIMASK( void ){ return test_primask; }
static inline void __set_PRIMASK( uint32_t m ){ test_primask = m; }
static inline void __disable_irq( void ){ test_primask = 1; }

#define BLOCK_IRQS(code) do{ \
                            uint32_t old_primask = __get_PRIMASK(); \
                            __disable_irq(); \
                            do{code} while(0); \
                            __set_PRIMASK( old_primask ); \
                        } while(0);
