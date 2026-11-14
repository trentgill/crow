#pragma once
#include "stm32f7xx.h"

#define UNUSED(X) (void)X

uint32_t HAL_GetTick( void ); // the fake clock in caw_test.c
