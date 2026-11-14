#include "flash.h"

#include <stdio.h>
#include <string.h> // memcpy()
#include "../ll/debug_usart.h"

#define USER_MAGIC 0xA  // bit pattern
#define USER_CLEAR 0xC  // bit pattern

// private declarations
static void clear_flash( uint32_t sector, uint32_t location );
static uint32_t version12b( void );

// USER LUA SCRIPT //

USERSCRIPT_t Flash_which_user_script( void )
{
    uint32_t script = (0xF & (*(__IO uint32_t*)USER_SCRIPT_LOCATION));
    switch( script ){
        case USER_MAGIC: return USERSCRIPT_User;
        case USER_CLEAR: return USERSCRIPT_Clear;
        default:         return USERSCRIPT_Default;
    }
}

void Flash_default_user_script( void )
{
    printf("loading First script\n");
    clear_flash( USER_SCRIPT_SECTOR, USER_SCRIPT_LOCATION );
}

void Flash_clear_user_script( void )
{
    printf("clear user script\n");
// clear the flash
    HAL_FLASH_Unlock();
    FLASH_EraseInitTypeDef erase_setup =
        { .TypeErase    = FLASH_TYPEERASE_SECTORS
        , .Sector       = USER_SCRIPT_SECTOR
        , .NbSectors    = 1
        , .VoltageRange = FLASH_VOLTAGE_RANGE_3
        };
    uint32_t error_status;
    HAL_FLASHEx_Erase( &erase_setup, &error_status );

// set status word
    uint32_t sd_addr = USER_SCRIPT_LOCATION;
    uint32_t status_word = USER_CLEAR          // b0:3   user script present
                         | (version12b() << 4) // b4:15  version control
                         | 0                   // b16:32 length in bytes
                         ;
    HAL_FLASH_Program( FLASH_TYPEPROGRAM_WORD
                     , sd_addr
                     , status_word
                     );
    HAL_FLASH_Lock();
}

uint8_t Flash_write_user_script( char* script, uint32_t length )
{
    if( length > USER_SCRIPT_SIZE ){ return 1; } // ERROR: Script too long

// clear the flash
    HAL_FLASH_Unlock();
    FLASH_EraseInitTypeDef erase_setup =
        { .TypeErase    = FLASH_TYPEERASE_SECTORS
        , .Sector       = USER_SCRIPT_SECTOR
        , .NbSectors    = 1
        , .VoltageRange = FLASH_VOLTAGE_RANGE_3
        };
    uint32_t error_status;
    HAL_FLASHEx_Erase( &erase_setup, &error_status );

// set status word
    uint32_t sd_addr = USER_SCRIPT_LOCATION;
    uint32_t status_word = USER_MAGIC          // b0:3   user script present
                         | (version12b() << 4) // b4:15  version control
                         | (length << 16)      // b16:32 length in bytes
                         ;
    HAL_FLASH_Program( FLASH_TYPEPROGRAM_WORD
                     , sd_addr
                     , status_word
                     );
// program script
    length >>= 2; length++;
    while( length ){
        sd_addr += 4;
        HAL_FLASH_Program( FLASH_TYPEPROGRAM_WORD
                         , sd_addr
                         , *(uint32_t*)script
                         );
        script += 4;
        length--;
    }
    HAL_FLASH_Lock();
    return 0;
}

uint16_t Flash_read_user_scriptlen( void )
{
    return (*((__IO uint16_t*)(USER_SCRIPT_LOCATION + 2)));
}

char* Flash_read_user_scriptaddr( void )
{
    return (char*)(USER_SCRIPT_LOCATION + 4);
}

uint8_t Flash_read_user_script( char* buffer )
{
    if( Flash_which_user_script() != USERSCRIPT_User ){ return 1; } // no script

    uint16_t byte_length = Flash_read_user_scriptlen();
    uint16_t word_length = byte_length >> 2; // drops 0-3 chars
    uint16_t trailing_bytes = byte_length & 0x3; // handle last 0-3 chars

    uint32_t sd_addr = USER_SCRIPT_LOCATION + 4;

    // write whole words
    uint32_t* word_buffer = (uint32_t*)buffer;
    while( word_length-- ){
        *word_buffer++ = (*(__IO uint32_t*)sd_addr);
        sd_addr += 4;
    }

    // write final 0-3 bytes
    uint32_t last_word = (*(__IO uint32_t*)sd_addr); // incomplete word
    char* char_buffer = (char*)word_buffer; // continue where word_buffer left off
    for( int i=0; i<trailing_bytes; i++ ){ // write final chars individually
        char_buffer[i] = (last_word >> (i*8)) & 0xFF; // LSB first
    }

    return 0;
}

// CALIBRATION //

uint8_t Flash_is_calibrated( void )
{
    return ((*(__IO uint32_t*)CALIBRATION_LOCATION) == USER_MAGIC );
}

void Flash_clear_calibration( void )
{
    clear_flash( CALIBRATION_SECTOR, CALIBRATION_LOCATION );
}

uint8_t Flash_write_calibration( uint8_t* data, uint32_t length )
{
    if( length > CALIBRATION_SIZE ){ return 1; } // ERROR: data too long

    // TODO: save the module version here? can also be queried from lua

// clear the flash
	HAL_FLASH_Unlock();
	FLASH_EraseInitTypeDef erase_setup =
		{ .TypeErase    = FLASH_TYPEERASE_SECTORS
		, .Sector       = CALIBRATION_SECTOR
		, .NbSectors    = 1
		, .VoltageRange = FLASH_VOLTAGE_RANGE_3
		};
    uint32_t error_status;
	HAL_FLASHEx_Erase( &erase_setup, &error_status );

// set status word
    uint32_t sd_addr = CALIBRATION_LOCATION;
	HAL_FLASH_Program( FLASH_TYPEPROGRAM_WORD
					 , sd_addr
					 , USER_MAGIC
					 );
// program script
    length >>= 2; length++;
    while( length ){
        sd_addr += 4;
	    HAL_FLASH_Program( FLASH_TYPEPROGRAM_WORD
	    				 , sd_addr
	    				 , *(uint32_t*)data
	    				 );
        data += 4;
        length--;
    }
	HAL_FLASH_Lock();
    return 0;
}

uint8_t Flash_read_calibration( uint8_t* data, uint32_t length )
{
    if( !Flash_is_calibrated() ){ return 1; } // no calibration

    // exactly length bytes: rounding up to whole words overruns the caller's buffer
    memcpy( data, (void*)(CALIBRATION_LOCATION + 4), length ); // skip status word
    return 0;
}


// private fns

static void clear_flash( uint32_t sector, uint32_t location )
{
	HAL_FLASH_Unlock();
	FLASH_EraseInitTypeDef erase_setup =
		{ .TypeErase    = FLASH_TYPEERASE_SECTORS
		, .Sector       = sector
		, .NbSectors    = 1
		, .VoltageRange = FLASH_VOLTAGE_RANGE_3
		};
    uint32_t error_status;
	HAL_FLASHEx_Erase( &erase_setup, &error_status );
	HAL_FLASH_Program( FLASH_TYPEPROGRAM_WORD
					 , location
					 , 0 // no script | zero length
					 );
    // TODO error check
	HAL_FLASH_Lock();
}

static uint32_t version12b( void )
{
    const uint32_t c = (uint32_t)( (VERSION[1]-0x30)<<8
                                 | (VERSION[3]-0x30)<<4
                                 | (VERSION[5]-0x30)
                                 ) & 0xFFF; // mask for safety
    return c;
}
