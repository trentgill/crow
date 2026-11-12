#include "usbd_main.h"

#include "../ll/debug_usart.h"

USBD_HandleTypeDef USBD_Device;

void USB_CDC_Init( void )
{
    // Init Device Library
    if( USBD_Init(&USBD_Device, &VCP_Desc, 0) != USBD_OK ){
        printf("Invalid Device handle\n");
    }

    // Add Supported Class
    if( USBD_RegisterClass(&USBD_Device, USBD_CDC_CLASS) != USBD_OK ){
        printf("Invalid class handle\n");
    }

    // Add CDC Interface Class
    if( USBD_CDC_RegisterInterface(&USBD_Device, &USBD_CDC_fops) != USBD_OK ){
        printf("Failed to register interface\n");
    }

    // Start Device Process
    USBD_Start(&USBD_Device);
}

void USB_CDC_DeInit(void)
{
}

extern PCD_HandleTypeDef hpcd;

#ifdef USE_USB_FS
void OTG_FS_IRQHandler(void)
#else
void OTG_HS_IRQHandler(void)
#endif
{
    HAL_PCD_IRQHandler(&hpcd);
}
