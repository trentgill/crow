#include "system.h"

#include <stm32f7xx.h>
#include <stm32f7xx_hal.h>
#include <stdio.h>

#include "ll/debug_usart.h" // U_PrintNow()
#include "lib/caw.h" // Caw_send_luachunk()

// private declarations
static void Sys_Clk_Config(void);
static void Error_Handler(void);
static void MPU_Config(void);
static void CPU_CACHE_Enable(void);

// public definitions
void system_init(void)
{
    MPU_Config();
    CPU_CACHE_Enable();
    HAL_Init();
    Sys_Clk_Config();
}

void system_print_version(void)
{
    char s[64];
    sprintf( s, "^^version('%s')", VERSION );
    Caw_send_luachunk( s );
}

void system_print_identity(void)
{
    char s[64];
    sprintf( s, "^^identity('0x%08x%08x%08x')"
              , getUID_Word(0)
              , getUID_Word(4)
              , getUID_Word(8)
              );
    Caw_send_luachunk( s );
}

// private definitions
static void Sys_Clk_Config(void)
{
    __HAL_RCC_PWR_CLK_ENABLE();

    // 216MHz needs scale 1 plus over-drive. scale 2 is only rated to 180MHz
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    static RCC_OscInitTypeDef osc;
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState       = RCC_HSE_ON;
    osc.PLL.PLLState   = RCC_PLL_ON;
    osc.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM       = 8;
    osc.PLL.PLLN       = 432;
    osc.PLL.PLLP       = RCC_PLLP_DIV2;
    osc.PLL.PLLQ       = 9;
    if(HAL_RCC_OscConfig(&osc) != HAL_OK){ Error_Handler(); }

    if(HAL_PWREx_EnableOverDrive() != HAL_OK) { Error_Handler(); }

    static RCC_ClkInitTypeDef clk;
    clk.ClockType      = RCC_CLOCKTYPE_SYSCLK
                       | RCC_CLOCKTYPE_HCLK
                       | RCC_CLOCKTYPE_PCLK1
                       | RCC_CLOCKTYPE_PCLK2
                       ;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV2;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    if(HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_7) != HAL_OK){
        Error_Handler();
    }
}

static void MPU_Config(void)
{
    MPU_Region_InitTypeDef mpu;

    HAL_MPU_Disable();

    // Configure the MPU attributes as WT for SRAM
    mpu.Enable           = MPU_REGION_ENABLE;
    // mpu.Enable           = MPU_REGION_DISABLE;
    // mpu.BaseAddress      = 0x20020000;
    mpu.BaseAddress      = 0x20000000;
    mpu.Size             = MPU_REGION_SIZE_256KB;
    mpu.AccessPermission = MPU_REGION_FULL_ACCESS;
    mpu.IsBufferable     = MPU_ACCESS_NOT_BUFFERABLE;
    mpu.IsCacheable      = MPU_ACCESS_CACHEABLE;
    mpu.IsShareable      = MPU_ACCESS_SHAREABLE;
    mpu.Number           = MPU_REGION_NUMBER0;
    mpu.TypeExtField     = MPU_TEX_LEVEL0;
    mpu.SubRegionDisable = 0x00;
    mpu.DisableExec      = MPU_INSTRUCTION_ACCESS_ENABLE;
    HAL_MPU_ConfigRegion(&mpu);

    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}

static void CPU_CACHE_Enable(void)
{
    SCB_EnableICache();
    SCB_EnableDCache();
}

static void Error_Handler(void)
{
    printf("Error Handler\n");
    U_PrintNow();
    while(1){;;}
}

unsigned int getUID_Word( unsigned int offset )
{
    const uint32_t base = 0x1FF07A10;
    //return (uint32_t)(READ_REG(*((uint32_t*)(UID_BASE + offset))));
    //return (uint32_t)(READ_REG(UID_BASE + offset));
    uint32_t* x = (uint32_t*)(base + offset);
    return (unsigned int)(READ_REG(*x));
    //return (uint32_t)x;
}
