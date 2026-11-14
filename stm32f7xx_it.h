#pragma once

#include <stdint.h>

extern volatile int CPU_count;
int CPU_GetCount( void );
void SysTick_Handler(void);

// fault handlers are in lib/crashreport.c

#ifndef RELEASE // DEBUG
void SVC_Handler(void);
void DebugMon_Handler(void);
void PendSV_Handler(void);
#endif
