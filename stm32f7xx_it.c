#include "stm32f7xx_it.h"

#include "stm32f7xx_hal.h" // HAL_IncTick

volatile int CPU_count = 0;

int counts[8] = {0,0,0,0,0,0,0,0};
int pCount = 0;
void SysTick_Handler(void)
{
    HAL_IncTick();
    counts[pCount] = CPU_count;
    if( (++pCount) >= 8 ){ pCount = 0; }
    CPU_count = 0;
}

int CPU_GetCount( void )
{
    int c = 0;
    for( int i=0; i<8; i++ ){
        c += counts[i];
    }
    return c;
}

// NMI, HardFault, MemManage, BusFault & UsageFault: lib/crashreport.c
// (in both RELEASE & DEBUG they record a crash report, then reset)

#ifndef RELEASE // DEBUG mode

#include <stdio.h> // printf
#include "ll/debug_usart.h" // U_PrintNow

static void error( char* msg ){
    printf("%s\n", msg);
    U_PrintNow();
    while(1);
}
void SVC_Handler(void){ error("!SVC"); }
void DebugMon_Handler(void){ error("!DebugMon"); }
void PendSV_Handler(void){ error("!PendSV"); }
#endif // DEBUG
